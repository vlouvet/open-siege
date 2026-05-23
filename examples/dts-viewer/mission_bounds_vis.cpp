// dts-viewer-side GL overlay for MissionBounds (track 26 spec 01 split).

#include "mission_bounds_vis.hpp"

#include <glm/gtc/type_ptr.hpp>

namespace dts_viewer
{

void draw_bounds_debug(
    const MissionBounds& b,
    GLint u_mvp_loc,
    GLint u_color_loc,
    const glm::mat4& view_proj)
{
    static GLuint vao = 0, vbo = 0;
    if (vao == 0) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
    }

    const float y = b.world_min[1] + 1.0f;
    const float verts[] = {
        b.world_min[0], y, b.world_min[2],
        b.world_max[0], y, b.world_min[2],
        b.world_max[0], y, b.world_max[2],
        b.world_min[0], y, b.world_max[2],
    };

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);

    if (u_mvp_loc >= 0)
        glUniformMatrix4fv(u_mvp_loc, 1, GL_FALSE, glm::value_ptr(view_proj));
    if (u_color_loc >= 0)
        glUniform3f(u_color_loc, 1.0f, 0.85f, 0.2f);

    glDrawArrays(GL_LINE_LOOP, 0, 4);

    glBindVertexArray(0);
}

} // namespace dts_viewer
