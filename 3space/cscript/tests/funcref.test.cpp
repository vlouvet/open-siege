// Open Siege spec 17/04 — namespace-qualified function-reference args.
//
// Tribes-1 scripts pass function references unquoted to engine callbacks:
//   Group::iterateRecursive(MissionGroup, ObjectiveMission::initCheck);
//                                         ^^^^^^^^^^^^^^^^^^^^^^^^^^
//
// Before this spec, the cscript grammar required `(` to follow
// `Namespace::Name`, so the parser failed at the `,` (or `)`). The grammar
// now also accepts `IDENT :: IDENT` as a bare expression that yields the
// string `"Namespace::Name"`. LALR(1) lookahead means the function-call
// form `Foo::bar(arg)` still parses correctly when `(` follows.

#include "console/console.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/torquescript/runtime.h"

#include <cstdio>
#include <cstring>

using namespace studio::content::cscript;

static int gPassed = 0, gFailed = 0;
static void check(bool c, const char* d)
{
    if (c) { ++gPassed; std::printf("  pass: %s\n", d); }
    else   { ++gFailed; std::fprintf(stderr, "  FAIL: %s\n", d); }
}
static void sink(unsigned int, const char*) {}

int main()
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    Con::init();
    Sim::init();
    Con::addConsumer(sink);
    Con::registerRuntime(0, TorqueScript::getRuntime());

    // 1. Bare `Foo::bar` as a value — produces the string "Foo::bar".
    std::printf("[group] bare Foo::bar expression\n");
    Con::evaluate("$ref = ObjectiveMission::initCheck;", false, "funcref.test");
    const char* ref = Con::getVariable("ref");
    check(ref && std::strcmp(ref, "ObjectiveMission::initCheck") == 0,
          "$ref = Foo::bar -> \"Foo::bar\"");

    // 2. Pass-as-argument — script-side function takes a callback name string.
    std::printf("\n[group] function-reference as argument\n");
    Con::evaluate(
        "function captureRef(%name) { $captured = %name; }\n"
        "captureRef(Group::iterateRecursive);\n",
        false, "funcref.test");
    const char* cap = Con::getVariable("captured");
    check(cap && std::strcmp(cap, "Group::iterateRecursive") == 0,
          "captureRef(Foo::bar) reaches script with \"Foo::bar\"");

    // 3. Mixed-arg form — the real T1 idiom from objectives.cs:300.
    std::printf("\n[group] mixed: ident + function-ref\n");
    Con::evaluate(
        "function captureBoth(%obj, %func) { $g_obj = %obj; $g_func = %func; }\n"
        "captureBoth(MissionGroup, ObjectiveMission::initCheck);\n",
        false, "funcref.test");
    const char* gObj  = Con::getVariable("g_obj");
    const char* gFunc = Con::getVariable("g_func");
    check(gObj && std::strcmp(gObj, "MissionGroup") == 0,
          "first arg (plain ident) = \"MissionGroup\"");
    check(gFunc && std::strcmp(gFunc, "ObjectiveMission::initCheck") == 0,
          "second arg (function-ref) = \"ObjectiveMission::initCheck\"");

    // 4. Regression: real function-call form `Foo::bar(arg)` still works.
    //    LALR lookahead must prefer SHIFT over REDUCE when '(' follows.
    std::printf("\n[group] regression: Foo::bar(arg) still a call\n");
    Con::evaluate(
        "function Math::addTwo(%a, %b) { return %a + %b; }\n"
        "$sum = Math::addTwo(40, 2);\n",
        false, "funcref.test");
    const char* sum = Con::getVariable("sum");
    check(sum && std::strcmp(sum, "42") == 0,
          "Math::addTwo(40, 2) still parses + executes -> '42'");

    // 5. Trailing function-ref: `func(arg1, arg2, Foo::bar)` — bare ident at
    //    end of arg list (preceding ')' instead of ',').
    std::printf("\n[group] function-ref as trailing argument\n");
    Con::evaluate(
        "function captureLast(%a, %b, %c) { $g_last = %c; }\n"
        "captureLast(1, 2, Foo::bar);\n",
        false, "funcref.test");
    const char* gLast = Con::getVariable("g_last");
    check(gLast && std::strcmp(gLast, "Foo::bar") == 0,
          "trailing function-ref before ')' = \"Foo::bar\"");

    // 6. T1 corpus example, near-verbatim. Mimics objectives.cs:300:
    //    Group::iterateRecursive(MissionGroup, ObjectiveMission::initCheck);
    //    Without the underlying iterateRecursive available we substitute a
    //    captureBoth equivalent. The point: the SCRIPT parses without a
    //    syntax error.
    std::printf("\n[group] objectives.cs:300 shape\n");
    bool errored = false;
    auto errSink = [](unsigned int /*lvl*/, const char* line) {
        if (std::strstr(line, "syntax error"))
            std::fprintf(stderr, "    [vm] %s\n", line);
    };
    (void)errSink;  // diagnostic only; the silent `sink` swallows in tests
    Con::evaluate(
        "function Group::iterateRecursive(%g, %fn) { $g_iter = %fn; }\n"
        "Group::iterateRecursive(MissionGroup, ObjectiveMission::initCheck);\n",
        false, "objectives-shape.test");
    const char* gIter = Con::getVariable("g_iter");
    check(gIter && std::strcmp(gIter, "ObjectiveMission::initCheck") == 0,
          "objectives.cs:300-shape line parses + runs");
    (void)errored;

    std::printf("\n========================================================\n");
    std::printf("cscript_funcref_test: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
