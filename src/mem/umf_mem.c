/*
 * umf_mem.c — §MEM: capability-gated, fault-guarded memory access
 *
 * The foundation for the scanner, trainer values, and CE-table import. Reads
 * and writes are wrapped in SEH so an unmapped or protected address returns
 * false instead of crashing the target — essential because "scan memory" and
 * "freeze a value" routinely probe addresses that come and go.
 *
 * Access is capability-gated: an owner mod set via umf_mem_set_owner() must
 * hold UMF_CAP_READ_MEM to read and UMF_CAP_WRITE_MEM to write. A NULL owner
 * is engine-internal and trusted. The owner is per-thread (TLS), so a hook
 * callback and a mod load do not fight over a single global.
 */

#include "umf/umf.h"

static __declspec(thread) UmfMod* t_mem_owner = NULL;

void umf_mem_set_owner(UmfMod* mod) { t_mem_owner = mod; }

static bool can_read(void)  {
    return !t_mem_owner || (t_mem_owner->capabilities & UMF_CAP_READ_MEM);
}
static bool can_write(void) {
    return !t_mem_owner || (t_mem_owner->capabilities & UMF_CAP_WRITE_MEM);
}

bool umf_mem_read(const void* addr, void* out, size_t size) {
    if (!addr || !out || size == 0) return false;
    if (!can_read()) {
        UMF_WARN("Memory read denied: mod '%s' lacks 'read_memory' capability",
                 t_mem_owner ? t_mem_owner->name : "?");
        return false;
    }

    __try {
        memcpy(out, addr, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool umf_mem_write(void* addr, const void* in, size_t size) {
    if (!addr || !in || size == 0) return false;
    if (!can_write()) {
        UMF_WARN("Memory write denied: mod '%s' lacks 'write_memory' capability",
                 t_mem_owner ? t_mem_owner->name : "?");
        return false;
    }

    bool ok = false;
    __try {
        DWORD old = 0;
        if (VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(addr, in, size);
            DWORD dummy;
            VirtualProtect(addr, size, old, &dummy);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

int umf_mem_enum_regions(UmfMemRegion* out, int max) {
    if (!out || max <= 0) return 0;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t limit = (uintptr_t)si.lpMaximumApplicationAddress;
    int count = 0;

    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) break;

        if (mbi.State == MEM_COMMIT) {
            if (count < max) {
                out[count].base    = (uintptr_t)mbi.BaseAddress;
                out[count].size    = mbi.RegionSize;
                out[count].protect = mbi.Protect;
                out[count].state   = mbi.State;
                out[count].type    = mbi.Type;
            }
            count++;
        }

        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;   /* guard against wrap / no progress */
        addr = next;
    }
    return count < max ? count : max;
}

bool umf_mem_query(const void* addr, UmfMemRegion* out) {
    if (!addr || !out) return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;

    out->base    = (uintptr_t)mbi.BaseAddress;
    out->size    = mbi.RegionSize;
    out->protect = mbi.Protect;
    out->state   = mbi.State;
    out->type    = mbi.Type;
    return true;
}
