// backend.cpp — Win32 implementation of the Hexforge native tool backend.
#include "backend.hpp"

#include <tlhelp32.h>
#include <psapi.h>
#include <cmath>
#include <cstring>
#include <algorithm>
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

    bool ok = false;
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        err = "VirtualAllocEx failed (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(h);
        return false;
    }

    if (WriteProcessMemory(h, remote, dll.c_str(), bytes, nullptr)) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        auto load = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(h, nullptr, 0, load, remote, 0, nullptr);
        if (th) {
            WaitForSingleObject(th, 15000);
            DWORD code = 0;
            GetExitCodeThread(th, &code);
            CloseHandle(th);
            if (code != 0) {
                ok = true;
            } else {
                err = "LoadLibraryW returned 0 in the target (DLL failed to load; "
                      "arch mismatch or dependency missing)";
            }
        } else {
            err = "CreateRemoteThread failed (error " + std::to_string(GetLastError()) + ")";
        }
    } else {
        err = "WriteProcessMemory failed (error " + std::to_string(GetLastError()) + ")";
    }

    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    return ok;
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

static uint64_t total_scan_bytes(const std::vector<RegionEntry>& regs, bool writable_only) {
    uint64_t total = 0;
    for (const auto& r : regs) {
        bool usable = writable_only ? is_writable(r.protect) : is_readable(r.protect);
        if (usable) total += r.size;
    }
    return total ? total : 1;
}

size_t Target::first_scan(ScanType t, double value, bool writable_only,
                          std::atomic<bool>* cancel, std::atomic<float>* progress) {
    clear_scan();
    stype_ = t;
    if (!h_) return 0;
    size_t ts = type_size(t);

    auto regs = regions();
    uint64_t total = total_scan_bytes(regs, writable_only), done = 0;
    std::vector<uintptr_t> rr;   // built locally, published under lock at the end
    std::vector<double> ll;
    std::vector<uint8_t> buf;
    for (const auto& r : regs) {
        if (cancel && cancel->load()) break;
        bool usable = writable_only ? is_writable(r.protect) : is_readable(r.protect);
        if (!usable) continue;

        const size_t CHUNK = 1u << 20;
        for (size_t off = 0; off < r.size; off += CHUNK) {
            if (cancel && cancel->load()) break;
            size_t len = r.size - off;
            if (len > CHUNK) len = CHUNK;
            if (len < ts) break;
            buf.resize(len);
            SIZE_T got = 0;
            if (ReadProcessMemory(h_, (LPCVOID)(r.base + off), buf.data(), len, &got) && got >= ts) {
                size_t usable_len = (size_t)got;
                for (size_t o = 0; o + ts <= usable_len; o += ts) {
                    double v = read_as_double(buf.data() + o, t);
                    if (approx(v, value, t)) {
                        rr.push_back(r.base + off + o);
                        ll.push_back(v);
                        if (rr.size() >= kScanCap) { done = total; goto done_label; }
                    }
                }
            }
            done += len;
            if (progress) progress->store((float)((double)done / (double)total));
        }
    }
done_label:
    if (progress) progress->store(1.0f);
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
    uint64_t total = total_scan_bytes(regs, writable_only), done = 0;
    std::vector<uintptr_t> rr;
    std::vector<double> ll;
    std::vector<uint8_t> buf;
    for (const auto& r : regs) {
        if (cancel && cancel->load()) break;
        bool usable = writable_only ? is_writable(r.protect) : is_readable(r.protect);
        if (!usable) continue;

        const size_t CHUNK = 1u << 20;
        for (size_t off = 0; off < r.size; off += CHUNK) {
            if (cancel && cancel->load()) break;
            size_t len = r.size - off;
            if (len > CHUNK) len = CHUNK;
            if (len < ts) break;
            buf.resize(len);
            SIZE_T got = 0;
            if (ReadProcessMemory(h_, (LPCVOID)(r.base + off), buf.data(), len, &got) && got >= ts) {
                size_t usable_len = (size_t)got;
                for (size_t o = 0; o + ts <= usable_len; o += ts) {
                    rr.push_back(r.base + off + o);
                    ll.push_back(read_as_double(buf.data() + o, t));
                    if (rr.size() >= kUnknownCap) { done = total; goto done_label; }
                }
            }
            done += len;
            if (progress) progress->store((float)((double)done / (double)total));
        }
    }
done_label:
    if (progress) progress->store(1.0f);
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

std::vector<PtrChain> Target::pointer_scan(uintptr_t target, int max_level,
                                           int64_t max_offset, int max_results) {
    std::vector<PtrChain> chains;
    if (!h_) return chains;
    if (max_level < 1) max_level = 1;
    if (max_level > 6) max_level = 6;

    // 1. Build an index of every plausible pointer slot: (value -> slot addr).
    const uintptr_t kLo = 0x10000, kHi = 0x00007FFFFFFFFFFFULL;
    std::vector<std::pair<uintptr_t, uintptr_t>> idx;   // (value, slot)
    idx.reserve(1 << 20);

    std::vector<uint8_t> buf;
    for (const auto& r : regions()) {
        if (!is_readable(r.protect)) continue;
        const size_t CHUNK = 1u << 20;
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
                if (v >= kLo && v < kHi)
                    idx.push_back({v, r.base + off + o});
                if (idx.size() >= (1u << 22)) break;   // 4M cap
            }
            if (idx.size() >= (1u << 22)) break;
        }
        if (idx.size() >= (1u << 22)) break;
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

} // namespace hx
