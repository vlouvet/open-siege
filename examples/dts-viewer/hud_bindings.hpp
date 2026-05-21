#ifndef DTS_VIEWER_HUD_BINDINGS_HPP
#define DTS_VIEWER_HUD_BINDINGS_HPP

// Open Siege spec 17/03 — script bindings for the mission HUD.
//
// Bound DefineEngineFunctions (see hud_bindings.cpp):
//
//   addObjective(text, id)         add an objective line to the HUD
//   setObjectiveState(id, state)   "complete" / "failed" / "in-progress"
//   removeObjective(id)            drop the objective with that id
//   notify(text)                   scrolling event-log entry
//   centerPrint(text, duration)    large center-screen text
//   bottomPrint(text)              small bottom-of-screen message bar
//   setMissionState(state)         "Won" / "Lost" / "Playing"
//
// These funnel into a HudBindingsState struct owned by the host (main.cpp)
// + a std::deque<std::string>* used by the existing Track 13 message feed.
// The HUD draw code can poll HudBindingsState to render objectives /
// centerprint banners.

#include <deque>
#include <string>
#include <vector>

namespace dts_viewer
{

struct HudObjective
{
    int         id    = 0;
    std::string text;
    std::string state = "in-progress";  // "in-progress" / "complete" / "failed"
};

struct HudBindingsState
{
    std::vector<HudObjective> objectives;
    std::string               center_print_text;
    float                     center_print_remaining = 0.0f;
    std::string               bottom_print_text;
    float                     bottom_print_remaining = 0.0f;
    std::string               mission_state = "Playing"; // "Won" / "Lost" / "Playing"

    // Diagnostic counters — incremented each time the binding fires.
    int n_addObjective    = 0;
    int n_setObjState     = 0;
    int n_removeObjective = 0;
    int n_notify          = 0;
    int n_centerPrint     = 0;
    int n_bottomPrint     = 0;
    int n_setMissionState = 0;
};

// Wire the bindings to the host's HUD state. Either pointer may be null
// to disable that surface (e.g. tests that only need centerprint can
// pass nullptr for the message_feed deque).
void hud_bindings_set_host(std::deque<std::string>* message_feed,
                           HudBindingsState* state);

// Age centerprint / bottomprint banners. Safe to call every frame.
void hud_bindings_tick(float dt_seconds);

// Force-link anchor — invoked from cscript_host.cpp so the static
// DefineEngineFunction registrations survive static-archive link.
void anchorHudBindings();

} // namespace dts_viewer

#endif // DTS_VIEWER_HUD_BINDINGS_HPP
