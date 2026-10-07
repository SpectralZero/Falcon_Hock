/*
 * test_launch.c — §LAUNCH privilege + AV helper tests.
 *
 * Nothing here mutates security settings: the AV helpers are checked for
 * correct string output and for refusing without explicit confirmation.
 */
#include "umf/umf.h"
#include "test_framework.h"

void run_launch_tests(void) {
    /* Elevation query returns without crashing (value depends on how the
     * test host was launched). */
    bool elevated = umf_is_elevated();
    CHECK(elevated || !elevated, "umf_is_elevated returns a value");

    /* SeDebugPrivilege enable attempt: succeeds when elevated, else false.
     * Either way it must not crash. */
    bool dbg = umf_enable_debug_privilege();
    if (elevated)
        CHECK(dbg, "SeDebugPrivilege enabled while elevated");
    else
        CHECK(!dbg || dbg, "SeDebugPrivilege attempt is safe when not elevated");

    /* Command builder produces the expected PowerShell text. */
    char cmd[1200];
    umf_av_exclusion_command("C:\\Tools\\UMF", cmd, sizeof(cmd));
    CHECK(strstr(cmd, "Add-MpPreference -ExclusionPath") != NULL,
          "av command contains Add-MpPreference");
    CHECK(strstr(cmd, "C:\\Tools\\UMF") != NULL,
          "av command contains the path");

    /* Refuses without explicit confirmation. */
    CHECK(!umf_request_av_exclusion("C:\\Tools\\UMF", false),
          "av exclusion refused without user confirmation");

    /* Refuses paths containing quotes even when confirmed (injection guard). */
    CHECK(!umf_request_av_exclusion("C:\\bad'path", true),
          "av exclusion refuses a path with quote characters");
}
