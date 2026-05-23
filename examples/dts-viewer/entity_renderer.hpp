#ifndef DTS_VIEWER_ENTITY_RENDERER_HPP
#define DTS_VIEWER_ENTITY_RENDERER_HPP

// Track 14 entity renderer — slim post-track-26-spec-01 split. State
// structs + collect/tick declarations live in <osengine/entity_state.hpp>.
// This header keeps only the GL line-loop helper `render_entity_cube`.

#include <array>
#include <glm/glm.hpp>

#include <osengine/entity_state.hpp>

#define GL_SILENCE_DEPRECATION
#include "gl_includes.hpp"

namespace dts_viewer
{

// Draws a single coloured wireframe cube at `world_pos`, scaled by `size`.
// Caller binds a flat-color shader and sets u_mvp / u_color per draw.
void render_entity_cube(
    const glm::vec3& world_pos,
    float size,
    const std::array<float, 3>& color,
    GLint u_mvp_loc,
    GLint u_color_loc,
    const glm::mat4& view_proj);

} // namespace dts_viewer

#endif // DTS_VIEWER_ENTITY_RENDERER_HPP
