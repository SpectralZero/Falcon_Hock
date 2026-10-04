/*
 * test_log.c — logging smoke tests.
 */
#include "umf/umf.h"
#include "test_framework.h"

void run_log_tests(void) {
    /* Exercise each level — must not crash and must respect the filter. */
    umf_log_set_level(UMF_LOG_TRACE);
    UMF_TRACE("trace %d", 1);
    UMF_DEBUG("debug %d", 2);
    UMF_INFO("info %d", 3);
    UMF_WARN("warn %d", 4);
    UMF_ERROR("error %d", 5);
    CHECK(1, "all log levels emit without crashing");

    /* Level filtering: raising the threshold should not crash on filtered
     * calls (we cannot easily observe suppression without a sink, so this
     * is a smoke check). */
    umf_log_set_level(UMF_LOG_ERROR);
    UMF_INFO("this info line is below threshold");
    CHECK(1, "level filter accepts below-threshold calls");

    umf_log_set_level(UMF_LOG_WARN);
}
