// Open Siege spec 17/03 — script bindings for the mission HUD.
//
// DefineEngineFunction-routed bindings that the script side fires from
// mission gameplay-rules and tutorial scripts. They funnel into a
// HudBindingsState owned by the host (main.cpp) and into the shared
// std::deque<std::string> message feed used by Track 13's HUD.
//
// The bindings are global free functions (DefineEngineFunction's
// preprocessor pastes the name into a generated symbol; placing them
// in a C++ namespace would fight that).

#include "hud_bindings.hpp"

#include "console/console.h"
#include "console/consoleTypes.h"
#include "console/engineAPI.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Module-local host pointers — null by default so tests that don't supply
// a host see the bindings as no-ops.
// ---------------------------------------------------------------------------
namespace {

std::deque<std::string>*           gMessageFeed = nullptr;
dts_viewer::HudBindingsState*      gHudState    = nullptr;

void appendFeed(const char* text)
{
    if (gMessageFeed && text) gMessageFeed->push_back(text);
}

} // namespace

namespace dts_viewer {

void hud_bindings_set_host(std::deque<std::string>* message_feed,
                           HudBindingsState*        state)
{
    gMessageFeed = message_feed;
    gHudState    = state;
}

void hud_bindings_tick(float dt_seconds)
{
    if (!gHudState || dt_seconds <= 0.0f) return;
    if (gHudState->center_print_remaining > 0.0f)
    {
        gHudState->center_print_remaining -= dt_seconds;
        if (gHudState->center_print_remaining <= 0.0f)
        {
            gHudState->center_print_remaining = 0.0f;
            gHudState->center_print_text.clear();
        }
    }
    if (gHudState->bottom_print_remaining > 0.0f)
    {
        gHudState->bottom_print_remaining -= dt_seconds;
        if (gHudState->bottom_print_remaining <= 0.0f)
        {
            gHudState->bottom_print_remaining = 0.0f;
            gHudState->bottom_print_text.clear();
        }
    }
}

} // namespace dts_viewer

// ---------------------------------------------------------------------------
// DefineEngineFunction surface
// ---------------------------------------------------------------------------

DefineEngineFunction(addObjective, void, (const char* text, S32 id), ,
    "Spec 17/03: add an objective line to the HUD with the given id.")
{
    if (!gHudState) return;
    ++gHudState->n_addObjective;
    // If we already have an objective with this id, update its text.
    for (auto& o : gHudState->objectives) {
        if (o.id == id) { o.text = text ? text : ""; return; }
    }
    dts_viewer::HudObjective o;
    o.id    = id;
    o.text  = text ? text : "";
    o.state = "in-progress";
    gHudState->objectives.push_back(std::move(o));
}

DefineEngineFunction(setObjectiveState, void,
                     (S32 id, const char* state), ,
    "Spec 17/03: change objective `id`'s state ('complete', 'failed', "
    "'in-progress'). Silently no-ops if id is unknown.")
{
    if (!gHudState) return;
    ++gHudState->n_setObjState;
    for (auto& o : gHudState->objectives) {
        if (o.id == id) { o.state = state ? state : ""; return; }
    }
    Con::warnf("setObjectiveState: unknown objective id %d", id);
}

DefineEngineFunction(removeObjective, void, (S32 id), ,
    "Spec 17/03: drop the objective with the given id.")
{
    if (!gHudState) return;
    ++gHudState->n_removeObjective;
    auto& v = gHudState->objectives;
    v.erase(std::remove_if(v.begin(), v.end(),
        [id](const dts_viewer::HudObjective& o) { return o.id == id; }),
        v.end());
}

DefineEngineFunction(notify, void, (const char* text), ,
    "Spec 17/03: push `text` onto the scrolling message feed.")
{
    if (gHudState) ++gHudState->n_notify;
    appendFeed(text);
}

DefineEngineFunction(centerPrint, void,
                     (const char* text, F32 duration), ,
    "Spec 17/03: show `text` as a large center-screen banner for "
    "`duration` seconds.")
{
    if (!gHudState) return;
    ++gHudState->n_centerPrint;
    gHudState->center_print_text      = text ? text : "";
    gHudState->center_print_remaining = (duration > 0.0f) ? duration : 3.0f;
}

DefineEngineFunction(bottomPrint, void, (const char* text), ,
    "Spec 17/03: show `text` in the bottom-of-screen message bar.")
{
    if (!gHudState) return;
    ++gHudState->n_bottomPrint;
    gHudState->bottom_print_text      = text ? text : "";
    gHudState->bottom_print_remaining = 3.0f;
}

DefineEngineFunction(setMissionState, void, (const char* state), ,
    "Spec 17/03: set mission outcome — 'Won' / 'Lost' / 'Playing'.")
{
    if (!gHudState) return;
    ++gHudState->n_setMissionState;
    gHudState->mission_state = state ? state : "";
    // Convenience: a "Won" / "Lost" state also fires a centerPrint banner
    // so the visual delivery happens automatically.
    if (gHudState->mission_state == "Won") {
        gHudState->center_print_text      = "Mission Complete";
        gHudState->center_print_remaining = 5.0f;
        ++gHudState->n_centerPrint;
    } else if (gHudState->mission_state == "Lost") {
        gHudState->center_print_text      = "Mission Failed";
        gHudState->center_print_remaining = 5.0f;
        ++gHudState->n_centerPrint;
    }
}

// ---------------------------------------------------------------------------
// Anchor — pulls this TU's static DefineEngineFunction registrations into
// the static-archive link. Same pattern as anchorEntityClasses() in
// entity_bindings.cpp.
// ---------------------------------------------------------------------------
namespace dts_viewer {
void anchorHudBindings()
{
    // No-op body; the mere act of being called from cscript_host.cpp is
    // enough to make the linker include this TU and its
    // _engineFunction_addObjective_caller (etc.) symbols.
    static volatile int s = 0;
    (void)s;
}
} // namespace dts_viewer
