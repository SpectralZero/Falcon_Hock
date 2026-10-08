/*
 * umf_overlay_gdi.c — §OVERLAYGDI: layered-window GDI overlay (non-D3D fallback)
 *
 * A transparent, click-through, top-most layered window drawn with per-pixel
 * alpha via UpdateLayeredWindow. Mods issue retained draw commands (rect/line/
 * text) from a frame callback; render() rasterises them and blits.
 *
 * Design: the shape rasteriser writes straight 0xAARRGGBB pixels directly into
 * a caller buffer with no window, GDI, or desktop dependency — so it is fully
 * deterministic and unit-testable. Text needs a real font, so it is composited
 * through a throwaway GDI DIB using a sentinel-colour mask (any pixel the font
 * touched becomes opaque; everything else stays transparent). The windowed
 * present path reuses the same rasteriser, premultiplies alpha, and calls
 * UpdateLayeredWindow — the DIB's BGRA byte order already matches 0xAARRGGBB.
 */

#include "umf/umf.h"

#define UMF_GDI_MAX_COMMANDS 2048
#define UMF_GDI_TEXT_SENTINEL 0x00FE02FDu   /* improbable "untouched" colour */

typedef enum { GDI_RECT, GDI_LINE, GDI_TEXT } UmfGdiCmdType;

typedef struct {
    UmfGdiCmdType type;
    int           a, b, c, d;   /* rect: x,y,w,h | line: x0,y0,x1,y1 | text: x,y */
    uint32_t      color;        /* straight 0xAARRGGBB */
    bool          filled;       /* rect only */
    char          text[128];
} UmfGdiCmd;

static UmfGdiCmd g_cmds[UMF_GDI_MAX_COMMANDS];
static int       g_cmd_count = 0;

static HWND              g_hwnd   = NULL;
static HWND              g_target = NULL;
static int               g_x = 0, g_y = 0, g_w = 0, g_h = 0;
static HDC               g_memdc = NULL;
static HBITMAP           g_dib    = NULL;
static uint32_t*         g_bits   = NULL;
static bool              g_active = false;
static UmfOverlayFrameFn g_frame_cb = NULL;

/* ────────────────────────────────────────────────────────────────
 * Colour + command list
 * ──────────────────────────────────────────────────────────────── */

uint32_t umf_gdi_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8)  |  (uint32_t)b;
}

void umf_gdi_overlay_begin_frame(void) { g_cmd_count = 0; }

int  umf_gdi_overlay_command_count(void) { return g_cmd_count; }

static UmfGdiCmd* push_cmd(void) {
    if (g_cmd_count >= UMF_GDI_MAX_COMMANDS) return NULL;
    UmfGdiCmd* c = &g_cmds[g_cmd_count++];
    memset(c, 0, sizeof(*c));
    return c;
}

void umf_gdi_rect(int x, int y, int w, int h, uint32_t argb, bool filled) {
    UmfGdiCmd* c = push_cmd();
    if (!c) return;
    c->type = GDI_RECT;
    c->a = x; c->b = y; c->c = w; c->d = h;
    c->color = argb; c->filled = filled;
}

void umf_gdi_line(int x0, int y0, int x1, int y1, uint32_t argb) {
    UmfGdiCmd* c = push_cmd();
    if (!c) return;
    c->type = GDI_LINE;
    c->a = x0; c->b = y0; c->c = x1; c->d = y1;
    c->color = argb;
}

void umf_gdi_text(int x, int y, uint32_t argb, const char* text) {
    UmfGdiCmd* c = push_cmd();
    if (!c) return;
    c->type = GDI_TEXT;
    c->a = x; c->b = y; c->color = argb;
    snprintf(c->text, sizeof(c->text), "%s", text ? text : "");
}

/* ────────────────────────────────────────────────────────────────
 * Software rasteriser (shapes) + GDI text compositor
 * ──────────────────────────────────────────────────────────────── */

static inline void put_px(uint32_t* buf, int w, int h, int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h)
        buf[(size_t)y * w + x] = c;
}

static void draw_shapes(uint32_t* buf, int w, int h) {
    for (int i = 0; i < g_cmd_count; i++) {
        const UmfGdiCmd* c = &g_cmds[i];
        if (c->type == GDI_RECT) {
            int x0 = c->a, y0 = c->b, x1 = c->a + c->c, y1 = c->b + c->d;
            if (c->filled) {
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                        put_px(buf, w, h, x, y, c->color);
            } else {
                for (int x = x0; x < x1; x++) {
                    put_px(buf, w, h, x, y0, c->color);
                    put_px(buf, w, h, x, y1 - 1, c->color);
                }
                for (int y = y0; y < y1; y++) {
                    put_px(buf, w, h, x0, y, c->color);
                    put_px(buf, w, h, x1 - 1, y, c->color);
                }
            }
        } else if (c->type == GDI_LINE) {
            int x0 = c->a, y0 = c->b, x1 = c->c, y1 = c->d;    /* Bresenham */
            int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
            int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
            int err = dx + dy;
            for (;;) {
                put_px(buf, w, h, x0, y0, c->color);
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        }
    }
}

/* Composite any text commands over `buf` using a throwaway GDI DIB. */
static void draw_text(uint32_t* buf, int w, int h) {
    bool any = false;
    for (int i = 0; i < g_cmd_count; i++)
        if (g_cmds[i].type == GDI_TEXT) { any = true; break; }
    if (!any || w <= 0 || h <= 0) return;

    HDC memdc = CreateCompatibleDC(NULL);
    if (!memdc) return;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;             /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* tbits = NULL;
    HBITMAP dib = CreateDIBSection(memdc, &bi, DIB_RGB_COLORS, &tbits, NULL, 0);
    if (!dib) { DeleteDC(memdc); return; }
    HGDIOBJ old = SelectObject(memdc, dib);

    uint32_t* tp = (uint32_t*)tbits;
    for (int i = 0; i < w * h; i++) tp[i] = UMF_GDI_TEXT_SENTINEL;

    SetBkMode(memdc, TRANSPARENT);
    for (int i = 0; i < g_cmd_count; i++) {
        const UmfGdiCmd* c = &g_cmds[i];
        if (c->type != GDI_TEXT) continue;
        uint8_t r = (c->color >> 16) & 0xFF, g = (c->color >> 8) & 0xFF,
                b =  c->color        & 0xFF;
        SetTextColor(memdc, RGB(r, g, b));
        TextOutA(memdc, c->a, c->b, c->text, (int)strlen(c->text));
    }
    GdiFlush();

    for (int i = 0; i < w * h; i++) {
        uint32_t p = tp[i];
        if (p != UMF_GDI_TEXT_SENTINEL)
            buf[i] = 0xFF000000u | (p & 0x00FFFFFFu);   /* opaque where touched */
    }

    SelectObject(memdc, old);
    DeleteObject(dib);
    DeleteDC(memdc);
}

bool umf_gdi_overlay_rasterize(uint32_t* out_argb, int w, int h) {
    if (!out_argb || w <= 0 || h <= 0) return false;
    memset(out_argb, 0, (size_t)w * h * sizeof(uint32_t));
    draw_shapes(out_argb, w, h);
    draw_text(out_argb, w, h);
    return true;
}

/* ────────────────────────────────────────────────────────────────
 * Layered window lifecycle + present
 * ──────────────────────────────────────────────────────────────── */

static const wchar_t* kGdiClass = L"UMFGdiOverlay";

static void register_class(void) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = kGdiClass;
    RegisterClassExW(&wc);
    done = true;
}

static void compute_bounds(HWND target) {
    if (target) {
        RECT rc; POINT tl = {0, 0};
        GetClientRect(target, &rc);
        ClientToScreen(target, &tl);
        g_x = tl.x; g_y = tl.y;
        g_w = rc.right - rc.left;
        g_h = rc.bottom - rc.top;
    } else {
        g_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        g_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    if (g_w <= 0) g_w = 256;
    if (g_h <= 0) g_h = 256;
}

bool umf_gdi_overlay_init(void* target_hwnd) {
    if (g_active) return true;

    g_target = (HWND)target_hwnd;
    compute_bounds(g_target);
    register_class();

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kGdiClass, L"", WS_POPUP,
        g_x, g_y, g_w, g_h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_hwnd) { UMF_ERROR("GDI overlay: CreateWindowExW failed"); return false; }

    g_memdc = CreateCompatibleDC(NULL);
    if (!g_memdc) { DestroyWindow(g_hwnd); g_hwnd = NULL; return false; }

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_w;
    bi.bmiHeader.biHeight      = -g_h;           /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_dib = CreateDIBSection(g_memdc, &bi, DIB_RGB_COLORS, (void**)&g_bits, NULL, 0);
    if (!g_dib) {
        DeleteDC(g_memdc); g_memdc = NULL;
        DestroyWindow(g_hwnd); g_hwnd = NULL;
        return false;
    }
    SelectObject(g_memdc, g_dib);

    g_cmd_count = 0;
    g_active = true;
    ShowWindow(g_hwnd, SW_SHOWNA);
    UMF_INFO("GDI overlay active (%dx%d @ %d,%d)", g_w, g_h, g_x, g_y);
    return true;
}

void umf_gdi_overlay_shutdown(void) {
    if (!g_active) return;
    g_active = false;
    if (g_dib)   { DeleteObject(g_dib);  g_dib = NULL; }
    if (g_memdc) { DeleteDC(g_memdc);    g_memdc = NULL; }
    if (g_hwnd)  { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    g_bits = NULL;
    g_cmd_count = 0;
}

bool umf_gdi_overlay_is_active(void) { return g_active; }

void* umf_gdi_overlay_hwnd(void) { return g_hwnd; }

bool umf_gdi_overlay_size(int* out_w, int* out_h) {
    if (!g_active) return false;
    if (out_w) *out_w = g_w;
    if (out_h) *out_h = g_h;
    return true;
}

void umf_gdi_overlay_set_frame_callback(UmfOverlayFrameFn fn) { g_frame_cb = fn; }

bool umf_gdi_overlay_render(void) {
    if (!g_active || !g_hwnd || !g_bits) return false;

    umf_gdi_overlay_begin_frame();
    if (g_frame_cb) g_frame_cb();

    if (!umf_gdi_overlay_rasterize(g_bits, g_w, g_h)) return false;

    /* Premultiply for UpdateLayeredWindow (opaque/transparent pass through). */
    for (int i = 0; i < g_w * g_h; i++) {
        uint32_t p = g_bits[i];
        uint32_t a = p >> 24;
        if (a == 0)        { g_bits[i] = 0; continue; }
        if (a == 0xFF)     continue;
        uint32_t r = ((p >> 16) & 0xFF) * a / 255;
        uint32_t g = ((p >> 8)  & 0xFF) * a / 255;
        uint32_t b = ( p        & 0xFF) * a / 255;
        g_bits[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }

    POINT src = {0, 0}, dst = {g_x, g_y};
    SIZE  sz  = {g_w, g_h};
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;

    HDC screen = GetDC(NULL);
    BOOL ok = UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, g_memdc, &src,
                                  0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    return ok != 0;
}
