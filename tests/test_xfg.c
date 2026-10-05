/*
 * test_xfg.c — §XFG detection + safe hash-copy tests.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <stdint.h>

void run_xfg_tests(void) {
    /* Our modules are not built with /guard:xfg → detection returns false. */
    HMODULE self = GetModuleHandleW(NULL);
    CHECK(!umf_module_has_xfg(self),
          "host EXE reports no XFG (not /guard:xfg compiled)");
    CHECK(!umf_module_has_xfg(GetModuleHandleW(L"ntdll.dll")),
          "ntdll reports no XFG");

    /* Safe hash copy inside a shared writable page. */
    static uint8_t buf[8192 + 64];
    uint8_t* page = (uint8_t*)(((uintptr_t)buf + 0xFFF) & ~(uintptr_t)0xFFF);
    uint8_t* target = page + 0x20;
    uint8_t* hook   = page + 0x80;

    uint64_t magic = 0x1122334455667788ULL;
    memcpy(target - 8, &magic, 8);

    bool did = false;
    CHECK(umf_xfg_copy_hash(target, hook, &did), "copy hash (same page)");
    CHECK(did, "hash copy reports it wrote");
    uint64_t got = 0;
    memcpy(&got, hook - 8, 8);
    CHECK(got == magic, "hash landed on the hook's -8 slot");

    /* Unsafe: hook at the very start of a page → -8 crosses into the prior
     * page. Must refuse and write nothing. */
    uint8_t* page_start = page + 0x1000;   /* next page boundary */
    uint64_t canary = 0xDEADBEEFCAFEBABEULL;
    memcpy(page_start - 8, &canary, 8);
    bool did2 = true;
    CHECK(!umf_xfg_copy_hash(target, page_start, &did2),
          "refuse hash copy across page boundary");
    CHECK(!did2, "refused copy wrote nothing");
    uint64_t after = 0;
    memcpy(&after, page_start - 8, 8);
    CHECK(after == canary, "preceding page left untouched");
}
