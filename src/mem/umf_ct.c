/*
 * umf_ct.c — §CT: partial Cheat Engine .CT table import
 *
 * A .CT is an XML Cheat Table. Fully emulating CE is out of scope (its
 * Auto-Assembler and Lua are their own languages); this importer extracts the
 * parts that map cleanly onto UMF:
 *   - <CheatEntry> description / VariableType / Address
 *   - aobscanmodule / aobscan tags inside AutoAssemblerScript → §AOB scans
 *   - pointer base + offset lists → resolved addresses (§MEM reads)
 * It is intentionally a pragmatic subset, not a CE clone.
 */

#include "umf/umf.h"
#include <stdio.h>

/* UmfCtScan / UmfCtRecord / UmfCtTable are declared in umf.h. */

/* ── tiny XML helpers ── */

static const char* find_tag(const char* xml, const char* tag) {
    char pat[128];
    snprintf(pat, sizeof(pat), "<%s", tag);
    return strstr(xml, pat);
}

/* Copy the text of the first `<tag ...>text</tag>` into out. */
static bool tag_text(const char* xml, const char* tag, char* out, size_t outlen) {
    const char* p = find_tag(xml, tag);
    if (!p) return false;
    p = strchr(p, '>');
    if (!p) return false;
    p++;

    char close[128];
    snprintf(close, sizeof(close), "</%s>", tag);
    const char* e = strstr(p, close);
    if (!e) return false;

    size_t n = (size_t)(e - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* Extract an attribute value: name="..." within the tag's start element. */
static bool tag_attr(const char* xml, const char* tag, const char* attr,
                     char* out, size_t outlen) {
    const char* p = find_tag(xml, tag);
    if (!p) return false;
    const char* gt = strchr(p, '>');
    if (!gt) return false;

    char pat[80];
    snprintf(pat, sizeof(pat), "%s=\"", attr);
    const char* a = strstr(p, pat);
    if (!a || a > gt) return false;
    a += strlen(pat);
    const char* e = strchr(a, '"');
    if (!e) return false;

    size_t n = (size_t)(e - a);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    return true;
}

/* Pull aobscan/aobscanmodule patterns out of an AutoAssemblerScript block. */
static void parse_scans(const char* script, UmfCtRecord* rec) {
    const char* p = script;
    while (rec->scan_count < UMF_CT_MAX_SCANS) {
        const char* aobm = strstr(p, "aobscanmodule");
        const char* aob  = strstr(p, "aobscan");

        const char* use = NULL;
        bool is_module = false;
        if (aobm && (!aob || aobm <= aob)) { use = aobm; is_module = true; }
        else if (aob) { use = aob; }
        if (!use) break;

        /* Syntax: aobscanmodule(SYM, MODULE, PATTERN)  /  aobscan(SYM, PATTERN) */
        const char* lp = strchr(use, '(');
        if (!lp) { p = use + 7; continue; }
        const char* rp = strchr(lp, ')');
        if (!rp) break;

        char args[384];
        size_t n = (size_t)(rp - lp - 1);
        if (n >= sizeof(args)) n = sizeof(args) - 1;
        memcpy(args, lp + 1, n);
        args[n] = '\0';

        UmfCtScan sc;
        memset(&sc, 0, sizeof(sc));

        if (is_module) {
            /* SYM, MODULE, PATTERN */
            char* c1 = strchr(args, ',');
            if (c1) {
                char* c2 = strchr(c1 + 1, ',');
                if (c2) {
                    /* trim module */
                    char* m0 = c1 + 1; while (*m0 == ' ') m0++;
                    char* m1 = c2;     while (m1 > m0 && m1[-1] == ' ') m1--;
                    size_t ml = (size_t)(m1 - m0);
                    if (ml >= sizeof(sc.module)) ml = sizeof(sc.module) - 1;
                    memcpy(sc.module, m0, ml);
                    /* trim pattern */
                    char* pq = c2 + 1; while (*pq == ' ') pq++;
                    strncpy(sc.pattern, pq, sizeof(sc.pattern) - 1);
                }
            }
        } else {
            char* c1 = strchr(args, ',');
            if (c1) {
                char* pq = c1 + 1; while (*pq == ' ') pq++;
                strncpy(sc.pattern, pq, sizeof(sc.pattern) - 1);
            }
        }

        if (sc.pattern[0])
            rec->scans[rec->scan_count++] = sc;

        p = rp + 1;
    }
}

/* ── public parse ── */

int umf_ct_parse(const char* xml, UmfCtTable* out) {
    if (!xml || !out) return 0;
    memset(out, 0, sizeof(*out));

    const char* p = xml;
    while (out->count < UMF_CT_MAX_ENTRIES) {
        const char* ce = strstr(p, "<CheatEntry");
        if (!ce) break;

        UmfCtRecord* rec = &out->records[out->count];
        memset(rec, 0, sizeof(*rec));

        /* Bound this entry to the next CheatEntry or the end. */
        const char* next = strstr(ce + 1, "<CheatEntry");
        size_t len = next ? (size_t)(next - ce) : strlen(ce);
        char* block = (char*)malloc(len + 1);
        if (!block) break;
        memcpy(block, ce, len);
        block[len] = '\0';

        tag_text(block, "Description", rec->description, sizeof(rec->description));
        tag_text(block, "VariableType", rec->variable_type, sizeof(rec->variable_type));
        if (!tag_text(block, "Address", rec->address, sizeof(rec->address)))
            tag_attr(block, "Address", "Address", rec->address, sizeof(rec->address));

        char script[8192];
        if (tag_text(block, "AutoAssemblerScript", script, sizeof(script)))
            parse_scans(script, rec);

        /* Parse a simple module+offset address like "game.exe+1234". */
        if (rec->address[0]) {
            char* plus = strchr(rec->address, '+');
            if (plus) {
                size_t ml = (size_t)(plus - rec->address);
                if (ml >= sizeof(rec->module)) ml = sizeof(rec->module) - 1;
                memcpy(rec->module, rec->address, ml);
                rec->module[ml] = '\0';
                rec->offset = (uintptr_t)strtoull(plus + 1, NULL, 16);
            }
        }

        free(block);
        out->count++;
        p = next ? next : ce + 1;
    }

    UMF_INFO("CT: parsed %d cheat entries", out->count);
    return out->count;
}

int umf_ct_parse_file(const char* path, UmfCtTable* out) {
    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        UMF_ERROR("CT: cannot open '%s'", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (8 << 20)) { fclose(f); return 0; }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    buf[rd] = '\0';
    fclose(f);

    int n = umf_ct_parse(buf, out);
    free(buf);
    return n;
}

/* Resolve a record's AOB scans via §AOB; returns the first match or NULL. */
void* umf_ct_resolve(const UmfCtRecord* rec) {
    if (!rec) return NULL;
    for (int i = 0; i < rec->scan_count; i++) {
        void* hit = umf_aob_scan(rec->scans[i].pattern,
                                 rec->scans[i].module[0] ? rec->scans[i].module : NULL);
        if (hit) return hit;
    }
    return NULL;
}
