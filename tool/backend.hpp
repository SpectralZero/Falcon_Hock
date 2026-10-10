// backend.hpp — Hexforge native tool: external process memory + injection.
//
// Cheat-Engine model: operate on a target via OpenProcess + ReadProcessMemory /
// WriteProcessMemory / VirtualQueryEx (no injection needed to view/scan/edit
// memory). Injection (CreateRemoteThread + LoadLibraryW) is used only to load
// umf_runtime.dll for the advanced in-target engine features.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

namespace hx {

struct ProcEntry {
    uint32_t pid;
    std::wstring name;
};

struct ModuleEntry {
    std::wstring name;
    uintptr_t base;
    size_t size;
};

struct RegionEntry {
    uintptr_t base;
    size_t size;
    uint32_t protect;
    uint32_t state;
    uint32_t type;
};

enum class ScanType { I8, I16, I32, I64, F32, F64 };
enum class ScanCompare { Exact, Changed, Unchanged, Increased, Decreased };

size_t type_size(ScanType t);
const char* protect_name(uint32_t protect);

std::vector<ProcEntry> list_processes();
std::vector<uint32_t> runtime_pids();            // pids exposing \\.\pipe\umf-studio-*
bool enable_debug_privilege();
bool is_elevated();
std::wstring runtime_dll_path();                 // umf_runtime.dll next to this exe
bool inject_dll(uint32_t pid, const std::wstring& dll, std::string& err);

// A static pointer path: module+module_offset -> +off0 -> +off1 ... -> target.
// Resolving: p = *(module_base + module_offset); p += off[0]; p = *p; ...
struct PtrChain {
    std::wstring module;
    size_t module_offset = 0;
    std::vector<int64_t> offsets;   // applied after each deref
};

// A live handle to a target process for external memory access.
class Target {
public:
    ~Target() { detach(); }

    bool attach(uint32_t pid, std::string& err);
    void detach();
    bool attached() const { return h_ != nullptr; }
    uint32_t pid() const { return pid_; }

    std::vector<ModuleEntry> modules() const;
    std::vector<RegionEntry> regions() const;

    bool read(uintptr_t addr, void* out, size_t n) const;
    bool write(uintptr_t addr, const void* in, size_t n) const;

    // Scanner
    // Scanner. `cancel`/`progress` are optional: a worker thread sets *cancel
    // to stop early and reads *progress (0..1) for a progress bar.
    size_t first_scan(ScanType t, double value, bool writable_only,
                      std::atomic<bool>* cancel = nullptr,
                      std::atomic<float>* progress = nullptr);
    // Unknown initial value: record every slot so Next Scan (increased/decreased/
    // changed) works without knowing the number.
    size_t first_scan_unknown(ScanType t, bool writable_only,
                              std::atomic<bool>* cancel = nullptr,
                              std::atomic<float>* progress = nullptr);
    size_t next_scan(ScanCompare c, double value,
                     std::atomic<bool>* cancel = nullptr,
                     std::atomic<float>* progress = nullptr);
    void clear_scan();
    ScanType scan_type() const { return stype_; }

    // Thread-safe result access (scans run on a worker thread).
    size_t result_count() const;
    std::vector<uintptr_t> results_snapshot(size_t max) const;

    // Static pointer scan: find chains of pointers (with small offsets) that
    // resolve to `target`. Depth `max_level`, per-hop offset limit
    // `max_offset`. Returns up to `max_results` chains rooted in a module.
    std::vector<PtrChain> pointer_scan(uintptr_t target, int max_level,
                                       int64_t max_offset, int max_results);

private:
    HANDLE h_ = nullptr;
    uint32_t pid_ = 0;
    ScanType stype_ = ScanType::I32;
    mutable std::mutex mtx_;
    std::vector<uintptr_t> results_;
    std::vector<double> last_;
};

} // namespace hx
