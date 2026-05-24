#include "spawn_ui.hpp"

#include <osengine/ghost_types.hpp>
#include <osengine/net_client.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace open_siege {

void update_spawn_ui(SpawnUiState&                ui,
                     const dts_viewer::NetClient& net,
                     int                          local_slot_or_neg1,
                     std::uint32_t                now_ms)
{
    if (local_slot_or_neg1 < 0) {
        ui.state = LocalLifeState::Alive;
        ui.dead_since_ms = 0;
        return;
    }
    const auto reg = net.snapshot_registry();
    auto it = reg.players.find(static_cast<std::uint16_t>(local_slot_or_neg1));
    if (it == reg.players.end()) {
        // Haven't received our ghost yet (or it was removed). Treat as
        // alive so the overlay doesn't briefly flash on disconnect.
        ui.state = LocalLifeState::Alive;
        ui.dead_since_ms = 0;
        return;
    }
    const auto& me = it->second;
    if (me.dead) {
        if (ui.state != LocalLifeState::Dead) {
            ui.state          = LocalLifeState::Dead;
            ui.dead_since_ms  = now_ms;
            ui.corpse_x       = me.pos_x;
            ui.corpse_y       = me.pos_y;
            ui.corpse_z       = me.pos_z;
        }
    } else {
        ui.state         = LocalLifeState::Alive;
        ui.dead_since_ms = 0;
    }
}

void draw_spawn_ui(const SpawnUiState& ui,
                   std::uint32_t       now_ms,
                   int                 viewport_w,
                   int                 viewport_h)
{
    if (ui.state != LocalLifeState::Dead) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const float vw = static_cast<float>(viewport_w);
    const float vh = static_cast<float>(viewport_h);

    // Dim the background slightly so the overlay reads as a state change.
    dl->AddRectFilled({0.0f, 0.0f}, {vw, vh}, IM_COL32(0, 0, 0, 80));

    const std::uint32_t elapsed_ms = (now_ms > ui.dead_since_ms)
        ? now_ms - ui.dead_since_ms : 0u;
    const float elapsed_sec  = float(elapsed_ms) * 0.001f;
    const float remaining_sec = std::max(0.0f, ui.assumed_delay_sec - elapsed_sec);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "You died.  Respawning in %.1f...",
                  remaining_sec);

    const ImVec2 sz = ImGui::CalcTextSize(buf);
    const ImVec2 anchor{(vw - sz.x) * 0.5f, vh * 0.40f};

    dl->AddRectFilled({anchor.x - 16.0f, anchor.y - 10.0f},
                      {anchor.x + sz.x + 16.0f, anchor.y + sz.y + 10.0f},
                      IM_COL32(20, 20, 30, 220));
    dl->AddRect      ({anchor.x - 16.0f, anchor.y - 10.0f},
                      {anchor.x + sz.x + 16.0f, anchor.y + sz.y + 10.0f},
                      IM_COL32(255, 255, 255, 200));
    dl->AddText(anchor, IM_COL32(240, 240, 240, 240), buf);
}

}  // namespace open_siege
