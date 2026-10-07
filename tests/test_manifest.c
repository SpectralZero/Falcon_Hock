/*
 * test_manifest.c — §MOD manifest category/audience metadata tests.
 *
 * Covers the advisory catalogue metadata: the "category" and "audience"
 * manifest keys, their string<->enum helpers, case-insensitive parsing, the
 * fail-soft path for unrecognised values, and the guarantee that this metadata
 * is descriptive only (it must not disturb capabilities or the rest of the
 * manifest).
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <stdio.h>

/* Build a minimal valid manifest with optional category/audience lines. */
static bool parse_with(const char* extra, UmfModManifest* out) {
    char json[512];
    snprintf(json, sizeof(json),
        "{\n"
        "  \"name\": \"meta_demo\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"entry\": \"meta_demo.dll\",\n"
        "%s"
        "  \"capabilities\": [\"hook\"]\n"
        "}\n",
        extra ? extra : "");
    return umf_parse_manifest_string(json, out);
}

void run_manifest_tests(void) {
    UmfModManifest m;

    /* ── the four categories ── */
    CHECK(parse_with("  \"category\": \"game\",\n", &m) &&
          m.category == UMF_MOD_CATEGORY_GAME, "category 'game' parsed");
    CHECK(parse_with("  \"category\": \"app\",\n", &m) &&
          m.category == UMF_MOD_CATEGORY_APP, "category 'app' parsed");
    CHECK(parse_with("  \"category\": \"research\",\n", &m) &&
          m.category == UMF_MOD_CATEGORY_RESEARCH, "category 'research' parsed");
    CHECK(parse_with("  \"category\": \"education\",\n", &m) &&
          m.category == UMF_MOD_CATEGORY_EDUCATION, "category 'education' parsed");

    /* ── both audiences ── */
    CHECK(parse_with("  \"audience\": \"beginner\",\n", &m) &&
          m.audience == UMF_MOD_AUDIENCE_BEGINNER, "audience 'beginner' parsed");
    CHECK(parse_with("  \"audience\": \"expert\",\n", &m) &&
          m.audience == UMF_MOD_AUDIENCE_EXPERT, "audience 'expert' parsed");

    /* ── both keys together, and the rest of the manifest is undisturbed ── */
    CHECK(parse_with("  \"category\": \"research\",\n"
                     "  \"audience\": \"expert\",\n", &m) &&
          m.category == UMF_MOD_CATEGORY_RESEARCH &&
          m.audience == UMF_MOD_AUDIENCE_EXPERT,
          "category and audience parsed together");
    CHECK(m.capabilities == UMF_CAP_HOOK,
          "metadata keys leave capabilities intact");
    CHECK(strcmp(m.name, "meta_demo") == 0 && strcmp(m.entry, "meta_demo.dll") == 0,
          "metadata keys leave name/entry intact");

    /* ── omitted keys default to unspecified ── */
    CHECK(parse_with(NULL, &m) &&
          m.category == UMF_MOD_CATEGORY_UNSPECIFIED &&
          m.audience == UMF_MOD_AUDIENCE_UNSPECIFIED,
          "absent keys default to unspecified");

    /* ── manifest spellings are case-insensitive ── */
    CHECK(parse_with("  \"category\": \"EDUCATION\",\n"
                     "  \"audience\": \"Beginner\",\n", &m) &&
          m.category == UMF_MOD_CATEGORY_EDUCATION &&
          m.audience == UMF_MOD_AUDIENCE_BEGINNER,
          "category/audience parsing is case-insensitive");

    /* ── unknown values degrade instead of failing the manifest ── */
    CHECK(parse_with("  \"category\": \"spaceship\",\n", &m),
          "unknown category still parses the manifest");
    CHECK(m.category == UMF_MOD_CATEGORY_UNSPECIFIED,
          "unknown category falls back to unspecified");
    CHECK(parse_with("  \"audience\": \"wizard\",\n", &m) &&
          m.audience == UMF_MOD_AUDIENCE_UNSPECIFIED,
          "unknown audience falls back to unspecified");

    /* ── from_string helpers ── */
    CHECK(umf_mod_category_from_string("game") == UMF_MOD_CATEGORY_GAME,
          "category_from_string maps a known value");
    CHECK(umf_mod_category_from_string(NULL) == UMF_MOD_CATEGORY_UNSPECIFIED,
          "category_from_string tolerates NULL");
    CHECK(umf_mod_category_from_string("") == UMF_MOD_CATEGORY_UNSPECIFIED,
          "category_from_string tolerates empty");
    CHECK(umf_mod_audience_from_string("expert") == UMF_MOD_AUDIENCE_EXPERT,
          "audience_from_string maps a known value");
    CHECK(umf_mod_audience_from_string(NULL) == UMF_MOD_AUDIENCE_UNSPECIFIED,
          "audience_from_string tolerates NULL");

    /* ── name helpers: stable strings, and round-trip with from_string ── */
    CHECK(strcmp(umf_mod_category_name(UMF_MOD_CATEGORY_GAME), "game") == 0 &&
          strcmp(umf_mod_category_name(UMF_MOD_CATEGORY_APP), "app") == 0 &&
          strcmp(umf_mod_category_name(UMF_MOD_CATEGORY_RESEARCH), "research") == 0 &&
          strcmp(umf_mod_category_name(UMF_MOD_CATEGORY_EDUCATION), "education") == 0,
          "category_name covers every category");
    CHECK(strcmp(umf_mod_category_name(UMF_MOD_CATEGORY_UNSPECIFIED),
                 "unspecified") == 0,
          "category_name reports unspecified");
    CHECK(strcmp(umf_mod_audience_name(UMF_MOD_AUDIENCE_BEGINNER), "beginner") == 0 &&
          strcmp(umf_mod_audience_name(UMF_MOD_AUDIENCE_EXPERT), "expert") == 0 &&
          strcmp(umf_mod_audience_name(UMF_MOD_AUDIENCE_UNSPECIFIED),
                 "unspecified") == 0,
          "audience_name covers every audience");

    bool cat_round_trip = true;
    for (int c = UMF_MOD_CATEGORY_UNSPECIFIED; c <= UMF_MOD_CATEGORY_EDUCATION; c++) {
        const char* s = umf_mod_category_name((UmfModCategory)c);
        if (!s || umf_mod_category_from_string(s) != (UmfModCategory)c)
            cat_round_trip = false;
    }
    CHECK(cat_round_trip, "every category round-trips name -> from_string");

    bool aud_round_trip = true;
    for (int a = UMF_MOD_AUDIENCE_UNSPECIFIED; a <= UMF_MOD_AUDIENCE_EXPERT; a++) {
        const char* s = umf_mod_audience_name((UmfModAudience)a);
        if (!s || umf_mod_audience_from_string(s) != (UmfModAudience)a)
            aud_round_trip = false;
    }
    CHECK(aud_round_trip, "every audience round-trips name -> from_string");

    /* An out-of-range value must still yield a usable string, not NULL. */
    CHECK(umf_mod_category_name((UmfModCategory)999) != NULL &&
          umf_mod_audience_name((UmfModAudience)999) != NULL,
          "name helpers never return NULL");
}
