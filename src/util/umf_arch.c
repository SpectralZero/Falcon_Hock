/*
 * umf_arch.c — §ARCH: architecture detection + per-arch strategy capability
 *
 * Portability groundwork that touches nothing in the (x64) hook engine. It
 * reports the build architecture, the machine of any PE image, emulation
 * state, and — crucially for a future port and for the Studio — which hook
 * strategies the current engine can actually implement on a given machine.
 *
 * The engine's inline relocator (Zydis x64), trampoline unwind
 * (RUNTIME_FUNCTION), and hardware-breakpoint path (Dr0..Dr3) are x64-only, so
 * only the data-only strategies (IAT/EAT/VTABLE) are reported as portable
 * until those paths are ported. This is honest capability reporting, not a
 * claim of support.
 */

#include "umf/umf.h"

UmfArch umf_arch_current(void) {
#if defined(_M_X64) || defined(__x86_64__)
    return UMF_ARCH_X64;
#elif defined(_M_IX86) || defined(__i386__)
    return UMF_ARCH_X86;
#elif defined(_M_ARM64) || defined(__aarch64__)
    return UMF_ARCH_ARM64;
#elif defined(_M_ARM) || defined(__arm__)
    return UMF_ARCH_ARM;
#else
    return UMF_ARCH_UNKNOWN;
#endif
}

static UmfArch arch_from_machine(WORD machine) {
    switch (machine) {
        case IMAGE_FILE_MACHINE_AMD64: return UMF_ARCH_X64;
        case IMAGE_FILE_MACHINE_I386:  return UMF_ARCH_X86;
        case IMAGE_FILE_MACHINE_ARM64: return UMF_ARCH_ARM64;
        case IMAGE_FILE_MACHINE_ARMNT: return UMF_ARCH_ARM;
        default:                       return UMF_ARCH_UNKNOWN;
    }
}

UmfArch umf_arch_of_module(HMODULE module) {
    if (!module) return UMF_ARCH_UNKNOWN;
    UmfArch result = UMF_ARCH_UNKNOWN;
    __try {
        uint8_t* base = (uint8_t*)module;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return UMF_ARCH_UNKNOWN;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return UMF_ARCH_UNKNOWN;
        result = arch_from_machine(nt->FileHeader.Machine);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = UMF_ARCH_UNKNOWN;
    }
    return result;
}

const char* umf_arch_name(UmfArch a) {
    switch (a) {
        case UMF_ARCH_X86:   return "x86";
        case UMF_ARCH_X64:   return "x64";
        case UMF_ARCH_ARM:   return "arm";
        case UMF_ARCH_ARM64: return "arm64";
        default:             return "unknown";
    }
}

bool umf_arch_is_emulated(void) {
    typedef BOOL (WINAPI *PFN_IsWow64Process2)(HANDLE, USHORT*, USHORT*);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return false;
    PFN_IsWow64Process2 fn =
        (PFN_IsWow64Process2)GetProcAddress(k32, "IsWow64Process2");
    if (!fn) return false;

    USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT native_machine  = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!fn(GetCurrentProcess(), &process_machine, &native_machine))
        return false;

    /* process_machine == UNKNOWN means "not running under WOW/emulation". */
    return process_machine != IMAGE_FILE_MACHINE_UNKNOWN;
}

UmfHookStrategyMask umf_arch_supported_strategies(UmfArch a) {
    /* Data-only strategies are architecture-agnostic (they rewrite pointers,
     * not code), so they are available everywhere the PE format is. */
    UmfHookStrategyMask data_only =
        UMF_STRAT_IAT | UMF_STRAT_EAT | UMF_STRAT_VTABLE;

    switch (a) {
        case UMF_ARCH_X64:
            /* Full engine: inline + gap relocation, HWBP, and data-only. */
            return UMF_STRAT_INLINE | UMF_STRAT_GAP | UMF_STRAT_HARDWARE_BP |
                   data_only;
        case UMF_ARCH_X86:
        case UMF_ARCH_ARM64:
        case UMF_ARCH_ARM:
            return data_only;
        default:
            return 0;
    }
}
