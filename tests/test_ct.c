/*
 * test_ct.c — §CT partial Cheat Engine table import tests.
 *
 * Parses a representative .CT snippet, checks records/addresses/AOB patterns,
 * and resolves an AOB scan against the module's own marker bytes.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <stdint.h>

static const uint8_t kCtMarker[6] = { 0x90, 0x8B, 0x45, 0xFC, 0xC3, 0xCC };

static const char* kCtXml =
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<CheatTable>\n"
"  <CheatEntries>\n"
"    <CheatEntry>\n"
"      <ID>1</ID>\n"
"      <Description>\"Infinite Health\"</Description>\n"
"      <VariableType>4 Bytes</VariableType>\n"
"      <Address>game.exe+1A2B3C</Address>\n"
"      <AutoAssemblerScript>{ aobscanmodule(HP, game.exe, 90 8B 45 FC C3 CC) }</AutoAssemblerScript>\n"
"    </CheatEntry>\n"
"    <CheatEntry>\n"
"      <ID>2</ID>\n"
"      <Description>\"Ammo\"</Description>\n"
"      <VariableType>Float</VariableType>\n"
"      <Address>0x7FF600001234</Address>\n"
"    </CheatEntry>\n"
"  </CheatEntries>\n"
"</CheatTable>\n";

void run_ct_tests(void) {
    UmfCtTable table;
    int n = umf_ct_parse(kCtXml, &table);
    CHECK(n == 2, "parsed two cheat entries");

    CHECK(strstr(table.records[0].description, "Infinite Health") != NULL,
          "record 1 description parsed");
    CHECK(strcmp(table.records[0].module, "game.exe") == 0,
          "record 1 module parsed from address");
    CHECK(table.records[0].offset == 0x1A2B3C,
          "record 1 offset parsed");
    CHECK(table.records[0].scan_count == 1,
          "record 1 aobscan extracted");
    CHECK(strcmp(table.records[0].scans[0].pattern,
                 "90 8B 45 FC C3 CC") == 0,
          "aobscan pattern captured");
    CHECK(strcmp(table.records[0].scans[0].module, "game.exe") == 0,
          "aobscan module captured");

    CHECK(strcmp(table.records[1].variable_type, "Float") == 0,
          "record 2 variable type parsed");
    CHECK(table.records[1].scan_count == 0,
          "record 2 has no scans");

    /* Resolution: the record's pattern matches our own marker in this module
     * (scan the main module by passing NULL module). */
    volatile uint8_t sink = kCtMarker[0];
    (void)sink;
    void* hit = umf_aob_scan("90 8B 45 FC C3 CC", NULL);
    CHECK(hit == (void*)kCtMarker, "ct pattern resolves to the marker address");

    /* umf_ct_resolve honors the scan's module: "game.exe" isn't loaded here, so
     * it must fail closed rather than fall back to the main module. */
    CHECK(umf_ct_resolve(&table.records[0]) == NULL,
          "ct resolve returns NULL for an unloaded module");

    /* Clearing the module scopes the scan to the main image, where the marker
     * lives — this exercises the real resolve path end to end. */
    table.records[0].scans[0].module[0] = '\0';
    CHECK(umf_ct_resolve(&table.records[0]) == (void*)kCtMarker,
          "ct resolve finds the marker via the record's pattern");

    /* A record with no scans has nothing to resolve. */
    CHECK(umf_ct_resolve(&table.records[1]) == NULL,
          "ct resolve returns NULL when the record has no scans");
}
