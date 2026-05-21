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
        // Tribes Z-up -> GL Y-up (see mis_axes.hpp).
        glm::mat4 M = glm::translate(glm::mat4(1.0f),
            glm::vec3(xf.position[0], xf.position[2], xf.position[1]));
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

// Textured-quad program for the command-map aerial backdrop (13/08).
GLuint g_hud2d_tex_prog = 0;
GLuint g_hud2d_tex_vao  = 0;
GLuint g_hud2d_tex_vbo  = 0;
GLint  g_hud2d_tex_u_tint = -1;
GLint  g_hud2d_tex_u_alpha = -1;
GLint  g_hud2d_tex_u_sampler = -1;

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

const char* HUD2D_TEX_VS = R"(
#version 410 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }
)";

const char* HUD2D_TEX_FS = R"(
#version 410 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec3 u_tint;
uniform float u_alpha;
out vec4 frag;
void main() {
    vec3 c = texture(u_tex, v_uv).rgb * u_tint;
    frag = vec4(c, u_alpha);
}
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

    // Textured-quad program: pos.xy + uv.xy interleaved (4 floats / vertex).
    GLuint tvs = compile_simple(GL_VERTEX_SHADER,   HUD2D_TEX_VS);
    GLuint tfs = compile_simple(GL_FRAGMENT_SHADER, HUD2D_TEX_FS);
    g_hud2d_tex_prog = link_simple(tvs, tfs);
    g_hud2d_tex_u_tint    = glGetUniformLocation(g_hud2d_tex_prog, "u_tint");
    g_hud2d_tex_u_alpha   = glGetUniformLocation(g_hud2d_tex_prog, "u_alpha");
    g_hud2d_tex_u_sampler = glGetUniformLocation(g_hud2d_tex_prog, "u_tex");
    glGenVertexArrays(1, &g_hud2d_tex_vao);
    glBindVertexArray(g_hud2d_tex_vao);
    glGenBuffers(1, &g_hud2d_tex_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_hud2d_tex_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glBindVertexArray(0);
}

void draw_textured_quad_ndc(float x0, float y0, float x1, float y1,
                            GLuint tex, float r, float g, float b, float alpha)
{
    if (!g_hud2d_tex_prog || !tex) return;
    // UV (0,0) at top-left -> map row 0 to NDC y0 (top). Since y0/y1 are
    // NDC (top = +1, bottom = -1), and rendered with y1 < y0 typically,
    // we feed UV.v = 0 at the top vertex.
    const float verts[] = {
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f,
        x1, y1, 1.0f, 1.0f,
        x0, y0, 0.0f, 0.0f,
        x1, y1, 1.0f, 1.0f,
        x0, y1, 0.0f, 1.0f,
    };
    glUseProgram(g_hud2d_tex_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(g_hud2d_tex_u_sampler, 0);
    glUniform3f(g_hud2d_tex_u_tint, r, g, b);
    glUniform1f(g_hud2d_tex_u_alpha, alpha);
    glBindVertexArray(g_hud2d_tex_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_hud2d_tex_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
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

// ---- Compass (spec 13/02) ----
void hud2d_render_compass(float yaw,
                          const std::vector<CompassTick>& teammates,
                          const glm::vec3& player_pos,
                          int viewport_w,
                          int viewport_h)
{
    if (viewport_w <= 0 || viewport_h <= 0) return;
    ensure_init();
    if (!g_hud2d_prog) return;
    glUseProgram(g_hud2d_prog);
    glDisable(GL_DEPTH_TEST);

    auto pxX = [&](float x){ return (x / viewport_w) * 2.0f - 1.0f; };
    auto pxY = [&](float y){ return 1.0f - (y / viewport_h) * 2.0f; };

    const float band_w = 480.0f;
    const float band_h = 18.0f;
    const float band_x = (viewport_w - band_w) * 0.5f;
    const float band_y = 12.0f;
    const float band_fov = 3.14159265f;   // ±90°

    // Background
    draw_quad_ndc(pxX(band_x),         pxY(band_y),
                  pxX(band_x + band_w), pxY(band_y + band_h),
                  0.05f, 0.05f, 0.08f);

    // Cardinal ticks: N at yaw=0 (+Z), E at yaw=π/2 (+X), S at yaw=π (-Z),
    // W at yaw=-π/2 (-X).  Plus four intercardinals.
    struct Card { float angle; float r, g, b; float size; };
    const Card cardinals[] = {
        { 0.0f,            1.0f, 0.85f, 0.20f, 1.0f },  // N (gold)
        { 1.5707963f,      1.0f, 1.0f,  1.0f,  0.8f },  // E
        { 3.14159265f,     1.0f, 1.0f,  1.0f,  0.8f },  // S
        { -1.5707963f,     1.0f, 1.0f,  1.0f,  0.8f },  // W
        { 0.7853982f,      0.7f, 0.7f,  0.7f,  0.5f },
        { 2.3561945f,      0.7f, 0.7f,  0.7f,  0.5f },
        { -0.7853982f,     0.7f, 0.7f,  0.7f,  0.5f },
        { -2.3561945f,     0.7f, 0.7f,  0.7f,  0.5f },
    };
    auto draw_tick = [&](float rel_bearing, float size,
                         float r, float g, float b) {
        while (rel_bearing >  3.14159265f) rel_bearing -= 6.2831853f;
        while (rel_bearing < -3.14159265f) rel_bearing += 6.2831853f;
        if (std::abs(rel_bearing) > band_fov) return;
        float x = band_x + band_w * 0.5f + (rel_bearing / band_fov) * (band_w * 0.5f);
        float w = 3.0f * size;
        draw_quad_ndc(pxX(x - w * 0.5f), pxY(band_y),
                      pxX(x + w * 0.5f), pxY(band_y + band_h * size),
                      r, g, b);
    };
    for (auto& c : cardinals) {
        draw_tick(c.angle - yaw, c.size, c.r, c.g, c.b);
    }
    // Teammate ticks
    for (auto& tm : teammates) {
        glm::vec3 d = tm.world_pos - player_pos;
        float bearing = std::atan2(d.x, d.z);
        draw_tick(bearing - yaw, 0.6f, tm.color[0], tm.color[1], tm.color[2]);
    }

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

// ---- Sensor radar (spec 13/03) ----
void hud2d_render_sensor(float yaw,
                         const glm::vec3& player_pos,
                         const std::vector<SensorBlip>& blips,
                         float range,
                         int viewport_w,
                         int viewport_h)
{
    if (viewport_w <= 0 || viewport_h <= 0 || range <= 0.0f) return;
    ensure_init();
    if (!g_hud2d_prog) return;
    glUseProgram(g_hud2d_prog);
    glDisable(GL_DEPTH_TEST);

    auto pxX = [&](float x){ return (x / viewport_w) * 2.0f - 1.0f; };
    auto pxY = [&](float y){ return 1.0f - (y / viewport_h) * 2.0f; };

    const float radius = 80.0f;
    const float cx = viewport_w - radius - 16.0f;
    const float cy = radius + 16.0f;

    // Background disc (square approximation)
    draw_quad_ndc(pxX(cx - radius), pxY(cy - radius),
                  pxX(cx + radius), pxY(cy + radius),
                  0.05f, 0.10f, 0.05f);

    // Center marker (player)
    draw_quad_ndc(pxX(cx - 2.0f), pxY(cy - 2.0f),
                  pxX(cx + 2.0f), pxY(cy + 2.0f),
                  1.0f, 1.0f, 1.0f);

    // Blips
    const float cos_y = std::cos(-yaw);
    const float sin_y = std::sin(-yaw);
    for (auto& b : blips) {
        glm::vec3 d = b.world_pos - player_pos;
        float dist = std::sqrt(d.x * d.x + d.z * d.z);
        if (dist > range) continue;
        // Rotate by -yaw so the radar's "up" is player-forward.
        float rx =  d.x * cos_y + d.z * sin_y;
        float rz = -d.x * sin_y + d.z * cos_y;
        float px = cx + (rx / range) * radius;
        float py = cy - (rz / range) * radius;   // up on screen
        draw_quad_ndc(pxX(px - 2.0f), pxY(py - 2.0f),
                      pxX(px + 2.0f), pxY(py + 2.0f),
                      b.color[0], b.color[1], b.color[2]);
    }

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

// ---- Message feed (spec 13/04) — coloured bars, no font ----
void hud2d_render_message_feed(const std::deque<std::string>& msgs,
                               int viewport_w,
                               int viewport_h)
{
    if (msgs.empty()) return;
    ensure_init();
    if (!g_hud2d_prog) return;
    glUseProgram(g_hud2d_prog);
    glDisable(GL_DEPTH_TEST);

    auto pxX = [&](float x){ return (x / viewport_w) * 2.0f - 1.0f; };
    auto pxY = [&](float y){ return 1.0f - (y / viewport_h) * 2.0f; };

    const float x  = 16.0f;
    const float w  = 360.0f;
    const float h  = 8.0f;
    const float gap = 4.0f;
    const std::size_t n = std::min<std::size_t>(msgs.size(), 6);
    for (std::size_t i = 0; i < n; ++i) {
        // Newer messages first; fade older ones.
        float age_frac = static_cast<float>(i) / static_cast<float>(n);
        float alpha = 1.0f - age_frac * 0.7f;
        const float y = 60.0f + i * (h + gap);
        const std::string& m = msgs[msgs.size() - 1 - i];
        const float bar_w = std::min(w, static_cast<float>(m.size()) * 4.0f);
        draw_quad_ndc(pxX(x),         pxY(y),
                      pxX(x + bar_w), pxY(y + h),
                      0.3f * alpha, 0.5f * alpha, 0.9f * alpha);
    }

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

// ---- Damage reticle (spec 13/05) ----
namespace {
struct DamagePing { float yaw_to_attacker; float age; };
std::vector<DamagePing> g_damage_pings;
const float kDamageFadeSeconds = 2.0f;
}

void hud2d_report_damage(float yaw_to_attacker)
{
    g_damage_pings.push_back({ yaw_to_attacker, 0.0f });
}

void hud2d_tick(float dt)
{
    for (auto& p : g_damage_pings) p.age += dt;
    g_damage_pings.erase(
        std::remove_if(g_damage_pings.begin(), g_damage_pings.end(),
            [](const DamagePing& p) { return p.age >= kDamageFadeSeconds; }),
        g_damage_pings.end());
}

void hud2d_render_damage_reticle(int viewport_w, int viewport_h)
{
    if (g_damage_pings.empty()) return;
    ensure_init();
    if (!g_hud2d_prog) return;
    glUseProgram(g_hud2d_prog);
    glDisable(GL_DEPTH_TEST);

    auto pxX = [&](float x){ return (x / viewport_w) * 2.0f - 1.0f; };
    auto pxY = [&](float y){ return 1.0f - (y / viewport_h) * 2.0f; };

    const float cx = viewport_w * 0.5f;
    const float cy = viewport_h * 0.5f;
    const float reticle_r = 60.0f;
    for (auto& p : g_damage_pings) {
        float a = 1.0f - (p.age / kDamageFadeSeconds);
        float dx = std::sin(p.yaw_to_attacker) * reticle_r;
        float dz = std::cos(p.yaw_to_attacker) * reticle_r;
        float x = cx + dx;
        float y = cy - dz;
        draw_quad_ndc(pxX(x - 5.0f), pxY(y - 5.0f),
                      pxX(x + 5.0f), pxY(y + 5.0f),
                      0.95f * a, 0.15f, 0.15f);
    }

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

// ---- Command map (spec 13/06) ----
namespace {
GLuint      g_map_tex  = 0;
int         g_map_size = 0;
const void* g_map_src  = nullptr;
}

void hud2d_render_command_map(const float* heightmap,
                              int size_plus_one,
                              float metres_per_quad,
                              const std::vector<MapIcon>& icons,
                              const glm::vec3& player_pos,
                              float player_yaw,
                              int viewport_w,
                              int viewport_h,
                              float world_origin_x,
                              float world_origin_z)
{
    if (viewport_w <= 0 || viewport_h <= 0 || !heightmap || size_plus_one <= 1)
        return;
    ensure_init();
    if (!g_hud2d_prog) return;

    // Lazily bake an aerial map texture once per heightmap (13/08).
    // Combines an elevation gradient (light=high, dark=low) with a
    // slope-shading factor (steep terrain darkens) so the map reads
    // topographically.
    if (g_map_tex == 0 || g_map_size != size_plus_one
        || g_map_src != static_cast<const void*>(heightmap)) {
        if (g_map_tex) glDeleteTextures(1, &g_map_tex);
        const int N = size_plus_one;
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(N) * N * 4);
        float hmin = heightmap[0], hmax = heightmap[0];
        for (int i = 0; i < N * N; ++i) {
            hmin = std::min(hmin, heightmap[i]);
            hmax = std::max(hmax, heightmap[i]);
        }
        const float rng = std::max(1.0f, hmax - hmin);
        // World step between samples (used to normalise slope into 0..1).
        const float step = std::max(1.0f, metres_per_quad);
        // Tunable: maximum slope (radians-equivalent) that maps to 0
        // shading. 0.7 ≈ 35° — beyond that we're at the dark end.
        const float slope_full = 0.7f;
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const int idx = y * N + x;
                const float h  = heightmap[idx];
                const float hL = heightmap[y * N + std::max(0, x - 1)];
                const float hR = heightmap[y * N + std::min(N - 1, x + 1)];
                const float hD = heightmap[std::max(0, y - 1) * N + x];
                const float hU = heightmap[std::min(N - 1, y + 1) * N + x];
                const float dx = (hR - hL) / (2.0f * step);
                const float dz = (hU - hD) / (2.0f * step);
                const float slope = std::sqrt(dx * dx + dz * dz);
                const float elev_t = (h - hmin) / rng;            // 0..1
                const float slope_t = std::min(1.0f, slope / slope_full);
                // Tribes-style topographic green: hue from dark green
                // (low + flat) to khaki (high) — slope darkens both.
                float base_r = 0.20f + 0.55f * elev_t;
                float base_g = 0.30f + 0.45f * elev_t;
                float base_b = 0.18f + 0.20f * elev_t;
                const float darken = 1.0f - 0.60f * slope_t;
                base_r *= darken; base_g *= darken; base_b *= darken;
                rgba[idx * 4 + 0] = static_cast<std::uint8_t>(
                    std::min(1.0f, base_r) * 255.0f);
                rgba[idx * 4 + 1] = static_cast<std::uint8_t>(
                    std::min(1.0f, base_g) * 255.0f);
                rgba[idx * 4 + 2] = static_cast<std::uint8_t>(
                    std::min(1.0f, base_b) * 255.0f);
                rgba[idx * 4 + 3] = 220;
            }
        }
        glGenTextures(1, &g_map_tex);
        glBindTexture(GL_TEXTURE_2D, g_map_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, N, N,
            0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        g_map_size = N;
        g_map_src  = static_cast<const void*>(heightmap);
    }

    glDisable(GL_DEPTH_TEST);

    auto pxX = [&](float x){ return (x / viewport_w) * 2.0f - 1.0f; };
    auto pxY = [&](float y){ return 1.0f - (y / viewport_h) * 2.0f; };

    const float side = std::min<float>(viewport_w, viewport_h) * 0.7f;
    const float x = (viewport_w - side) * 0.5f;
    const float y = (viewport_h - side) * 0.5f;
    const float world_side = size_plus_one * metres_per_quad;

    // Backdrop: baked aerial heightmap texture (13/08); falls through to
    // a dark-green flat fill if the bake failed.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (g_map_tex) {
        draw_textured_quad_ndc(pxX(x),        pxY(y),
                               pxX(x + side), pxY(y + side),
                               g_map_tex, 1.0f, 1.0f, 1.0f, 0.92f);
    } else {
        glUseProgram(g_hud2d_prog);
        draw_quad_ndc(pxX(x),        pxY(y),
                      pxX(x + side), pxY(y + side),
                      0.05f, 0.08f, 0.05f);
    }
    glDisable(GL_BLEND);

    // Switch back to flat program for icon overlays.
    glUseProgram(g_hud2d_prog);

    auto world_to_map = [&](float wx, float wz, float& mx, float& my) {
        mx = x + ((wx - world_origin_x) / world_side) * side;
        my = y + ((wz - world_origin_z) / world_side) * side;
    };

    // Icons
    for (auto& ic : icons) {
        float mx, my; world_to_map(ic.world_pos.x, ic.world_pos.z, mx, my);
        draw_quad_ndc(pxX(mx - 3.0f), pxY(my - 3.0f),
                      pxX(mx + 3.0f), pxY(my + 3.0f),
                      ic.color[0], ic.color[1], ic.color[2]);
    }

    // Player marker (arrow approximation — small green square + tick in
    // facing direction)
    float pmx, pmy; world_to_map(player_pos.x, player_pos.z, pmx, pmy);
    draw_quad_ndc(pxX(pmx - 4.0f), pxY(pmy - 4.0f),
                  pxX(pmx + 4.0f), pxY(pmy + 4.0f),
                  0.2f, 1.0f, 0.2f);
    float tx = pmx + std::sin(player_yaw) * 8.0f;
    float ty = pmy + std::cos(player_yaw) * 8.0f;
    draw_quad_ndc(pxX(tx - 2.0f), pxY(ty - 2.0f),
                  pxX(tx + 2.0f), pxY(ty + 2.0f),
                  1.0f, 1.0f, 0.2f);

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

} // namespace dts_viewer
