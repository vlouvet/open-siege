#ifndef OPEN_SIEGE_T1_CLIENT_IMGUI_HPP
#define OPEN_SIEGE_T1_CLIENT_IMGUI_HPP

// Spec 29/05b — slim ImGui lifecycle wrapper for open-siege-t1-client.
// Stripped down from examples/dts-viewer/imgui_layer.cpp: no menu bar,
// no editor panels, just the SDL2 + OpenGL3 backends + per-frame
// NewFrame/Render so HUD text via text_renderer's foreground draw list
// actually flushes.

#include <SDL.h>

namespace open_siege {

void imgui_init(SDL_Window* window, SDL_GLContext ctx);
void imgui_shutdown();

// Forward an SDL event to ImGui. Returns true if ImGui captured it
// (keyboard typed into a text field, mouse over a popup) — the host
// should skip its own dispatch when true.
bool imgui_process_event(const SDL_Event& ev);

// Per-frame bookkeeping. Call begin_frame after SDL event pump and
// before HUD drawing; call end_frame after HUD drawing and before
// SDL_GL_SwapWindow.
void imgui_begin_frame();
void imgui_end_frame();

}  // namespace open_siege

#endif  // OPEN_SIEGE_T1_CLIENT_IMGUI_HPP
