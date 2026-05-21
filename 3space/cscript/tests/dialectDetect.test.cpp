// Open Siege spec 15/05 — dialect detector unit tests.
//
// Drives studio::content::cscript::TorqueScript::detectDialect() against
// canned inputs that mirror the shapes found in scripts.vol (.cs) and
// ted.vol (dialect-B). No Catch2 / GoogleTest dependency — keeps the
// test runnable while the broader VM is still un-built.
//
// Build target: cscript_dialect_test (EXCLUDE_FROM_ALL).
// Run:          ctest --test-dir build -R dialect    (after enable_testing)
//          or:  ./build/cscript/tests/cscript_dialect_test

#include "console/torquescript/dialectDetect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using studio::content::cscript::TorqueScript::CScriptDialect;
using studio::content::cscript::TorqueScript::detectDialect;

static int g_failures = 0;

#define CHECK(expr)                                                    \
    do {                                                               \
        if (!(expr)) {                                                 \
            std::fprintf(stderr, "FAIL [%s:%d] %s\n",                  \
                         __FILE__, __LINE__, #expr);                   \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

#define CHECK_DIALECT(src, expected)                                   \
    do {                                                               \
        auto got = detectDialect((src), 0);                            \
        if (got != (expected)) {                                       \
            std::fprintf(stderr,                                       \
                "FAIL detectDialect: input=<<<%.50s>>> "               \
                "expected=%s got=%s\n",                                \
                (src),                                                 \
                (expected) == CScriptDialect::A ? "A" : "B",           \
                got == CScriptDialect::A ? "A" : "B");                 \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

int main()
{
    // ---- Dialect-A canonical opening forms ----
    CHECK_DIALECT("function foo() { return 1; }\n", CScriptDialect::A);
    CHECK_DIALECT("instant SimGroup \"M\" { ... };\n", CScriptDialect::A);
    CHECK_DIALECT("datablock ItemData(Foo) { mass = 1; };\n", CScriptDialect::A);
    CHECK_DIALECT("new SimGroup(MissionGroup) { };\n", CScriptDialect::A);
    CHECK_DIALECT("$myvar = 42;\n", CScriptDialect::A);
    CHECK_DIALECT("%localvar = 7;\n", CScriptDialect::A);
    CHECK_DIALECT("if ($x == 1) echo(\"yes\");\n", CScriptDialect::A);

    // ---- Dialect-A with leading comments / blanks ----
    CHECK_DIALECT("// header comment\nfunction foo() {}\n", CScriptDialect::A);
    CHECK_DIALECT("\n\n\n  // ...\n$x = 1;\n", CScriptDialect::A);
    CHECK_DIALECT("/* block */\ninstant SimGroup \"M\" {};\n", CScriptDialect::A);

    // ---- Dialect-B canonical opening forms ----
    CHECK_DIALECT("if test $foo == 1\n  echo ok\nendif\n",          CScriptDialect::B);
    CHECK_DIALECT("set Var 42\n",                                   CScriptDialect::B);
    CHECK_DIALECT("newClient\nfocusClient\n",                       CScriptDialect::B);
    CHECK_DIALECT("focusServer\nsetcat missionName $1 \".mis\"\n",  CScriptDialect::B);
    CHECK_DIALECT("loadMission $missionName\n",                     CScriptDialect::B);
    CHECK_DIALECT("newToolWindow config toolbar.bmp 0\n",           CScriptDialect::B);
    CHECK_DIALECT("addToolButton config setcam 0\n",                CScriptDialect::B);
    CHECK_DIALECT("edit2Box \"Antigrow\" \"...\" iX iY a b\n",      CScriptDialect::B);

    // ---- Dialect-B with leading '#' comments and blank lines ----
    CHECK_DIALECT("# comment\n\nset Var 42\n",                      CScriptDialect::B);
    CHECK_DIALECT("\n\n# header\nfocusClient\n",                    CScriptDialect::B);

    // ---- Real-corpus byte-for-byte samples ----
    // changeMission.cs (the one shipping non-editor dialect-B file)
    CHECK_DIALECT(
        "focusServer\n"
        "setcat missionName $1 \".mis\"\n"
        "loadMission $missionName\n"
        "focusClient\n",
        CScriptDialect::B);

    // ted.vol antigrow.cs (representative editor script)
    CHECK_DIALECT(
        "edit2Box \"Antigrow\" \"Fill in the values\" \"iDetail\" \"Deviation\""
        " Ted::res Ted::res2\n"
        "if test $dlgResult != [cancel]\n"
        "   Ted::antiGrow $Ted::res $Ted::res2\n"
        "endif\n",
        CScriptDialect::B);

    // ---- Ambiguous / empty inputs default to A ----
    CHECK_DIALECT("",                          CScriptDialect::A);
    CHECK_DIALECT("\n\n   \n",                 CScriptDialect::A);
    CHECK_DIALECT("// just comments\n// more", CScriptDialect::A);
    CHECK_DIALECT("/* ... */",                 CScriptDialect::A);

    // ---- False-positive guard: identifiers that START with 'set' ----
    // dialect-A code referencing a global like $setting must not classify as B.
    CHECK_DIALECT("$setting = 42;\n",          CScriptDialect::A);
    CHECK_DIALECT("setVarTo(1, 2);\n",         CScriptDialect::A);  // bareword call w/o ' '

    // ---- Length-bound input (not null-terminated) ----
    {
        const char* src = "set Var 42 extra-data-not-part-of-script";
        // Only the first 11 bytes form the input.
        auto got = detectDialect(src, 11);
        CHECK(got == CScriptDialect::B);
    }

    if (g_failures == 0)
        std::printf("cscript_dialect_test: PASS (all %d cases)\n", 28);
    else
        std::printf("cscript_dialect_test: FAIL (%d failures)\n", g_failures);

    return g_failures == 0 ? 0 : 1;
}
