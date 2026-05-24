#include "flag_ui.hpp"

#include <osengine/ghost_types.hpp>
#include <osengine/net_client.hpp>

#include <entity_renderer.hpp>     // from dts_viewer_render

#include <imgui.h>

#include <array>
#include <cmath>
#include <cstdio>

namespace open_siege {

namespace {

// Pillar = cube with a tall axis, drawn as four stacked unit cubes so
// the wireframe helper from dts_viewer_render works without needing a
// new mesh.
void draw_pillar(const glm::vec3& base,
                 const std::array<float, 3>& color,
                 std::uint32_t u_mvp_loc,
                 std::uint32_t u_color_loc,
                 const glm::mat4& vp)
{
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 p{base.x, base.y + 0.5f + float(i) * 1.0f, base.z};
        dts_viewer::render_entity_cube(p, 0.5f, color, GLint(u_mvp_loc),
                                       GLint(u_color_loc), vp);
    }
}

}  // namespace

void render_flag_pillars(const dts_viewer::NetClient& net,
                         std::uint32_t                u_mvp_loc,
                         std::uint32_t                u_color_loc,
                         const glm::mat4&             view_proj)
{
    const auto reg = net.snapshot_registry();
    for (const auto& kv : reg.items) {
        const auto& it = kv.second;
        // v1: no item_data_file_id filter. Color by base.team_id so
        // Red flags are red, Blue flags blue, neutral pickups grey.
        std::array<float, 3> col{0.7f, 0.7f, 0.7f};
        switch (it.base.team_id) {
            case 1: col = {1.0f, 0.25f, 0.25f}; break;
            case 2: col = {0.25f, 0.50f, 1.0f}; break;
            default: break;
        }
        const glm::vec3 base{it.pos_x, it.pos_y, it.pos_z};
        draw_pillar(base, col, u_mvp_loc, u_color_loc, view_proj);
    }
}

void draw_flag_carrier_hud(int carried_flag_team,
                           int viewport_w,
                           int viewport_h)
{
    if (carried_flag_team <= 0) return;
    const char* label = carried_flag_team == 1 ? "RED FLAG — return to base!"
                      : carried_flag_team == 2 ? "BLUE FLAG — return to base!"
                                                : "ENEMY FLAG — return to base!";
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 vp{float(viewport_w), float(viewport_h)};
    const ImVec2 sz = ImGui::CalcTextSize(label);
    const ImVec2 anchor{(vp.x - sz.x) * 0.5f, vp.y * 0.62f};
    dl->AddRectFilled({anchor.x - 12.0f, anchor.y - 6.0f},
                      {anchor.x + sz.x + 12.0f, anchor.y + sz.y + 6.0f},
                      IM_COL32(40, 0, 0, 200));
    const ImU32 fg = carried_flag_team == 1 ? IM_COL32(255, 110, 110, 255)
                   : carried_flag_team == 2 ? IM_COL32(110, 170, 255, 255)
                                            : IM_COL32(255, 255, 255, 255);
    dl->AddText(anchor, fg, label);
}

void draw_match_end_banner(const FlagUiState& s,
                           std::uint32_t      now_ms,
                           int                viewport_w,
                           int                viewport_h)
{
    if (!s.end_banner_visible) return;
    char buf[96];
    const char* who = s.winning_team_id == 1 ? "RED WINS"
                    : s.winning_team_id == 2 ? "BLUE WINS"
                                             : "DRAW";
    const std::uint32_t elapsed_ms = (now_ms > s.end_at_ms)
        ? now_ms - s.end_at_ms : 0u;
    const int countdown = std::max(0, 10 - int(elapsed_ms / 1000u));
    std::snprintf(buf, sizeof(buf), "%s  —  next map in %ds", who, countdown);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 vp{float(viewport_w), float(viewport_h)};
    const ImVec2 sz = ImGui::CalcTextSize(buf);
    const ImVec2 anchor{(vp.x - sz.x) * 0.5f, vp.y * 0.30f};
    dl->AddRectFilled({anchor.x - 20.0f, anchor.y - 12.0f},
                      {anchor.x + sz.x + 20.0f, anchor.y + sz.y + 12.0f},
                      IM_COL32(10, 10, 20, 220));
    dl->AddRect      ({anchor.x - 20.0f, anchor.y - 12.0f},
                      {anchor.x + sz.x + 20.0f, anchor.y + sz.y + 12.0f},
                      IM_COL32(255, 255, 255, 200));
    dl->AddText(anchor, IM_COL32(250, 240, 200, 255), buf);
}

}  // namespace open_siege
