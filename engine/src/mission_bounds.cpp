// Engine-side MissionBounds compute + clamp. GL line-loop debug overlay
// split to apps/dts-viewer/mission_bounds_vis.cpp (track 26 spec 01).

#include "osengine/mission_bounds.hpp"

#include <algorithm>
#include <cmath>

namespace dts_viewer
{

MissionBounds compute_bounds(
    const studio::content::mission::scene_graph& scene,
    float terrain_metres_per_side,
    float terrain_y_min,
    float terrain_y_max,
    float terrain_world_origin_x,
    float terrain_world_origin_z)
{
    MissionBounds b;

    const float half = 0.5f * terrain_metres_per_side;
    b.world_min[0] = terrain_world_origin_x;
    b.world_min[2] = terrain_world_origin_z;
    b.world_max[0] = terrain_world_origin_x + terrain_metres_per_side;
    b.world_max[2] = terrain_world_origin_z + terrain_metres_per_side;
    b.horizontal_radius = half;

    b.world_min[1] = std::min(terrain_y_min, -50.0f);
    b.world_max[1] = std::max(terrain_y_max + 500.0f, terrain_y_min + 1000.0f);

    // Default far plane based on diagonal.
    const float diag = std::sqrt(2.0f) * terrain_metres_per_side;
    b.recommended_far_plane = std::clamp(1.2f * diag, 1000.0f, 10000.0f);

    if (scene.terrain) {
        const auto& fog = scene.terrain->fog;
        float visible = fog[0];
        if (std::isfinite(visible) && visible > 100.0f) {
            b.recommended_far_plane =
                std::clamp(1.2f * visible, 1000.0f, 10000.0f);
        }
    }

    return b;
}

std::array<float, 3> clamp_to_bounds(
    const std::array<float, 3>& pos,
    const MissionBounds& b,
    bool* did_clamp_out)
{
    std::array<float, 3> out = pos;
    bool clamped = false;
    for (int i = 0; i < 3; ++i) {
        if (out[i] < b.world_min[i]) { out[i] = b.world_min[i]; clamped = true; }
        if (out[i] > b.world_max[i]) { out[i] = b.world_max[i]; clamped = true; }
    }
    if (did_clamp_out) *did_clamp_out = clamped;
    return out;
}

} // namespace dts_viewer
