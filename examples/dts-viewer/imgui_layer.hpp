#pragma once

// Spec 25/01 — thin wrapper around Dear ImGui + SDL2 / OpenGL3 backends.
// One call from the SDL setup, one call per frame, one shutdown.

#include <SDL.h>

namespace dts_viewer {

void imgui_init(SDL_Window* window, SDL_GLContext ctx);
void imgui_shutdown();

// Forward an SDL event to ImGui. Returns true if ImGui has captured the
// event (i.e. the host should not act on it — e.g. keyboard goes to a
// text field, mouse goes to a menu). Always call BEFORE the host's own
// event handling.
bool imgui_process_event(const SDL_Event& ev);

// Frame bookkeeping. begin_frame() prepares a new ImGui frame and runs
// the UI build. end_frame() submits draw lists to GL — call after all
// the host's 3D rendering and before SDL_GL_SwapWindow.
void imgui_begin_frame();
void imgui_end_frame();

} // namespace dts_viewer
