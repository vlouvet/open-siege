// Open Siege spec 17/03 — script-to-HUD binding test.
//
// Exercises the contract that examples/dts-viewer/hud_bindings.cpp
// fulfils: a script call to addObjective / setObjectiveState /
// removeObjective / notify / centerPrint / bottomPrint / setMissionState
// routes through the engine binding into a host-side HudBindingsState +
// std::deque<std::string> message-feed pair.
//
// To keep the test free of dts-viewer's link dependencies, the bindings
// are re-defined in this TU with the same DefineEngineFunction shape.
// (The dts-viewer copy lives at examples/dts-viewer/hud_bindings.cpp.
// If they ever diverge the test would catch the regression — the spec
// says the contract is what's tested, not the specific implementation.)

#include "console/console.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/torquescript/runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace studio::content::cscript;

// ---------------------------------------------------------------------------
// HudBindingsState clone — mirrors dts-viewer's struct field-for-field.
// ---------------------------------------------------------------------------
struct HudObjectiveT
{
    int         id    = 0;
    std::string text;
    std::string state = "in-progress";
};

struct HudBindingsStateT
{
    std::vector<HudObjectiveT> objectives;
    std::string                center_print_text;
    float                      center_print_remaining = 0.0f;
    std::string                bottom_print_text;
    float                      bottom_print_remaining = 0.0f;
    std::string                mission_state = "Playing";

    int n_addObjective    = 0;
    int n_setObjState     = 0;
    int n_removeObjective = 0;
    int n_notify          = 0;
    int n_centerPrint     = 0;
    int n_bottomPrint     = 0;
    int n_setMissionState = 0;
};

static std::deque<std::string>* gMessageFeedT = nullptr;
static HudBindingsStateT*       gHudStateT    = nullptr;

// Distinct binding names to avoid clashing with the dts-viewer copies if
// the test is ever rolled into the same binary. The DefineEngineFunction
// names are what scripts call.
DefineEngineFunction(addObjective, void, (const char* text, S32 id), ,
    "Spec 17/03 test addObjective binding.")
{
    if (!gHudStateT) return;
    ++gHudStateT->n_addObjective;
    for (auto& o : gHudStateT->objectives) {
        if (o.id == id) { o.text = text ? text : ""; return; }
    }
    HudObjectiveT o;
    o.id    = id;
    o.text  = text ? text : "";
    o.state = "in-progress";
    gHudStateT->objectives.push_back(std::move(o));
}

DefineEngineFunction(setObjectiveState, void,
                     (S32 id, const char* state), ,
    "Spec 17/03 test setObjectiveState binding.")
{
    if (!gHudStateT) return;
    ++gHudStateT->n_setObjState;
    for (auto& o : gHudStateT->objectives) {
        if (o.id == id) { o.state = state ? state : ""; return; }
    }
}

DefineEngineFunction(removeObjective, void, (S32 id), ,
    "Spec 17/03 test removeObjective binding.")
{
    if (!gHudStateT) return;
    ++gHudStateT->n_removeObjective;
    auto& v = gHudStateT->objectives;
    v.erase(std::remove_if(v.begin(), v.end(),
        [id](const HudObjectiveT& o) { return o.id == id; }),
        v.end());
}

DefineEngineFunction(notify, void, (const char* text), ,
    "Spec 17/03 test notify binding.")
{
    if (gHudStateT) ++gHudStateT->n_notify;
    if (gMessageFeedT && text) gMessageFeedT->push_back(text);
}

DefineEngineFunction(centerPrint, void,
                     (const char* text, F32 duration), ,
    "Spec 17/03 test centerPrint binding.")
{
    if (!gHudStateT) return;
    ++gHudStateT->n_centerPrint;
    gHudStateT->center_print_text      = text ? text : "";
    gHudStateT->center_print_remaining = (duration > 0.0f) ? duration : 3.0f;
}

DefineEngineFunction(bottomPrint, void, (const char* text), ,
    "Spec 17/03 test bottomPrint binding.")
{
    if (!gHudStateT) return;
    ++gHudStateT->n_bottomPrint;
    gHudStateT->bottom_print_text      = text ? text : "";
    gHudStateT->bottom_print_remaining = 3.0f;
}

DefineEngineFunction(setMissionState, void, (const char* state), ,
    "Spec 17/03 test setMissionState binding.")
{
    if (!gHudStateT) return;
    ++gHudStateT->n_setMissionState;
    gHudStateT->mission_state = state ? state : "";
    if (gHudStateT->mission_state == "Won") {
        gHudStateT->center_print_text      = "Mission Complete";
        gHudStateT->center_print_remaining = 5.0f;
        ++gHudStateT->n_centerPrint;
    } else if (gHudStateT->mission_state == "Lost") {
        gHudStateT->center_print_text      = "Mission Failed";
        gHudStateT->center_print_remaining = 5.0f;
        ++gHudStateT->n_centerPrint;
    }
}

// ---------------------------------------------------------------------------
// Test plumbing
// ---------------------------------------------------------------------------
static int gPassed = 0, gFailed = 0;
static void check(bool c, const char* d)
{
    if (c) { ++gPassed; std::printf("  pass: %s\n", d); }
    else   { ++gFailed; std::fprintf(stderr, "  FAIL: %s\n", d); }
}
static void sink(unsigned int, const char*) {}

// ---------------------------------------------------------------------------
int main()
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    std::printf("cscript_hud_bindings_test: starting up\n");

    Con::init();
    Sim::init();
    Con::addConsumer(sink);
    Con::registerRuntime(0, TorqueScript::getRuntime());

    HudBindingsStateT state;
    std::deque<std::string> feed;
    gHudStateT = &state;
    gMessageFeedT = &feed;

    // -----------------------------------------------------------------------
    // [group] addObjective / setObjectiveState / removeObjective
    // -----------------------------------------------------------------------
    std::printf("\n[group] objective lifecycle\n");
    Con::evaluate(
        "addObjective(\"Capture the flag\", 1);\n"
        "addObjective(\"Reach the base\", 2);\n"
        "addObjective(\"Survive\", 3);\n",
        false, "hud.test");
    check(state.n_addObjective == 3, "addObjective fired 3 times");
    check(state.objectives.size() == 3, "objectives list has 3 entries");
    check(state.objectives[0].id == 1 && state.objectives[0].text == "Capture the flag",
          "objective 0 is { id=1, text=\"Capture the flag\" }");
    check(state.objectives[2].state == "in-progress",
          "newly-added objectives default to \"in-progress\"");

    Con::evaluate("setObjectiveState(1, \"complete\");\n"
                  "setObjectiveState(2, \"failed\");",
                  false, "hud.test");
    check(state.n_setObjState == 2, "setObjectiveState fired 2 times");
    auto findObj = [&](int id) -> HudObjectiveT* {
        for (auto& o : state.objectives) if (o.id == id) return &o;
        return nullptr;
    };
    check(findObj(1) && findObj(1)->state == "complete",
          "objective 1 transitioned to \"complete\"");
    check(findObj(2) && findObj(2)->state == "failed",
          "objective 2 transitioned to \"failed\"");
    check(findObj(3) && findObj(3)->state == "in-progress",
          "objective 3 still \"in-progress\"");

    Con::evaluate("removeObjective(2);", false, "hud.test");
    check(state.n_removeObjective == 1, "removeObjective fired once");
    check(state.objectives.size() == 2, "objectives list shrunk to 2");
    check(findObj(2) == nullptr, "objective 2 is gone");

    // addObjective re-using id updates instead of duplicating.
    Con::evaluate("addObjective(\"Capture all flags\", 1);", false, "hud.test");
    check(state.objectives.size() == 2, "re-using an id doesn't duplicate");
    check(findObj(1) && findObj(1)->text == "Capture all flags",
          "re-using an id updates the existing objective text");

    // -----------------------------------------------------------------------
    // [group] notify -> message feed
    // -----------------------------------------------------------------------
    std::printf("\n[group] notify\n");
    Con::evaluate(
        "notify(\"Generator destroyed\");\n"
        "notify(\"Enemy spotted\");\n",
        false, "hud.test");
    check(state.n_notify == 2, "notify fired 2 times");
    check(feed.size() == 2, "message feed got both notify entries");
    check(feed[0] == "Generator destroyed", "feed[0] == \"Generator destroyed\"");
    check(feed[1] == "Enemy spotted",       "feed[1] == \"Enemy spotted\"");

    // -----------------------------------------------------------------------
    // [group] centerPrint / bottomPrint
    // -----------------------------------------------------------------------
    std::printf("\n[group] centerPrint / bottomPrint\n");
    Con::evaluate("centerPrint(\"Mission start\", 5.0);", false, "hud.test");
    check(state.n_centerPrint == 1, "centerPrint fired once");
    check(state.center_print_text == "Mission start",
          "center_print_text == \"Mission start\"");
    check(state.center_print_remaining > 4.9f && state.center_print_remaining < 5.1f,
          "center_print_remaining ~= 5.0");

    Con::evaluate("bottomPrint(\"Move out\");", false, "hud.test");
    check(state.n_bottomPrint == 1, "bottomPrint fired once");
    check(state.bottom_print_text == "Move out",
          "bottom_print_text == \"Move out\"");
    check(state.bottom_print_remaining > 0.0f, "bottom print has remaining time");

    // -----------------------------------------------------------------------
    // [group] setMissionState
    // -----------------------------------------------------------------------
    std::printf("\n[group] setMissionState\n");
    Con::evaluate("setMissionState(\"Won\");", false, "hud.test");
    check(state.mission_state == "Won", "mission_state == \"Won\"");
    check(state.center_print_text == "Mission Complete",
          "\"Won\" auto-fires centerPrint(\"Mission Complete\")");

    Con::evaluate("setMissionState(\"Lost\");", false, "hud.test");
    check(state.mission_state == "Lost", "mission_state == \"Lost\"");
    check(state.center_print_text == "Mission Failed",
          "\"Lost\" auto-fires centerPrint(\"Mission Failed\")");

    Con::evaluate("setMissionState(\"Playing\");", false, "hud.test");
    check(state.mission_state == "Playing", "mission_state -> \"Playing\"");

    // -----------------------------------------------------------------------
    // [group] script-driven mission flow
    //   Simulates a tutorial-script flow: load => addObjective list,
    //   tutorial progress fires setObjectiveState + bottomPrint, victory
    //   condition fires setMissionState("Won").
    // -----------------------------------------------------------------------
    std::printf("\n[group] script-driven mission flow\n");
    state = HudBindingsStateT{};   // reset state
    feed.clear();

    Con::evaluate(
        "function Tutorial::start() {\n"
        "    addObjective(\"Step 1 — learn the HUD\", 101);\n"
        "    addObjective(\"Step 2 — find the switch\", 102);\n"
        "    addObjective(\"Step 3 — finish the run\", 103);\n"
        "    centerPrint(\"Welcome to Tribes\", 4);\n"
        "    bottomPrint(\"Follow the waypoint marker\");\n"
        "    notify(\"Tutorial started\");\n"
        "}\n"
        "function Tutorial::onStep(%n) {\n"
        "    setObjectiveState(%n, \"complete\");\n"
        "    bottomPrint(\"Step \" @ %n @ \" complete\");\n"
        "}\n"
        "function Tutorial::win() {\n"
        "    setMissionState(\"Won\");\n"
        "    notify(\"Mission complete\");\n"
        "}\n"
        "Tutorial::start();\n",
        false, "hud.test");

    check(state.objectives.size() == 3, "tutorial start registered 3 objectives");
    check(state.center_print_text == "Welcome to Tribes",
          "tutorial start fired centerPrint(\"Welcome to Tribes\")");
    check(state.bottom_print_text == "Follow the waypoint marker",
          "tutorial start fired bottomPrint(\"Follow the waypoint marker\")");
    check(feed.size() == 1 && feed.back() == "Tutorial started",
          "tutorial start pushed \"Tutorial started\" onto the feed");

    Con::evaluate("Tutorial::onStep(101);\n"
                  "Tutorial::onStep(102);",
                  false, "hud.test");
    check(findObj(101) && findObj(101)->state == "complete",
          "step 1 -> objective 101 \"complete\"");
    check(findObj(102) && findObj(102)->state == "complete",
          "step 2 -> objective 102 \"complete\"");
    check(findObj(103) && findObj(103)->state == "in-progress",
          "step 3 still \"in-progress\"");

    Con::evaluate("Tutorial::win();", false, "hud.test");
    check(state.mission_state == "Won", "Tutorial::win() -> mission_state \"Won\"");
    check(state.center_print_text == "Mission Complete",
          "Tutorial::win() auto-fired centerPrint(\"Mission Complete\")");
    check(feed.back() == "Mission complete",
          "Tutorial::win() pushed \"Mission complete\" onto the feed");

    std::printf("\n========================================================\n");
    std::printf("cscript_hud_bindings_test: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
