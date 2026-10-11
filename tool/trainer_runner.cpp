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
#include <shellapi.h>
#include <wincodec.h>

#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <cmath>

// ── D3D11 plumbing ───────────────────────────────────────────────────────────
static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT                    g_resizeW = 0, g_resizeH = 0;

// ── Logo texture ─────────────────────────────────────────────────────────────
static ID3D11ShaderResourceView* g_logoSRV = nullptr;
static int g_logoW = 0, g_logoH = 0;

static ID3D11ShaderResourceView* LoadIconTexture(ID3D11Device* dev, HINSTANCE hInst,
                                                  int resId, int* outW, int* outH) {
    HICON hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(resId),
                                    IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR);
    if (!hIcon) hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(resId),
                                          IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!hIcon) return nullptr;

    ICONINFO ii = {};
    if (!GetIconInfo(hIcon, &ii)) { DestroyIcon(hIcon); return nullptr; }
    BITMAP bm = {};
    GetObject(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    if (w <= 0 || h <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        DestroyIcon(hIcon); return nullptr;
    }

    HDC hdc = GetDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px(w * h * 4);
    GetDIBits(hdc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);

    for (int i = 0; i < w * h; ++i) {
        std::swap(px[i * 4 + 0], px[i * 4 + 2]);
        if (px[i * 4 + 3] == 0 && (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]))
            px[i * 4 + 3] = 255;
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DestroyIcon(hIcon);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = px.data(); sd.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr;
    dev->CreateTexture2D(&td, &sd, &tex);
    if (!tex) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (outW) *outW = w;
    if (outH) *outH = h;
    return srv;
}

// ── Cover image (decoded from embedded PNG/JPG via WIC) ──────────────────────
static ID3D11ShaderResourceView* g_gameSRV = nullptr;
static int g_gameW = 0, g_gameH = 0;

static ID3D11ShaderResourceView* LoadTextureFromImageBytes(ID3D11Device* dev,
                                                           const uint8_t* data, size_t size,
                                                           int* outW, int* outH) {
    if (!data || size == 0) return nullptr;
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac))))
        return nullptr;

    ID3D11ShaderResourceView* srv = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    do {
        if (FAILED(fac->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size))) break;
        if (FAILED(fac->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        if (FAILED(fac->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                    nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            break;
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (!w || !h) break;
        std::vector<uint8_t> px((size_t)w * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) break;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = px.data(); sd.SysMemPitch = w * 4;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex)) && tex) {
            dev->CreateShaderResourceView(tex, nullptr, &srv);
            tex->Release();
            if (outW) *outW = (int)w;
            if (outH) *outH = (int)h;
        }
    } while (false);

    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (stream) stream->Release();
    fac->Release();
    return srv;
}

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

// Freeze honoring the freeze mode: locked / floor / cap.
static void apply_freeze(Runner& r, hx::Cheat& c) {
    hx::ScanType ty = hx::scan_type_of(c.type);
    double target = strtod(c.value, nullptr);
    if (c.freezeMode == 0) { hx::write_typed(r.target, c.addr, ty, target); return; }
    uint8_t b[8] = {};
    if (!r.target.read(c.addr, b, hx::type_size(ty))) return;
    double cur = 0;
    switch (ty) {
        case hx::ScanType::I8: cur = *(int8_t*)b; break;
        case hx::ScanType::I16: cur = *(int16_t*)b; break;
        case hx::ScanType::I32: cur = *(int32_t*)b; break;
        case hx::ScanType::I64: cur = (double)*(int64_t*)b; break;
        case hx::ScanType::F32: cur = *(float*)b; break;
        case hx::ScanType::F64: cur = *(double*)b; break;
    }
    if (c.freezeMode == 1 && cur < target) hx::write_typed(r.target, c.addr, ty, target);
    else if (c.freezeMode == 2 && cur > target) hx::write_typed(r.target, c.addr, ty, target);
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

// ── Styled link-button helper (with a drawn GitHub/Discord glyph) ────────────
enum LinkIcon { LINK_NONE = 0, LINK_GITHUB = 1, LINK_DISCORD = 2 };

static void draw_github_glyph(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    // Simplified Octocat: round head, two ears, two eyes, little tail.
    dl->AddCircleFilled(c, r, col, 24);
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.75f, c.y - r * 0.55f),
                          ImVec2(c.x - r * 0.15f, c.y - r * 0.55f),
                          ImVec2(c.x - r * 0.45f, c.y - r * 1.15f), col);
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.75f, c.y - r * 0.55f),
                          ImVec2(c.x + r * 0.15f, c.y - r * 0.55f),
                          ImVec2(c.x + r * 0.45f, c.y - r * 1.15f), col);
    ImU32 eye = IM_COL32(20, 22, 30, 255);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.33f, c.y + r * 0.05f), r * 0.17f, eye, 10);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.33f, c.y + r * 0.05f), r * 0.17f, eye, 10);
    dl->AddBezierQuadratic(ImVec2(c.x + r * 0.9f, c.y + r * 0.4f),
                           ImVec2(c.x + r * 1.25f, c.y + r * 0.9f),
                           ImVec2(c.x + r * 0.6f, c.y + r * 1.1f), col, 2.0f);
}

static void draw_discord_glyph(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    // Rounded "Clyde" face: pill body + two white eyes.
    ImVec2 a(c.x - r * 1.05f, c.y - r * 0.7f), b(c.x + r * 1.05f, c.y + r * 0.75f);
    dl->AddRectFilled(a, b, col, r * 0.7f);
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, b.y - r * 0.1f),
                          ImVec2(c.x - r * 0.1f, b.y - r * 0.1f),
                          ImVec2(c.x - r * 0.5f, b.y + r * 0.5f), col);
    ImU32 eye = IM_COL32(255, 255, 255, 255);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.42f, c.y + r * 0.02f), r * 0.22f, eye, 12);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.42f, c.y + r * 0.02f), r * 0.22f, eye, 12);
}

static void DrawLinkButton(const char* id, const char* label, const std::string& url, int icon,
                           ImU32 bgCol, ImU32 bgHoverCol, ImU32 borderCol, float width, float height = 36.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    bool hov = ImGui::IsItemHovered();
    bool has = !url.empty();
    if (ImGui::IsItemClicked() && has) {
        int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(), w.data(), n);
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), hov ? bgHoverCol : bgCol, 6.0f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + height), borderCol, 6.0f, 0, 1.4f);
    ImU32 fg = IM_COL32(255, 255, 255, (hov && has) ? 255 : 190);
    float gr = height * 0.28f;
    ImVec2 gc(p.x + 6 + gr, p.y + height * 0.5f);
    if (icon == LINK_GITHUB) draw_github_glyph(dl, gc, gr, fg);
    else if (icon == LINK_DISCORD) draw_discord_glyph(dl, gc, gr, fg);
    ImVec2 ts = ImGui::CalcTextSize(label);
    float textX = icon ? (gc.x + gr + 8) : (p.x + (width - ts.x) * 0.5f);
    dl->AddText(ImVec2(textX, p.y + (height - ts.y) * 0.5f), fg, label);
    if (hov && has) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

static void StyleTrainer() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.ChildRounding = 4;
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
                apply_freeze(r, c);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##trainer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── Top bar: logo icon + title + status ──────────────────────────────────
    const float logoSize = 42.0f;
    if (g_logoSRV) {
        ImGui::Image((ImTextureID)g_logoSRV, ImVec2(logoSize, logoSize));
        ImGui::SameLine();
        // vertically nudge text to align with icon center
        float offsetY = (logoSize - ImGui::GetTextLineHeight() * 2.0f - ImGui::GetStyle().ItemSpacing.y) * 0.5f;
        if (offsetY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);
    }
    ImGui::BeginGroup();
    {
        // Glitch title: broken-light letter replacement every ~3.7s
        const char* titleText = r.doc.title.empty() ? "Trainer" : r.doc.title.c_str();
        ImGui::SetWindowFontScale(1.5f);
        ImVec2 titlePos = ImGui::GetCursorScreenPos();
        ImVec2 titleSize = ImGui::CalcTextSize(titleText);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        float fontSize = ImGui::GetFontSize();

        double now = (double)GetTickCount64() / 1000.0;
        double phase = fmod(now, 3.7);
        // Two quick flickers: 0-120ms and 160-210ms (broken light feel)
        bool glitching = (phase < 0.12) || (phase > 0.16 && phase < 0.21);

        if (glitching) {
            // Build a glitched copy: replace ~35% of letters with symbols
            static const char syms[] = "#@$!%&*_=+<>?/|~^";
            std::string g = titleText;
            unsigned seed = (unsigned)(now * 837.0);
            for (size_t i = 0; i < g.size(); ++i) {
                seed = seed * 1103515245u + 12345u;
                if (g[i] != ' ' && ((seed >> 16) % 100) < 35)
                    g[i] = syms[(seed >> 8) % (sizeof(syms) - 1)];
            }
            // Chromatic split during glitch
            float jitter = ((seed % 5) - 2) * 1.0f;
            dl->AddText(font, fontSize, ImVec2(titlePos.x + 2.5f + jitter, titlePos.y),
                        IM_COL32(255, 50, 80, 70), g.c_str());
            dl->AddText(font, fontSize, ImVec2(titlePos.x - 2.0f, titlePos.y + 1.0f),
                        IM_COL32(60, 200, 255, 60), g.c_str());
            dl->AddText(font, fontSize, titlePos, IM_COL32(125, 92, 255, 255), g.c_str());
        } else {
            // Clean title
            dl->AddText(font, fontSize, titlePos, IM_COL32(125, 92, 255, 255), titleText);
        }

        ImGui::Dummy(titleSize);
        ImGui::SetWindowFontScale(1.0f);
    }
    bool ok = r.attached;
    ImGui::TextColored(ok ? ImVec4(0.24f, 0.86f, 0.52f, 1) : ImVec4(1.0f, 0.71f, 0.33f, 1),
                       "%s%s", ok ? "> " : "... ", r.status.c_str());
    ImGui::EndGroup();
    ImGui::Separator();

    // ── Main content: left panel (game image + links) | right panel (cheats) ─
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;
    float leftW = availW * 0.24f;
    float gap = ImGui::GetStyle().ItemSpacing.x;

    // ── Left panel ───────────────────────────────────────────────────────────
    ImGui::BeginChild("##left_panel", ImVec2(leftW, availH), ImGuiChildFlags_Borders);
    {
        float panelW = ImGui::GetContentRegionAvail().x;

        // Game cover: embedded image if present, else a placeholder.
        float imgH = availH * 0.58f;
        ImGui::BeginChild("##game_img", ImVec2(panelW, imgH), ImGuiChildFlags_Borders);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 sz = ImGui::GetContentRegionAvail();
            ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
            dl->AddRectFilledMultiColor(p0, p1,
                IM_COL32(18, 20, 32, 255), IM_COL32(18, 20, 32, 255),
                IM_COL32(30, 35, 55, 255), IM_COL32(30, 35, 55, 255));
            if (g_gameSRV && g_gameW > 0 && g_gameH > 0) {
                // Fit the image inside the box, preserving aspect ratio.
                float scale = (std::min)(sz.x / g_gameW, sz.y / g_gameH);
                float w = g_gameW * scale, h = g_gameH * scale;
                ImVec2 a(p0.x + (sz.x - w) * 0.5f, p0.y + (sz.y - h) * 0.5f);
                dl->AddImage((ImTextureID)g_gameSRV, a, ImVec2(a.x + w, a.y + h));
            } else {
                const char* ph = "Game Image";
                ImVec2 ts = ImGui::CalcTextSize(ph);
                dl->AddText(ImVec2(p0.x + (sz.x - ts.x) * 0.5f, p0.y + (sz.y - ts.y) * 0.5f),
                            IM_COL32(100, 105, 130, 180), ph);
            }
        }
        ImGui::EndChild();

        // Push buttons to bottom of left panel
        float btnH = 36.0f;
        float btnSpacing = 6.0f;
        float totalBtnH = btnH * 2 + btnSpacing;
        float remainH = ImGui::GetContentRegionAvail().y;
        if (remainH > totalBtnH + 4)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + remainH - totalBtnH - 4);

        // GitHub + Discord: use the author's URLs baked into the trainer.
        DrawLinkButton("##gh", "GitHub", r.doc.github, LINK_GITHUB,
                       IM_COL32(22, 27, 38, 255), IM_COL32(35, 42, 58, 255),
                       IM_COL32(60, 65, 90, 255), panelW, btnH);
        ImGui::Spacing();
        DrawLinkButton("##dc", "Discord", r.doc.discord, LINK_DISCORD,
                       IM_COL32(40, 28, 80, 255), IM_COL32(58, 42, 110, 255),
                       IM_COL32(88, 70, 150, 255), panelW, btnH);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right panel: trainer controls ────────────────────────────────────────
    ImGui::BeginChild("##right_panel", ImVec2(0, availH), ImGuiChildFlags_Borders);
    {
        if (r.doc.cheats.empty()) {
            ImGui::TextWrapped("This trainer has no cheats embedded.");
        } else {
            if (ImGui::BeginTable("cheats", 5,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Feature");
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 42);
                ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 78);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 96);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < r.doc.cheats.size(); ++i) {
                    hx::Cheat& c = r.doc.cheats[i];
                    ImGui::TableNextRow();
                    ImGui::PushID((int)i);

                    ImGui::TableSetColumnIndex(0);
                    if (c.mode == 0 && c.freeze)
                        ImGui::TextColored(ImVec4(0.24f, 0.86f, 0.52f, 1), "* %s", c.label);
                    else
                        ImGui::TextUnformatted(c.label);
                    if (c.note[0]) {
                        ImGui::SameLine(0, 4);
                        ImGui::TextColored(ImVec4(0.3f, 0.66f, 1.0f, 1), "(i)");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.note);
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(c.hotkey ? hx::hk_name(c.hotkey) : "-");

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(ok ? hx::fmt_value_at(r.target, c.addr, hx::scan_type_of(c.type)).c_str() : "-");

                    // Editable amount: change ammo/money here without recreating anything.
                    ImGui::TableSetColumnIndex(3);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##v", c.value, sizeof(c.value));

                    ImGui::TableSetColumnIndex(4);
                    if (!ok) ImGui::BeginDisabled();
                    if (c.mode == 0) {
                        ImGui::Checkbox("freeze", &c.freeze);
                    } else {
                        if (ImGui::SmallButton("Activate")) fire_set_once(r, (int)i);
                    }
                    if (!ok) ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }

        // Info text at bottom of right panel
        ImGui::Separator();
        ImGui::TextDisabled("Edit the Value box to change the amount live (e.g. type a new ammo count) - no need to remake a cheat.");
        ImGui::TextDisabled("Toggle: freeze locks the value every frame (infinite). Set once: writes the value now. Hotkeys work in-game.");
        {
            std::string credit = "Made with Hexforge";
            if (!r.doc.author.empty()) credit += "  -  by " + r.doc.author;
            if (!r.doc.version.empty()) credit += "  v" + r.doc.version;
            ImGui::TextDisabled("%s", credit.c_str());
        }
    }
    ImGui::EndChild();

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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // for WIC image decoding

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
                                CW_USEDEFAULT, CW_USEDEFAULT, 900, 600, nullptr, nullptr, hInst, nullptr);
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

    g_logoSRV = LoadIconTexture(g_device, hInst, 101, &g_logoW, &g_logoH);
    if (!run.doc.image.empty())
        g_gameSRV = LoadTextureFromImageBytes(g_device, run.doc.image.data(),
                                              run.doc.image.size(), &g_gameW, &g_gameH);

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
    if (g_logoSRV) { g_logoSRV->Release(); g_logoSRV = nullptr; }
    if (g_gameSRV) { g_gameSRV->Release(); g_gameSRV = nullptr; }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
