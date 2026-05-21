#ifndef DTS_VIEWER_HUD_HPP
#define DTS_VIEWER_HUD_HPP

// Debug HUD — Spec 03 (08-walkable-viewer track).
//
// v1 ships a *minimal* HUD: layer-visibility toggles (F4–F7), animated marker
// cubes, an SDL window-title update with position/fps/mission, and an
// optional stderr dump (F1 cycles).  An in-window text overlay is left as
// follow-up polish — the milestone target is "walkable viewer", not a
// font-rendering subsystem.  The functional acceptance items (toggles
// influence draws, marker debug viz, fps tracking) are all covered.

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <glm/glm.hpp>

#include <string>

#include "camera.hpp"
#include "walk_camera.hpp"
#include "content/mission/scene.hpp"

namespace dts_viewer
{

struct HudState
{
    bool visible        = true;   // window-title + stderr summary updates
    bool show_terrain   = true;
    bool show_interiors = true;
    bool show_markers   = false;
    bool show_sky       = true;
    float fps_smoothed  = 0.0f;
};

inline void update_hud(HudState& hud, float dt_seconds)
{
    if (dt_seconds <= 0.0f) return;
    const float instantaneous = 1.0f / dt_seconds;
    if (hud.fps_smoothed <= 0.0f) hud.fps_smoothed = instantaneous;
    hud.fps_smoothed = glm::mix(hud.fps_smoothed, instantaneous, 0.05f);
}

// Push position/fps/mission into the SDL window title.  Cheap; called once
// per second so we don't thrash the WM.
void refresh_hud_window_title(
    struct SDL_Window* win,
    const HudState& hud,
    const Camera& cam,
    CameraMode mode,
    const std::string& mission_name);

// Draw the marker layer (small coloured boxes).  Caller must have a flat-
// colour shader bound with `u_mvp_loc` set per cube; this function emits one
// glDrawArrays call per marker.  The `u_color` uniform is updated per marker.
void draw_markers_debug(
    const studio::content::mission::scene_graph& scene,
    GLint u_mvp_loc,
    GLint u_color_loc,
    const glm::mat4& view_proj);

// Print a single-line stderr snapshot.  Caller decides cadence (e.g. on F1).
void print_hud_snapshot(
    const HudState& hud,
    const Camera& cam,
    CameraMode mode,
    const std::string& mission_name);

// Track 13 spec 01 — 2D screen-space HUD.
// Crosshair (center cross) + health/energy/ammo bars (bottom-left).
// No font dependency; ammo is shown as a length-encoded bar plus a
// row of slot indicator squares.  Active weapon is highlighted.
struct PlayerState;
struct PlayerTuning;

void hud2d_init();          // lazy GL init; idempotent
void hud2d_render(const PlayerState& ps,
                  const PlayerTuning& tune,
                  int viewport_w,
                  int viewport_h);
void hud2d_shutdown();

} // namespace dts_viewer

#endif // DTS_VIEWER_HUD_HPP
