#ifndef DTS_VIEWER_TEXT_RENDERER_HPP
#define DTS_VIEWER_TEXT_RENDERER_HPP

// Spec 13/07 — HUD text rendering. Wraps Dear ImGui's foreground draw list
// to put real glyphs at screen-pixel positions, so HUD widgets can show
// "Health: 75/100" instead of coloured-bar placeholders. Uses ImGui's
// built-in Proggy font (13 px native, scales linearly).
//
// Why ImGui rather than a fresh stb_truetype + bundled TTF: ImGui is
// already in the build (spec 25/01), already has a font atlas uploaded
// to GL, and ImGui's draw-list flush at imgui_end_frame() runs AFTER our
// hud2d_render calls — so the text lands on top of the HUD bars without
// any extra GL state management on our side.

#include <string>

namespace dts_viewer
{

struct TextColor { float r; float g; float b; float a = 1.0f; };

// Draw `text` at screen pixel position (x_px, y_px). y grows downward (so
// y=0 is the top of the viewport). `size_px` is the cap height in pixels;
// 13 matches the ImGui default font's native size.
void text_draw(float x_px, float y_px,
               const std::string& text,
               TextColor color,
               float size_px = 13.0f);

// Convenience: estimate the width of `text` rendered at `size_px`. Useful
// for right-aligning numeric readouts on top of a bar.
float text_measure_width(const std::string& text, float size_px = 13.0f);

} // namespace dts_viewer

#endif // DTS_VIEWER_TEXT_RENDERER_HPP
