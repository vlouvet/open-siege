#ifndef OPEN_SIEGE_T1_CLIENT_PAUSE_MENU_HPP
#define OPEN_SIEGE_T1_CLIENT_PAUSE_MENU_HPP

// Spec 29/09 — Esc-driven pause overlay.
//
// Pure local UI; the server continues to tick. Menu options:
//   * Resume      — close overlay
//   * Disconnect  — stop NetClient; client falls back to idle render
//   * Quit        — push SDL_QUIT

#include <SDL.h>

namespace open_siege {

enum class PauseAction { None, Resume, Disconnect, Quit };

struct PauseMenuState {
    bool visible      = false;
    int  highlighted  = 0;       // 0=Resume, 1=Disconnect, 2=Quit
    bool disconnected = false;   // sticky after Disconnect was chosen
};

// Toggle visibility (Esc rising edge).
void toggle(PauseMenuState& s);

// Feed an SDL_KEYDOWN while visible. Returns the action triggered
// (PauseAction::None for navigation-only keys).
PauseAction handle_key(PauseMenuState& s, SDL_Keycode key);

// Per-frame draw between imgui_begin_frame / imgui_end_frame.
void draw(const PauseMenuState& s,
          int viewport_w,
          int viewport_h);

// Bottom-center "Disconnected from server" banner while idle.
void draw_disconnected_banner(const PauseMenuState& s,
                              int viewport_w,
                              int viewport_h);

}  // namespace open_siege

#endif  // OPEN_SIEGE_T1_CLIENT_PAUSE_MENU_HPP
