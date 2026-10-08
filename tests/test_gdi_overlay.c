/*
 * test_gdi_overlay.c — §OVERLAYGDI: software rasteriser + layered-window path.
 *
 * The shape rasteriser is checked pixel-exactly against a plain buffer (no
 * window). The windowed path then creates a hidden target, installs the
 * overlay, and presents one frame driven by a frame callback.
 */
#include "umf/umf.h"
#include "test_framework.h"

static int g_cb_calls = 0;
static void gdi_frame(void) {
    g_cb_calls++;
    umf_gdi_rect(10, 10, 40, 30, umf_gdi_rgba(0, 255, 0, 255), true);
    umf_gdi_text(12, 12, umf_gdi_rgba(255, 255, 255, 255), "hud");
}

void run_gdi_overlay_tests(void) {
    /* ── Colour packing ── */
    CHECK(umf_gdi_rgba(255, 0, 0, 255) == 0xFFFF0000u, "rgba packs 0xAARRGGBB");
    CHECK(umf_gdi_rgba(0, 0, 0, 0) == 0x00000000u, "rgba transparent black");
    CHECK(umf_gdi_rgba(0, 0, 255, 128) == 0x800000FFu, "rgba blue, half alpha");

    /* ── Retained command list ── */
    umf_gdi_overlay_begin_frame();
    CHECK(umf_gdi_overlay_command_count() == 0, "begin_frame clears the list");
    umf_gdi_rect(0, 0, 5, 5, umf_gdi_rgba(255, 0, 0, 255), true);
    umf_gdi_line(0, 0, 9, 9, umf_gdi_rgba(0, 255, 0, 255));
    umf_gdi_text(1, 1, umf_gdi_rgba(255, 255, 255, 255), "x");
    CHECK(umf_gdi_overlay_command_count() == 3, "three commands recorded");

    /* ── Filled rect rasterises exactly ── */
    static uint32_t buf[32 * 32];
    umf_gdi_overlay_begin_frame();
    umf_gdi_rect(4, 4, 8, 8, umf_gdi_rgba(255, 0, 0, 255), true);
    CHECK(umf_gdi_overlay_rasterize(buf, 32, 32), "rasterize succeeds");
    CHECK(buf[6 * 32 + 6] == 0xFFFF0000u, "filled interior is red");
    CHECK(buf[4 * 32 + 4] == 0xFFFF0000u, "rect top-left corner painted");
    CHECK(buf[11 * 32 + 11] == 0xFFFF0000u, "rect bottom-right corner painted");
    CHECK(buf[0] == 0x00000000u, "outside the rect is transparent");
    CHECK(buf[12 * 32 + 12] == 0x00000000u, "one pixel past the rect is clear");

    /* ── Outline rect: border only ── */
    umf_gdi_overlay_begin_frame();
    umf_gdi_rect(2, 2, 10, 10, umf_gdi_rgba(0, 0, 255, 255), false);
    umf_gdi_overlay_rasterize(buf, 32, 32);
    CHECK(buf[2 * 32 + 2] == 0xFF0000FFu, "outline corner painted");
    CHECK(buf[6 * 32 + 6] == 0x00000000u, "outline interior stays clear");

    /* ── Line endpoints ── */
    umf_gdi_overlay_begin_frame();
    umf_gdi_line(0, 0, 10, 10, umf_gdi_rgba(0, 255, 0, 255));
    umf_gdi_overlay_rasterize(buf, 32, 32);
    CHECK(buf[0] == 0xFF00FF00u, "line start pixel set");
    CHECK(buf[10 * 32 + 10] == 0xFF00FF00u, "line end pixel set");

    /* ── Robustness ── */
    CHECK(!umf_gdi_overlay_rasterize(NULL, 32, 32), "rasterize rejects NULL buffer");
    CHECK(!umf_gdi_overlay_render(), "render while inactive returns false");

    /* ── Windowed present path over a hidden target ── */
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"UMFGdiTargetWnd";
    RegisterClassExW(&wc);
    HWND target = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
                                  0, 0, 200, 150, NULL, NULL, wc.hInstance, NULL);
    CHECK(target != NULL, "create hidden target window");

    CHECK(umf_gdi_overlay_init((void*)target), "gdi overlay init");
    CHECK(umf_gdi_overlay_is_active(), "overlay reports active");

    RECT rc; GetClientRect(target, &rc);
    int ow = 0, oh = 0;
    umf_gdi_overlay_size(&ow, &oh);
    CHECK(ow == rc.right - rc.left && oh == rc.bottom - rc.top,
          "overlay size matches the target client area");

    void* ohwnd = umf_gdi_overlay_hwnd();
    CHECK(ohwnd != NULL, "overlay exposes its hwnd");
    LONG ex = GetWindowLongW((HWND)ohwnd, GWL_EXSTYLE);
    CHECK((ex & WS_EX_LAYERED) != 0, "overlay window is layered");
    CHECK((ex & WS_EX_TRANSPARENT) != 0, "overlay window is click-through");

    g_cb_calls = 0;
    umf_gdi_overlay_set_frame_callback(gdi_frame);
    CHECK(umf_gdi_overlay_render(), "render presents a frame");
    CHECK(g_cb_calls == 1, "render invoked the frame callback once");
    CHECK(umf_gdi_overlay_command_count() == 2, "callback recorded rect + text");

    umf_gdi_overlay_shutdown();
    CHECK(!umf_gdi_overlay_is_active(), "overlay inactive after shutdown");
    CHECK(umf_gdi_overlay_hwnd() == NULL, "hwnd cleared after shutdown");

    DestroyWindow(target);
}
