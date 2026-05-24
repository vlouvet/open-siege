#ifndef OPEN_SIEGE_T1_CLIENT_SPAWN_UI_HPP
#define OPEN_SIEGE_T1_CLIENT_SPAWN_UI_HPP

// Spec 29/06 — spawn/respawn overlay.
//
// Client-side cosmetic overlay that shows "You died — respawning in
// N.N..." when the local player's GhostPlayer.dead flag is true. The
// server is authoritative — when the next batch shows dead=false the
// overlay disappears, regardless of the local countdown.

#include <cstdint>

namespace dts_viewer { class NetClient; }

namespace open_siege {

enum class LocalLifeState { Alive, Dead };

struct SpawnUiState {
    LocalLifeState state                = LocalLifeState::Alive;
    std::uint32_t  dead_since_ms        = 0;     // local clock; 0 if alive
    float          assumed_delay_sec    = 3.0f;  // matches DamageRules default
    float          corpse_x = 0.0f, corpse_y = 0.0f, corpse_z = 0.0f;
};

// Per frame, BEFORE drawing the overlay. Updates state from the local
// Player ghost (looked up via player_slot). Pass local_slot_or_neg1 = -1
// when server_info hasn't arrived yet — overlay stays Alive.
void update_spawn_ui(SpawnUiState&                ui,
                     const dts_viewer::NetClient& net,
                     int                          local_slot_or_neg1,
                     std::uint32_t                now_ms);

// Per frame, between imgui_begin_frame / imgui_end_frame.
void draw_spawn_ui(const SpawnUiState& ui,
                   std::uint32_t       now_ms,
                   int                 viewport_w,
                   int                 viewport_h);

}  // namespace open_siege

#endif  // OPEN_SIEGE_T1_CLIENT_SPAWN_UI_HPP
