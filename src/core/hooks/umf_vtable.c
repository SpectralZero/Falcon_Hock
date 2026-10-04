/*
 * umf_vtable.c — §VTABLE: C++ virtual-method hooking
 *
 * Many C++ methods are too small to inline-hook or are reached only through
 * a vtable; swapping the vtable slot is the clean path. This is a data-only
 * edit (a pointer-sized aligned store, atomic on x64), so no trampoline or
 * thread freeze is required.
 *
 * The hook is class-wide: every instance sharing the vtable is affected. The
 * hook function receives the object pointer as its first argument (x64 RCX),
 * identical to the virtual method's own signature, and calls `original` to
 * invoke the real method.
 */

#include "umf/umf.h"

bool umf_hook_vtable(void** vtable, int index, void* hook,
                     void** out_original, UmfVtableLocation* out_loc) {
    if (!vtable || index < 0 || !hook) return false;

    void** slot = &vtable[index];
    void*  original = *slot;

    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        UMF_ERROR("VirtualProtect vtable slot %p failed: %lu",
                  (void*)slot, GetLastError());
        return false;
    }
    *slot = hook;                      /* aligned pointer store — atomic */
    VirtualProtect(slot, sizeof(void*), old, &old);

    if (out_original) *out_original = original;
    if (out_loc) {
        out_loc->vtable   = vtable;
        out_loc->index    = index;
        out_loc->original = original;
    }

    UMF_INFO("Vtable hook: [%d] in %p (%p -> %p)",
             index, (void*)vtable, original, hook);
    return true;
}

bool umf_hook_vtable_object(void* object, int index, void* hook,
                            void** out_original, UmfVtableLocation* out_loc) {
    if (!object) return false;
    void** vtable = *(void***)object;  /* first member is the vtable pointer */
    return umf_hook_vtable(vtable, index, hook, out_original, out_loc);
}

bool umf_unhook_vtable(const UmfVtableLocation* loc) {
    if (!loc || !loc->vtable) return false;

    void** slot = &loc->vtable[loc->index];
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = loc->original;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}
