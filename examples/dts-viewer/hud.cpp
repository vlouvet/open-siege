#include "hud.hpp"
#include "player_controller.hpp"

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

// ---- Track 13 spec 01 — 2D screen-space HUD ----
namespace
{

GLuint g_hud2d_prog = 0;
GLuint g_hud2d_vao  = 0;
GLuint g_hud2d_vbo  = 0;
GLint  g_hud2d_u_color = -1;

const char* HUD2D_VS = R"(
#version 410 core
layout(location = 0) in vec2 a_pos;   // NDC-space quad vertices
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }
)";

const char* HUD2D_FS = R"(
#version 410 core
uniform vec3 u_color;
out vec4 frag;
void main() { frag = vec4(u_color, 1.0); }
)";

GLuint compile_simple(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

GLuint link_simple(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    return p;
}

void ensure_init()
{
    if (g_hud2d_prog) return;
    GLuint vs = compile_simple(GL_VERTEX_SHADER,   HUD2D_VS);
    GLuint fs = compile_simple(GL_FRAGMENT_SHADER, HUD2D_FS);
    g_hud2d_prog = link_simple(vs, fs);
    g_hud2d_u_color = glGetUniformLocation(g_hud2d_prog, "u_color");
    glGenVertexArrays(1, &g_hud2d_vao);
    glBindVertexArray(g_hud2d_vao);
    glGenBuffers(1, &g_hud2d_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_hud2d_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
}

void draw_quad_ndc(float x0, float y0, float x1, float y1,
                   float r, float g, float b)
{
    const float verts[] = {
        x0, y0,   x1, y0,   x1, y1,
        x0, y0,   x1, y1,   x0, y1,
    };
    glBindVertexArray(g_hud2d_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_hud2d_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glUniform3f(g_hud2d_u_color, r, g, b);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void draw_lines_ndc(const float* verts, int n_pairs,
                    float r, float g, float b)
{
    glBindVertexArray(g_hud2d_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_hud2d_vbo);
    glBufferData(GL_ARRAY_BUFFER, n_pairs * 2 * 2 * sizeof(float),
                 verts, GL_DYNAMIC_DRAW);
    glUniform3f(g_hud2d_u_color, r, g, b);
    glDrawArrays(GL_LINES, 0, n_pairs * 2);
}

} // anonymous namespace

void hud2d_init()
{
    ensure_init();
}

void hud2d_shutdown()
{
    if (g_hud2d_prog) { glDeleteProgram(g_hud2d_prog); g_hud2d_prog = 0; }
    if (g_hud2d_vbo)  { glDeleteBuffers(1, &g_hud2d_vbo);  g_hud2d_vbo  = 0; }
    if (g_hud2d_vao)  { glDeleteVertexArrays(1, &g_hud2d_vao); g_hud2d_vao = 0; }
}

void hud2d_render(const PlayerState& ps,
                  const PlayerTuning& tune,
                  int viewport_w,
                  int viewport_h)
{
    if (viewport_w <= 0 || viewport_h <= 0) return;
    ensure_init();
    if (!g_hud2d_prog) return;

    // Helper: pixel -> NDC.
    auto pxX = [&](float x) { return (x / viewport_w) * 2.0f - 1.0f; };
    auto pxY = [&](float y) { return 1.0f - (y / viewport_h) * 2.0f; };

    GLboolean depth = GL_FALSE; glGetBooleanv(GL_DEPTH_TEST, &depth);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(g_hud2d_prog);

    // ---- Crosshair (4 short line segments at screen centre) ----
    const float cx = viewport_w * 0.5f;
    const float cy = viewport_h * 0.5f;
    const float arm = 6.0f;        // pixels
    const float gap = 3.0f;
    const float ch_verts[] = {
        pxX(cx - arm - gap), pxY(cy),
        pxX(cx - gap),       pxY(cy),
        pxX(cx + gap),       pxY(cy),
        pxX(cx + arm + gap), pxY(cy),
        pxX(cx),             pxY(cy - arm - gap),
        pxX(cx),             pxY(cy - gap),
        pxX(cx),             pxY(cy + gap),
        pxX(cx),             pxY(cy + arm + gap),
    };
    draw_lines_ndc(ch_verts, 4, 1.0f, 1.0f, 1.0f);

    // ---- Health bar (bottom-left, red) ----
    const float bar_w = 200.0f, bar_h = 14.0f, bar_x = 12.0f;
    const float bar_y_base = viewport_h - 20.0f;
    float health_frac = (tune.eye_height > 0.0f && ps.health_max > 0.0f)
        ? (ps.health / ps.health_max) : 0.0f;
    health_frac = std::max(0.0f, std::min(1.0f, health_frac));
    draw_quad_ndc(pxX(bar_x),                pxY(bar_y_base),
                  pxX(bar_x + bar_w),        pxY(bar_y_base - bar_h),
                  0.15f, 0.05f, 0.05f);        // background
    draw_quad_ndc(pxX(bar_x),                pxY(bar_y_base),
                  pxX(bar_x + bar_w * health_frac),
                  pxY(bar_y_base - bar_h),
                  0.85f, 0.20f, 0.20f);        // fill

    // ---- Energy/jet bar ----
    const float ebar_y = bar_y_base - bar_h - 6.0f;
    float fuel_frac = (tune.jet_fuel_max > 0.0f)
        ? (ps.jet_fuel / tune.jet_fuel_max) : 0.0f;
    fuel_frac = std::max(0.0f, std::min(1.0f, fuel_frac));
    draw_quad_ndc(pxX(bar_x),                pxY(ebar_y),
                  pxX(bar_x + bar_w),        pxY(ebar_y - bar_h),
                  0.05f, 0.05f, 0.15f);
    draw_quad_ndc(pxX(bar_x),                pxY(ebar_y),
                  pxX(bar_x + bar_w * fuel_frac),
                  pxY(ebar_y - bar_h),
                  0.20f, 0.40f, 0.90f);

    // ---- Ammo bar (active weapon only) ----
    const auto& w = active_weapon(ps.inventory);
    const float abar_y = ebar_y - bar_h - 6.0f;
    float ammo_frac = (w.max_ammo > 0)
        ? (static_cast<float>(w.ammo) / w.max_ammo) : 0.0f;
    draw_quad_ndc(pxX(bar_x),                pxY(abar_y),
                  pxX(bar_x + bar_w),        pxY(abar_y - bar_h),
                  0.15f, 0.10f, 0.02f);
    draw_quad_ndc(pxX(bar_x),                pxY(abar_y),
                  pxX(bar_x + bar_w * ammo_frac),
                  pxY(abar_y - bar_h),
                  0.95f, 0.80f, 0.10f);

    // ---- Slot indicator squares (one per equipped weapon) ----
    const float slot_size = 12.0f;
    const float slot_y    = abar_y - bar_h - 8.0f;
    float sx = bar_x;
    for (std::size_t i = 0; i < ps.inventory.weapons.size(); ++i) {
        const auto& sw = ps.inventory.weapons[i];
        if (!sw.equipped) continue;
        const bool active = (static_cast<int>(i) == ps.inventory.active_slot);
        float r = sw.ammo > 0 ? 0.7f : 0.3f;
        float gg = sw.ammo > 0 ? 0.7f : 0.3f;
        float bb = active     ? 0.1f : 0.5f;
        if (active) { r = 1.0f; gg = 0.9f; bb = 0.2f; }
        draw_quad_ndc(pxX(sx), pxY(slot_y),
                      pxX(sx + slot_size), pxY(slot_y - slot_size),
                      r, gg, bb);
        sx += slot_size + 4.0f;
    }

    glBindVertexArray(0);
    if (depth) glEnable(GL_DEPTH_TEST);
}

} // namespace dts_viewer
