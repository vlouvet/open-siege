// Open Siege spec 16/07 — bidirectional C++ <-> script callbacks.
//
// Torque exposes `Con::executef(simObject, "funcName", args...)` as the
// engine-side invocation hook. The class's auto-generated namespace
// (set up by IMPLEMENT_CONOBJECT) is the dispatch target, so a script
// function declared as `function Foo::onEvent(%self, %arg) { ... }`
// is resolved on a `Foo*` instance with no extra wiring.
//
// This TU asserts the contract end-to-end:
//   1. Script registers `function CbActor::onCollision(%self, %other) {...}`
//   2. C++ invokes Con::executef(actorA, "onCollision", actorB)
//   3. The script body runs (verified via a $marker global it sets)
//   4. Args round-trip — both %self and %other resolve to the right ids.
//
// Covers onCollision, onPickup, and onTriggerEnter — the three callback
// shapes called out in the spec.

#include "console/console.h"
#include "console/consoleInternal.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/simBase.h"
#include "console/simObject.h"
#include "console/torquescript/runtime.h"

#include <cstdio>
#include <cstring>

using namespace studio::content::cscript;

class CbActor : public SimObject
{
    typedef SimObject Parent;
public:
    StringTableEntry mTag;

    CbActor() : mTag(StringTable->insert("")) {}

    static void initPersistFields()
    {
        addField("tag", TypeString, Offset(mTag, CbActor));
        Parent::initPersistFields();
    }

    DECLARE_CONOBJECT(CbActor);
};
IMPLEMENT_CONOBJECT(CbActor);
ConsoleDocClass(CbActor, "@brief Spec 16/07 callback target.");

// A tiny C++ method that simulates an engine event firing the script
// callback. Engine code in the dts-viewer (or any consumer) calls a
// helper like this when a real collision happens — for example from
// projectile.cpp's splash routine, the C++ side might `Con::executef(
// hitPlayer, "onDamage", ...)`.
//
// We expose it as a script-callable wrapper so the test can drive it
// from a single Con::evaluate block.
DefineEngineMethod(CbActor, simulateCollision, void, (SimObject* other), (nullAsType<SimObject*>()),
    "Spec 16/07: simulate an engine collision event firing onCollision.")
{
    if (object && other)
        Con::executef(object, "onCollision", other);
}

DefineEngineMethod(CbActor, simulatePickup, void, (SimObject* picker), (nullAsType<SimObject*>()),
    "Spec 16/07: simulate an engine pickup event firing onPickup.")
{
    if (object && picker)
        Con::executef(object, "onPickup", picker);
}

DefineEngineMethod(CbActor, simulateTriggerEnter, void, (SimObject* who), (nullAsType<SimObject*>()),
    "Spec 16/07: simulate a trigger-enter event firing onTriggerEnter.")
{
    if (object && who)
        Con::executef(object, "onTriggerEnter", who);
}

// ---------------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // 1. Register the script callbacks against the CbActor namespace, then
    //    construct two instances to exchange events.
    // -----------------------------------------------------------------------
    std::printf("[group] declare + register callbacks\n");
    Con::evaluate(
        "function CbActor::onCollision(%self, %other) {\n"
        "    $coll_self  = %self.tag;\n"
        "    $coll_other = %other.tag;\n"
        "}\n"
        "function CbActor::onPickup(%self, %picker) {\n"
        "    $pick_self   = %self.tag;\n"
        "    $pick_picker = %picker.tag;\n"
        "}\n"
        "function CbActor::onTriggerEnter(%self, %who) {\n"
        "    $trig_self = %self.tag;\n"
        "    $trig_who  = %who.tag;\n"
        "}\n"
        "new CbActor(Hero)    { tag = \"Hero\"; };\n"
        "new CbActor(Villain) { tag = \"Villain\"; };\n"
        "new CbActor(Pad)     { tag = \"Pad\"; };\n",
        false, "callbacks.test");

    check(Sim::findObject("Hero") != nullptr,    "Hero constructed");
    check(Sim::findObject("Villain") != nullptr, "Villain constructed");

    // -----------------------------------------------------------------------
    // 2. Fire onCollision — C++ Con::executef(Hero, "onCollision", Villain)
    //    via the simulateCollision wrapper.
    // -----------------------------------------------------------------------
    std::printf("\n[group] onCollision\n");
    Con::evaluate("Hero.simulateCollision(Villain);", false, "callbacks.test");
    const char* cs = Con::getVariable("coll_self");
    const char* co = Con::getVariable("coll_other");
    check(cs && std::strcmp(cs, "Hero")    == 0, "onCollision: %self == Hero");
    check(co && std::strcmp(co, "Villain") == 0, "onCollision: %other == Villain");

    // -----------------------------------------------------------------------
    // 3. onPickup with different arg ordering — Villain picks up Hero.
    //    The first arg is the receiver (self), second is the picker.
    // -----------------------------------------------------------------------
    std::printf("\n[group] onPickup\n");
    Con::evaluate("Villain.simulatePickup(Hero);", false, "callbacks.test");
    const char* ps = Con::getVariable("pick_self");
    const char* pp = Con::getVariable("pick_picker");
    check(ps && std::strcmp(ps, "Villain") == 0, "onPickup: %self == Villain");
    check(pp && std::strcmp(pp, "Hero")    == 0, "onPickup: %picker == Hero");

    // -----------------------------------------------------------------------
    // 4. onTriggerEnter — Pad's trigger fires when Hero enters.
    // -----------------------------------------------------------------------
    std::printf("\n[group] onTriggerEnter\n");
    Con::evaluate("Pad.simulateTriggerEnter(Hero);", false, "callbacks.test");
    const char* ts = Con::getVariable("trig_self");
    const char* tw = Con::getVariable("trig_who");
    check(ts && std::strcmp(ts, "Pad")  == 0, "onTriggerEnter: %self == Pad");
    check(tw && std::strcmp(tw, "Hero") == 0, "onTriggerEnter: %who == Hero");

    // -----------------------------------------------------------------------
    // 5. Args survive across multiple fires (no stale-pointer marshalling).
    // -----------------------------------------------------------------------
    std::printf("\n[group] repeated callback invocations\n");
    Con::evaluate(
        "Hero.simulateCollision(Pad);\n"
        "$saved_other = $coll_other;\n"
        "Hero.simulateCollision(Villain);\n",
        false, "callbacks.test");
    const char* saved = Con::getVariable("saved_other");
    const char* now   = Con::getVariable("coll_other");
    check(saved && std::strcmp(saved, "Pad")     == 0,
          "first call: %other.tag == Pad");
    check(now   && std::strcmp(now,   "Villain") == 0,
          "second call overwrites: %other.tag == Villain");

    // -----------------------------------------------------------------------
    // 6. Missing-callback case: firing an event whose script function
    //    wasn't defined must not crash — it's a benign no-op (Torque
    //    semantics).
    // -----------------------------------------------------------------------
    std::printf("\n[group] missing callback (no-op)\n");
    auto* a = static_cast<CbActor*>(Sim::findObject("Hero"));
    auto* b = static_cast<CbActor*>(Sim::findObject("Villain"));
    // Direct C++ invoke for a method that has no script function.
    if (a && b) Con::executef(a, "onSomethingUndeclared", b);
    check(true, "missing callback handled (no crash)");

    std::printf("\n========================================================\n");
    std::printf("cscript_script_callbacks_test: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
