#ifndef OSENGINE_MISSION_BOUNDS_HPP
#define OSENGINE_MISSION_BOUNDS_HPP

// Mission playable-area bounds — Spec 06 (07-mission track).
//
// Combines the terrain's quad-grid extent (authoritative for X/Z playable
// area) with `node_sim_terrain` fog/visibility values to recommend a far
// plane. `MissionCenterPos` is a minimap rectangle, not a physics bound;
// it's NOT used for the hard X/Z clamp here.
//
// Y axis is treated separately: world_min.y is the terrain heightmap
// minimum; world_max.y is the recommended_far_plane scaled by 0.5 (so the
// camera can climb high enough to see the whole map but not fly to infinity).
//
// 2026-05-23 (track 26 spec 01): moved here from
// examples/dts-viewer/mission_bounds.hpp; the GL `draw_bounds_debug` helper
// split to apps/dts-viewer/src/mission_bounds_vis.cpp.

#include "content/mission/scene.hpp"

#include <array>

namespace dts_viewer
{

struct MissionBounds
{
    std::array<float, 3> world_min{ 0.0f, 0.0f, 0.0f };
    std::array<float, 3> world_max{ 0.0f, 0.0f, 0.0f };
    float recommended_far_plane = 5000.0f;
    float horizontal_radius     = 1024.0f;   // for spherical clamp variants
};

// Compute bounds from a parsed scene graph and the terrain side length.
//   terrain_metres_per_side = (size+1) * metres_per_quad
//   terrain_y_min/max       = observed range from heightmap bbox
//   terrain_world_origin_x/z = world-space origin of the rendered tile
//     (mission mode passes -half_size so bounds center at origin).
MissionBounds compute_bounds(
    const studio::content::mission::scene_graph& scene,
    float terrain_metres_per_side,
    float terrain_y_min,
    float terrain_y_max,
    float terrain_world_origin_x = 0.0f,
    float terrain_world_origin_z = 0.0f);

// Clamp a position to the box.  Optional out-param flags whether any
// component changed.
std::array<float, 3> clamp_to_bounds(
    const std::array<float, 3>& pos,
    const MissionBounds& b,
    bool* did_clamp_out = nullptr);

} // namespace dts_viewer

#endif // OSENGINE_MISSION_BOUNDS_HPP
