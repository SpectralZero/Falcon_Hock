/*
 * umf_mitigations.c — Detect ACG, HVCI, CFG, CET (SS + IBT)
 *
 * All detection functions are pure queries — no side effects,
 * no memory writes, safe to call at any time.
 *
 * CET IBT detection is heuristic (no public Windows API exposes it).
 * The heuristic is conservative: errs toward "IBT active" (false
 * positive = harmless extra ENDBR64 checks; false negative = crash).
 */

#include "umf/umf.h"
#include <intrin.h>

/* ── ntdll types for HVCI query ── */
typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *PFN_NtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);

typedef struct {
    ULONG Length;
    ULONG CodeIntegrityOptions;
} UMF_SYSTEM_CODEINTEGRITY_INFORMATION;

#define UMF_SYSTEM_CODE_INTEGRITY_INFO_CLASS   103
#define UMF_CODEINTEGRITY_OPTION_HVCI          0x00000200

/* ────────────────────────────────────────────────────────────────
 * ACG Detection
 * ──────────────────────────────────────────────────────────────── */

static void detect_acg(UmfMitigationStatus* m) {
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy;
    memset(&policy, 0, sizeof(policy));

    if (GetProcessMitigationPolicy(GetCurrentProcess(),
            ProcessDynamicCodePolicy, &policy, sizeof(policy))) {
        m->acg_enforced   = (policy.ProhibitDynamicCode != 0);
        m->acg_audit_only = (policy.AuditProhibitDynamicCode != 0);
    }
}

/* ────────────────────────────────────────────────────────────────
 * HVCI Detection
 * ──────────────────────────────────────────────────────────────── */

static void detect_hvci(UmfMitigationStatus* m) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return;

    PFN_NtQuerySystemInformation pNtQSI =
        (PFN_NtQuerySystemInformation)GetProcAddress(
            ntdll, "NtQuerySystemInformation");
    if (!pNtQSI) return;

    UMF_SYSTEM_CODEINTEGRITY_INFORMATION sci;
    memset(&sci, 0, sizeof(sci));
    sci.Length = sizeof(sci);
    ULONG returned = 0;

    NTSTATUS status = pNtQSI(UMF_SYSTEM_CODE_INTEGRITY_INFO_CLASS,
                              &sci, sizeof(sci), &returned);
    if (status == 0) {
        m->hvci_active = (sci.CodeIntegrityOptions &
                          UMF_CODEINTEGRITY_OPTION_HVCI) != 0;
    }
}

/* ────────────────────────────────────────────────────────────────
 * CFG Detection
 * ──────────────────────────────────────────────────────────────── */

static void detect_cfg(UmfMitigationStatus* m) {
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (GetProcessMitigationPolicy(GetCurrentProcess(),
            ProcessControlFlowGuardPolicy, &cfg, sizeof(cfg))) {
        m->cfg_enforced = (cfg.EnableControlFlowGuard != 0);
    }
}

/* ────────────────────────────────────────────────────────────────
 * CET Detection (Shadow Stack + IBT)
 *
 * Shadow Stack: query via ProcessUserShadowStackPolicy
 * IBT: heuristic — no direct public API
 *   (a) Main EXE has IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT in Debug Dir
 *   (b) ntdll exports begin with ENDBR64
 *   (c) CPU supports CET (XCR0 bit 11)
 *   All three → IBT is almost certainly enforced.
 * ──────────────────────────────────────────────────────────────── */

bool umf_is_endbr64(const uint8_t* code) {
    /* ENDBR64 = F3 0F 1E FA */
    return code[0] == 0xF3 && code[1] == 0x0F &&
           code[2] == 0x1E && code[3] == 0xFA;
}

bool umf_pe_has_cet_compat(HMODULE module) {
    uint8_t* base = (uint8_t*)module;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_DATA_DIRECTORY* dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dd->VirtualAddress == 0 || dd->Size == 0) return false;

    IMAGE_DEBUG_DIRECTORY* entries =
        (IMAGE_DEBUG_DIRECTORY*)(base + dd->VirtualAddress);
    DWORD count = dd->Size / sizeof(IMAGE_DEBUG_DIRECTORY);

    for (DWORD i = 0; i < count; i++) {
        /* IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS = 20 */
        if (entries[i].Type == 20) {
            uint32_t ex_chars = 0;
            if (entries[i].AddressOfRawData != 0) {
                memcpy(&ex_chars,
                       base + entries[i].AddressOfRawData,
                       sizeof(uint32_t));
            }
            /* IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT = 0x0001 */
            return (ex_chars & 0x0001) != 0;
        }
    }
    return false;
}

static void detect_cet(UmfMitigationStatus* m) {
    /* ── Shadow Stack ── */
    /* ProcessUserShadowStackPolicy = 13 (may not be in older SDK headers) */
    typedef struct {
        union {
            DWORD Flags;
            struct {
                DWORD EnableUserShadowStack          : 1;
                DWORD AuditUserShadowStack           : 1;
                DWORD SetContextIpValidationPolicy   : 1;
                DWORD AuditContextIpValidation       : 1;
                DWORD EnableUserShadowStackStrictMode: 1;
                DWORD BlockNonCetBinaries            : 1;
                DWORD BlockNonCetBinariesNonEhcont   : 1;
                DWORD AuditBlockNonCetBinaries       : 1;
                DWORD ReservedFlags                  : 24;
            };
        };
    } MY_PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY;

    MY_PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY ss;
    memset(&ss, 0, sizeof(ss));

    if (GetProcessMitigationPolicy(GetCurrentProcess(),
            (PROCESS_MITIGATION_POLICY)13, /* ProcessUserShadowStackPolicy */
            &ss, sizeof(ss))) {
        m->cet_shadow_stack = (ss.EnableUserShadowStack != 0);
    }

    /* ── IBT (heuristic) ── */

    /* (a) Main executable has CET_COMPAT flag */
    bool exe_compat = umf_pe_has_cet_compat(GetModuleHandleW(NULL));

    /* (b) ntdll exports start with ENDBR64 */
    bool ntdll_endbr = false;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        const char* test_fns[] = {
            "NtClose", "NtCreateFile", "RtlAllocateHeap", NULL
        };
        int found = 0, endbr = 0;
        for (int i = 0; test_fns[i]; i++) {
            FARPROC fn = GetProcAddress(ntdll, test_fns[i]);
            if (fn) {
                found++;
                if (umf_is_endbr64((const uint8_t*)fn))
                    endbr++;
            }
        }
        ntdll_endbr = (found > 0 && endbr == found);
    }

    /* (c) CPU supports CET (XCR0 bit 11 = CET user-mode state) */
    bool cpu_cet = false;
    /* PF_XSAVE_ENABLED = 17 */
    if (IsProcessorFeaturePresent(17)) {
        unsigned long long xcr0 = _xgetbv(0);
        cpu_cet = (xcr0 & (1ULL << 11)) != 0;
    }

    m->cet_ibt = (exe_compat && ntdll_endbr && cpu_cet);
}

/* ════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════ */

void umf_detect_all_mitigations(UmfMitigationStatus* out) {
    memset(out, 0, sizeof(*out));
    detect_acg(out);
    detect_hvci(out);
    detect_cfg(out);
    detect_cet(out);

    UMF_INFO("=== Mitigation Status ===");
    UMF_INFO("  ACG enforced:     %s%s",
             out->acg_enforced ? "YES" : "no",
             out->acg_audit_only ? " (audit)" : "");
    UMF_INFO("  HVCI active:      %s", out->hvci_active   ? "YES" : "no");
    UMF_INFO("  CFG enforced:     %s", out->cfg_enforced   ? "YES" : "no");
    UMF_INFO("  CET Shadow Stack: %s", out->cet_shadow_stack ? "YES" : "no");
    UMF_INFO("  CET IBT:          %s", out->cet_ibt        ? "YES" : "no");

    if (out->acg_enforced)
        UMF_WARN("ACG is active — only IAT/EAT/hardware-BP hooks available. "
                 "Inline/gap/vtable hooks will fail.");
    if (out->hvci_active)
        UMF_WARN("HVCI is active — cannot modify existing code pages. "
                 "XFG hash patching and ENDBR64 preservation disabled.");
}

bool umf_can_write_code_page(const UmfMitigationStatus* m) {
    return !m->hvci_active;
}

UmfHookStrategyMask umf_viable_strategies(const UmfMitigationStatus* m) {
    UmfHookStrategyMask mask = 0;

    if (!m->acg_enforced) {
        mask |= UMF_STRAT_INLINE | UMF_STRAT_GAP | UMF_STRAT_VTABLE;
    }

    /* IAT/EAT hooks modify data sections (import/export tables).
     * Hardware BP hooks use debug registers. None require executable
     * memory allocation. All work under ACG and HVCI. */
    mask |= UMF_STRAT_IAT | UMF_STRAT_EAT | UMF_STRAT_HARDWARE_BP;

    return mask;
}
