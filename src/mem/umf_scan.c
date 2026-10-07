/*
 * umf_scan.c — §SCAN: value scanner (first/next scan)
 *
 * A Cheat-Engine-style scanner on top of the §MEM layer: find a value, take
 * an action in the app, then narrow the candidate set. Values are read
 * through umf_mem_read (fault-guarded, capability-gated), and only committed
 * readable regions are scanned. Alignment follows the value width so a
 * 4-byte int is only matched on 4-byte boundaries.
 */

#include "umf/umf.h"

#define UMF_SCAN_MAX_RESULTS (1 << 20)   /* cap: 1M addresses */

struct UmfScanSession {
    UmfScanType type;
    size_t      width;
    void**      addrs;
    int         count;
    int         capacity;
};

static size_t type_width(UmfScanType t) {
    switch (t) {
        case UMF_SCAN_I8:  return 1;
        case UMF_SCAN_I16: return 2;
        case UMF_SCAN_I32: return 4;
        case UMF_SCAN_I64: return 8;
        case UMF_SCAN_F32: return 4;
        case UMF_SCAN_F64: return 8;
    }
    return 4;
}

/* Read the value at p (as `type`) into a double. Returns false on failure. */
static bool read_value(UmfScanType type, const void* p, double* out) {
    switch (type) {
        case UMF_SCAN_I8:  { int8_t  v; if (!umf_mem_read(p, &v, 1)) return false; *out = v; return true; }
        case UMF_SCAN_I16: { int16_t v; if (!umf_mem_read(p, &v, 2)) return false; *out = v; return true; }
        case UMF_SCAN_I32: { int32_t v; if (!umf_mem_read(p, &v, 4)) return false; *out = v; return true; }
        case UMF_SCAN_I64: { int64_t v; if (!umf_mem_read(p, &v, 8)) return false; *out = (double)v; return true; }
        case UMF_SCAN_F32: { float   v; if (!umf_mem_read(p, &v, 4)) return false; *out = v; return true; }
        case UMF_SCAN_F64: { double  v; if (!umf_mem_read(p, &v, 8)) return false; *out = v; return true; }
    }
    return false;
}

static bool value_matches(UmfScanCompare cmp, double cur, double want) {
    switch (cmp) {
        case UMF_SCAN_EXACT:     return cur == want;
        case UMF_SCAN_CHANGED:   return cur != want;
        case UMF_SCAN_INCREASED: return cur >  want;
        case UMF_SCAN_DECREASED: return cur <  want;
    }
    return false;
}

static bool region_scan_ok(uint32_t protect) {
    DWORD p = protect & 0xFF;
    return !(protect & PAGE_GUARD) &&
           (p == PAGE_READONLY || p == PAGE_READWRITE ||
            p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
            p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY);
}

static void session_push(UmfScanSession* s, void* addr) {
    if (s->count >= UMF_SCAN_MAX_RESULTS) return;
    if (s->count >= s->capacity) {
        int cap = s->capacity ? s->capacity * 2 : 1024;
        if (cap > UMF_SCAN_MAX_RESULTS) cap = UMF_SCAN_MAX_RESULTS;
        void** na = (void**)realloc(s->addrs, (size_t)cap * sizeof(void*));
        if (!na) return;
        s->addrs = na;
        s->capacity = cap;
    }
    s->addrs[s->count++] = addr;
}

UmfScanSession* umf_scan_first(UmfScanType type, double value) {
    UmfScanSession* s = (UmfScanSession*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->type  = type;
    s->width = type_width(type);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t limit = (uintptr_t)si.lpMaximumApplicationAddress;

    while (addr < limit && s->count < UMF_SCAN_MAX_RESULTS) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) break;

        uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && region_scan_ok(mbi.Protect) &&
            mbi.RegionSize <= (64ULL << 20)) {   /* skip giant regions */
            uint8_t* base = (uint8_t*)mbi.BaseAddress;
            for (size_t off = 0; off + s->width <= mbi.RegionSize;
                 off += s->width) {
                void* p = base + off;
                /* Align to the value width within the region. */
                if (((uintptr_t)p % s->width) != 0) continue;

                double cur;
                if (!read_value(s->type, p, &cur)) continue;
                if (cur == value) session_push(s, p);
                if (s->count >= UMF_SCAN_MAX_RESULTS) break;
            }
        }

        if (region_end <= addr) break;
        addr = region_end;
    }

    UMF_INFO("Scan: %d result(s) for %.6g (type %d)", s->count, value, type);
    return s;
}

int umf_scan_next(UmfScanSession* s, UmfScanCompare compare, double value) {
    if (!s) return 0;

    int kept = 0;
    for (int i = 0; i < s->count; i++) {
        double cur;
        if (!read_value(s->type, s->addrs[i], &cur)) continue;   /* unreadable → drop */
        if (value_matches(compare, cur, value))
            s->addrs[kept++] = s->addrs[i];
    }
    s->count = kept;
    UMF_INFO("Scan next: %d result(s) remain", kept);
    return kept;
}

int umf_scan_results(UmfScanSession* s, void** out, int max) {
    if (!s || !out || max <= 0) return 0;
    int n = s->count < max ? s->count : max;
    for (int i = 0; i < n; i++) out[i] = s->addrs[i];
    return n;
}

int umf_scan_count(UmfScanSession* s) { return s ? s->count : 0; }

bool umf_scan_value_at(UmfScanSession* s, int i, double* out) {
    if (!s || !out || i < 0 || i >= s->count) return false;
    return read_value(s->type, s->addrs[i], out);
}

void umf_scan_free(UmfScanSession* s) {
    if (!s) return;
    free(s->addrs);
    free(s);
}
