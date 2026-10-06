/*
 * umf_overlay_dx11.cpp — §OVERLAY: in-target Dear ImGui Direct3D 11 overlay
 *
 * Draws a transparent ImGui UI over any D3D11 application by hooking the
 * swapchain's Present (and ResizeBuffers) through the vtable strategy, and
 * routing input by subclassing the window procedure. This is the in-target
 * UI surface mods draw onto; it is deliberately minimal (no chrome, no
 * window manager) because it lives inside the host process.
 *
 * The vtable-hook strategy is used because it reuses the engine's existing,
 * tested hook path; if the DXGI vtable page cannot be written (e.g. HVCI),
 * init fails cleanly.
 */

#include "umf/umf.h"

#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

/* Declared (not included) to avoid pulling the dxgi1_*.h chain. */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *ResizeBuffersFn)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static IDXGISwapChain*        g_swapchain  = nullptr;
static ID3D11Device*          g_device     = nullptr;
static ID3D11DeviceContext*   g_context    = nullptr;
static ID3D11RenderTargetView* g_rtv       = nullptr;
static HWND                   g_hwnd       = nullptr;
static WNDPROC                g_orig_wndproc = nullptr;

static UmfVtableLocation g_loc_present;
static UmfVtableLocation g_loc_resize;
static PresentFn         g_orig_present  = nullptr;
static ResizeBuffersFn   g_orig_resize   = nullptr;

static bool            g_ready      = false;
static bool            g_active     = false;
static bool            g_in_frame   = false;
static bool            g_block_input = false;
static UmfOverlayFrameFn g_frame_cb = nullptr;

static void create_rtv() {
    if (!g_swapchain || !g_device) return;
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         (void**)&back)) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

/* ── WndProc subclass ── */
static LRESULT CALLBACK hk_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_active && g_ready)
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);

    if (g_active && g_block_input && g_ready) {
        ImGuiIO& io = ImGui::GetIO();
        switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:   case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:  case WM_MOUSEMOVE:
                if (io.WantCaptureMouse) return 1;
                break;
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                if (io.WantCaptureKeyboard) return 1;
                break;
            default: break;
        }
    }
    return CallWindowProcW(g_orig_wndproc, h, msg, wp, lp);
}

/* ── Present hook ── */
static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync,
                                            UINT flags) {
    if (!g_ready && !g_in_frame) {
        g_in_frame = true;   /* guard against re-entrancy during init */
        g_swapchain = sc;

        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))
            && g_device) {
            g_device->GetImmediateContext(&g_context);
            create_rtv();

            ImGui::CreateContext();
            ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
            umf_overlay_apply_modern_theme();

            HWND hwnd = g_hwnd;
            if (!hwnd && g_context) {
                /* Fall back to the swapchain's output window if none given. */
                DXGI_SWAP_CHAIN_DESC d{};
                if (SUCCEEDED(sc->GetDesc(&d))) hwnd = d.OutputWindow;
            }
            if (hwnd) {
                g_hwnd = hwnd;
                ImGui_ImplWin32_Init(hwnd);
                g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(
                    hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndproc);
            }
            ImGui_ImplDX11_Init(g_device, g_context);
            g_ready = true;
        }
        g_in_frame = false;
    }

    if (g_active && g_ready && g_context) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (g_frame_cb) g_frame_cb();
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return g_orig_present(sc, sync, flags);
}

/* ── ResizeBuffers hook ── */
static HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT count,
                                           UINT w, UINT h, DXGI_FORMAT fmt,
                                           UINT flags) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    HRESULT hr = g_orig_resize(sc, count, w, h, fmt, flags);
    if (g_ready) create_rtv();
    return hr;
}

/* ════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════ */

bool umf_overlay_dx11_init(void* swapchain, void* hwnd) {
    if (g_active) return true;
    if (!swapchain) return false;

    g_swapchain = (IDXGISwapChain*)swapchain;
    g_hwnd      = (HWND)hwnd;

    void** vtable = *(void***)swapchain;   /* first member is the vtable ptr */

    if (!umf_hook_vtable(vtable, 8, (void*)&hk_present,
                         (void**)&g_orig_present, &g_loc_present)) {
        UMF_ERROR("Overlay: failed to hook Present (vtable)");
        return false;
    }
    if (!umf_hook_vtable(vtable, 13, (void*)&hk_resize,
                         (void**)&g_orig_resize, &g_loc_resize)) {
        umf_unhook_vtable(&g_loc_present);
        UMF_ERROR("Overlay: failed to hook ResizeBuffers (vtable)");
        return false;
    }

    g_active = true;
    UMF_INFO("DX11 overlay installed (Present/ResizeBuffers hooked)");
    return true;
}

void umf_overlay_dx11_shutdown(void) {
    if (!g_active) return;

    g_active = false;

    if (g_hwnd && g_orig_wndproc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_wndproc);
        g_orig_wndproc = nullptr;
    }

    if (g_ready) {
        ImGui_ImplDX11_Shutdown();
        if (g_hwnd) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ready = false;
    }

    if (g_rtv)      { g_rtv->Release();      g_rtv = nullptr; }
    if (g_context)  { g_context->Release();  g_context = nullptr; }
    if (g_device)   { g_device->Release();   g_device = nullptr; }

    umf_unhook_vtable(&g_loc_resize);
    umf_unhook_vtable(&g_loc_present);

    g_swapchain = nullptr;
    UMF_INFO("DX11 overlay removed");
}

void umf_overlay_set_frame_callback(UmfOverlayFrameFn fn) { g_frame_cb = fn; }
void umf_overlay_block_input(bool block) { g_block_input = block; }
bool umf_overlay_is_active(void) { return g_active; }

/* Draw helpers — every ImGui call stays inside the module that owns the
 * ImGui context (the runtime), so hosts linked against a second static
 * copy of ImGui are unaffected. */
void umf_imgui_begin(const char* title) { ImGui::Begin(title); }
void umf_imgui_text(const char* text)   { ImGui::TextUnformatted(text); }
void umf_imgui_end(void)                { ImGui::End(); }

void umf_overlay_apply_modern_theme(void) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 8.0f;
    s.FrameRounding     = 6.0f;
    s.GrabRounding      = 6.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 12.0f;
    s.WindowPadding     = ImVec2(12, 12);
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(8, 6);
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]       = ImVec4(0.10f, 0.11f, 0.13f, 0.94f);
    c[ImGuiCol_Border]         = ImVec4(0.24f, 0.26f, 0.30f, 0.80f);
    c[ImGuiCol_FrameBg]        = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_Button]         = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonHovered]  = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);
    c[ImGuiCol_Header]         = ImVec4(0.22f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_CheckMark]      = ImVec4(0.42f, 0.78f, 0.95f, 1.00f);
    c[ImGuiCol_SliderGrab]     = ImVec4(0.42f, 0.78f, 0.95f, 1.00f);
    c[ImGuiCol_Text]           = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]   = ImVec4(0.55f, 0.57f, 0.60f, 1.00f);
}
