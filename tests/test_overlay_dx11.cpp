/*
 * test_overlay_dx11.cpp — §OVERLAY integration test.
 *
 * Creates a hidden window + a real D3D11 swapchain, installs the overlay,
 * pumps Present, and confirms the engine's Present hook drives the frame
 * callback. Verifies clean teardown. The window is never shown, so this
 * runs headlessly on any D3D11-capable machine (falls back to WARP).
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <d3d11.h>
#include <dxgi.h>

static int g_frames = 0;
static void on_frame() {
    g_frames++;
    umf_imgui_begin("UMF Test");
    char buf[64];
    snprintf(buf, sizeof(buf), "umf overlay test frame %d", g_frames);
    umf_imgui_text(buf);
    umf_imgui_end();
}

static LRESULT CALLBACK test_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

void run_overlay_dx11_tests(void) {
    if (!umf_init()) { CHECK(0, "umf_init for overlay"); return; }

    /* ── Hidden window ── */
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = test_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"UMFOverlayTestWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"umf", 0,
                                0, 0, 256, 256, nullptr, nullptr,
                                wc.hInstance, nullptr);
    CHECK(hwnd != nullptr, "create hidden test window");

    /* ── D3D11 device + swapchain ── */
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount        = 2;
    sd.BufferDesc.Width   = 256;
    sd.BufferDesc.Height  = 256;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = hwnd;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain*       sc  = nullptr;
    ID3D11Device*         dev = nullptr;
    ID3D11DeviceContext*  ctx = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

    if (FAILED(hr)) {   /* fall back to WARP (software) */
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    }
    CHECK(SUCCEEDED(hr) && sc != nullptr, "create D3D11 device + swapchain");

    if (sc) {
        umf_overlay_set_frame_callback(on_frame);
        bool init = umf_overlay_dx11_init((void*)sc, (void*)hwnd);
        CHECK(init, "install DX11 overlay");
        CHECK(umf_overlay_is_active(), "overlay reports active");

        g_frames = 0;
        for (int i = 0; i < 3; i++)
            sc->Present(0, 0);
        CHECK(g_frames == 3, "Present hook drove the frame callback 3x");

        umf_overlay_dx11_shutdown();
        CHECK(!umf_overlay_is_active(), "overlay reports inactive after shutdown");
    }

    if (sc)  sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (hwnd) DestroyWindow(hwnd);

    umf_shutdown();
    CHECK(1, "umf_shutdown after overlay tests");
}
