// backend.cpp — Win32 implementation of the Hexforge native tool backend.
#include "backend.hpp"

#include <tlhelp32.h>
#include <psapi.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace hx {

size_t type_size(ScanType t) {
    switch (t) {
        case ScanType::I8: return 1;
        case ScanType::I16: return 2;
        case ScanType::I32: return 4;
        case ScanType::I64: return 8;
        case ScanType::F32: return 4;
        case ScanType::F64: return 8;
    }
    return 4;
}

const char* protect_name(uint32_t p) {
    uint32_t base = p & 0xFF;
    switch (base) {
        case PAGE_NOACCESS: return "---";
        case PAGE_READONLY: return "R--";
        case PAGE_READWRITE: return "RW-";
        case PAGE_WRITECOPY: return "RWC";
        case PAGE_EXECUTE: return "--X";
        case PAGE_EXECUTE_READ: return "R-X";
        case PAGE_EXECUTE_READWRITE: return "RWX";
        case PAGE_EXECUTE_WRITECOPY: return "RWCX";
        default: return "?";
    }
}

static bool is_readable(uint32_t protect) {
    if (protect & PAGE_GUARD) return false;
    uint32_t b = protect & 0xFF;
    return b == PAGE_READONLY || b == PAGE_READWRITE || b == PAGE_WRITECOPY ||
           b == PAGE_EXECUTE_READ || b == PAGE_EXECUTE_READWRITE || b == PAGE_EXECUTE_WRITECOPY;
}

static bool is_writable(uint32_t protect) {
    if (protect & PAGE_GUARD) return false;
    uint32_t b = protect & 0xFF;
    return b == PAGE_READWRITE || b == PAGE_WRITECOPY || b == PAGE_EXECUTE_READWRITE ||
           b == PAGE_EXECUTE_WRITECOPY;
}

std::vector<ProcEntry> list_processes() {
    std::vector<ProcEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e;
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e)) {
        do {
            if (e.th32ProcessID != 0)
                out.push_back({e.th32ProcessID, e.szExeFile});
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return out;
}

bool enable_debug_privilege() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    LUID luid;
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(tok);
    return ok;
}

std::vector<uint32_t> runtime_pids() {
    std::vector<uint32_t> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"\\\\.\\pipe\\umf-studio-*", &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;          // "umf-studio-<pid>"
        size_t dash = name.find_last_of(L'-');
        if (dash != std::wstring::npos) {
            uint32_t pid = (uint32_t)wcstoul(name.c_str() + dash + 1, nullptr, 10);
            if (pid) out.push_back(pid);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

bool is_elevated() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD n = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &n) && el.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

std::wstring runtime_dll_path() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    p += L"umf_runtime.dll";
    return p;
}

// Run a single-argument function in the target via a remote thread. `arg` (if
// given) is copied into the target and passed as the thread parameter.
static bool remote_call(HANDLE h, void* fn, const void* arg, SIZE_T argBytes,
                        DWORD waitMs, DWORD* exitCode) {
    LPVOID remote = nullptr;
    if (arg && argBytes) {
        remote = VirtualAllocEx(h, nullptr, argBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) return false;
        if (!WriteProcessMemory(h, remote, arg, argBytes, nullptr)) {
            VirtualFreeEx(h, remote, 0, MEM_RELEASE);
            return false;
        }
    }
    HANDLE th = CreateRemoteThread(h, nullptr, 0, (LPTHREAD_START_ROUTINE)fn, remote, 0, nullptr);
    bool ok = false;
    if (th) {
        WaitForSingleObject(th, waitMs);
        if (exitCode) GetExitCodeThread(th, exitCode);
        CloseHandle(th);
        ok = true;
    }
    if (remote) VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    return ok;
}

static uintptr_t remote_module_base(uint32_t pid, const wchar_t* name_lower) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W m;
    m.dwSize = sizeof(m);
    uintptr_t base = 0;
    if (Module32FirstW(snap, &m)) {
        do {
            std::wstring n = m.szModule;
            for (auto& ch : n) ch = (wchar_t)towlower(ch);
            if (n == name_lower) { base = (uintptr_t)m.modBaseAddr; break; }
        } while (Module32NextW(snap, &m));
    }
    CloseHandle(snap);
    return base;
}

bool inject_dll(uint32_t pid, const std::wstring& dll, std::string& err) {
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "runtime DLL not found next to the tool: umf_runtime.dll";
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) {
        err = "OpenProcess failed (error " + std::to_string(GetLastError()) +
              "). Try running elevated.";
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void* pSetDllDir = (void*)GetProcAddress(k32, "SetDllDirectoryW");
    void* pLoadLib = (void*)GetProcAddress(k32, "LoadLibraryW");
    if (!pLoadLib) { CloseHandle(h); err = "GetProcAddress(LoadLibraryW) failed"; return false; }

    // 1. Add the tool's directory to the target's DLL search path so the
    //    runtime's dependencies (Zydis.dll, lua.dll, CRT DLLs) resolve there.
    std::wstring dir = dll;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    if (pSetDllDir)
        remote_call(h, pSetDllDir, dir.c_str(), (dir.size() + 1) * sizeof(wchar_t), 5000, nullptr);

    // 2. LoadLibraryW(full path) in the target.
    remote_call(h, pLoadLib, dll.c_str(), (dll.size() + 1) * sizeof(wchar_t), 15000, nullptr);

    // Success is "the module is now mapped" (the x64 thread exit code truncates
    // the HMODULE, so we verify by enumerating the target's modules instead).
    uintptr_t base = remote_module_base(pid, L"umf_runtime.dll");
    if (!base) {
        CloseHandle(h);
        err = "LoadLibrary failed in the target: a dependency could not be loaded "
              "(Zydis.dll / lua.dll, or the Debug CRT). Keep those DLLs beside the tool, "
              "or use the Release build whose CRT the system already has.";
        return false;
    }

    // 3. Bootstrap: run umf_runtime_start in the target so it starts its IPC
    //    pipe server. Compute the export RVA from a local, non-executing load.
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    bool started = false;
    if (local) {
        FARPROC startLocal = GetProcAddress(local, "umf_runtime_start");
        if (startLocal) {
            uintptr_t rva = (uintptr_t)startLocal - (uintptr_t)local;
            HANDLE th = CreateRemoteThread(h, nullptr, 0,
                                           (LPTHREAD_START_ROUTINE)(base + rva), nullptr, 0, nullptr);
            if (th) { WaitForSingleObject(th, 8000); CloseHandle(th); started = true; }
        }
        FreeLibrary(local);
    }
    CloseHandle(h);
    if (!started)
        err = "runtime loaded but bootstrap (umf_runtime_start) did not run; "
              "rebuild the DLL. Connect may not find a pipe.";
    return true;
}

// ── Target ──────────────────────────────────────────────────────────────────

bool Target::attach(uint32_t pid, std::string& err) {
    detach();
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                               PROCESS_VM_OPERATION,
                           FALSE, pid);
    if (!h) {
        err = "OpenProcess failed (error " + std::to_string(GetLastError()) +
              "). Elevated rights may be required.";
        return false;
    }
    h_ = h;
    pid_ = pid;
    return true;
}

void Target::detach() {
    if (h_) {
        CloseHandle(h_);
        h_ = nullptr;
    }
    pid_ = 0;
    clear_scan();
}

std::vector<ModuleEntry> Target::modules() const {
    std::vector<ModuleEntry> out;
    if (!pid_) return out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W m;
    m.dwSize = sizeof(m);
    if (Module32FirstW(snap, &m)) {
        do {
            out.push_back({m.szModule, (uintptr_t)m.modBaseAddr, (size_t)m.modBaseSize});
        } while (Module32NextW(snap, &m));
    }
    CloseHandle(snap);
    return out;
}

std::vector<RegionEntry> Target::regions() const {
    std::vector<RegionEntry> out;
    if (!h_) return out;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t limit = (uintptr_t)si.lpMaximumApplicationAddress;
    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQueryEx(h_, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT) {
            out.push_back({(uintptr_t)mbi.BaseAddress, mbi.RegionSize, mbi.Protect, mbi.State, mbi.Type});
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    return out;
}

bool Target::read(uintptr_t addr, void* out, size_t n) const {
    if (!h_) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(h_, (LPCVOID)addr, out, n, &got) && got == n;
}

bool Target::write(uintptr_t addr, const void* in, size_t n) const {
    if (!h_) return false;
    DWORD oldp = 0;
    VirtualProtectEx(h_, (LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &oldp);
    SIZE_T put = 0;
    bool ok = WriteProcessMemory(h_, (LPVOID)addr, in, n, &put) && put == n;
    DWORD tmp;
    VirtualProtectEx(h_, (LPVOID)addr, n, oldp, &tmp);
    return ok;
}

void Target::clear_scan() {
    std::lock_guard<std::mutex> lk(mtx_);
    results_.clear();
    last_.clear();
}

size_t Target::result_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return results_.size();
}

std::vector<uintptr_t> Target::results_snapshot(size_t max) const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t n = results_.size() < max ? results_.size() : max;
    return std::vector<uintptr_t>(results_.begin(), results_.begin() + n);
}

static double read_as_double(const uint8_t* p, ScanType t) {
    switch (t) {
        case ScanType::I8: return (double)*(const int8_t*)p;
        case ScanType::I16: return (double)*(const int16_t*)p;
        case ScanType::I32: return (double)*(const int32_t*)p;
        case ScanType::I64: return (double)*(const int64_t*)p;
        case ScanType::F32: return (double)*(const float*)p;
        case ScanType::F64: return *(const double*)p;
    }
    return 0.0;
}

static bool approx(double a, double b, ScanType t) {
    if (t == ScanType::F32 || t == ScanType::F64)
        return std::fabs(a - b) <= 0.01 * (std::fabs(b) > 1.0 ? std::fabs(b) : 1.0);
    return a == b;
}

static const size_t kScanCap = 2'000'000;
static const size_t kUnknownCap = 12'000'000;

// ── Parallel region scanning ─────────────────────────────────────────────────
// The target's usable regions are split into fixed-size jobs and consumed by a
// pool of worker threads (one per core). Each worker reads a job with a single
// large ReadProcessMemory and appends matches to its own thread-local vectors,
// so there is no shared-state contention in the hot loop; results merge at the
// end. This turns multi-minute scans on big 64-bit games into a few seconds.
namespace {
struct ScanJob { uintptr_t base; size_t size; };
struct LocalOut { std::vector<uintptr_t> a; std::vector<double> v; };
// per-chunk callback: inspect [buf,buf+len) that lives at `base`, append hits.
using ChunkFn = std::function<void(const uint8_t*, size_t, uintptr_t, LocalOut&)>;
}

static void run_parallel(HANDLE h, const std::vector<RegionEntry>& regs, bool writable_only,
                         size_t overlap, size_t cap,
                         std::atomic<bool>* cancel, std::atomic<float>* progress,
                         const ChunkFn& fn,
                         std::vector<uintptr_t>& outA, std::vector<double>& outV) {
    const size_t CHUNK = 16u << 20;   // 16 MB reads amortise RPM overhead
    std::vector<ScanJob> jobs;
    for (const auto& r : regs) {
        bool usable = writable_only ? is_writable(r.protect) : is_readable(r.protect);
        if (!usable) continue;
        size_t step = (overlap < CHUNK) ? (CHUNK - overlap) : CHUNK;
        for (size_t off = 0; off < r.size; off += step) {
            size_t len = r.size - off;
            if (len > CHUNK) len = CHUNK;
            jobs.push_back({r.base + off, len});
            if (len < CHUNK) break;
        }
    }
    uint64_t total = 0;
    for (const auto& j : jobs) total += j.size;
    if (!total) total = 1;

    unsigned nthreads = std::thread::hardware_concurrency();
    if (!nthreads) nthreads = 4;
    if ((size_t)nthreads > jobs.size()) nthreads = (unsigned)(jobs.empty() ? 1 : jobs.size());

    std::atomic<size_t> nextJob{0};
    std::atomic<uint64_t> done{0};
    std::vector<LocalOut> locals(nthreads);

    auto worker = [&](unsigned tid) {
        std::vector<uint8_t> buf;
        LocalOut& out = locals[tid];
        for (;;) {
            if (cancel && cancel->load()) break;
            size_t i = nextJob.fetch_add(1);
            if (i >= jobs.size()) break;
            const ScanJob& job = jobs[i];
            buf.resize(job.size);
            SIZE_T got = 0;
            if (ReadProcessMemory(h, (LPCVOID)job.base, buf.data(), job.size, &got) && got > 0)
                fn(buf.data(), (size_t)got, job.base, out);
            uint64_t d = done.fetch_add(job.size) + job.size;
            if (progress) progress->store((float)((double)d / (double)total));
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nthreads; ++t) pool.emplace_back(worker, t);
    worker(0);                       // run one worker on this thread too
    for (auto& th : pool) th.join();
    if (progress) progress->store(1.0f);

    size_t tot = 0;
    for (auto& l : locals) tot += l.a.size();
    outA.reserve(tot < cap ? tot : cap);
    outV.reserve(tot < cap ? tot : cap);
    for (auto& l : locals) {
        for (size_t i = 0; i < l.a.size() && outA.size() < cap; ++i) {
            outA.push_back(l.a[i]);
            outV.push_back(l.v[i]);
        }
        if (outA.size() >= cap) break;
    }
}

size_t Target::first_scan(ScanType t, double value, bool writable_only,
                          std::atomic<bool>* cancel, std::atomic<float>* progress) {
    clear_scan();
    stype_ = t;
    if (!h_) return 0;
    size_t ts = type_size(t);
    auto regs = regions();
    std::vector<uintptr_t> rr;
    std::vector<double> ll;
    run_parallel(h_, regs, writable_only, 0, kScanCap, cancel, progress,
        [ts, t, value](const uint8_t* buf, size_t len, uintptr_t base, LocalOut& out) {
            for (size_t o = 0; o + ts <= len; o += ts) {
                double v = read_as_double(buf + o, t);
                if (approx(v, value, t)) { out.a.push_back(base + o); out.v.push_back(v); }
            }
        }, rr, ll);
    std::lock_guard<std::mutex> lk(mtx_);
    results_.swap(rr);
    last_.swap(ll);
    return results_.size();
}

size_t Target::first_scan_unknown(ScanType t, bool writable_only,
                                  std::atomic<bool>* cancel, std::atomic<float>* progress) {
    clear_scan();
    stype_ = t;
    if (!h_) return 0;
    size_t ts = type_size(t);
    auto regs = regions();
    std::vector<uintptr_t> rr;
    std::vector<double> ll;
    run_parallel(h_, regs, writable_only, 0, kUnknownCap, cancel, progress,
        [ts, t](const uint8_t* buf, size_t len, uintptr_t base, LocalOut& out) {
            for (size_t o = 0; o + ts <= len; o += ts) {
                out.a.push_back(base + o);
                out.v.push_back(read_as_double(buf + o, t));
            }
        }, rr, ll);
    std::lock_guard<std::mutex> lk(mtx_);
    results_.swap(rr);
    last_.swap(ll);
    return results_.size();
}

size_t Target::next_scan(ScanCompare c, double value,
                         std::atomic<bool>* cancel, std::atomic<float>* progress) {
    if (!h_) return 0;

    // Snapshot the current set under lock, then work on the copy with no lock
    // held (the long memory-read loop must not block the UI's snapshot reads).
    std::vector<uintptr_t> rr;
    std::vector<double> ll;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rr = results_;
        ll = last_;
    }

    size_t ts = type_size(stype_);
    std::vector<uintptr_t> keep;
    std::vector<double> keepLast;
    keep.reserve(rr.size());
    keepLast.reserve(rr.size());

    uint8_t tmp[8];
    size_t n = rr.size();
    for (size_t i = 0; i < n; ++i) {
        if (cancel && (i & 0xFFFF) == 0 && cancel->load()) break;
        if (progress && (i & 0xFFFF) == 0 && n) progress->store((float)((double)i / (double)n));
        SIZE_T got = 0;
        if (!ReadProcessMemory(h_, (LPCVOID)rr[i], tmp, ts, &got) || got != ts) continue;
        double cur = read_as_double(tmp, stype_);
        double prev = ll[i];
        bool ok = false;
        switch (c) {
            case ScanCompare::Exact: ok = approx(cur, value, stype_); break;
            case ScanCompare::Changed: ok = !approx(cur, prev, stype_); break;
            case ScanCompare::Unchanged: ok = approx(cur, prev, stype_); break;
            case ScanCompare::Increased: ok = cur > prev; break;
            case ScanCompare::Decreased: ok = cur < prev; break;
        }
        if (ok) {
            keep.push_back(rr[i]);
            keepLast.push_back(cur);
        }
    }
    if (progress) progress->store(1.0f);
    std::lock_guard<std::mutex> lk(mtx_);
    results_.swap(keep);
    last_.swap(keepLast);
    return results_.size();
}

bool parse_aob(const std::string& text, std::vector<uint8_t>& pattern, std::vector<bool>& mask) {
    pattern.clear();
    mask.clear();
    size_t i = 0;
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size()) break;
        if (text[i] == '?') {
            pattern.push_back(0);
            mask.push_back(false);
            ++i;
            if (i < text.size() && text[i] == '?') ++i;   // allow "??"
        } else {
            int hi = hexval(text[i]);
            if (hi < 0) return false;
            int lo = (i + 1 < text.size()) ? hexval(text[i + 1]) : -1;
            if (lo < 0) { pattern.push_back((uint8_t)hi); i += 1; }
            else { pattern.push_back((uint8_t)(hi * 16 + lo)); i += 2; }
            mask.push_back(true);
        }
    }
    return !pattern.empty();
}

size_t Target::aob_scan(const std::vector<uint8_t>& pattern, const std::vector<bool>& mask,
                        bool writable_only, std::atomic<bool>* cancel, std::atomic<float>* progress) {
    clear_scan();
    if (!h_ || pattern.empty()) return 0;
    size_t plen = pattern.size();

    // Index of the first non-wildcard byte: we memchr for it to skip the vast
    // majority of positions instead of comparing the full pattern everywhere.
    int firstFixed = -1;
    for (size_t k = 0; k < plen; ++k)
        if (mask[k]) { firstFixed = (int)k; break; }

    auto regs = regions();
    std::vector<uintptr_t> rr;
    std::vector<double> ll;
    run_parallel(h_, regs, writable_only, plen - 1, kScanCap, cancel, progress,
        [&pattern, &mask, plen, firstFixed](const uint8_t* buf, size_t len, uintptr_t base, LocalOut& out) {
            if (len < plen) return;
            size_t limit = len - plen;   // last valid start offset
            auto full_match = [&](size_t s) {
                for (size_t k = 0; k < plen; ++k)
                    if (mask[k] && buf[s + k] != pattern[k]) return false;
                return true;
            };
            if (firstFixed < 0) {   // all wildcards: every position matches
                for (size_t s = 0; s <= limit; ++s) { out.a.push_back(base + s); out.v.push_back(0.0); }
                return;
            }
            uint8_t fb = pattern[firstFixed];
            size_t searchFrom = (size_t)firstFixed;
            while (searchFrom < len) {
                const uint8_t* found = (const uint8_t*)memchr(buf + searchFrom, fb, len - searchFrom);
                if (!found) break;
                size_t j = (size_t)(found - buf);
                size_t start = j - (size_t)firstFixed;
                if (j >= (size_t)firstFixed && start <= limit && full_match(start)) {
                    out.a.push_back(base + start);
                    out.v.push_back(0.0);
                }
                searchFrom = j + 1;
            }
        }, rr, ll);

    // Chunks overlap by plen-1, so a match on a boundary can be reported twice;
    // de-duplicate.
    std::sort(rr.begin(), rr.end());
    rr.erase(std::unique(rr.begin(), rr.end()), rr.end());
    ll.assign(rr.size(), 0.0);

    std::lock_guard<std::mutex> lk(mtx_);
    results_.swap(rr);
    last_.swap(ll);
    return results_.size();
}

size_t Target::string_scan(const std::string& needle, bool utf16, bool writable_only,
                           std::atomic<bool>* cancel, std::atomic<float>* progress) {
    std::vector<uint8_t> pat;
    std::vector<bool> mask;
    if (utf16) {
        for (char c : needle) { pat.push_back((uint8_t)c); mask.push_back(true);
                                pat.push_back(0); mask.push_back(true); }
    } else {
        for (char c : needle) { pat.push_back((uint8_t)c); mask.push_back(true); }
    }
    return aob_scan(pat, mask, writable_only, cancel, progress);
}

std::vector<Target::ExportEntry> Target::module_exports(uintptr_t base) const {
    std::vector<ExportEntry> out;
    if (!h_ || !base) return out;
    IMAGE_DOS_HEADER dos;
    if (!read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return out;
    IMAGE_NT_HEADERS64 nt;
    if (!read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return out;
    IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return out;
    IMAGE_EXPORT_DIRECTORY exp;
    if (!read(base + dir.VirtualAddress, &exp, sizeof(exp))) return out;

    uint32_t nNames = exp.NumberOfNames;
    uint32_t nFuncs = exp.NumberOfFunctions;
    if (nNames > 65535) nNames = 65535;
    if (nFuncs > 65535) nFuncs = 65535;
    std::vector<uint32_t> nameRvas(nNames), funcRvas(nFuncs);
    std::vector<uint16_t> ords(nNames);
    if (nNames && !read(base + exp.AddressOfNames, nameRvas.data(), nNames * 4)) return out;
    if (nNames && !read(base + exp.AddressOfNameOrdinals, ords.data(), nNames * 2)) return out;
    if (nFuncs && !read(base + exp.AddressOfFunctions, funcRvas.data(), nFuncs * 4)) return out;

    auto read_name = [&](uintptr_t a) -> std::string {
        char nm[192];
        for (size_t len = 128; len >= 16; len -= 16)
            if (read(a, nm, len)) { nm[len - 1] = 0; return std::string(nm); }
        return "";
    };
    out.reserve(nNames);
    for (uint32_t i = 0; i < nNames; ++i) {
        uint16_t ord = ords[i];
        uint32_t frva = (ord < funcRvas.size()) ? funcRvas[ord] : 0;
        out.push_back({ read_name(base + nameRvas[i]), base + frva, frva });
    }
    return out;
}

uintptr_t Target::aob_find_first(const std::vector<uint8_t>& pattern, const std::vector<bool>& mask,
                                 bool writable_only) const {
    if (!h_ || pattern.empty()) return 0;
    size_t plen = pattern.size();
    int firstFixed = -1;
    for (size_t k = 0; k < plen; ++k)
        if (mask[k]) { firstFixed = (int)k; break; }
    std::vector<uint8_t> buf;
    const size_t CHUNK = 4u << 20;
    for (const auto& r : regions()) {
        bool usable = writable_only ? is_writable(r.protect) : is_readable(r.protect);
        if (!usable) continue;
        size_t step = (plen - 1 < CHUNK) ? (CHUNK - (plen - 1)) : CHUNK;
        for (size_t off = 0; off < r.size; off += step) {
            size_t len = r.size - off;
            if (len > CHUNK) len = CHUNK;
            if (len < plen) break;
            buf.resize(len);
            SIZE_T got = 0;
            if (ReadProcessMemory(h_, (LPCVOID)(r.base + off), buf.data(), len, &got) && got >= plen) {
                size_t n = (size_t)got, limit = n - plen;
                auto full = [&](size_t s) {
                    for (size_t k = 0; k < plen; ++k)
                        if (mask[k] && buf[s + k] != pattern[k]) return false;
                    return true;
                };
                if (firstFixed < 0) return r.base + off;   // all wildcards
                uint8_t fb = pattern[firstFixed];
                size_t from = (size_t)firstFixed;
                while (from < n) {
                    const uint8_t* f = (const uint8_t*)memchr(buf.data() + from, fb, n - from);
                    if (!f) break;
                    size_t j = (size_t)(f - buf.data());
                    size_t start = j - (size_t)firstFixed;
                    if (j >= (size_t)firstFixed && start <= limit && full(start))
                        return r.base + off + start;
                    from = j + 1;
                }
            }
            if (len < CHUNK) break;
        }
    }
    return 0;
}

std::vector<PtrChain> Target::pointer_scan(uintptr_t target, int max_level,
                                           int64_t max_offset, int max_results) {
    std::vector<PtrChain> chains;
    if (!h_) return chains;
    if (max_level < 1) max_level = 1;
    if (max_level > 6) max_level = 6;

    // 1. Build an index of real pointer slots: (value -> slot addr). We only
    //    keep values that actually point into a committed region, so the index
    //    stays small enough to cover module space (the old "any 48-bit value"
    //    rule exhausted a 4M cap in the first heaps and returned 0 chains on
    //    64-bit games).
    const uintptr_t kLo = 0x10000, kHi = 0x00007FFFFFFFFFFFULL;
    auto regs_all = regions();

    std::vector<std::pair<uintptr_t, uintptr_t>> ranges;   // (base, end), sorted by base
    ranges.reserve(regs_all.size());
    for (const auto& r : regs_all) ranges.push_back({r.base, r.base + r.size});
    std::sort(ranges.begin(), ranges.end());
    auto in_committed = [&](uintptr_t v) -> bool {
        size_t lo = 0, hi = ranges.size();
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (ranges[mid].first <= v) lo = mid + 1; else hi = mid; }
        if (lo == 0) return false;
        return v >= ranges[lo - 1].first && v < ranges[lo - 1].second;
    };

    const size_t kIdxCap = 8u << 20;   // 8M real pointers (~128 MB)
    std::vector<std::pair<uintptr_t, uintptr_t>> idx;   // (value, slot)
    idx.reserve(1 << 20);

    std::vector<uint8_t> buf;
    for (const auto& r : regs_all) {
        if (!is_readable(r.protect)) continue;
        const size_t CHUNK = 4u << 20;
        for (size_t off = 0; off < r.size; off += CHUNK) {
            size_t len = r.size - off;
            if (len > CHUNK) len = CHUNK;
            if (len < 8) break;
            buf.resize(len);
            SIZE_T got = 0;
            if (!ReadProcessMemory(h_, (LPCVOID)(r.base + off), buf.data(), len, &got) || got < 8)
                continue;
            size_t n = (size_t)got;
            for (size_t o = 0; o + 8 <= n; o += 8) {
                uintptr_t v = *(uintptr_t*)(buf.data() + o);
                if (v >= kLo && v < kHi && in_committed(v))
                    idx.push_back({v, r.base + off + o});
                if (idx.size() >= kIdxCap) break;
            }
            if (idx.size() >= kIdxCap) break;
        }
        if (idx.size() >= kIdxCap) break;
    }
    std::sort(idx.begin(), idx.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    auto modules_now = modules();
    auto in_module = [&](uintptr_t addr, std::wstring& name, size_t& moff) -> bool {
        for (const auto& m : modules_now) {
            if (addr >= m.base && addr < m.base + m.size) {
                name = m.name;
                moff = addr - m.base;
                return true;
            }
        }
        return false;
    };

    // 2. Reverse BFS: level 0 is the target; expand outward through pointer
    //    slots whose value is within `max_offset` below a known node.
    std::unordered_map<uintptr_t, std::pair<uintptr_t, int64_t>> parent;
    std::unordered_set<uintptr_t> visited;
    std::vector<uintptr_t> frontier{target};

    for (int depth = 1; depth <= max_level && chains.size() < (size_t)max_results; ++depth) {
        std::vector<uintptr_t> next;
        for (uintptr_t node : frontier) {
            uintptr_t lo = (node > (uintptr_t)max_offset) ? node - (uintptr_t)max_offset : kLo;
            auto it = std::lower_bound(
                idx.begin(), idx.end(), std::make_pair((uintptr_t)lo, (uintptr_t)0),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            for (; it != idx.end() && it->first <= node; ++it) {
                uintptr_t V = it->first;
                uintptr_t S = it->second;
                if (visited.count(S)) continue;
                visited.insert(S);
                parent[S] = {node, (int64_t)(node - V)};

                std::wstring mod;
                size_t moff = 0;
                if (in_module(S, mod, moff)) {
                    if (chains.size() < (size_t)max_results) {
                        PtrChain c;
                        c.module = mod;
                        c.module_offset = moff;
                        uintptr_t cur = S;
                        while (cur != target) {
                            auto pit = parent.find(cur);
                            if (pit == parent.end()) break;
                            c.offsets.push_back(pit->second.second);
                            cur = pit->second.first;
                            if (c.offsets.size() > 16) break;
                        }
                        if (cur == target && !c.offsets.empty())
                            chains.push_back(std::move(c));
                    }
                }
                next.push_back(S);
            }
        }
        frontier.swap(next);
        if (frontier.empty()) break;
    }
    return chains;
}

uintptr_t Target::resolve_chain(const PtrChain& c) const {
    if (!h_ || c.offsets.empty()) return 0;
    uintptr_t base = 0;
    for (const auto& m : modules())
        if (m.name == c.module) { base = m.base; break; }
    if (!base) return 0;
    uintptr_t p = 0;
    if (!read(base + c.module_offset, &p, 8)) return 0;
    p += (uintptr_t)c.offsets[0];
    for (size_t i = 1; i < c.offsets.size(); ++i) {
        if (!read(p, &p, 8)) return 0;
        p += (uintptr_t)c.offsets[i];
    }
    return p;
}

// ── DebugWatch: find what writes/accesses an address ────────────────────────────────────────────────

bool DebugWatch::start(uint32_t pid, uintptr_t addr, int size, bool on_write, std::string& err) {
    if (running_.load()) { err = "a watch is already running"; return false; }
    if (size != 1 && size != 2 && size != 4 && size != 8) size = 4;
    // Hardware breakpoints require the address aligned to the length; fall back
    // to a 1-byte watch if it is not aligned.
    if (addr % (uintptr_t)size != 0) size = 1;
    pid_ = pid;
    addr_ = addr;
    size_ = size;
    on_write_ = on_write;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        hits_.clear();
        status_ = "starting...";
    }
    stop_ = false;
    running_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void DebugWatch::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    running_ = false;
}

std::vector<WriteHit> DebugWatch::hits() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return hits_;
}

std::string DebugWatch::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

void DebugWatch::arm_thread(HANDLE th) {
    if (!th) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &ctx)) return;
    DWORD64 len;   // encode length: 1->00, 2->01, 8->10, 4->11
    switch (size_) {
        case 1: len = 0b00; break;
        case 2: len = 0b01; break;
        case 8: len = 0b10; break;
        default: len = 0b11; break;
    }
    DWORD64 rw = on_write_ ? 0b01 : 0b11;   // write-only, or read/write
    ctx.Dr0 = addr_;
    DWORD64 dr7 = ctx.Dr7;
    dr7 |= 1;                       // L0 (local enable for DR0)
    dr7 &= ~(0xFull << 16);         // clear RW0 + LEN0
    dr7 |= (rw << 16) | (len << 18);
    ctx.Dr7 = dr7;
    ctx.Dr6 = 0;
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    SetThreadContext(th, &ctx);
}

void DebugWatch::disarm_all() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid_) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                ctx.Dr0 = 0;
                ctx.Dr7 &= ~(DWORD64)(1 | (0xFull << 16));   // disable DR0
                ctx.Dr6 = 0;
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                SetThreadContext(th, &ctx);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void DebugWatch::loop() {
    if (!DebugActiveProcess(pid_)) {
        std::lock_guard<std::mutex> lk(mtx_);
        status_ = "DebugActiveProcess failed (error " + std::to_string(GetLastError()) +
                  "); run elevated and detach other debuggers";
        running_ = false;
        return;
    }
    DebugSetProcessKillOnExit(FALSE);   // never take the game down with us
    {
        std::lock_guard<std::mutex> lk(mtx_);
        status_ = "watching... change the value in-game to catch the writer";
    }

    bool alive = true;
    while (alive && !stop_.load()) {
        DEBUG_EVENT ev;
        if (!WaitForDebugEvent(&ev, 200)) continue;   // timeout: re-check stop_

        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                arm_thread(ev.u.CreateProcessInfo.hThread);
                if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
                break;
            case CREATE_THREAD_DEBUG_EVENT:
                arm_thread(ev.u.CreateThread.hThread);
                break;
            case EXCEPTION_DEBUG_EVENT: {
                DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
                if (code == EXCEPTION_SINGLE_STEP) {
                    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, ev.dwThreadId);
                    if (th) {
                        CONTEXT ctx{};
                        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL;
                        if (GetThreadContext(th, &ctx) && (ctx.Dr6 & 0xF)) {
                            uintptr_t rip = (uintptr_t)ctx.Rip;
                            std::lock_guard<std::mutex> lk(mtx_);
                            bool found = false;
                            for (auto& hhit : hits_)
                                if (hhit.rip == rip) { hhit.count++; found = true; break; }
                            if (!found && hits_.size() < 256)
                                hits_.push_back({rip, ev.dwThreadId, 1});
                        }
                        ctx.Dr6 = 0;
                        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                        SetThreadContext(th, &ctx);
                        CloseHandle(th);
                    }
                    cont = DBG_CONTINUE;
                } else if (code == EXCEPTION_BREAKPOINT) {
                    cont = DBG_CONTINUE;             // initial loader breakpoint
                } else {
                    cont = DBG_EXCEPTION_NOT_HANDLED; // let the app handle its own
                }
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                alive = false;
                break;
            default:
                break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    disarm_all();
    DebugActiveProcessStop(pid_);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        status_ = alive ? "stopped" : "target exited";
    }
    running_ = false;
}

} // namespace hx
