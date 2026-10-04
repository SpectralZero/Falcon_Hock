/*
 * umf_unwind.c — §8 Exception-unwind metadata for trampolines
 *
 * x86-64 Windows uses table-based exception handling: every piece of
 * executable code must have a RUNTIME_FUNCTION + UNWIND_INFO entry, or
 * the OS cannot unwind through it. A trampoline that lacks this will
 * break SEH/C++ exception propagation and stack walks whenever an
 * exception crosses it (M2 fix).
 *
 * Layout inside the 96-byte slot:
 *   [0 .. code_size)                : trampoline code      (written by builder)
 *   [unwind_off .. +UNWIND_INFO]    : UNWIND_INFO_MINIMAL  (dword-aligned)
 *   [rf_off .. +RUNTIME_FUNCTION]   : RUNTIME_FUNCTION     (dword-aligned)
 *
 * The RUNTIME_FUNCTION's Begin/End/UnwindData are RVAs relative to the
 * trampoline code base, which is also the BaseAddress handed to
 * RtlAddFunctionTable.
 *
 * ORDERING NOTE: this must run while the slot page is still writable
 * (RW), i.e. BEFORE umf_trampoline_finalize() flips it to RX. The
 * structures stay readable once the page is RX, so RtlAddFunctionTable
 * (which only records the pointer) remains valid for the slot lifetime.
 */

#include "umf/umf.h"

#pragma pack(push, 1)
typedef struct {
    BYTE Version       : 3;   /* 1                                   */
    BYTE Flags         : 5;   /* 0 (no handler, no chaining)         */
    BYTE SizeOfProlog;        /* 0 — trampoline has no formal prolog */
    BYTE CountOfCodes;        /* 0 — no unwind codes                 */
    BYTE FrameRegister : 4;   /* 0 — RSP-based frame                 */
    BYTE FrameOffset   : 4;   /* 0                                   */
} UmfUnwindInfoMinimal;
#pragma pack(pop)

RUNTIME_FUNCTION* umf_register_unwind_info(void* trampoline, size_t code_size) {
    uint8_t* base = (uint8_t*)trampoline;

    /* Place UNWIND_INFO after the code, dword-aligned */
    size_t unwind_off = (code_size + 3) & ~(size_t)3;
    size_t rf_off =
        (unwind_off + sizeof(UmfUnwindInfoMinimal) + 3) & ~(size_t)3;

    if (rf_off + sizeof(RUNTIME_FUNCTION) > UMF_TRAMPOLINE_SLOT_SIZE) {
        UMF_ERROR("Trampoline slot too small for unwind metadata "
                  "(code=%zu, need=%zu, have=%d)",
                  code_size, rf_off + sizeof(RUNTIME_FUNCTION),
                  UMF_TRAMPOLINE_SLOT_SIZE);
        return NULL;
    }

    UmfUnwindInfoMinimal* unwind = (UmfUnwindInfoMinimal*)(base + unwind_off);
    unwind->Version       = 1;
    unwind->Flags         = 0;
    unwind->SizeOfProlog  = 0;
    unwind->CountOfCodes  = 0;
    unwind->FrameRegister = 0;
    unwind->FrameOffset   = 0;

    RUNTIME_FUNCTION* rf = (RUNTIME_FUNCTION*)(base + rf_off);
    rf->BeginAddress = 0;
    rf->EndAddress   = (DWORD)code_size;
    rf->UnwindData   = (DWORD)unwind_off;

    if (!RtlAddFunctionTable(rf, 1, (DWORD64)base)) {
        UMF_ERROR("RtlAddFunctionTable failed: %lu", GetLastError());
        return NULL;
    }

    return rf;
}

void umf_unregister_unwind_info(RUNTIME_FUNCTION* rf) {
    if (rf) {
        RtlDeleteFunctionTable(rf);
    }
}
