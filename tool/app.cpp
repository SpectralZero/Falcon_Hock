// app.cpp — Hexforge native tool (Win32 + Dear ImGui + Direct3D 11).
//
// A real external memory tool: enumerate processes, attach for live memory,
// inject the runtime, browse modules/regions, view & edit memory, and run a
// Cheat-Engine-style first/next scanner. No simulation — every value is read
// from the live target with ReadProcessMemory.

#include "backend.hpp"
#include "ipc.hpp"
#include "trainer.hpp"

#include <Zydis/Zydis.h>
#include <dbghelp.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <ctime>

// ── D3D11 plumbing ───────────────────────────────────────────────────────────
static ID3D11Device*            g_device = nullptr;
static ID3D11DeviceContext*     g_ctx = nullptr;
static IDXGISwapChain*          g_swap = nullptr;
static ID3D11RenderTargetView*  g_rtv = nullptr;
static UINT                     g_resizeW = 0, g_resizeH = 0;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ── logging + crash handling ─────────────────────────────────────────────────
static std::wstring exe_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s.resize(slash + 1);
    return s;
}
static FILE* g_logf = nullptr;
static void log_line(const std::string& s) {
    if (!g_logf) return;
    time_t t = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &t);
    fprintf(g_logf, "[%02d:%02d:%02d] %s\n", lt.tm_hour, lt.tm_min, lt.tm_sec, s.c_str());
    fflush(g_logf);
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    std::wstring dmp = exe_dir() + L"hexforge-crash.dmp";
    HANDLE hf = CreateFileW(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hf, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hf);
    }
    if (g_logf) {
        fprintf(g_logf, "*** CRASH: exception 0x%08lX — minidump written to hexforge-crash.dmp\n",
                ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0);
        fflush(g_logf);
    }
    MessageBoxW(nullptr, L"Hexforge hit an error and wrote hexforge-crash.dmp + hexforge.log next to the exe.",
                L"Hexforge", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void install_logging() {
    std::wstring lp = exe_dir() + L"hexforge.log";
    _wfopen_s(&g_logf, lp.c_str(), L"w");
    log_line("Hexforge started");
    SetUnhandledExceptionFilter(crash_filter);
}

static void on_hotkey(WPARAM id);   // toggles a cheat's freeze; defined after app state

static void CreateRTV() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}
static void CleanupRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}
static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                               &g_device, &fl, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
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

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
            return 0;
        case WM_HOTKEY:
            on_hotkey(wp);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── helpers ──────────────────────────────────────────────────────────────────
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

static std::string json_escape(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

// ── application state ────────────────────────────────────────────────────────
using hx::Cheat;   // shared cheat model (see trainer.hpp)

struct App {
    hx::Target target;
    std::vector<hx::ProcEntry> procs;
    char procFilter[128] = "";
    uint32_t selectedPid = 0;
    std::string selectedName;
    std::string attachedName;   // exe name of the currently attached target

    std::vector<hx::ModuleEntry> modules;
    std::vector<hx::RegionEntry> regions;

    int scanType = 2;        // int32
    int scanCompare = 0;     // exact
    char scanValue[64] = "";
    char setValue[64] = "";
    bool writableOnly = true;
    std::string scanStatus = "no scan yet";
    bool hasScanned = false;

    // live single-address write (poke) opened from a scanner result
    uintptr_t editAddr = 0;
    char editBuf[64] = "";
    bool wantEditPopup = false;

    // background scan worker
    std::thread scanThread;
    std::atomic<bool> scanning{false};
    std::atomic<bool> scanCancel{false};
    std::atomic<float> scanProgress{0.0f};

    char hexAddr[32] = "";
    uint8_t hexBuf[256] = {};
    uint8_t hexPrev[256] = {};
    bool hexHasPrev = false;
    bool hexAuto = false;
    bool hexValid = false;

    // pointer scanner
    char ptrAddr[32] = "";
    int ptrLevels = 3;
    int64_t ptrMaxOff = 0x1000;
    std::vector<hx::PtrChain> ptrChains;
    std::string ptrStatus = "no pointer scan yet";

    // disassembler
    char disasmAddr[32] = "";
    std::vector<std::string> disasmLines;

    // runtime (injected engine over IPC)
    hx::RuntimeClient rt;
    char luaCode[2048] =
        "local p = umf.resolve(\"kernel32.dll\", \"GetProcAddress\")\n"
        "umf.log(\"GetProcAddress @ \" .. tostring(p))\n";
    std::string rtOutput;
    std::unordered_set<uint32_t> runtimePids;

    std::vector<Cheat> cheats;
    char trainerFile[64] = "cheats.hexforge";
    char trainerTitle[64] = "My Trainer";
    std::string exportStatus;
    bool hotkeysDirty = false;
    bool gotoTrainer = false;   // wizard asks the UI to open the Trainer tab

    // trainer wizard (guided scanner)
    int wizStep = 0;            // 0 pick, 1 first value, 2 narrow/finish
    int wizType = 2;
    char wizName[48] = "Value";
    char wizValue[64] = "";

    std::vector<std::string> log;
    char logFilter[96] = "";

    void addlog(const std::string& s) {
        time_t t = time(nullptr);
        struct tm lt;
        localtime_s(&lt, &t);
        char ts[16];
        snprintf(ts, sizeof(ts), "[%02d:%02d:%02d] ", lt.tm_hour, lt.tm_min, lt.tm_sec);
        log.push_back(std::string(ts) + s);
        log_line(s);
        if (log.size() > 500) log.erase(log.begin(), log.begin() + (log.size() - 500));
    }
    void refreshProcs() {
        procs = hx::list_processes();
        runtimePids.clear();
        for (uint32_t p : hx::runtime_pids()) runtimePids.insert(p);
        std::sort(procs.begin(), procs.end(), [](const hx::ProcEntry& a, const hx::ProcEntry& b) {
            return lower(w2u(a.name)) < lower(w2u(b.name));
        });
    }
};

static hx::ScanType scanTypeOf(int i) { return hx::scan_type_of(i); }
static std::string fmtValueAt(hx::Target& t, uintptr_t addr, hx::ScanType ty) {
    return hx::fmt_value_at(t, addr, ty);
}
static void writeTyped(App& app, uintptr_t addr, hx::ScanType ty, double v) {
    hx::write_typed(app.target, addr, ty, v);
}

static App* g_app = nullptr;
static HWND g_hwnd = nullptr;
static const int HK_BASE = 0xB000;

static void sync_hotkeys(App& app) {
    if (!g_hwnd) return;
    for (int i = 0; i < 64; ++i) UnregisterHotKey(g_hwnd, HK_BASE + i);
    for (size_t i = 0; i < app.cheats.size() && i < 64; ++i)
        if (app.cheats[i].hotkey)
            RegisterHotKey(g_hwnd, HK_BASE + (int)i, 0, app.cheats[i].hotkey);
}

static void on_hotkey(WPARAM id) {
    int idx = (int)id - HK_BASE;
    if (!g_app || idx < 0 || idx >= (int)g_app->cheats.size()) return;
    Cheat& c = g_app->cheats[idx];
    if (c.mode == 1) {   // Set once: write the value now
        if (g_app->target.attached())
            writeTyped(*g_app, c.addr, scanTypeOf(c.type), strtod(c.value, nullptr));
        g_app->addlog(std::string("hotkey set: ") + c.label + " = " + c.value);
    } else {             // Toggle: flip the freeze lock
        c.freeze = !c.freeze;
        g_app->addlog(std::string("hotkey toggled: ") + c.label + (c.freeze ? " ON" : " OFF"));
    }
}

static void start_first_scan(App& app, bool unknown) {
    if (app.scanning.load()) return;
    if (app.scanThread.joinable()) app.scanThread.join();
    app.scanCancel = false;
    app.scanProgress = 0.0f;
    app.scanning = true;
    int type = app.scanType;
    double v = strtod(app.scanValue, nullptr);
    bool wo = app.writableOnly;
    app.scanThread = std::thread([&app, unknown, type, v, wo] {
        size_t n = unknown
            ? app.target.first_scan_unknown(scanTypeOf(type), wo, &app.scanCancel, &app.scanProgress)
            : app.target.first_scan(scanTypeOf(type), v, wo, &app.scanCancel, &app.scanProgress);
        app.scanStatus = std::to_string(n) + (app.scanCancel.load() ? " matches (cancelled)" : " matches");
        app.hasScanned = true;
        app.scanning = false;
    });
}

static void start_next_scan(App& app) {
    if (app.scanning.load()) return;
    if (app.scanThread.joinable()) app.scanThread.join();
    app.scanCancel = false;
    app.scanProgress = 0.0f;
    app.scanning = true;
    int cmp = app.scanCompare;
    double v = strtod(app.scanValue, nullptr);
    app.scanThread = std::thread([&app, cmp, v] {
        size_t n = app.target.next_scan((hx::ScanCompare)cmp, v, &app.scanCancel, &app.scanProgress);
        app.scanStatus = std::to_string(n) + (app.scanCancel.load() ? " matches (cancelled)" : " matches");
        app.scanning = false;
    });
}

static void add_cheat_from(App& app, uintptr_t addr, int type) {
    Cheat c;
    c.addr = addr;
    c.type = type;
    std::string cur = app.target.attached() ? fmtValueAt(app.target, addr, scanTypeOf(type)) : "0";
    snprintf(c.value, sizeof(c.value), "%s", cur.c_str());
    snprintf(c.label, sizeof(c.label), "cheat %zu", app.cheats.size() + 1);
    app.cheats.push_back(c);
    app.addlog("added cheat @ " + std::string([&] { char b[20]; snprintf(b, 20, "%llX", (unsigned long long)addr); return std::string(b); }()));
}

// ── UI theme ─────────────────────────────────────────────────────────────────
static void StyleHexforge() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.ChildRounding = 8;
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7);
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.486f, 0.361f, 1.0f, 1.0f);   // #7C5CFF
    const ImVec4 accent2(0.208f, 0.878f, 0.816f, 1.0f); // #35E0D0
    c[ImGuiCol_WindowBg] = ImVec4(0.039f, 0.047f, 0.071f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.067f, 0.098f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.11f, 0.16f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.16f, 0.23f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.06f, 0.09f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.08f, 0.12f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.28f);
    c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.65f);
    c[ImGuiCol_Button] = ImVec4(0.13f, 0.16f, 0.23f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.8f);
    c[ImGuiCol_Tab] = ImVec4(0.09f, 0.11f, 0.16f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.6f);
    c[ImGuiCol_TabActive] = ImVec4(accent.x, accent.y, accent.z, 0.75f);
    c[ImGuiCol_CheckMark] = accent2;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
}

// ── panels ───────────────────────────────────────────────────────────────────
static void drawTargets(App& app) {
    ImGui::BeginChild("targets", ImVec2(360, 0), true);
    ImGui::TextColored(ImVec4(0.49f, 0.36f, 1.0f, 1.0f), "TARGETS");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
    if (ImGui::SmallButton("Rescan")) app.refreshProcs();
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "search name or PID", app.procFilter, sizeof(app.procFilter));

    std::string f = lower(app.procFilter);
    ImGui::BeginChild("proclist", ImVec2(0, -84), true);
    for (const auto& p : app.procs) {
        std::string name = w2u(p.name);
        std::string pids = std::to_string(p.pid);
        if (!f.empty() && lower(name).find(f) == std::string::npos && pids.find(f) == std::string::npos)
            continue;
        bool hasRuntime = app.runtimePids.count(p.pid) != 0;
        char label[192];
        snprintf(label, sizeof(label), "%-24s %-7u%s", name.c_str(), p.pid,
                 hasRuntime ? "  [runtime]" : "");
        if (hasRuntime)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.24f, 0.86f, 0.52f, 1.0f));
        if (ImGui::Selectable(label, app.selectedPid == p.pid)) {
            app.selectedPid = p.pid;
            app.selectedName = name;
        }
        if (hasRuntime) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Text("Selected: %s", app.selectedPid ? app.selectedName.c_str() : "(none)");
    bool has = app.selectedPid != 0;
    if (!has) ImGui::BeginDisabled();
    if (ImGui::Button("Attach", ImVec2(110, 0))) {
        std::string err;
        if (app.target.attach(app.selectedPid, err)) {
            app.addlog("attached to pid " + std::to_string(app.selectedPid) + " (" + app.selectedName + ")");
            app.attachedName = app.selectedName;
            app.modules = app.target.modules();
            app.regions = app.target.regions();
        } else {
            app.addlog("attach failed: " + err);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Inject runtime", ImVec2(150, 0))) {
        std::string err;
        if (hx::inject_dll(app.selectedPid, hx::runtime_dll_path(), err))
            app.addlog("injected umf_runtime.dll into pid " + std::to_string(app.selectedPid));
        else
            app.addlog("inject failed: " + err);
    }
    if (!has) ImGui::EndDisabled();
    ImGui::EndChild();
}

static void drawScanner(App& app) {
    const char* types[] = { "int8", "int16", "int32", "int64", "float", "double" };
    const char* cmps[] = { "exact", "changed", "unchanged", "increased", "decreased" };
    bool attached = app.target.attached();
    bool scanning = app.scanning.load();

    ImGui::TextDisabled("1) type + number -> First Scan.  2) change it in-game -> pick increased/decreased -> Next Scan.  3) repeat. Right-click a result for options.");
    ImGui::Separator();

    ImGui::SetNextItemWidth(120);
    ImGui::Combo("type", &app.scanType, types, 6);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("value", app.scanValue, sizeof(app.scanValue));

    if (!attached || scanning) ImGui::BeginDisabled();
    if (ImGui::Button("First Scan", ImVec2(110, 0))) {
        start_first_scan(app, false);
        app.addlog("first scan started (" + std::string(types[app.scanType]) + " = " + app.scanValue + ")");
    }
    ImGui::SameLine();
    if (ImGui::Button("First Scan (unknown)", ImVec2(160, 0))) {
        start_first_scan(app, true);
        app.addlog("unknown-value first scan started");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("##cmp", &app.scanCompare, cmps, 5);
    ImGui::SameLine();
    if (ImGui::Button("Next Scan", ImVec2(110, 0))) {
        start_next_scan(app);
        app.addlog("next scan (" + std::string(cmps[app.scanCompare]) + ")");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        app.target.clear_scan();
        app.scanStatus = "cleared";
        app.hasScanned = false;
    }
    if (!attached || scanning) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("writable only", &app.writableOnly);

    ImGui::Separator();

    if (scanning) {
        ImGui::Text("Scanning... (window stays responsive)");
        ImGui::ProgressBar(app.scanProgress.load(), ImVec2(-1, 0));
        if (ImGui::Button("Cancel scan")) app.scanCancel = true;
        return;   // don't touch results while the worker owns them
    }

    ImGui::Text("Results: %s", app.scanStatus.c_str());

    ImGui::SetNextItemWidth(160);
    ImGui::InputText("new value", app.setValue, sizeof(app.setValue));
    ImGui::SameLine();
    size_t resultCount = app.target.result_count();
    bool noResults = resultCount == 0;
    if (!attached || noResults) ImGui::BeginDisabled();
    if (ImGui::Button("Apply to all results")) {
        double v = strtod(app.setValue, nullptr);
        auto all = app.target.results_snapshot(100000);
        for (uintptr_t a : all)
            writeTyped(app, a, scanTypeOf(app.scanType), v);
        app.addlog("wrote value to " + std::to_string(all.size()) + " addresses");
    }
    if (!attached || noResults) ImGui::EndDisabled();
    ImGui::TextDisabled("Writes go straight into the game (no trainer needed): a row's [Set] pokes the 'new value' into that one address; right-click -> Edit value to type a value; [Apply to all] writes every result.");

    ImGui::BeginChild("results", ImVec2(0, 0), true);
    if (ImGui::BeginTable("res", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Value (live)");
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableHeadersRow();
        // Iterate a private copy: the worker thread may rewrite the live
        // result set at any time, so never hold a reference into it.
        std::vector<uintptr_t> res = app.target.results_snapshot(500);
        size_t shown = res.size();
        for (size_t i = 0; i < shown; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char a[32];
            snprintf(a, sizeof(a), "%016llX", (unsigned long long)res[i]);
            if (ImGui::Selectable(a, false, ImGuiSelectableFlags_SpanAllColumns)) {
                snprintf(app.hexAddr, sizeof(app.hexAddr), "%llX", (unsigned long long)res[i]);
                snprintf(app.disasmAddr, sizeof(app.disasmAddr), "%llX", (unsigned long long)res[i]);
                snprintf(app.ptrAddr, sizeof(app.ptrAddr), "%llX", (unsigned long long)res[i]);
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Add to Trainer")) add_cheat_from(app, res[i], app.scanType);
                if (ImGui::MenuItem("Edit value (write live)")) {
                    app.editAddr = res[i];
                    snprintf(app.editBuf, sizeof(app.editBuf), "%s",
                             fmtValueAt(app.target, res[i], scanTypeOf(app.scanType)).c_str());
                    app.wantEditPopup = true;
                }
                if (ImGui::MenuItem("Browse in Hex")) snprintf(app.hexAddr, sizeof(app.hexAddr), "%llX", (unsigned long long)res[i]);
                if (ImGui::MenuItem("Disassemble")) snprintf(app.disasmAddr, sizeof(app.disasmAddr), "%llX", (unsigned long long)res[i]);
                if (ImGui::MenuItem("Pointer scan")) snprintf(app.ptrAddr, sizeof(app.ptrAddr), "%llX", (unsigned long long)res[i]);
                if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(a);
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(fmtValueAt(app.target, res[i], scanTypeOf(app.scanType)).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("Set")) {
                writeTyped(app, res[i], scanTypeOf(app.scanType), strtod(app.setValue, nullptr));
                app.addlog("set " + std::string(a) + " = " + app.setValue);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+cheat")) add_cheat_from(app, res[i], app.scanType);
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (resultCount > shown)
            ImGui::TextDisabled("... %zu more (showing first %zu). Narrow further with Next Scan.", resultCount - shown, shown);
    }
    ImGui::EndChild();

    // Live single-address poke (right-click a result -> Edit value). This writes
    // straight into the running game so you can test a value with no trainer.
    if (app.wantEditPopup) { ImGui::OpenPopup("poke_value"); app.wantEditPopup = false; }
    if (ImGui::BeginPopup("poke_value")) {
        ImGui::Text("Write live to %016llX", (unsigned long long)app.editAddr);
        ImGui::SetNextItemWidth(180);
        bool enter = ImGui::InputText("value", app.editBuf, sizeof(app.editBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Write") || enter) {
            writeTyped(app, app.editAddr, scanTypeOf(app.scanType), strtod(app.editBuf, nullptr));
            char ab[20];
            snprintf(ab, sizeof(ab), "%llX", (unsigned long long)app.editAddr);
            app.addlog("wrote live " + std::string(ab) + " = " + app.editBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Interpret the bytes at an address as every common type at once. This is the
// fastest way to tell a stable value (health/ammo) from one that updates every
// frame (position, timers, RNG): watch which row stops changing when you act.
static void drawDataInspector(uintptr_t base, const uint8_t* b) {
    if (ImGui::BeginTable("inspect", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("int8");
        ImGui::TableSetupColumn("int16");
        ImGui::TableSetupColumn("int32");
        ImGui::TableSetupColumn("int64");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", *(int8_t*)b);
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", *(int16_t*)b);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%d", *(int32_t*)b);
        ImGui::TableSetColumnIndex(3); ImGui::Text("%lld", (long long)*(int64_t*)b);
        ImGui::EndTable();
    }
    ImGui::Text("uint32 %u    float %.4f    double %.6f    ptr %016llX",
                *(uint32_t*)b, *(float*)b, *(double*)b, (unsigned long long)*(uintptr_t*)b);
    (void)base;
}

static void drawHex(App& app) {
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##addr", "address (hex, e.g. 7FF6...)", app.hexAddr, sizeof(app.hexAddr));
    ImGui::SameLine();
    bool attached = app.target.attached();
    uintptr_t base = (uintptr_t)strtoull(app.hexAddr, nullptr, 16);
    if (!attached) ImGui::BeginDisabled();
    if (ImGui::Button("Read")) {
        app.hexValid = base && app.target.read(base, app.hexBuf, sizeof(app.hexBuf));
        app.hexHasPrev = false;
        if (!app.hexValid) app.addlog("read failed @ " + std::string(app.hexAddr));
    }
    ImGui::SameLine();
    ImGui::Checkbox("Live", &app.hexAuto);
    if (!attached) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Live re-reads every frame; bytes that changed since last frame turn red.");

    // Live refresh: snapshot previous bytes, read fresh, diff for highlighting.
    if (app.hexAuto && attached && base) {
        memcpy(app.hexPrev, app.hexBuf, sizeof(app.hexBuf));
        app.hexHasPrev = app.hexValid;
        app.hexValid = app.target.read(base, app.hexBuf, sizeof(app.hexBuf));
    }

    ImGui::Separator();
    if (app.hexValid) {
        drawDataInspector(base, app.hexBuf);
        ImGui::Separator();
    }

    ImGui::BeginChild("hexdump", ImVec2(0, 0), true);
    if (app.hexValid) {
        const ImVec4 white(0.85f, 0.86f, 0.9f, 1.0f);
        const ImVec4 red(1.0f, 0.33f, 0.33f, 1.0f);
        const ImVec4 dim(0.5f, 0.52f, 0.58f, 1.0f);
        for (int row = 0; row < 16; ++row) {
            int off = row * 16;
            ImGui::TextColored(dim, "%016llX ", (unsigned long long)(base + off));
            for (int i = 0; i < 16; ++i) {
                ImGui::SameLine(0, 6);
                uint8_t byte = app.hexBuf[off + i];
                bool changed = app.hexHasPrev && byte != app.hexPrev[off + i];
                ImGui::TextColored(changed ? red : white, "%02X", byte);
            }
            ImGui::SameLine(0, 12);
            char ascii[17];
            for (int i = 0; i < 16; ++i) {
                uint8_t ch = app.hexBuf[off + i];
                ascii[i] = (ch >= 32 && ch < 127) ? (char)ch : '.';
            }
            ascii[16] = 0;
            ImGui::TextColored(dim, "%s", ascii);
        }
    } else {
        ImGui::TextDisabled("Enter an address and press Read (attach a target first). Tip: click a Scanner result to fill this.");
    }
    ImGui::EndChild();
}

static void drawMemoryMap(App& app) {
    static bool writableOnly = false;
    if (ImGui::Button("Refresh")) app.regions = app.target.regions();
    ImGui::SameLine();
    ImGui::Checkbox("writable only", &writableOnly);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu regions", app.regions.size());
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.3f, 0.66f, 1.0f, 1), "  exec");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.24f, 0.86f, 0.52f, 1), "write");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.85f, 0.86f, 0.9f, 1), "read");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.33f, 0.33f, 1), "guard/none");
    ImGui::BeginChild("regions", ImVec2(0, 0), true);
    if (ImGui::BeginTable("rgn", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Base");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Prot");
        ImGui::TableSetupColumn("Type");
        ImGui::TableHeadersRow();
        for (const auto& r : app.regions) {
            uint32_t b = r.protect & 0xFF;
            bool guard = (r.protect & PAGE_GUARD) || b == PAGE_NOACCESS;
            bool exec = b == PAGE_EXECUTE || b == PAGE_EXECUTE_READ ||
                        b == PAGE_EXECUTE_READWRITE || b == PAGE_EXECUTE_WRITECOPY;
            bool writ = b == PAGE_READWRITE || b == PAGE_WRITECOPY ||
                        b == PAGE_EXECUTE_READWRITE || b == PAGE_EXECUTE_WRITECOPY;
            if (writableOnly && !writ) continue;
            ImVec4 col = guard ? ImVec4(1.0f, 0.33f, 0.33f, 1)
                       : exec  ? ImVec4(0.3f, 0.66f, 1.0f, 1)
                       : writ  ? ImVec4(0.24f, 0.86f, 0.52f, 1)
                               : ImVec4(0.85f, 0.86f, 0.9f, 1);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char a[32];
            snprintf(a, sizeof(a), "%016llX", (unsigned long long)r.base);
            if (ImGui::Selectable(a, false, ImGuiSelectableFlags_SpanAllColumns))
                snprintf(app.hexAddr, sizeof(app.hexAddr), "%llX", (unsigned long long)r.base);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu KB", (unsigned long long)(r.size / 1024));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(col, "%s", hx::protect_name(r.protect));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s", r.type == MEM_IMAGE ? "image" : r.type == MEM_MAPPED ? "mapped" : "private");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

static void drawModules(App& app) {
    if (ImGui::Button("Refresh")) app.modules = app.target.modules();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu modules", app.modules.size());
    ImGui::BeginChild("mods", ImVec2(0, 0), true);
    if (ImGui::BeginTable("mod", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Module");
        ImGui::TableSetupColumn("Base");
        ImGui::TableSetupColumn("Size");
        ImGui::TableHeadersRow();
        for (const auto& m : app.modules) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(w2u(m.name).c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%016llX", (unsigned long long)m.base);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu KB", (unsigned long long)(m.size / 1024));
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

static void drawPointerScan(App& app) {
    bool attached = app.target.attached();
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##paddr", "target address (hex)", app.ptrAddr, sizeof(app.ptrAddr));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt("depth", &app.ptrLevels, 1, 6);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    int maxoff = (int)app.ptrMaxOff;
    if (ImGui::SliderInt("max offset", &maxoff, 0, 0x4000)) app.ptrMaxOff = maxoff;
    ImGui::SameLine();
    if (!attached) ImGui::BeginDisabled();
    if (ImGui::Button("Scan", ImVec2(90, 0))) {
        uintptr_t t = (uintptr_t)strtoull(app.ptrAddr, nullptr, 16);
        if (t) {
            app.ptrChains = app.target.pointer_scan(t, app.ptrLevels, app.ptrMaxOff, 100);
            app.ptrStatus = std::to_string(app.ptrChains.size()) + " chains";
            app.addlog("pointer scan from " + std::string(app.ptrAddr) + ": " + app.ptrStatus);
        }
    }
    if (!attached) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", app.ptrStatus.c_str());

    ImGui::Separator();
    ImGui::BeginChild("chains", ImVec2(0, 0), true);
    if (ImGui::BeginTable("chain", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Module");
        ImGui::TableSetupColumn("offset");
        ImGui::TableSetupColumn("chain -> target");
        ImGui::TableHeadersRow();
        for (const auto& c : app.ptrChains) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(w2u(c.module).c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llX", (unsigned long long)c.module_offset);
            ImGui::TableSetColumnIndex(2);
            std::string s = "base";
            for (auto off : c.offsets) {
                char b[32];
                snprintf(b, sizeof(b), " + 0x%llX", (unsigned long long)off);
                s += b;
            }
            ImGui::TextUnformatted(s.c_str());
        }
        ImGui::EndTable();
    }
    if (app.ptrChains.empty())
        ImGui::TextDisabled("No chains yet. Attach, scan for a value, click a result to fill the address, then Scan.");
    ImGui::EndChild();
}

static void drawDisasm(App& app) {
    bool attached = app.target.attached();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##daddr", "address (hex)", app.disasmAddr, sizeof(app.disasmAddr));
    ImGui::SameLine();
    if (!attached) ImGui::BeginDisabled();
    if (ImGui::Button("Disassemble")) {
        uintptr_t a = (uintptr_t)strtoull(app.disasmAddr, nullptr, 16);
        app.disasmLines.clear();
        if (a) {
            uint8_t code[256];
            if (app.target.read(a, code, sizeof(code))) {
                ZydisDecoder dec;
                ZydisFormatter fmt;
                ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
                ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
                size_t off = 0;
                for (int i = 0; i < 40 && off < sizeof(code); ++i) {
                    ZydisDecodedInstruction ins;
                    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + off, sizeof(code) - off, &ins, ops)))
                        break;
                    char text[256];
                    ZydisFormatterFormatInstruction(&fmt, &ins, ops, ins.operand_count_visible,
                                                    text, sizeof(text), a + off, ZYAN_NULL);
                    std::string bytes;
                    for (int b = 0; b < ins.length; ++b) {
                        char hb[8];
                        snprintf(hb, sizeof(hb), "%02X ", code[off + b]);
                        bytes += hb;
                    }
                    char line[400];
                    snprintf(line, sizeof(line), "%016llX  %-30s %s", (unsigned long long)(a + off),
                             bytes.c_str(), text);
                    app.disasmLines.push_back(line);
                    off += ins.length;
                }
                if (app.disasmLines.empty()) app.addlog("disasm: could not decode @ " + std::string(app.disasmAddr));
            } else {
                app.addlog("disasm: read failed @ " + std::string(app.disasmAddr));
            }
        }
    }
    if (!attached) ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginChild("dis", ImVec2(0, 0), true);
    for (const auto& l : app.disasmLines) ImGui::TextUnformatted(l.c_str());
    if (app.disasmLines.empty()) ImGui::TextDisabled("Enter an address and Disassemble (attached target).");
    ImGui::EndChild();
}

static void drawRuntime(App& app) {
    if (ImGui::Button("Refresh runtimes")) app.refreshProcs();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu runtime%s detected", app.runtimePids.size(),
                        app.runtimePids.size() == 1 ? "" : "s");

    ImGui::Separator();
    bool connected = app.rt.connected();
    ImGui::Text("Selected: %s (%u)", app.selectedPid ? app.selectedName.c_str() : "(none)", app.selectedPid);
    ImGui::SameLine();
    if (!connected) {
        if (!app.selectedPid || !app.runtimePids.count(app.selectedPid)) {
            if (ImGui::Button("Connect to runtime (inject first)")) app.addlog("selected pid has no runtime pipe");
        } else {
            if (ImGui::Button("Connect to runtime")) {
                std::string err;
                if (app.rt.connect(app.selectedPid, err)) app.addlog("runtime connected (pid " + std::to_string(app.selectedPid) + ")");
                else app.addlog("runtime connect failed: " + err);
            }
        }
    } else {
        if (ImGui::Button("Disconnect")) { app.rt.disconnect(); app.addlog("runtime disconnected"); }
        ImGui::SameLine();
        std::string err;
        if (ImGui::Button("Mitigations")) app.rtOutput = app.rt.call("getMitigations", "null", err);
        ImGui::SameLine();
        if (ImGui::Button("Hooks")) app.rtOutput = app.rt.call("listHooks", "null", err);
        ImGui::SameLine();
        if (ImGui::Button("Mods")) app.rtOutput = app.rt.call("listMods", "null", err);
        ImGui::SameLine();
        if (ImGui::Button("Process info")) app.rtOutput = app.rt.call("getProcessInfo", "null", err);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Lua (sandboxed, via evalLua):");
    ImGui::InputTextMultiline("##lua", app.luaCode, sizeof(app.luaCode), ImVec2(-1, 120));
    if (!connected) ImGui::BeginDisabled();
    if (ImGui::Button("Run Lua")) {
        std::string params = "{\"code\":\"" + json_escape(app.luaCode) + "\"}";
        std::string err;
        std::string res = app.rt.call("evalLua", params, err);
        app.rtOutput = res.empty() ? ("error: " + err) : res;
    }
    if (!connected) ImGui::EndDisabled();

    ImGui::TextUnformatted("Response:");
    ImGui::BeginChild("rtresp", ImVec2(0, 130), true);
    ImGui::TextWrapped("%s", app.rtOutput.empty() ? "(no response yet)" : app.rtOutput.c_str());
    ImGui::EndChild();

    ImGui::TextUnformatted("Runtime log stream:");
    ImGui::BeginChild("rtlog", ImVec2(0, 0), true);
    const auto& lg = app.rt.logs();
    size_t shown = std::min<size_t>(lg.size(), 400);
    for (size_t i = lg.size() - shown; i < lg.size(); ++i) ImGui::TextUnformatted(lg[i].c_str());
    if (lg.empty()) ImGui::TextDisabled("(no log notifications)");
    ImGui::EndChild();
}

static int hk_index(int vk) { return hx::hk_index(vk); }
static int hk_vk(int index) { return hx::hk_vk(index); }

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// Build a shareable trainer document from the current table.
static hx::TrainerDoc trainer_doc(App& app) {
    hx::TrainerDoc d;
    d.title = app.trainerTitle[0] ? app.trainerTitle : "Hexforge Trainer";
    d.target = !app.attachedName.empty() ? app.attachedName : app.selectedName;
    d.cheats = app.cheats;
    return d;
}

static std::string read_whole_file(const std::wstring& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s(sz > 0 ? (size_t)sz : 0, 0);
    if (sz > 0) { size_t got = fread(s.data(), 1, (size_t)sz, f); s.resize(got); }
    fclose(f);
    return s;
}

static void save_trainer(App& app) {
    std::wstring path = exe_dir() + widen(app.trainerFile);
    std::string text = hx::serialize_trainer(trainer_doc(app));
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) { app.addlog("save failed"); return; }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    app.addlog(std::string("saved ") + app.trainerFile + " (" + std::to_string(app.cheats.size()) + " cheats)");
}

static void load_trainer(App& app) {
    std::wstring path = exe_dir() + widen(app.trainerFile);
    std::string text = read_whole_file(path);
    if (text.empty()) { app.addlog("load failed: file not found next to exe"); return; }
    hx::TrainerDoc d;
    if (!hx::parse_trainer(text, d)) { app.addlog("load failed: not a hexforge trainer file"); return; }
    app.cheats = d.cheats;
    if (!d.title.empty()) snprintf(app.trainerTitle, sizeof(app.trainerTitle), "%s", d.title.c_str());
    if (!d.target.empty() && app.attachedName.empty()) app.attachedName = d.target;
    app.hotkeysDirty = true;
    app.addlog(std::string("loaded ") + app.trainerFile + " (" + std::to_string(app.cheats.size()) + " cheats)");
}

// Export a standalone single-file trainer: copy the prebuilt runner and append
// the current cheat table as an overlay the runner reads at launch.
static void export_trainer(App& app) {
    if (app.cheats.empty()) { app.exportStatus = "add at least one cheat first"; return; }
    hx::TrainerDoc d = trainer_doc(app);
    if (d.target.empty()) {
        app.exportStatus = "attach to the game first so the trainer knows which process to find";
        app.addlog(app.exportStatus);
        return;
    }
    std::wstring base = exe_dir() + L"hexforge_trainer.exe";
    if (GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES) {
        app.exportStatus = "hexforge_trainer.exe not found next to the tool (rebuild to generate it)";
        app.addlog(app.exportStatus);
        return;
    }
    // Output filename from the title, stripped to filesystem-safe characters.
    std::string safe;
    for (char c : d.title)
        safe += (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') ? c : '_';
    if (safe.empty()) safe = "Hexforge Trainer";
    std::wstring outPath = exe_dir() + widen(safe) + L".exe";

    if (!CopyFileW(base.c_str(), outPath.c_str(), FALSE)) {
        app.exportStatus = "could not write the trainer exe (error " + std::to_string(GetLastError()) + ")";
        app.addlog(app.exportStatus);
        return;
    }
    if (!hx::append_trainer_overlay(outPath, d)) {
        app.exportStatus = "copied runner but failed to embed the cheat table";
        app.addlog(app.exportStatus);
        return;
    }
    app.exportStatus = "exported " + safe + ".exe  (targets " + d.target + ", " +
                       std::to_string(d.cheats.size()) + " cheats) next to the tool";
    app.addlog(app.exportStatus);
}

// ── Trainer Wizard: a guided front-end over the scanner ──────────────────────
static void drawWizard(App& app) {
    bool attached = app.target.attached();
    bool scanning = app.scanning.load();
    ImGui::TextColored(ImVec4(0.49f, 0.36f, 1.0f, 1.0f), "Trainer Wizard");
    ImGui::TextDisabled("A guided way to find a value and turn it into a cheat - no need to understand the Scanner tab.");
    ImGui::Separator();
    if (!attached) {
        ImGui::TextColored(ImVec4(1.0f, 0.71f, 0.33f, 1), "Attach to a game first using the left panel (pick it, click Attach).");
        return;
    }

    const char* types[] = { "int8", "int16", "int32", "int64", "float", "double" };

    // Step 1 — choose what to modify.
    ImGui::Text("Step 1  -  What do you want to change?");
    struct Kind { const char* btn; const char* label; int type; };
    static const Kind kinds[] = {
        { "Health", "Health", 4 }, { "Money", "Money", 2 }, { "Ammo", "Ammo", 2 },
        { "Speed", "Speed", 4 },   { "Other", "Value", 2 },
    };
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(kinds[i].btn, ImVec2(84, 0))) {
            snprintf(app.wizName, sizeof(app.wizName), "%s", kinds[i].label);
            app.wizType = kinds[i].type;
            app.wizStep = 1;
            app.target.clear_scan();
            app.scanStatus = "cleared";
            app.hasScanned = false;
            app.wizValue[0] = 0;
        }
    }
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("name", app.wizName, sizeof(app.wizName));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("type", &app.wizType, types, 6);
    ImGui::TextDisabled("Tip: Health/Speed are usually float; Ammo/Money usually int32.");

    // Step 2 — find the starting value.
    if (app.wizStep >= 1) {
        ImGui::Separator();
        ImGui::Text("Step 2  -  Find the starting value");
        ImGui::TextWrapped("Look at your %s in the game. Type that number below and click 'Find it'. "
                           "If it has no visible number, click \"I don't know\".", app.wizName);
        ImGui::SetNextItemWidth(160);
        ImGui::InputText("current value", app.wizValue, sizeof(app.wizValue));
        if (scanning) ImGui::BeginDisabled();
        if (ImGui::Button("Find it", ImVec2(110, 0))) {
            app.scanType = app.wizType;
            snprintf(app.scanValue, sizeof(app.scanValue), "%s", app.wizValue);
            start_first_scan(app, false);
            app.wizStep = 2;
            app.addlog(std::string("wizard: find ") + app.wizName + " = " + app.wizValue);
        }
        ImGui::SameLine();
        if (ImGui::Button("I don't know", ImVec2(120, 0))) {
            app.scanType = app.wizType;
            start_first_scan(app, true);
            app.wizStep = 2;
            app.addlog(std::string("wizard: find ") + app.wizName + " (unknown start)");
        }
        if (scanning) ImGui::EndDisabled();
    }

    // Step 3 — narrow down.
    if (app.wizStep >= 2) {
        ImGui::Separator();
        ImGui::Text("Step 3  -  Narrow it down");
        ImGui::TextWrapped("Change your %s in the game (take damage, spend money, fire a shot), then tell me what happened. "
                           "Repeat until only a few addresses remain.", app.wizName);
        if (scanning) ImGui::BeginDisabled();
        if (ImGui::Button("It went DOWN")) { app.scanCompare = 4; start_next_scan(app); }
        ImGui::SameLine();
        if (ImGui::Button("It went UP")) { app.scanCompare = 3; start_next_scan(app); }
        ImGui::SameLine();
        if (ImGui::Button("It CHANGED")) { app.scanCompare = 1; start_next_scan(app); }
        ImGui::SameLine();
        if (ImGui::Button("Still the SAME")) { app.scanCompare = 2; start_next_scan(app); }
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("exact now", app.wizValue, sizeof(app.wizValue));
        ImGui::SameLine();
        if (ImGui::Button("It's this now")) {
            app.scanCompare = 0;
            snprintf(app.scanValue, sizeof(app.scanValue), "%s", app.wizValue);
            start_next_scan(app);
        }
        if (scanning) ImGui::EndDisabled();
    }

    // Progress / status.
    ImGui::Separator();
    if (scanning) {
        ImGui::Text("Scanning...");
        ImGui::ProgressBar(app.scanProgress.load(), ImVec2(-1, 0));
        if (ImGui::Button("Cancel")) app.scanCancel = true;
        return;
    }
    size_t count = app.target.result_count();
    ImGui::Text("Matches: %s", app.scanStatus.c_str());

    // Step 4 — pick the address and make the cheat.
    if (app.wizStep >= 2) {
        if (count == 0) {
            ImGui::TextDisabled("No matches. Start over above (pick a type, maybe float vs int32).");
        } else if (count > 40) {
            ImGui::TextDisabled("%zu matches - change the value in-game again and press a button above to narrow.", count);
        } else {
            ImGui::Separator();
            ImGui::Text("Step 4  -  Pick the right address and make the cheat");
            ImGui::BeginChild("wizres", ImVec2(0, 0), true);
            if (ImGui::BeginTable("wr", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Address");
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableHeadersRow();
                auto res = app.target.results_snapshot(60);
                for (size_t i = 0; i < res.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::PushID((int)i);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%016llX", (unsigned long long)res[i]);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(fmtValueAt(app.target, res[i], scanTypeOf(app.wizType)).c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (ImGui::SmallButton("Make cheat")) {
                        add_cheat_from(app, res[i], app.wizType);
                        snprintf(app.cheats.back().label, sizeof(app.cheats.back().label), "%s", app.wizName);
                        app.gotoTrainer = true;
                        app.exportStatus = std::string("added '") + app.wizName + "' - set Freeze or a hotkey in the Trainer tab";
                        app.addlog(std::string("wizard: added cheat '") + app.wizName + "'");
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
    }
}

static void drawTrainer(App& app) {
    bool attached = app.target.attached();
    ImGui::SetNextItemWidth(170);
    ImGui::InputText("title", app.trainerTitle, sizeof(app.trainerTitle));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    ImGui::InputText("file", app.trainerFile, sizeof(app.trainerFile));
    ImGui::SameLine(); if (ImGui::Button("Save")) save_trainer(app);
    ImGui::SameLine(); if (ImGui::Button("Load")) load_trainer(app);

    if (ImGui::Button("Add blank")) { app.cheats.push_back(Cheat{}); app.hotkeysDirty = true; }
    ImGui::SameLine(); if (ImGui::Button("All OFF")) { for (auto& c : app.cheats) c.freeze = false; }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.33f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.70f, 0.42f, 1.0f));
    if (ImGui::Button("Export Trainer (.exe)")) export_trainer(app);
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    const char* tgt = !app.attachedName.empty() ? app.attachedName.c_str()
                      : (app.selectedName.empty() ? "(attach a game)" : app.selectedName.c_str());
    ImGui::TextDisabled("target: %s", tgt);

    if (!app.exportStatus.empty())
        ImGui::TextColored(ImVec4(0.24f, 0.86f, 0.52f, 1), "%s", app.exportStatus.c_str());
    ImGui::TextDisabled("To change an amount (e.g. ammo), just edit its Value box and click Set - keep the same cheat, no need to add a new one. "
                        "Mode Toggle = Freeze/hotkey locks the value on/off (god mode, infinite ammo). Mode Set once = hotkey writes it one time. "
                        "Export bundles the table into a shareable single .exe that auto-finds the game.");
    ImGui::Separator();

    if (app.cheats.empty()) {
        ImGui::TextDisabled("No cheats yet. Right-click a Scanner result -> Add to Trainer, or click Add blank.");
        return;
    }

    const char* types[] = { "int8", "int16", "int32", "int64", "float", "double" };
    const char* hks[] = { "none", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
    const char* modes[] = { "Toggle", "Set once" };
    int removeIdx = -1;
    ImGui::BeginChild("trainerc", ImVec2(0, 0), true);
    if (ImGui::BeginTable("tr", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Freeze", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < app.cheats.size(); ++i) {
            Cheat& c = app.cheats[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##n", c.label, sizeof(c.label));
            ImGui::TableSetColumnIndex(1);
            char ab[20];
            snprintf(ab, sizeof(ab), "%llX", (unsigned long long)c.addr);
            if (ImGui::Selectable(ab)) {
                snprintf(app.hexAddr, sizeof(app.hexAddr), "%s", ab);
                snprintf(app.disasmAddr, sizeof(app.disasmAddr), "%s", ab);
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##t", &c.type, types, 6);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(attached ? fmtValueAt(app.target, c.addr, scanTypeOf(c.type)).c_str() : "-");
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##v", c.value, sizeof(c.value));
            ImGui::TableSetColumnIndex(5);
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##m", &c.mode, modes, 2);
            ImGui::TableSetColumnIndex(6);
            if (c.mode == 1) ImGui::BeginDisabled();
            ImGui::Checkbox("##f", &c.freeze);
            if (c.mode == 1) ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(7);
            int hi = hk_index(c.hotkey);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##h", &hi, hks, 13)) { c.hotkey = hk_vk(hi); app.hotkeysDirty = true; }
            ImGui::TableSetColumnIndex(8);
            if (ImGui::SmallButton("Set"))
                writeTyped(app, c.addr, scanTypeOf(c.type), strtod(c.value, nullptr));
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) removeIdx = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (removeIdx >= 0) { app.cheats.erase(app.cheats.begin() + removeIdx); app.hotkeysDirty = true; }
}

static void drawHelp(App& app) {
    (void)app;
    ImGui::TextColored(ImVec4(0.49f, 0.36f, 1.0f, 1.0f), "What each tab does");
    ImGui::Separator();
    auto item = [](const char* name, const char* desc) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.21f, 0.88f, 0.82f, 1.0f), "%s", name);
        ImGui::SameLine();
        ImGui::TextWrapped("- %s", desc);
    };
    ImGui::BeginChild("helpc", ImVec2(0, 0), true);
    item("TARGETS (left)", "Pick a process (search by name or PID). Attach = read/write its memory. Inject runtime = load the engine for hooks/overlay (not needed for scanning). Green [runtime] = already injected.");
    item("Wizard", "Start here. A guided 4-step flow: choose Health/Money/Ammo/Speed/Other, enter the in-game number, change it and say up/down/same, then click 'Make cheat' to drop it straight into the Trainer. No scanner knowledge needed.");
    item("Scanner", "Find a value. Set type (int32 first; HP is often float; money often double), type the number, First Scan. Change it in-game, pick increased/decreased, Next Scan. Repeat to a few results. Right-click a result -> Add to Trainer / Hex / Disasm. 'First Scan (unknown)' when you don't know the number.");
    item("Trainer", "Your saved cheats. Each has a value, a Mode (Toggle = hotkey flips Freeze on/off for god mode; Set once = hotkey writes the value one time), a Freeze lock, and a Hotkey (F1-F12) that works while in-game. Save/Load writes a .hexforge file next to the exe. 'Export Trainer (.exe)' bundles the table into a single shareable program that auto-finds the game by name.");
    item("Pointer Scan", "Find a permanent path (module+offsets) to an address so it survives game restarts. Fill the target address (click a Scanner result first), set depth, Scan.");
    item("Disasm", "Show the CPU instructions at an address (Zydis). Useful to understand code around a value.");
    item("Hex", "Raw bytes at an address (hex + text). Tick 'Live' to re-read every frame - bytes that just changed flash red, and the Data Inspector shows the bytes as int/float/double at once. This is how you tell a steady value (health, ammo) from one that updates constantly (position, timers).");
    item("Memory Map", "All memory regions of the target with protection (RW-, R-X...). Click a region to jump to it in Hex.");
    item("Modules", "Loaded DLLs with base address + size. Click to send the base to Hex.");
    item("Runtime", "Only after Inject runtime: talk to the engine over its pipe - mitigations, hook/mod lists, and sandboxed Lua, with a live log.");
    item("Log", "What the tool did. A full copy is also written to hexforge.log next to the exe. Crashes write hexforge-crash.dmp.");
    ImGui::Separator();
    ImGui::TextWrapped("Quick infinite-ammo recipe: Attach -> Scanner int32 = ammo -> shoot -> Next Scan decreased -> repeat to 1-3 results -> right-click -> Add to Trainer -> set Value + tick Freeze (or assign F1).");
    ImGui::TextWrapped("Values changing too fast? That is normal - lots of memory (positions, timers, animation, RNG) updates every frame. Lock onto the steady one: use 'changed/unchanged' between actions, or open the address in Hex with Live on and watch which bytes stop moving when you act. Health is usually float; ammo/money usually int; big money sometimes double.");
    ImGui::EndChild();
}

static void drawLog(App& app) {
    static bool autoscroll = true;
    if (ImGui::Button("Clear")) app.log.clear();
    ImGui::SameLine();
    if (ImGui::Button("Copy all")) {
        std::string all;
        for (const auto& l : app.log) { all += l; all += '\n'; }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to file")) {
        std::wstring path = exe_dir() + L"hexforge-log.txt";
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"w");
        if (f) {
            for (const auto& l : app.log) fprintf(f, "%s\n", l.c_str());
            fclose(f);
            app.addlog("log saved to hexforge-log.txt");
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("autoscroll", &autoscroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##logf", "filter text", app.logFilter, sizeof(app.logFilter));
    ImGui::TextDisabled("Full timestamped log is also written to hexforge.log next to the exe. Red = error, yellow = warning.");

    ImGui::BeginChild("logc", ImVec2(0, 0), true);
    std::string f = lower(app.logFilter);
    const ImVec4 white(0.85f, 0.86f, 0.9f, 1.0f);
    const ImVec4 red(1.0f, 0.4f, 0.4f, 1.0f);
    const ImVec4 yellow(1.0f, 0.82f, 0.33f, 1.0f);
    for (const auto& l : app.log) {
        std::string low = lower(l);
        if (!f.empty() && low.find(f) == std::string::npos) continue;
        ImVec4 col = white;
        if (low.find("fail") != std::string::npos || low.find("error") != std::string::npos ||
            low.find("crash") != std::string::npos)
            col = red;
        else if (low.find("warn") != std::string::npos)
            col = yellow;
        ImGui::TextColored(col, "%s", l.c_str());
    }
    if (autoscroll) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

static void drawUI(App& app) {
    if (app.rt.connected()) app.rt.pump();
    if (app.target.attached()) {
        for (auto& c : app.cheats)
            if (c.mode == 0 && c.freeze) writeTyped(app, c.addr, scanTypeOf(c.type), strtod(c.value, nullptr));
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Hexforge##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // header
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.49f, 0.36f, 1.0f, 1.0f));
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextUnformatted("HEXFORGE");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("native memory + injection tool");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 240);
    ImGui::TextColored(hx::is_elevated() ? ImVec4(0.24f, 0.86f, 0.52f, 1) : ImVec4(1, 0.71f, 0.33f, 1),
                       hx::is_elevated() ? "ADMIN" : "not elevated");
    ImGui::SameLine();
    if (app.target.attached())
        ImGui::TextColored(ImVec4(0.24f, 0.86f, 0.52f, 1), "attached pid %u", app.target.pid());
    else
        ImGui::TextDisabled("not attached");
    ImGui::SameLine();
    if (app.scanning.load())
        ImGui::TextColored(ImVec4(0.3f, 0.66f, 1.0f, 1), "| scanning %.0f%%", app.scanProgress.load() * 100.0f);
    else if (!app.cheats.empty())
        ImGui::TextDisabled("| %zu cheats", app.cheats.size());
    ImGui::Separator();

    drawTargets(app);
    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0, 0), false);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Wizard")) { drawWizard(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Scanner")) { drawScanner(app); ImGui::EndTabItem(); }
        ImGuiTabItemFlags trFlags = app.gotoTrainer ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Trainer", nullptr, trFlags)) { drawTrainer(app); ImGui::EndTabItem(); }
        app.gotoTrainer = false;
        if (ImGui::BeginTabItem("Pointer Scan")) { drawPointerScan(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Disasm")) { drawDisasm(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Hex")) { drawHex(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Memory Map")) { drawMemoryMap(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Modules")) { drawModules(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Runtime")) { drawRuntime(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Help")) { drawHelp(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log")) { drawLog(app); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::End();
}

// ── entry point ──────────────────────────────────────────────────────────────
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    install_logging();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HexforgeToolWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hexforge", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1240, 820, nullptr, nullptr, hInst, nullptr);
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
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    StyleHexforge();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    App app;
    g_app = &app;
    hx::enable_debug_privilege();
    app.refreshProcs();
    app.addlog("Hexforge ready. Rights: " + std::string(hx::is_elevated() ? "elevated" : "standard"));

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (app.hotkeysDirty) { sync_hotkeys(app); app.hotkeysDirty = false; }

        if (g_resizeW && g_resizeH) {
            CleanupRTV();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRTV();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawUI(app);
        ImGui::Render();

        const float clear[4] = { 0.02f, 0.03f, 0.05f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    app.scanCancel = true;
    if (app.scanThread.joinable()) app.scanThread.join();
    for (int i = 0; i < 64; ++i) UnregisterHotKey(g_hwnd, HK_BASE + i);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
