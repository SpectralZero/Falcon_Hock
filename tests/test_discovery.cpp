/*
 * test_discovery.cpp — §DISCOVERY: RTTI reader + dbghelp symbol resolver.
 *
 * C++ so we can define genuine polymorphic classes (and therefore real MSVC
 * RTTI) and take addresses of named functions for the symbol round-trip.
 */
#include "umf/umf.h"
#include "test_framework.h"

/* ── Test fixtures: polymorphic classes so the compiler emits vtables+RTTI ── */
namespace umftest {
    struct Shape {
        virtual int  area() const { return 0; }
        virtual ~Shape() {}
    };
    struct Circle : Shape {
        int r;
        explicit Circle(int rr) : r(rr) {}
        int area() const override { return r * r * 3; }
    };
}

class UmfWidget {
public:
    virtual void on_a() {}
    virtual void on_b() {}
    virtual ~UmfWidget() {}
};

/* A named, non-inlined, C-linkage function for the symbol round-trip. */
extern "C" __declspec(noinline) void umf_discovery_sym_marker(void) {
    volatile int x = 0;
    (void)x;
}

extern "C" void run_discovery_tests(void) {
    /* ── RTTI: from a concrete object ── */
    UmfWidget widget;
    UmfRttiClass rc;
    bool okw = umf_rtti_from_object(&widget, &rc);
    CHECK(okw, "rtti_from_object resolves a global class");
    CHECK(okw && strcmp(rc.name, "UmfWidget") == 0, "rtti class name is UmfWidget");
    CHECK(okw && strstr(rc.raw_name, "UmfWidget") != NULL,
          "rtti raw name retains the mangled form");
    CHECK(okw && rc.vfunc_count >= 2, "rtti counts the declared virtuals");
    CHECK(okw && rc.image_base == (uintptr_t)GetModuleHandleW(NULL),
          "rtti recovers the main module base");

    /* ── RTTI: namespaced class reached through a base pointer ── */
    umftest::Circle circle(4);
    umftest::Shape* sp = &circle;        /* static Shape*, dynamic Circle */
    UmfRttiClass rc2;
    bool okc = umf_rtti_from_object(sp, &rc2);
    CHECK(okc, "rtti_from_object resolves through a base pointer");
    CHECK(okc && strcmp(rc2.name, "umftest::Circle") == 0,
          "rtti demangles the namespace scope");

    /* ── RTTI: from the vtable directly matches the object path ── */
    void** vt = *(void***)&widget;
    UmfRttiClass rc3;
    bool okv = umf_rtti_from_vtable(vt, &rc3);
    CHECK(okv && strcmp(rc3.name, "UmfWidget") == 0,
          "rtti_from_vtable matches the object path");

    /* ── RTTI: non-objects are rejected, never crash ── */
    int plain = 1234;
    UmfRttiClass rc4;
    CHECK(!umf_rtti_from_object(&plain, &rc4), "rtti rejects a non-object pointer");
    CHECK(!umf_rtti_from_vtable((void**)&plain, &rc4), "rtti rejects a junk vtable");

    /* ── RTTI: demangle unit cases ── */
    char dbuf[128];
    umf_rtti_demangle(".?AVFoo@@", dbuf, sizeof(dbuf));
    CHECK(strcmp(dbuf, "Foo") == 0, "demangle a simple class");
    umf_rtti_demangle(".?AVBar@ns@@", dbuf, sizeof(dbuf));
    CHECK(strcmp(dbuf, "ns::Bar") == 0, "demangle one namespace scope");
    umf_rtti_demangle("garbage", dbuf, sizeof(dbuf));
    CHECK(dbuf[0] == 0, "demangle rejects non-RTTI input");

    /* ── RTTI: module scan finds our instantiated class ── */
    static UmfRttiClass classes[512];
    int n = umf_rtti_scan_module(NULL, classes, 512);
    CHECK(n > 0, "module scan finds vtables");
    bool found_widget = false;
    for (int i = 0; i < n && i < 512; i++)
        if (strcmp(classes[i].name, "UmfWidget") == 0) { found_widget = true; break; }
    CHECK(found_widget, "module scan lists UmfWidget");

    /* ── Symbols: init + name/address round-trip through dbghelp ── */
    bool si = umf_sym_init();
    CHECK(si, "sym_init brings up dbghelp");

    volatile void* keep = (void*)&umf_discovery_sym_marker;   /* keep the symbol */
    (void)keep;
    uintptr_t a = umf_sym_resolve("umf_discovery_sym_marker");
    CHECK(a != 0, "sym_resolve finds a known function by name");

    UmfSymbol sym;
    bool fa = umf_sym_from_address((void*)a, &sym);
    CHECK(fa, "sym_from_address resolves the function back");
    CHECK(fa && strstr(sym.name, "umf_discovery_sym_marker") != NULL,
          "sym name round-trips");
    CHECK(fa && sym.displacement == 0, "sym base address has zero displacement");

    CHECK(umf_sym_resolve("umf_no_such_symbol_zzzqqq") == 0,
          "sym_resolve misses an unknown name");

    /* ── Symbols: offline undecorate (deterministic) ── */
    char ubuf[256];
    int ul = umf_undecorate("?example@@YAHH@Z", ubuf, sizeof(ubuf));
    CHECK(ul > 0 && strstr(ubuf, "example") != NULL, "undecorate a free function");
    umf_undecorate("?method@Klass@@QEAAXXZ", ubuf, sizeof(ubuf));
    CHECK(strstr(ubuf, "Klass::method") != NULL, "undecorate a member function");

    umf_sym_cleanup();
}
