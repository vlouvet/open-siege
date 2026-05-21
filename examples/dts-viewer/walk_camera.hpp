#ifndef DTS_VIEWER_WALK_CAMERA_HPP
#define DTS_VIEWER_WALK_CAMERA_HPP

// Walk-mode camera extension — Spec 02 (08-walkable-viewer track).
//
// Glues the free-fly Camera (camera.hpp) onto the terrain heightmap so the
// eye stays at terrain_height + kEyeHeight.  WASD slides horizontally;
// Space/Ctrl are ignored.  Designed to be called instead of
// update_camera_free when the user has Tab'd into walk mode.

#include "camera.hpp"
#include "height_sampler.hpp"
#include "mission_bounds.hpp"
#include "content/mission/scene.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <variant>

namespace dts_viewer
{

enum class CameraMode { Free, Walk };

constexpr float kEyeHeight = 1.8f;

// One-shot snap: place the camera at terrain + eye height for its current XZ.
inline void snap_camera_to_terrain(Camera& c, const HeightSampler& terrain)
{
    if (!terrain.valid()) return;
    c.position.y = terrain.sample(c.position.x, c.position.z) + kEyeHeight;
}

// Move horizontally with WASD; ignore vertical input; snap to terrain after.
inline void update_camera_walk(
    Camera& c,
    float dt,
    const HeightSampler& terrain,
    const MissionBounds& bounds)
{
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    bool sprint = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    float speed = c.move_speed * (sprint ? c.sprint_mult : 1.0f) * dt;

    glm::vec3 fwd_h(std::sin(c.yaw), 0.0f, std::cos(c.yaw));
    glm::vec3 right_h(std::cos(c.yaw), 0.0f, -std::sin(c.yaw));

    if (keys[SDL_SCANCODE_W]) c.position += fwd_h   * speed;
    if (keys[SDL_SCANCODE_S]) c.position -= fwd_h   * speed;
    if (keys[SDL_SCANCODE_A]) c.position -= right_h * speed;
    if (keys[SDL_SCANCODE_D]) c.position += right_h * speed;

    // FOV adjust still works in walk mode (helpful for screenshots).
    if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS])
        c.fov_deg = glm::clamp(c.fov_deg + 30.0f * dt, 40.0f, 110.0f);
    if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS])
        c.fov_deg = glm::clamp(c.fov_deg - 30.0f * dt, 40.0f, 110.0f);

    // Clamp horizontally to mission bounds (XZ only).  Y is owned by the
    // PlayerState physics integrator once Track 09 spec 02 lands; this
    // function intentionally no longer snaps Y.
    std::array<float, 3> p{ c.position.x, c.position.y, c.position.z };
    p = clamp_to_bounds(p, bounds);
    c.position.x = p[0];
    c.position.z = p[2];
}

// Linear scan over scene_graph for nearest Marker with dataBlock "DropPointMarker".
// Returns std::nullopt if none found.
inline std::optional<std::array<float, 3>> nearest_drop_point(
    const studio::content::mission::scene_graph& scene,
    const std::array<float, 3>& near_pos)
{
    using namespace studio::content::mission;

    std::optional<std::array<float, 3>> best;
    float best_d2 = 1e30f;

    auto consider = [&](const transform& xf) {
        float dx = xf.position[0] - near_pos[0];
        float dy = xf.position[1] - near_pos[1];
        float dz = xf.position[2] - near_pos[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = xf.position;
        }
    };

    auto walk = [&](auto& self, const scene_node& n) -> void {
        if (auto* m = std::get_if<node_marker>(&n.payload)) {
            if (m->data_block.name == "DropPointMarker") {
                consider(m->xf);
            }
        }
        for (auto& c : n.children) self(self, c);
    };
    walk(walk, scene.root);
    return best;
}

} // namespace dts_viewer

#endif // DTS_VIEWER_WALK_CAMERA_HPP
