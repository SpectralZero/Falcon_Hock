/*
 * test_arch.c — §ARCH: architecture detection + per-arch strategy capability.
 *
 * The suite runs on an x64 build, so the running/module architecture is x64
 * and the full strategy set is reported; the ARM64/x86 branches are checked
 * for the data-only capability map that the (unported) engine can honour.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <string.h>

void run_arch_tests(void) {
    /* ── Current + module architecture (this is an x64 build) ── */
    CHECK(umf_arch_current() == UMF_ARCH_X64, "runtime built for x64");
    CHECK(umf_arch_of_module(GetModuleHandleW(NULL)) == UMF_ARCH_X64,
          "main module is x64");
    HMODULE rt = GetModuleHandleA("umf_runtime.dll");
    CHECK(rt && umf_arch_of_module(rt) == UMF_ARCH_X64, "umf_runtime.dll is x64");
    CHECK(umf_arch_of_module(NULL) == UMF_ARCH_UNKNOWN, "NULL module is unknown");

    /* ── Names round-trip + non-NULL ── */
    CHECK(strcmp(umf_arch_name(UMF_ARCH_X64), "x64") == 0, "name of x64");
    CHECK(strcmp(umf_arch_name(UMF_ARCH_X86), "x86") == 0, "name of x86");
    CHECK(strcmp(umf_arch_name(UMF_ARCH_ARM64), "arm64") == 0, "name of arm64");
    CHECK(strcmp(umf_arch_name(UMF_ARCH_ARM), "arm") == 0, "name of arm");
    CHECK(umf_arch_name(UMF_ARCH_UNKNOWN) != NULL &&
          strcmp(umf_arch_name((UmfArch)999), "unknown") == 0,
          "unknown/out-of-range name is 'unknown'");

    /* ── Per-arch strategy capability ── */
    UmfHookStrategyMask x64 = umf_arch_supported_strategies(UMF_ARCH_X64);
    CHECK((x64 & UMF_STRAT_INLINE) != 0, "x64 supports inline hooks");
    CHECK((x64 & UMF_STRAT_HARDWARE_BP) != 0, "x64 supports hardware breakpoints");
    CHECK((x64 & UMF_STRAT_IAT) != 0, "x64 supports IAT hooks");

    UmfHookStrategyMask a64 = umf_arch_supported_strategies(UMF_ARCH_ARM64);
    CHECK((a64 & UMF_STRAT_INLINE) == 0, "arm64 has no inline hooks yet");
    CHECK((a64 & UMF_STRAT_HARDWARE_BP) == 0, "arm64 has no HWBP path yet");
    CHECK((a64 & (UMF_STRAT_IAT | UMF_STRAT_EAT | UMF_STRAT_VTABLE)) ==
          (UMF_STRAT_IAT | UMF_STRAT_EAT | UMF_STRAT_VTABLE),
          "arm64 supports the data-only strategies");

    UmfHookStrategyMask x86 = umf_arch_supported_strategies(UMF_ARCH_X86);
    CHECK((x86 & UMF_STRAT_INLINE) == 0, "x86 inline not supported by x64 engine");
    CHECK((x86 & UMF_STRAT_VTABLE) != 0, "x86 supports vtable hooks");

    CHECK(umf_arch_supported_strategies(UMF_ARCH_UNKNOWN) == 0,
          "unknown architecture supports nothing");

    /* ── Emulation query is informational; just exercise it safely ── */
    bool emu = umf_arch_is_emulated();
    CHECK(emu == true || emu == false, "is_emulated returns a boolean");
}
