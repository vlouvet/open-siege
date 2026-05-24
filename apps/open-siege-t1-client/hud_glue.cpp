#include "hud_glue.hpp"

#include <osengine/ghost_types.hpp>
#include <osengine/net_client.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace open_siege {

namespace {

constexpr std::size_t kMessageFeedCap = 8;
constexpr float kSensorRangeMetres = 200.0f;

ImU32 team_color_u32(std::uint8_t team_raw)
{
    // 1 = red, 2 = blue, others = neutral grey. Matches the server's
    // Team enum (Spectator=0, Red=1, Blue=2).
    switch (team_raw) {
        case 1: return IM_COL32(220,  60,  60, 255);
        case 2: return IM_COL32( 60, 110, 220, 255);
        default: return IM_COL32(160, 160, 160, 255);
    }
}

}  // namespace

void push_message(HudGlueState& s, std::string line)
{
    s.message_feed.push_back(std::move(line));
    while (s.message_feed.size() > kMessageFeedCap) s.message_feed.pop_front();
}

void render_net_hud(const dts_viewer::NetClient& net,
                    HudGlueState&                ui,
                    int                          local_slot_or_neg1,
                    int                          viewport_w,
                    int                          viewport_h)
{
    const auto reg = net.snapshot_registry();

    const net20::GhostPlayer* me = nullptr;
    if (local_slot_or_neg1 >= 0) {
        auto it = reg.players.find(static_cast<std::uint16_t>(local_slot_or_neg1));
        if (it != reg.players.end()) me = &it->second;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 vp{static_cast<float>(viewport_w), static_cast<float>(viewport_h)};

    // -- Health + energy bars (bottom-left). Health derived from
    //    damage_level (1.0 = dead); energy is 0..1 direct.
    if (me) {
        const float health = std::clamp(1.0f - me->damage_level, 0.0f, 1.0f);
        const float energy = std::clamp(me->energy,              0.0f, 1.0f);

        const float bar_w = 220.0f, bar_h = 14.0f;
        const float x0 = 24.0f, y0 = vp.y - 64.0f;

        // Health
        dl->AddRectFilled({x0, y0}, {x0 + bar_w, y0 + bar_h},
                          IM_COL32(40, 40, 40, 200));
        dl->AddRectFilled({x0, y0}, {x0 + bar_w * health, y0 + bar_h},
                          me->dead ? IM_COL32(120, 120, 120, 220)
                                   : IM_COL32(80, 200, 80, 220));
        dl->AddRect({x0, y0}, {x0 + bar_w, y0 + bar_h},
                    IM_COL32(255, 255, 255, 200));
        char hbuf[32]; std::snprintf(hbuf, sizeof(hbuf), "HP %3d",
                                     int(std::lround(health * 100.0f)));
        dl->AddText({x0 + 6.0f, y0 - 2.0f}, IM_COL32(255, 255, 255, 230), hbuf);

        // Energy
        const float ey = y0 + bar_h + 6.0f;
        dl->AddRectFilled({x0, ey}, {x0 + bar_w, ey + bar_h},
                          IM_COL32(40, 40, 40, 200));
        dl->AddRectFilled({x0, ey}, {x0 + bar_w * energy, ey + bar_h},
                          IM_COL32(80, 150, 230, 220));
        dl->AddRect({x0, ey}, {x0 + bar_w, ey + bar_h},
                    IM_COL32(255, 255, 255, 200));
        char ebuf[32]; std::snprintf(ebuf, sizeof(ebuf), "EN %3d",
                                     int(std::lround(energy * 100.0f)));
        dl->AddText({x0 + 6.0f, ey - 2.0f}, IM_COL32(255, 255, 255, 230), ebuf);
    }

    // -- Compass strip (top-center). Each other player is a tick mark
    //    whose horizontal position is the bearing relative to the local
    //    player's yaw. Width covers ±90° in front of the camera.
    {
        const float band_w = 360.0f, band_h = 22.0f;
        const float bx = (vp.x - band_w) * 0.5f, by = 12.0f;
        dl->AddRectFilled({bx, by}, {bx + band_w, by + band_h},
                          IM_COL32(20, 20, 20, 180));
        dl->AddRect({bx, by}, {bx + band_w, by + band_h},
                    IM_COL32(255, 255, 255, 160));

        // Cardinal ticks every 30° in the ±90° window
        const float my_yaw = me ? me->yaw : 0.0f;
        for (int deg = -90; deg <= 90; deg += 30) {
            const float frac = (deg + 90.0f) / 180.0f;
            const float x    = bx + frac * band_w;
            dl->AddLine({x, by}, {x, by + band_h},
                        IM_COL32(180, 180, 180, 200));
        }

        if (me) {
            for (const auto& kv : reg.players) {
                if (kv.first == static_cast<std::uint16_t>(local_slot_or_neg1)) continue;
                const auto& p = kv.second;
                const float dx = p.pos_x - me->pos_x;
                const float dz = p.pos_z - me->pos_z;
                const float bearing = std::atan2(dx, dz);                  // 0 = forward
                float rel = bearing - my_yaw;
                while (rel >  3.14159265f) rel -= 6.2831853f;
                while (rel < -3.14159265f) rel += 6.2831853f;
                if (std::fabs(rel) > 1.5707963f) continue;                 // off-band
                const float frac = (rel + 1.5707963f) / 3.1415927f;
                const float x    = bx + frac * band_w;
                dl->AddRectFilled({x - 2.0f, by + 2.0f},
                                  {x + 2.0f, by + band_h - 2.0f},
                                  team_color_u32(p.base.team_id));
            }
        }
    }

    // -- Sensor radar (top-right). Circle of radius 70 px; each blip is
    //    a 3 px dot; range = 200 m.
    {
        const float r  = 70.0f;
        const ImVec2 c{vp.x - 24.0f - r, 24.0f + r};
        dl->AddCircleFilled(c, r, IM_COL32(20, 20, 20, 180), 36);
        dl->AddCircle(c, r,         IM_COL32(255, 255, 255, 160), 36);
        dl->AddCircle(c, r * 0.5f,  IM_COL32(255, 255, 255, 80),  36);
        dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, IM_COL32(255, 255, 255, 60));
        dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, IM_COL32(255, 255, 255, 60));

        if (me) {
            const float my_yaw = me->yaw;
            const float ca = std::cos(-my_yaw), sa = std::sin(-my_yaw);
            // Local-player arrow pointing up
            dl->AddTriangleFilled({c.x, c.y - 6.0f}, {c.x - 4.0f, c.y + 4.0f},
                                  {c.x + 4.0f, c.y + 4.0f},
                                  IM_COL32(240, 240, 240, 255));
            for (const auto& kv : reg.players) {
                if (kv.first == static_cast<std::uint16_t>(local_slot_or_neg1)) continue;
                const auto& p = kv.second;
                const float dx = p.pos_x - me->pos_x;
                const float dz = p.pos_z - me->pos_z;
                const float dist = std::sqrt(dx * dx + dz * dz);
                if (dist > kSensorRangeMetres) continue;
                // Rotate world (dx, dz) into player frame so forward = +y up.
                const float lx =  ca * dx - sa * dz;
                const float lz =  sa * dx + ca * dz;
                const float scale = r / kSensorRangeMetres;
                const ImVec2 pos{c.x + lx * scale, c.y - lz * scale};
                dl->AddCircleFilled(pos, 3.0f, team_color_u32(p.base.team_id));
            }
        }
    }

    // -- Message feed (bottom-left, above the bars). Each line is a
    //    semi-transparent strip of text.
    {
        const float fx = 24.0f;
        float fy = vp.y - 96.0f;
        for (auto it = ui.message_feed.rbegin(); it != ui.message_feed.rend(); ++it) {
            const ImVec2 sz = ImGui::CalcTextSize(it->c_str());
            dl->AddRectFilled({fx - 4.0f, fy - 2.0f},
                              {fx + sz.x + 4.0f, fy + sz.y + 2.0f},
                              IM_COL32(0, 0, 0, 140));
            dl->AddText({fx, fy}, IM_COL32(230, 230, 230, 230), it->c_str());
            fy -= sz.y + 4.0f;
            if (fy < 24.0f) break;
        }
    }

    // -- Scoreboard overlay (Tab toggle). Pure client-derived rows:
    //    one entry per Player ghost, with placeholder name + ghost-slot.
    if (ui.scoreboard_visible) {
        const float w = 480.0f;
        const float row_h = 22.0f;
        const float h = 60.0f + row_h * std::max<std::size_t>(reg.players.size(), 1);
        const ImVec2 tl{(vp.x - w) * 0.5f, (vp.y - h) * 0.5f};
        const ImVec2 br{tl.x + w, tl.y + h};
        dl->AddRectFilled(tl, br, IM_COL32(15, 15, 22, 220));
        dl->AddRect      (tl, br, IM_COL32(255, 255, 255, 200));

        dl->AddText({tl.x + 12.0f, tl.y + 8.0f},
                    IM_COL32(255, 255, 255, 240), "Scoreboard");

        const float hdr_y = tl.y + 32.0f;
        dl->AddText({tl.x +  12.0f, hdr_y}, IM_COL32(200, 200, 200, 220), "Slot");
        dl->AddText({tl.x +  72.0f, hdr_y}, IM_COL32(200, 200, 200, 220), "Team");
        dl->AddText({tl.x + 140.0f, hdr_y}, IM_COL32(200, 200, 200, 220), "Name");
        dl->AddText({tl.x + 340.0f, hdr_y}, IM_COL32(200, 200, 200, 220), "Dmg");
        dl->AddText({tl.x + 400.0f, hdr_y}, IM_COL32(200, 200, 200, 220), "Pos");

        float ry = hdr_y + row_h;
        for (const auto& kv : reg.players) {
            const auto& p = kv.second;
            char slot_buf[16];   std::snprintf(slot_buf, sizeof(slot_buf), "%u",
                                               unsigned(kv.first));
            const char* team_str = p.base.team_id == 1 ? "Red"
                                  : p.base.team_id == 2 ? "Blue"
                                  : "Spec";
            char name_buf[32];   std::snprintf(name_buf, sizeof(name_buf),
                                               "Player %u", unsigned(kv.first));
            char dmg_buf[16];    std::snprintf(dmg_buf, sizeof(dmg_buf), "%3d",
                                               int(std::lround(p.damage_level * 100.0f)));
            char pos_buf[40];    std::snprintf(pos_buf, sizeof(pos_buf),
                                               "%.0f,%.0f,%.0f",
                                               p.pos_x, p.pos_y, p.pos_z);
            const ImU32 col = team_color_u32(p.base.team_id);
            dl->AddText({tl.x +  12.0f, ry}, col, slot_buf);
            dl->AddText({tl.x +  72.0f, ry}, col, team_str);
            dl->AddText({tl.x + 140.0f, ry}, col, name_buf);
            dl->AddText({tl.x + 340.0f, ry}, col, dmg_buf);
            dl->AddText({tl.x + 400.0f, ry}, col, pos_buf);
            ry += row_h;
        }
    }
}

}  // namespace open_siege
