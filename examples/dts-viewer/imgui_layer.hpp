#pragma once

// Spec 25/01 — thin wrapper around Dear ImGui + SDL2 / OpenGL3 backends.
// Spec 25/02 — adds the top-level menu bar.

#include <SDL.h>

#include <functional>
#include <string>

namespace dts_viewer {

void imgui_init(SDL_Window* window, SDL_GLContext ctx);
void imgui_shutdown();

// Forward an SDL event to ImGui. Returns true if ImGui has captured the
// event (i.e. the host should not act on it — e.g. keyboard goes to a
// text field, mouse goes to a menu). Always call BEFORE the host's own
// event handling.
bool imgui_process_event(const SDL_Event& ev);

// Frame bookkeeping. begin_frame() prepares a new ImGui frame and runs
// the UI build (menu bar + any panels). end_frame() submits draw lists
// to GL — call after the host's 3D rendering and before SDL_GL_SwapWindow.
void imgui_begin_frame();
void imgui_end_frame();

// Spec 25/02 — menu action table. The host populates the callbacks it
// can service in the current mode (mission mode wires Reload Scripts;
// shape-viewer mode leaves it unset and the menu item is greyed out).
//
// View toggles take a pair of {is_*, toggle_*} — is_* returns the
// current state for the checkmark, toggle_* flips it. Same shape for
// the camera radio pair.
struct MenuActions
{
    // File
    std::function<void()> open_mission;
    std::function<void()> open_shape;
    std::function<void()> quit;

    // Edit
    std::function<void()> reload_scripts;
    std::function<void()> toggle_edit_mode;

    // View
    std::function<bool()> is_wireframe;     std::function<void()> toggle_wireframe;
    std::function<bool()> is_hud;           std::function<void()> toggle_hud;
    std::function<bool()> is_bbox;          std::function<void()> toggle_bbox;
    std::function<bool()> is_fps;           std::function<void()> toggle_fps;
    std::function<bool()> is_free_camera;   std::function<void()> set_free_camera;
    std::function<bool()> is_walk_camera;   std::function<void()> set_walk_camera;
    std::function<bool()> is_asset_browser; std::function<void()> toggle_asset_browser;
    std::function<bool()> is_inspector;     std::function<void()> toggle_inspector;

    // Help
    std::function<void()> help_user_guide;
    std::function<void()> help_controls;
    std::function<void()> help_scripting;
    std::function<void()> help_online_docs;
    std::function<void()> help_report_issue;
    std::function<void()> help_about;
};

void set_menu_actions(const MenuActions& a);

// Internal flag for the Asset Browser panel; the host wires the View
// menu toggle to this ref.
bool& asset_browser_visible_ref();

// Dispatch a Cmd/Ctrl-modified key as if its menu item had been clicked.
// Called by the host's SDL key handler before the host acts on the
// event, so that Cmd-O fires File > Open Mission regardless of where
// the mouse is. Returns true if an action was dispatched.
bool imgui_try_menu_shortcut(const SDL_Event& ev);

} // namespace dts_viewer
