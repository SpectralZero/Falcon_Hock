/*
 * umf_aob.c — §AOB: array-of-bytes (byte-pattern) scanning
 *
 * Locates code/data by signature so hooks survive module rebuilds. Patterns
 * are whitespace-separated hex tokens with '?'/'x' wildcard nibbles:
 *     "48 8B ?? 89"   full-byte wildcard
 *     "4? ?F"         nibble wildcards
 *     "E9"            single value byte (0xE9)
 * A token of one hex char is a low-nibble value byte; two chars are a full
 * byte. A byte matches when (mem & mask) == (value & mask).
 *
 * Module scans walk committed, readable regions of the module image and guard
 * every read with SEH, so an unreadable page never faults the scan.
 */

#include "umf/umf.h"

#define UMF_AOB_MAX_BYTES 256

typedef struct {
    uint8_t value[UMF_AOB_MAX_BYTES];
    uint8_t mask[UMF_AOB_MAX_BYTES];
    size_t  length;
} UmfAobPattern;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_wild(char c) { return c == '?' || c == 'x' || c == 'X'; }

static bool decode_token(const char* tok, int n, uint8_t* val, uint8_t* mask) {
    *val = 0; *mask = 0;
    if (n == 1) {
        if (is_wild(tok[0])) { *mask = 0x00; return true; }   /* "?" */
        int nb = hex_nibble(tok[0]);
        if (nb < 0) return false;
        *val = (uint8_t)nb; *mask = 0x0F;                      /* low nibble */
        return true;
    }
    /* n == 2 */
    for (int i = 0; i < 2; i++) {
        uint8_t nib_mask = (i == 0) ? 0xF0 : 0x0F;
        if (is_wild(tok[i])) continue;                         /* wild nibble */
        int nb = hex_nibble(tok[i]);
        if (nb < 0) return false;
        *mask |= nib_mask;
        *val  |= (uint8_t)((i == 0) ? (nb << 4) : nb);
    }
    return true;
}

static bool parse_pattern(const char* pattern, UmfAobPattern* out) {
    out->length = 0;
    const char* p = pattern;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        char tok[4] = {0, 0, 0, 0};
        int n = 0;
        while (*p && *p != ' ' && *p != '\t') {
            if (n < 3) tok[n] = *p;
            n++;
            p++;
        }
        if (n > 2) return false;

        if (out->length >= UMF_AOB_MAX_BYTES) return false;

        uint8_t val, mask;
        if (!decode_token(tok, n, &val, &mask)) return false;
        out->value[out->length] = val;
        out->mask[out->length]  = mask;
        out->length++;
    }
    return out->length > 0;
}

static bool match_at(const uint8_t* data, const UmfAobPattern* pat) {
    for (size_t i = 0; i < pat->length; i++) {
        if ((data[i] & pat->mask[i]) != (pat->value[i] & pat->mask[i]))
            return false;
    }
    return true;
}

/* Walk module image regions; honor first_only by returning the first hit. */
static void* scan_module(const UmfAobPattern* pat, HMODULE mod, bool first_only,
                         void** out, int max, int* out_count) {
    uint8_t* base = (uint8_t*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    uint8_t* mod_end = base + nt->OptionalHeader.SizeOfImage;
    void* first = NULL;
    int count = 0;

    for (uint8_t* p = base; p < mod_end; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) break;

        uint8_t* region_end = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        uint8_t* scan_end = region_end < mod_end ? region_end : mod_end;

        DWORD prot = mbi.Protect & 0xFF;
        bool readable = mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & PAGE_GUARD) &&
                        (prot == PAGE_READONLY  || prot == PAGE_READWRITE ||
                         prot == PAGE_EXECUTE_READ ||
                         prot == PAGE_EXECUTE_READWRITE ||
                         prot == PAGE_WRITECOPY ||
                         prot == PAGE_EXECUTE_WRITECOPY);

        if (readable) {
            size_t size = (size_t)(scan_end - p);
            if (size >= pat->length) {
                for (size_t i = 0; i + pat->length <= size; i++) {
                    bool hit;
                    __try {
                        hit = match_at(p + i, pat);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        break;   /* page unreadable mid-scan; skip region */
                    }
                    if (!hit) continue;

                    void* addr = (void*)(uintptr_t)(p + i);
                    if (!first) first = addr;
                    if (out && count < max) out[count] = addr;
                    count++;
                    if (first_only) goto done;
                    if (out && count >= max) goto done;   /* collected enough */
                }
            }
        }
        p = region_end;
    }

done:
    if (out_count) *out_count = count;
    return first;
}

void* umf_aob_scan_range(const char* pattern, const void* start, size_t size) {
    UmfAobPattern pat;
    if (!pattern || !start || !parse_pattern(pattern, &pat)) return NULL;
    if (size < pat.length) return NULL;

    const uint8_t* p = (const uint8_t*)start;
    for (size_t i = 0; i + pat.length <= size; i++) {
        bool hit;
        __try {
            hit = match_at(p + i, &pat);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return NULL;
        }
        if (hit) return (void*)(uintptr_t)(p + i);
    }
    return NULL;
}

int umf_aob_scan_all(const char* pattern, const char* module_name,
                     void** out, int max) {
    UmfAobPattern pat;
    if (!pattern || !out || max <= 0 || !parse_pattern(pattern, &pat))
        return 0;

    HMODULE mod = (module_name && module_name[0])
                ? GetModuleHandleA(module_name)
                : GetModuleHandleW(NULL);
    if (!mod) return 0;

    int count = 0;
    scan_module(&pat, mod, false, out, max, &count);
    return count;
}

void* umf_aob_scan(const char* pattern, const char* module_name) {
    UmfAobPattern pat;
    if (!pattern || !parse_pattern(pattern, &pat)) return NULL;

    HMODULE mod = (module_name && module_name[0])
                ? GetModuleHandleA(module_name)
                : GetModuleHandleW(NULL);
    if (!mod) return NULL;

    return scan_module(&pat, mod, true, NULL, 0, NULL);
}
