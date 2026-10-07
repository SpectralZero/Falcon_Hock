/*
 * umf_manifest.c — §MOD: minimal JSON manifest parser
 *
 * Parses a flat mod.json object (string/number/array-of-string values) into
 * a UmfModManifest. Self-contained — no JSON dependency — and tolerant of
 * unknown keys (skipped). Sufficient for the mod manifest schema:
 *
 *   {
 *     "name": "...", "version": "...", "type": "native"|"lua",
 *     "entry": "...", "priority": 10,
 *     "category": "game"|"app"|"research"|"education",
 *     "audience": "beginner"|"expert",
 *     "dependencies": ["a", "b"],
 *     "capabilities": ["hook", "read_memory", ...]
 *   }
 *
 * "category" and "audience" are advisory catalogue metadata for the manager
 * and Studio; unlike capabilities they never gate what a mod may do.
 */

#include "umf/umf.h"
#include <stdio.h>

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char* parse_string(const char* p, char* buf, size_t buflen) {
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case '/': c = '/';  break;
                default:  c = e;    break;
            }
        }
        if (i + 1 < buflen) buf[i++] = c;
    }
    if (*p != '"') return NULL;
    buf[i] = '\0';
    return p + 1;
}

static const char* parse_int(const char* p, int* out) {
    char* end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return NULL;
    *out = (int)v;
    return end;
}

static const char* parse_str_array(const char* p, char (*arr)[128],
                                   int maxn, int* count) {
    *count = 0;
    p = skip_ws(p);
    if (*p != '[') return NULL;
    p = skip_ws(p + 1);
    if (*p == ']') return p + 1;

    while (*p) {
        p = skip_ws(p);
        char tmp[128];
        const char* np = parse_string(p, tmp, sizeof(tmp));
        if (!np) return NULL;
        if (*count < maxn) {
            strncpy(arr[*count], tmp, 127);
            arr[*count][127] = '\0';
            (*count)++;
        }
        p = skip_ws(np);
        if (*p == ',') { p++; continue; }
        if (*p == ']') return p + 1;
        return NULL;
    }
    return NULL;
}

/* Skip one JSON value (string/array/object/literal). Bracket counting does
 * not special-case strings, which is fine for the controlled manifest set. */
static const char* skip_value(const char* p) {
    p = skip_ws(p);
    if (*p == '"') { char t[256]; return parse_string(p, t, sizeof(t)); }
    if (*p == '[' || *p == '{') {
        char open = *p, close = (open == '[') ? ']' : '}';
        int depth = 0;
        while (*p) {
            if (*p == open) depth++;
            else if (*p == close) { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static uint32_t cap_flag(const char* name) {
    if (_stricmp(name, "hook") == 0)         return UMF_CAP_HOOK;
    if (_stricmp(name, "read_memory") == 0)  return UMF_CAP_READ_MEM;
    if (_stricmp(name, "write_memory") == 0) return UMF_CAP_WRITE_MEM;
    if (_stricmp(name, "overlay") == 0)      return UMF_CAP_OVERLAY;
    if (_stricmp(name, "file_io") == 0)      return UMF_CAP_FILE_IO;
    UMF_WARN("Unknown capability '%s' in manifest", name);
    return 0;
}

/* ── category / audience: descriptive metadata, never a gate ──
 *
 * Unknown values warn and fall back to UNSPECIFIED instead of failing the
 * manifest: a mod shipped for a newer catalogue vocabulary still loads, it
 * just carries no opinion about where it belongs. */

UmfModCategory umf_mod_category_from_string(const char* s) {
    if (!s || !s[0]) return UMF_MOD_CATEGORY_UNSPECIFIED;
    if (_stricmp(s, "game") == 0)      return UMF_MOD_CATEGORY_GAME;
    if (_stricmp(s, "app") == 0)       return UMF_MOD_CATEGORY_APP;
    if (_stricmp(s, "research") == 0)  return UMF_MOD_CATEGORY_RESEARCH;
    if (_stricmp(s, "education") == 0) return UMF_MOD_CATEGORY_EDUCATION;
    UMF_WARN("Unknown manifest category '%s' — treating as unspecified", s);
    return UMF_MOD_CATEGORY_UNSPECIFIED;
}

UmfModAudience umf_mod_audience_from_string(const char* s) {
    if (!s || !s[0]) return UMF_MOD_AUDIENCE_UNSPECIFIED;
    if (_stricmp(s, "beginner") == 0) return UMF_MOD_AUDIENCE_BEGINNER;
    if (_stricmp(s, "expert") == 0)   return UMF_MOD_AUDIENCE_EXPERT;
    UMF_WARN("Unknown manifest audience '%s' — treating as unspecified", s);
    return UMF_MOD_AUDIENCE_UNSPECIFIED;
}

const char* umf_mod_category_name(UmfModCategory c) {
    switch (c) {
        case UMF_MOD_CATEGORY_GAME:      return "game";
        case UMF_MOD_CATEGORY_APP:       return "app";
        case UMF_MOD_CATEGORY_RESEARCH:  return "research";
        case UMF_MOD_CATEGORY_EDUCATION: return "education";
        default:                         return "unspecified";
    }
}

const char* umf_mod_audience_name(UmfModAudience a) {
    switch (a) {
        case UMF_MOD_AUDIENCE_BEGINNER: return "beginner";
        case UMF_MOD_AUDIENCE_EXPERT:   return "expert";
        default:                        return "unspecified";
    }
}

bool umf_parse_manifest_string(const char* json, UmfModManifest* out) {
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));
    out->type = UMF_MOD_NATIVE;

    const char* p = skip_ws(json);
    if (*p != '{') return false;
    p++;

    for (;;) {
        p = skip_ws(p);
        if (*p == '}') { p++; break; }

        char key[64];
        p = parse_string(p, key, sizeof(key));
        if (!p) return false;
        p = skip_ws(p);
        if (*p != ':') return false;
        p = skip_ws(p + 1);

        if (strcmp(key, "name") == 0) {
            p = parse_string(p, out->name, sizeof(out->name));
        } else if (strcmp(key, "version") == 0) {
            p = parse_string(p, out->version, sizeof(out->version));
        } else if (strcmp(key, "entry") == 0) {
            p = parse_string(p, out->entry, sizeof(out->entry));
        } else if (strcmp(key, "type") == 0) {
            char t[32];
            p = parse_string(p, t, sizeof(t));
            if (p) out->type = (_stricmp(t, "lua") == 0) ? UMF_MOD_LUA
                                                         : UMF_MOD_NATIVE;
        } else if (strcmp(key, "priority") == 0) {
            p = parse_int(p, &out->priority);
        } else if (strcmp(key, "category") == 0) {
            char t[32];
            p = parse_string(p, t, sizeof(t));
            if (p) out->category = umf_mod_category_from_string(t);
        } else if (strcmp(key, "audience") == 0) {
            char t[32];
            p = parse_string(p, t, sizeof(t));
            if (p) out->audience = umf_mod_audience_from_string(t);
        } else if (strcmp(key, "dependencies") == 0) {
            p = parse_str_array(p, out->deps, UMF_MOD_MAX_DEPS, &out->dep_count);
        } else if (strcmp(key, "capabilities") == 0) {
            char caps[16][128];
            int n = 0;
            p = parse_str_array(p, caps, 16, &n);
            if (p) for (int i = 0; i < n; i++) out->capabilities |= cap_flag(caps[i]);
        } else {
            p = skip_value(p);
        }
        if (!p) return false;

        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return false;
    }

    if (out->name[0] == '\0' || out->entry[0] == '\0') {
        UMF_ERROR("Manifest missing required 'name' or 'entry'");
        return false;
    }
    return true;
}

bool umf_parse_manifest_file(const char* path, UmfModManifest* out) {
    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        UMF_ERROR("Cannot open manifest '%s'", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (1 << 20)) { fclose(f); return false; }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    buf[rd] = '\0';
    fclose(f);

    bool ok = umf_parse_manifest_string(buf, out);
    free(buf);
    return ok;
}
