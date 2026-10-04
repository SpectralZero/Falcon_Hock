/*
 * test_mitigations.c — mitigation detection + strategy selection.
 *
 * Values are host-dependent, so we assert invariants rather than exact
 * flags: the detector must run, and the strategy mask must always offer
 * at least one data-only strategy.
 */
#include "umf/umf.h"
#include "test_framework.h"

void run_mitigation_tests(void) {
    UmfMitigationStatus m;
    umf_detect_all_mitigations(&m);
    CHECK(1, "umf_detect_all_mitigations ran");

    UmfHookStrategyMask mask = umf_viable_strategies(&m);
    CHECK((mask & (UMF_STRAT_IAT | UMF_STRAT_EAT | UMF_STRAT_HARDWARE_BP)) != 0,
          "IAT/EAT/HW-BP strategies always viable");

    if (!m.acg_enforced) {
        CHECK((mask & UMF_STRAT_INLINE) != 0,
              "inline/gap/vtable viable without ACG");
    } else {
        CHECK((mask & UMF_STRAT_INLINE) == 0,
              "inline suppressed under ACG");
    }

    /* ENDBR64 detector */
    uint8_t endbr[4]    = { 0xF3, 0x0F, 0x1E, 0xFA };
    uint8_t notendbr[4] = { 0x90, 0x90, 0x90, 0x90 };
    CHECK(umf_is_endbr64(endbr),     "ENDBR64 recognised");
    CHECK(!umf_is_endbr64(notendbr), "non-ENDBR64 rejected");

    /* can_write_code_page mirrors HVCI */
    CHECK(umf_can_write_code_page(&m) == !m.hvci_active,
          "can_write_code_page tracks HVCI");

    /* Windows version query works */
    UmfWindowsVersion v;
    CHECK(umf_get_windows_version(&v), "RtlGetVersion query succeeds");
    CHECK(v.major >= 6, "reported major version is sane");

    /* Strategy selector returns a viable strategy */
    UmfHookStrategy s = umf_select_strategy((void*)&run_mitigation_tests, &m);
    if (!m.acg_enforced)
        CHECK(s == UMF_HOOK_INLINE, "selector prefers inline without ACG");
    else
        CHECK(s == UMF_HOOK_IAT, "selector falls back to IAT under ACG");
}
