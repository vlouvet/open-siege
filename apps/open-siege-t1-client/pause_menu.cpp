#include "pause_menu.hpp"

#include <imgui.h>

#include <array>
#include <cstdio>

namespace open_siege {

namespace {

constexpr std::array<const char*, 3> kLabels = {
    "Resume", "Disconnect", "Quit",
};

}  // namespace

void toggle(PauseMenuState& s) { s.visible = !s.visible; }

PauseAction handle_key(PauseMenuState& s, SDL_Keycode key)
{
    if (!s.visible) return PauseAction::None;
    if (key == SDLK_UP) {
        s.highlighted = (s.highlighted + int(kLabels.size()) - 1)
                        % int(kLabels.size());
        return PauseAction::None;
    }
    if (key == SDLK_DOWN) {
        s.highlighted = (s.highlighted + 1) % int(kLabels.size());
        return PauseAction::None;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        switch (s.highlighted) {
            case 0: s.visible = false; return PauseAction::Resume;
            case 1:
                s.visible      = false;
                s.disconnected = true;
                return PauseAction::Disconnect;
            case 2: return PauseAction::Quit;
        }
    }
    if (key == SDLK_ESCAPE) {
        // Second Esc cancels back to gameplay.
        s.visible = false;
        return PauseAction::Resume;
    }
    return PauseAction::None;
}

void draw(const PauseMenuState& s,
          int viewport_w,
          int viewport_h)
{
    if (!s.visible) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float vw = float(viewport_w);
    const float vh = float(viewport_h);
    dl->AddRectFilled({0.0f, 0.0f}, {vw, vh}, IM_COL32(0, 0, 0, 110));

    const float box_w = 320.0f;
    const float row_h = 32.0f;
    const float box_h = 28.0f + row_h * float(kLabels.size()) + 12.0f;
    const ImVec2 tl{(vw - box_w) * 0.5f, (vh - box_h) * 0.5f};
    const ImVec2 br{tl.x + box_w, tl.y + box_h};
    dl->AddRectFilled(tl, br, IM_COL32(20, 20, 30, 230));
    dl->AddRect      (tl, br, IM_COL32(255, 255, 255, 200));

    dl->AddText({tl.x + 14.0f, tl.y + 8.0f},
                IM_COL32(255, 255, 255, 240), "PAUSED");

    float ry = tl.y + 36.0f;
    for (std::size_t i = 0; i < kLabels.size(); ++i) {
        const bool sel = (int(i) == s.highlighted);
        if (sel) {
            dl->AddRectFilled({tl.x + 6.0f, ry - 4.0f},
                              {br.x - 6.0f, ry + row_h - 8.0f},
                              IM_COL32(60, 100, 180, 200));
        }
        dl->AddText({tl.x + 24.0f, ry},
                    sel ? IM_COL32(255, 255, 255, 255)
                        : IM_COL32(200, 200, 200, 220),
                    kLabels[i]);
        ry += row_h;
    }
}

void draw_disconnected_banner(const PauseMenuState& s,
                              int viewport_w,
                              int viewport_h)
{
    if (!s.disconnected) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float vw = float(viewport_w);
    const float vh = float(viewport_h);
    const char* msg = "Disconnected from server (Esc for menu, Quit to exit)";
    const ImVec2 sz = ImGui::CalcTextSize(msg);
    const ImVec2 anchor{(vw - sz.x) * 0.5f, vh * 0.50f};
    dl->AddRectFilled({anchor.x - 12.0f, anchor.y - 6.0f},
                      {anchor.x + sz.x + 12.0f, anchor.y + sz.y + 6.0f},
                      IM_COL32(40, 40, 50, 200));
    dl->AddText(anchor, IM_COL32(240, 240, 240, 240), msg);
}

}  // namespace open_siege
