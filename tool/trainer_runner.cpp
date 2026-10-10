// trainer_runner.cpp — the standalone trainer that Hexforge exports.
//
// A single, reusable executable: "Export Trainer" in the main tool copies this
// exe and appends the chosen cheat table as an overlay (see trainer.cpp). On
// launch the runner reads that embedded table, auto-attaches to the named game,
// and presents a small FLiNG-style window: a feature list with hotkeys where
// Toggle cheats lock a value on/off and Set-once cheats write a value when fired.
//
// No scanner, no injection — just the cheats the author already found. Shareable
// as one file.

#include "backend.hpp"
#include "trainer.hpp"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <dxgi.h>

#include <string>
#include <vector>
#include <algorithm>
#include <ctime>

// ── D3D11 plumbing ───────────────────────────────────────────────────────────
static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT                    g_resizeW = 0, g_resizeH = 0;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static void CreateRTV() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (back) { g_device->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
static void CleanupRTV() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }
static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                               &g_device, &fl, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           levels, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                           &g_device, &fl, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRTV();
    return true;
}
static void CleanupDeviceD3D() {
    CleanupRTV();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ── runner state ─────────────────────────────────────────────────────────────
struct Runner {
    hx::Target target;
    hx::TrainerDoc doc;
    std::string status = "looking for game...";
    bool attached = false;
    double nextProbe = 0.0;   // seconds; throttle attach / liveness probing
};

static Runner* g_run = nullptr;
static HWND    g_hwnd = nullptr;
static const int HK_BASE = 0xC000;

static std::string w2u(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

// Apply a Set-once cheat right now (write its value a single time).
static void fire_set_once(Runner& r, int idx) {
    if (!r.attached || idx < 0 || idx >= (int)r.doc.cheats.size()) return;
    hx::Cheat& c = r.doc.cheats[idx];
    hx::write_typed(r.target, c.addr, hx::scan_type_of(c.type), strtod(c.value, nullptr));
}

static void on_hotkey(WPARAM id) {
    int idx = (int)id - HK_BASE;
    if (!g_run || idx < 0 || idx >= (int)g_run->doc.cheats.size()) return;
    hx::Cheat& c = g_run->doc.cheats[idx];
    if (c.mode == 1) fire_set_once(*g_run, idx);   // Set once
    else c.freeze = !c.freeze;                      // Toggle freeze
}

static void sync_hotkeys(Runner& r) {
    if (!g_hwnd) return;
    for (int i = 0; i < 64; ++i) UnregisterHotKey(g_hwnd, HK_BASE + i);
    for (size_t i = 0; i < r.doc.cheats.size() && i < 64; ++i)
        if (r.doc.cheats[i].hotkey)
            RegisterHotKey(g_hwnd, HK_BASE + (int)i, 0, r.doc.cheats[i].hotkey);
}

static uint32_t find_pid_by_name(const std::string& name) {
    std::string want = lower(name);
    for (const auto& p : hx::list_processes())
        if (lower(w2u(p.name)) == want) return p.pid;
    return 0;
}

// Attach when the game appears; drop the handle when it exits. Throttled.
static void maintain_attach(Runner& r, double now) {
    if (now < r.nextProbe) return;
    r.nextProbe = now + 1.0;
    if (r.doc.target.empty()) { r.status = "no target process set in this trainer"; return; }

    uint32_t pid = find_pid_by_name(r.doc.target);
    if (!pid) {
        if (r.attached) { r.target.detach(); r.attached = false; }
        r.status = "waiting for " + r.doc.target + " to start...";
        return;
    }
    if (r.attached && r.target.pid() == pid) {
        r.status = "attached to " + r.doc.target + "  (pid " + std::to_string(pid) + ")";
        return;
    }
    std::string err;
    if (r.target.attach(pid, err)) {
        r.attached = true;
        r.status = "attached to " + r.doc.target + "  (pid " + std::to_string(pid) + ")";
    } else {
        r.attached = false;
        r.status = "found " + r.doc.target + " but attach failed (run as admin): " + err;
    }
}

static void StyleTrainer() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 8);
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.486f, 0.361f, 1.0f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.039f, 0.047f, 0.071f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.067f, 0.098f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.11f, 0.16f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.13f, 0.16f, 0.23f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.8f);
    c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.28f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
}

static void draw(Runner& r) {
    // Keep every frozen Toggle cheat locked to its value.
    if (r.attached) {
        for (auto& c : r.doc.cheats)
            if (c.mode == 0 && c.freeze)
                hx::write_typed(r.target, c.addr, hx::scan_type_of(c.type), strtod(c.value, nullptr));
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##trainer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.49f, 0.36f, 1.0f, 1.0f));
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(r.doc.title.empty() ? "Trainer" : r.doc.title.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    bool ok = r.attached;
    ImGui::TextColored(ok ? ImVec4(0.24f, 0.86f, 0.52f, 1) : ImVec4(1.0f, 0.71f, 0.33f, 1),
                       "%s%s", ok ? "> " : "... ", r.status.c_str());
    ImGui::Separator();

    if (r.doc.cheats.empty()) {
        ImGui::TextWrapped("This trainer has no cheats embedded.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("cheats", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Feature");
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < r.doc.cheats.size(); ++i) {
            hx::Cheat& c = r.doc.cheats[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(c.label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(c.hotkey ? hx::hk_name(c.hotkey) : "-");
            ImGui::TableSetColumnIndex(2);
            if (c.mode == 0) {
                if (c.freeze) ImGui::TextColored(ImVec4(0.24f, 0.86f, 0.52f, 1), "ON");
                else ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1), "OFF");
            } else {
                ImGui::TextColored(ImVec4(0.3f, 0.66f, 1.0f, 1), "set %s", c.value);
            }
            ImGui::TableSetColumnIndex(3);
            if (!ok) ImGui::BeginDisabled();
            if (c.mode == 0) {
                if (ImGui::SmallButton(c.freeze ? "Turn off" : "Turn on")) c.freeze = !c.freeze;
            } else {
                if (ImGui::SmallButton("Activate")) fire_set_once(r, (int)i);
            }
            if (!ok) ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Toggle cheats lock a value on/off. Set-once cheats write a value when fired.");
    ImGui::TextDisabled("Hotkeys work even while the game is focused. Made with Hexforge.");
    ImGui::End();
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
            return 0;
        case WM_HOTKEY: on_hotkey(wp); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    Runner run;
    g_run = &run;
    if (!hx::read_embedded_trainer(run.doc)) {
        run.doc.title = "Hexforge Trainer";
        run.status = "no trainer data embedded in this exe";
    }
    hx::enable_debug_privilege();

    std::wstring wtitle(run.doc.title.begin(), run.doc.title.end());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HexforgeTrainerWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 560, 520, nullptr, nullptr, hInst, nullptr);
    g_hwnd = hwnd;
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInst);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    StyleTrainer();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    sync_hotkeys(run);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        double now = (double)GetTickCount64() / 1000.0;
        maintain_attach(run, now);

        if (g_resizeW && g_resizeH) {
            CleanupRTV();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRTV();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw(run);
        ImGui::Render();

        const float clear[4] = { 0.02f, 0.03f, 0.05f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    for (int i = 0; i < 64; ++i) UnregisterHotKey(g_hwnd, HK_BASE + i);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
