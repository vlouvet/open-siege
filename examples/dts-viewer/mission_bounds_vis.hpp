#ifndef DTS_VIEWER_MISSION_BOUNDS_VIS_HPP
#define DTS_VIEWER_MISSION_BOUNDS_VIS_HPP

// GL line-loop overlay for debug-visualising the playable bounds box.
// Split from mission_bounds.cpp in track 26 spec 01 so the engine-side
// data struct (MissionBounds + compute_bounds + clamp_to_bounds) can live
// in libosengine without dragging in GL.

#define GL_SILENCE_DEPRECATION
#include "gl_includes.hpp"
#include <glm/glm.hpp>

#include <osengine/mission_bounds.hpp>

namespace dts_viewer
{

// Draws an axis-aligned line-loop on the ground (y = world_min.y) showing
// the playable boundary. Caller must have a flat-color shader bound.
//   u_mvp_loc — uniform location of the mat4 MVP
//   u_color_loc — uniform location of a vec3 colour (may be -1)
void draw_bounds_debug(
    const MissionBounds& b,
    GLint u_mvp_loc,
    GLint u_color_loc,
    const glm::mat4& view_proj);

} // namespace dts_viewer

#endif // DTS_VIEWER_MISSION_BOUNDS_VIS_HPP
