#ifndef OPEN_SIEGE_T1_CLIENT_FLAG_UI_HPP
#define OPEN_SIEGE_T1_CLIENT_FLAG_UI_HPP

// Spec 29/07 — flag rendering + carrier indicator + match-end banner.
//
// v1 limitations (see spec): the server doesn't yet emit Flag entities
// over the ghost stream (deferred to spec 28/04c — Item encoder).
// This module pulls Item ghosts from the registry and treats those
// whose `item_data_file_id` matches a configured "flag" id as flags.
// Until 28/04c lands, the registry's items map stays empty and no
// pillars are drawn — but the rendering path + carrier indicator are
// wired so dropping in real Flag data later is a one-line change.

#include <cstdint>
#include <glm/glm.hpp>

namespace dts_viewer { class NetClient; }

namespace open_siege {

struct FlagUiState {
    // Track 31 will wire match-end banner from a server-broadcast
    // event. Until then this is a placeholder the caller can set
    // manually.
    bool        end_banner_visible = false;
    int         winning_team_id    = 0;     // 1=Red, 2=Blue, 0=Draw
    std::uint32_t end_at_ms        = 0;     // when the banner appeared
};

// Draw 0.5x0.5x2 m colored pillars at each Item ghost position. v1
// doesn't filter — every Item is drawn — once 28/04c is in place the
// caller will pass the flag item_data_file_id filter.
void render_flag_pillars(const dts_viewer::NetClient& net,
                         std::uint32_t                u_mvp_loc,
                         std::uint32_t                u_color_loc,
                         const glm::mat4&             view_proj);

// HUD overlay: "ENEMY FLAG — return!" when local player carries an
// enemy flag. v1: no FlagWorld on client; local_carrier_flag_team is
// passed in by caller (will be derived from inventory / mounted item
// once that ghost field is exposed). 0 = not carrying.
void draw_flag_carrier_hud(int           carried_flag_team,
                           int           viewport_w,
                           int           viewport_h);

void draw_match_end_banner(const FlagUiState& s,
                           std::uint32_t      now_ms,
                           int                viewport_w,
                           int                viewport_h);

}  // namespace open_siege

#endif  // OPEN_SIEGE_T1_CLIENT_FLAG_UI_HPP
