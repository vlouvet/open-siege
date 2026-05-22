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
#include "gl_includes.hpp"
#include <glm/glm.hpp>

#include <array>
#include <deque>
#include <string>
#include <vector>

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

// Track 13 spec 02 — compass band at the top of the screen.  Cardinal
// ticks scroll as yaw rotates; small ticks mark teammate bearings.
struct CompassTick { glm::vec3 world_pos; std::array<float, 3> color; };
void hud2d_render_compass(float yaw,
                          const std::vector<CompassTick>& teammates,
                          const glm::vec3& player_pos,
                          int viewport_w,
                          int viewport_h);

// Track 13 spec 03 — sensor radar (top-right circle).  Renders the
// player's local field as concentric range rings + tick blips for
// each entity within `range`.
struct SensorBlip { glm::vec3 world_pos; std::array<float, 3> color; };
void hud2d_render_sensor(float yaw,
                         const glm::vec3& player_pos,
                         const std::vector<SensorBlip>& blips,
                         float range,
                         int viewport_w,
                         int viewport_h);

// Track 13 spec 04 — objective/message text feed.  No text renderer in
// v1, so each message is shown as a coloured horizontal bar; older
// messages fade.  The shared `std::deque<std::string>` from Track 14
// spec 06 (trigger feed) is the input.
void hud2d_render_message_feed(const std::deque<std::string>& msgs,
                               int viewport_w,
                               int viewport_h);

// Track 13 spec 05 — damage-direction reticle.  Caller registers each
// incoming damage with `hud2d_report_damage(direction_from_attacker)`;
// the chevron fades over `damage_fade_seconds`.
void hud2d_report_damage(float yaw_to_attacker);
void hud2d_tick(float dt);    // ages reticle chevrons
void hud2d_render_damage_reticle(int viewport_w, int viewport_h);

// Track 13 spec 06 — command-map overlay.  Toggle with M.  Renders a
// top-down view: heightmap as grayscale aerial (cached after first
// build) + icons for each tracked entity.  The icon list mirrors the
// CompassTick / SensorBlip shapes so callers can reuse data.
struct MapIcon { glm::vec3 world_pos; std::array<float, 3> color; };
void hud2d_render_command_map(const float* heightmap,    // row-major (size+1)^2
                              int size_plus_one,
                              float metres_per_quad,
                              const std::vector<MapIcon>& icons,
                              const glm::vec3& player_pos,
                              float player_yaw,
                              int viewport_w,
                              int viewport_h,
                              float world_origin_x = 0.0f,
                              float world_origin_z = 0.0f);

// Spec 13/07 — text overlays driven by the script-side bindings
// (HudBindingsState from spec 17/03). Renders the objective list in
// the top-right corner, a centerPrint banner in the middle, and a
// bottomPrint band above the health bar. No bars / no placeholders —
// all real text via the ImGui draw list.
struct HudBindingsState;
void hud2d_render_script_text(const HudBindingsState& s,
                              int viewport_w,
                              int viewport_h);

} // namespace dts_viewer

#endif // DTS_VIEWER_HUD_HPP
