// Open Siege spec 16/04 — engine-binding macro proof.
//
// Spec was written expecting Torque's pre-2013 macro names
// (ConsoleMethod / ConsoleFunction). Torque3D removed those when we
// re-forked at SHA 3661499 (spec 15/01) — they're now spelled
// DefineEngineMethod / DefineEngineFunction with the same purpose.
// Per project CLAUDE.md ("don't add backwards-compat shims"), this
// test exercises the modern macro forms as the binding API.
//
// Acceptance checks:
//   - DECLARE_CONOBJECT + IMPLEMENT_CONOBJECT register a SimObject
//     subclass; `instant/new Demo(...)` constructs it from script.
//   - DefineEngineMethod(Demo, fire, ...) binds a C++ method that
//     script calls as `Hero.fire();` and the C++ side observes the call.
//   - DefineEngineMethod with typed args (S32) round-trips parameters.
//   - DefineEngineFunction(addNumbers, S32, (a, b), ...) defines a
//     top-level function callable from script with a return value.
//   - `echo(...)` (Torque-supplied DefineEngineFunction) runs.

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/engineAPI.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/simObject.h"
#include "console/torquescript/runtime.h"

#include <cstdio>
#include <cstring>

using namespace studio::content::cscript;

// ---------------------------------------------------------------------------
// Demo SimObject — exposes fire() and damage(S32 amount).
// ---------------------------------------------------------------------------
class Demo : public SimObject
{
    typedef SimObject Parent;
public:
    static int sFireCalls;
    static int sLastDamageAmount;

    void fire()
    {
        ++sFireCalls;
    }

    void damage(S32 amount)
    {
        sLastDamageAmount = amount;
    }

    DECLARE_CONOBJECT(Demo);
};
int Demo::sFireCalls = 0;
int Demo::sLastDamageAmount = 0;
IMPLEMENT_CONOBJECT(Demo);
ConsoleDocClass(Demo, "@brief Spec 16/04 binding-macro test object.");

DefineEngineMethod(Demo, fire, void, (), ,
    "Spec 16/04: trivial method binding. Script calls Hero.fire();")
{
    object->fire();
}

DefineEngineMethod(Demo, damage, void, (S32 amount), (0),
    "Spec 16/04: typed-argument method binding.")
{
    object->damage(amount);
}

DefineEngineMethod(Demo, getFireCount, S32, (), ,
    "Spec 16/04: return-value method binding.")
{
    (void)object;
    return Demo::sFireCalls;
}

// ---------------------------------------------------------------------------
// Top-level function: addNumbers(a, b) -> S32.
// ---------------------------------------------------------------------------
DefineEngineFunction(addNumbers, S32, (S32 a, S32 b), ,
    "Spec 16/04: top-level function returning the sum.")
{
    return a + b;
}

// ---------------------------------------------------------------------------
static int gPassed = 0, gFailed = 0;
static void check(bool c, const char* d)
{
    if (c) { ++gPassed; std::printf("  pass: %s\n", d); }
    else   { ++gFailed; std::fprintf(stderr, "  FAIL: %s\n", d); }
}
static void sink(unsigned int, const char* /*line*/) { /* silent */ }

int main()
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    std::printf("cscript_binding_macros_test: starting up\n");

    Con::init();
    Sim::init();
    Con::addConsumer(sink);
    Con::registerRuntime(0, TorqueScript::getRuntime());

    // 1. Construct via the registered class. IMPLEMENT_CONOBJECT made
    //    `Demo` a script-creatable type.
    std::printf("\n[group] DECLARE_CONOBJECT / IMPLEMENT_CONOBJECT\n");
    Con::evaluate("$h = new Demo(Hero){}; echo($h);", false, "macros.test");
    Demo* hero = dynamic_cast<Demo*>(Sim::findObject("Hero"));
    check(hero != nullptr, "new Demo(Hero) constructs a Demo SimObject");

    // 2. DefineEngineMethod — Hero.fire() bumps C++ counter.
    std::printf("\n[group] DefineEngineMethod (void, no args)\n");
    const int before = Demo::sFireCalls;
    Con::evaluate("Hero.fire(); Hero.fire(); Hero.fire();", false, "macros.test");
    check(Demo::sFireCalls == before + 3,
          "Hero.fire() x3 invoked C++ Demo::fire() three times");

    // 3. DefineEngineMethod with typed S32 arg.
    std::printf("\n[group] DefineEngineMethod (typed S32 arg)\n");
    Con::evaluate("Hero.damage(42);", false, "macros.test");
    check(Demo::sLastDamageAmount == 42,
          "Hero.damage(42) reached C++ with amount=42");
    Con::evaluate("Hero.damage(-7);", false, "macros.test");
    check(Demo::sLastDamageAmount == -7,
          "Hero.damage(-7) reached C++ with amount=-7");

    // 4. DefineEngineMethod with S32 return.
    std::printf("\n[group] DefineEngineMethod (S32 return)\n");
    Con::evaluate("$c = Hero.getFireCount();", false, "macros.test");
    const char* c = Con::getVariable("c");
    check(c && std::strcmp(c, "3") == 0,
          "Hero.getFireCount() returns '3' to script");

    // 5. DefineEngineFunction — top-level call from script.
    std::printf("\n[group] DefineEngineFunction (top-level)\n");
    Con::evaluate("$sum = addNumbers(40, 2);", false, "macros.test");
    const char* sum = Con::getVariable("sum");
    check(sum && std::strcmp(sum, "42") == 0,
          "addNumbers(40, 2) returns '42'");

    // 6. Built-in echo() (Torque-provided DefineEngineFunction) executes.
    //    The sink swallows the output; we verify it returns without error
    //    by checking a sentinel set after.
    std::printf("\n[group] built-in echo() (DefineEngineFunction)\n");
    Con::evaluate("echo(\"hello-from-script\"); $marker = \"ok\";",
                  false, "macros.test");
    const char* mark = Con::getVariable("marker");
    check(mark && std::strcmp(mark, "ok") == 0,
          "echo() runs without aborting the eval");

    std::printf("\n========================================================\n");
    std::printf("cscript_binding_macros_test: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
