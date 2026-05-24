#ifndef OPEN_SIEGE_T1_CLIENT_HUD_GLUE_HPP
#define OPEN_SIEGE_T1_CLIENT_HUD_GLUE_HPP

// Spec 29/05 — HUD live updates from net state.
//
// Pulls the local player's GhostPlayer + the other players' positions
// out of NetClient's snapshot registry and renders:
//   * health + energy bars (bottom-left)
//   * compass strip          (top-center)
//   * sensor radar           (top-right)
//   * scoreboard overlay     (Tab toggle)
//
// Everything is drawn via Dear ImGui's foreground draw list so the
// existing dts_viewer_render world rendering is undisturbed.

#include <cstdint>
#include <deque>
#include <string>

namespace dts_viewer { class NetClient; }

namespace open_siege {

struct HudGlueState {
    bool          scoreboard_visible = false;
    std::deque<std::string> message_feed;  // bottom-left lines; capped at 8
};

// Add a system / kill-feed line. Trims to last 8 entries.
void push_message(HudGlueState& s, std::string line);

// Per-frame call — between imgui_begin_frame() and imgui_end_frame().
// `local_slot_or_neg1` should be the value of NetClient::server_info()
// ->player_slot when known; pass -1 if not yet assigned (no local-player
// bars are drawn in that case but the sensor/compass still render
// from the other Player ghosts).
void render_net_hud(const dts_viewer::NetClient& net,
                    HudGlueState&                ui,
                    int                          local_slot_or_neg1,
                    int                          viewport_w,
                    int                          viewport_h);

}  // namespace open_siege

#endif  // OPEN_SIEGE_T1_CLIENT_HUD_GLUE_HPP
