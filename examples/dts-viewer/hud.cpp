#include "hud.hpp"

#include <SDL.h>
#include <cstdio>
#include <variant>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace dts_viewer
{

void refresh_hud_window_title(
    SDL_Window* win,
    const HudState& hud,
    const Camera& cam,
    CameraMode mode,
    const std::string& mission_name)
{
    if (!win || !hud.visible) return;
    char buf[256];
    const float yaw_deg   = cam.yaw   * (180.0f / 3.14159265f);
    const float pitch_deg = cam.pitch * (180.0f / 3.14159265f);
    std::snprintf(buf, sizeof(buf),
        "dts-viewer | %s | %s | (%.1f,%.1f,%.1f) yaw=%.0f pitch=%.0f | %.1f fps",
        mission_name.empty() ? "(no mission)" : mission_name.c_str(),
        mode == CameraMode::Walk ? "walk" : "free",
        cam.position.x, cam.position.y, cam.position.z,
        yaw_deg, pitch_deg, hud.fps_smoothed);
    SDL_SetWindowTitle(win, buf);
}

void print_hud_snapshot(
    const HudState& hud,
    const Camera& cam,
    CameraMode mode,
    const std::string& mission_name)
{
    const float yaw_deg   = cam.yaw   * (180.0f / 3.14159265f);
    const float pitch_deg = cam.pitch * (180.0f / 3.14159265f);
    std::fprintf(stderr,
        "HUD | mission=%s | mode=%s | pos=(%.1f,%.1f,%.1f) yaw=%.0f pitch=%.0f | fps=%.1f | layers: ter=%d int=%d mark=%d sky=%d\n",
        mission_name.empty() ? "(none)" : mission_name.c_str(),
        mode == CameraMode::Walk ? "walk" : "free",
        cam.position.x, cam.position.y, cam.position.z,
        yaw_deg, pitch_deg, hud.fps_smoothed,
        hud.show_terrain ? 1 : 0,
        hud.show_interiors ? 1 : 0,
        hud.show_markers ? 1 : 0,
        hud.show_sky ? 1 : 0);
}

namespace
{

// 1 m cube, centred at origin.  12 triangles emitted as GL_LINES from a
// shared VBO (just 24 edge vertices) — cheaper than triangles and lets
// markers stay visible against any backdrop.
const float kCubeEdges[] = {
    // bottom
    -0.5f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,
     0.5f, -0.5f, -0.5f,   0.5f, -0.5f,  0.5f,
     0.5f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,  -0.5f, -0.5f, -0.5f,
    // top
    -0.5f,  0.5f, -0.5f,   0.5f,  0.5f, -0.5f,
     0.5f,  0.5f, -0.5f,   0.5f,  0.5f,  0.5f,
     0.5f,  0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,  -0.5f,  0.5f, -0.5f,
    // verticals
    -0.5f, -0.5f, -0.5f,  -0.5f,  0.5f, -0.5f,
     0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,
     0.5f, -0.5f,  0.5f,   0.5f,  0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,  -0.5f,  0.5f,  0.5f,
};

} // anonymous namespace

void draw_markers_debug(
    const studio::content::mission::scene_graph& scene,
    GLint u_mvp_loc,
    GLint u_color_loc,
    const glm::mat4& view_proj)
{
    using namespace studio::content::mission;

    static GLuint vao = 0, vbo = 0;
    if (vao == 0) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeEdges), kCubeEdges, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    }
    glBindVertexArray(vao);

    auto draw_at = [&](const transform& xf, const std::array<float, 3>& color, float scale) {
        glm::mat4 M = glm::translate(glm::mat4(1.0f),
            glm::vec3(xf.position[0], xf.position[1], xf.position[2]));
        M = glm::scale(M, glm::vec3(scale));
        glm::mat4 MVP = view_proj * M;
        if (u_mvp_loc >= 0)
            glUniformMatrix4fv(u_mvp_loc, 1, GL_FALSE, glm::value_ptr(MVP));
        if (u_color_loc >= 0)
            glUniform3fv(u_color_loc, 1, color.data());
        glDrawArrays(GL_LINES, 0, 24);
    };

    auto walk = [&](auto& self, const scene_node& n) -> void {
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, node_marker>) {
                std::array<float, 3> c{ 1.0f, 1.0f, 1.0f };
                float s = 2.0f;
                if (p.data_block.name == "DropPointMarker") c = { 0.2f, 0.9f, 0.2f };
                else if (p.data_block.name == "PathMarker") c = { 0.2f, 0.4f, 0.9f };
                else if (p.data_block.name == "MapMarker")  c = { 0.9f, 0.9f, 0.2f };
                draw_at(p.xf, c, s);
            }
            else if constexpr (std::is_same_v<T, node_static_shape>) {
                draw_at(p.xf, { 0.8f, 0.4f, 0.1f }, 1.0f);
            }
            else if constexpr (std::is_same_v<T, node_item>) {
                draw_at(p.xf, { 1.0f, 0.2f, 1.0f }, 1.0f);
            }
            else if constexpr (std::is_same_v<T, node_interior>) {
                draw_at(p.xf, { 0.5f, 0.5f, 0.5f }, 3.0f);
            }
            else if constexpr (std::is_same_v<T, node_turret>) {
                draw_at(p.xf, { 0.9f, 0.1f, 0.1f }, 2.0f);
            }
        }, n.payload);
        for (auto& c : n.children) self(self, c);
    };
    walk(walk, scene.root);

    glBindVertexArray(0);
}

} // namespace dts_viewer
