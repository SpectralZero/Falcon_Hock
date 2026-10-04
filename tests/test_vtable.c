/*
 * test_vtable.c — §VTABLE hooking test.
 *
 * Simulates a C++ object (first member = vtable pointer), hooks a virtual
 * slot, verifies dispatch routes through the hook, the original is callable,
 * and unhook restores the slot.
 */
#include "umf/umf.h"
#include "test_framework.h"

typedef int (*vfn)(void* self, int x);

static int   v_impl(void* self, int x) { (void)self; return x + 1; }
static vfn   g_orig_v = NULL;
static int   v_hook(void* self, int x) { return g_orig_v(self, x) * 10; }

/* Virtual dispatch: always read the current slot through the object. */
static int call_slot1(void* obj, int x) {
    void** vt = *(void***)obj;
    return ((vfn)vt[1])(obj, x);
}

void run_vtable_tests(void) {
    static void* vtable[3];
    vtable[0] = (void*)0;              /* e.g. destructor */
    vtable[1] = (void*)&v_impl;        /* the method under test */
    vtable[2] = (void*)0;

    struct { void** vptr; } obj;
    obj.vptr = vtable;

    CHECK(call_slot1(&obj, 5) == 6, "vtable baseline (5 + 1)");

    void* orig = NULL;
    UmfVtableLocation loc;
    bool hooked = umf_hook_vtable_object(&obj, 1, (void*)&v_hook, &orig, &loc);
    CHECK(hooked, "hook vtable slot 1 via object");
    g_orig_v = (vfn)orig;

    CHECK(call_slot1(&obj, 5) == 60, "dispatch routes through hook ((5+1)*10)");
    CHECK(g_orig_v(&obj, 5) == 6, "original callable via saved pointer");

    CHECK(umf_unhook_vtable(&loc), "unhook vtable slot");
    CHECK(call_slot1(&obj, 5) == 6, "vtable slot restored");
}
