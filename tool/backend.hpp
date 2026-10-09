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
bool enable_debug_privilege();
bool is_elevated();
std::wstring runtime_dll_path();                 // umf_runtime.dll next to this exe
bool inject_dll(uint32_t pid, const std::wstring& dll, std::string& err);

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
    size_t first_scan(ScanType t, double value, bool writable_only);
    size_t next_scan(ScanCompare c, double value);
    void clear_scan();
    ScanType scan_type() const { return stype_; }
    const std::vector<uintptr_t>& results() const { return results_; }

private:
    HANDLE h_ = nullptr;
    uint32_t pid_ = 0;
    ScanType stype_ = ScanType::I32;
    std::vector<uintptr_t> results_;
    std::vector<double> last_;
};

} // namespace hx
