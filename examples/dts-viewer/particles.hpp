#ifndef DTS_VIEWER_PARTICLES_HPP
#define DTS_VIEWER_PARTICLES_HPP

// Snow + star-field ambient particle systems — Spec 14/12.
//
// Two preset kinds, both rendered as GL_POINTS:
//   - Snow:  CPU-stepped per frame; particles drift down around the
//            player, despawn when they pass below feet, respawn above.
//   - Stars: static positions on a sphere at infinite distance; alpha
//            twinkles via per-particle phase. Drawn with a view matrix
//            stripped of translation so they stay fixed to the sky.

#define GL_SILENCE_DEPRECATION
#include "gl_includes.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace dts_viewer
{

struct ParticleSystem
{
    enum class Kind { Snow, Stars };
    Kind   kind = Kind::Snow;
    GLuint vao  = 0;
    GLuint vbo  = 0;
    GLuint prog = 0;
    GLint  u_mvp = -1;
    GLint  u_time = -1;
    GLint  u_tint = -1;
    GLint  u_size = -1;
    std::size_t count = 0;
    // Snow only: live world-space positions, velocities, plus a spawn
    // box reference (player position + radius). 4 floats per particle:
    // x, y, z, phase (phase used for star twinkle; harmless for snow).
    std::vector<float>            cpu_buf;
    std::vector<glm::vec3>        snow_vel;
    float                          snow_radius = 50.0f;
    bool                           enabled    = true;
};

namespace particles_detail
{

inline const char* PARTICLES_VS = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in float a_phase;
uniform mat4 u_mvp;
uniform float u_size;
uniform float u_time;
out float v_alpha;
void main() {
    gl_Position  = u_mvp * vec4(a_pos, 1.0);
    gl_PointSize = u_size;
    // Twinkle for stars (phase is per-particle offset). Snow ignores
    // the result via a tint multiplier set on the host side.
    v_alpha = 0.55 + 0.45 * sin(u_time * 1.2 + a_phase * 6.2831);
}
)";

inline const char* PARTICLES_FS = R"(
#version 410 core
in float v_alpha;
uniform vec3 u_tint;
uniform float u_size;
out vec4 frag;
void main() {
    // Round-ish point: drop the corners of the GL_POINT square.
    vec2 c = gl_PointCoord - vec2(0.5);
    float d = dot(c, c);
    if (d > 0.25) discard;
    float falloff = 1.0 - smoothstep(0.10, 0.25, d);
    frag = vec4(u_tint, v_alpha * falloff);
}
)";

inline GLuint compile_shader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

inline GLuint link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    return p;
}

inline void init_shared(ParticleSystem& s)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   PARTICLES_VS);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, PARTICLES_FS);
    s.prog = link_program(vs, fs);
    s.u_mvp  = glGetUniformLocation(s.prog, "u_mvp");
    s.u_time = glGetUniformLocation(s.prog, "u_time");
    s.u_tint = glGetUniformLocation(s.prog, "u_tint");
    s.u_size = glGetUniformLocation(s.prog, "u_size");

    glGenVertexArrays(1, &s.vao);
    glBindVertexArray(s.vao);
    glGenBuffers(1, &s.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    constexpr GLsizei stride = 4 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindVertexArray(0);
}

} // namespace particles_detail

inline ParticleSystem make_snow(std::size_t n = 800, float radius = 60.0f,
                                glm::vec3 origin = glm::vec3(0.0f))
{
    using namespace particles_detail;
    ParticleSystem s;
    s.kind = ParticleSystem::Kind::Snow;
    s.count = n;
    s.snow_radius = radius;
    init_shared(s);
    s.cpu_buf.assign(n * 4, 0.0f);
    s.snow_vel.assign(n, glm::vec3(0.0f));
    std::mt19937 rng(0xCAFEBABEu);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (std::size_t i = 0; i < n; ++i) {
        s.cpu_buf[i * 4 + 0] = origin.x + u(rng) * radius;
        s.cpu_buf[i * 4 + 1] = origin.y + (u(rng) * 0.5f + 0.5f) * 60.0f;
        s.cpu_buf[i * 4 + 2] = origin.z + u(rng) * radius;
        s.cpu_buf[i * 4 + 3] = u(rng) * 0.5f + 0.5f; // phase (unused for snow)
        s.snow_vel[i] = glm::vec3(u(rng) * 0.5f, -2.0f - std::abs(u(rng)) * 1.5f,
                                  u(rng) * 0.5f);
    }
    return s;
}

inline ParticleSystem make_stars(std::size_t n = 600)
{
    using namespace particles_detail;
    ParticleSystem s;
    s.kind = ParticleSystem::Kind::Stars;
    s.count = n;
    init_shared(s);
    s.cpu_buf.assign(n * 4, 0.0f);
    std::mt19937 rng(0xB17E51A8u);
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    for (std::size_t i = 0; i < n; ++i) {
        // Uniform point on a sphere.
        float z = uni(rng);
        float t = uni(rng) * 3.14159265f;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        // Bias up (more stars overhead than underfoot).
        const float ry = 0.3f + 0.7f * std::abs(uni(rng));
        const float R  = 800.0f;
        s.cpu_buf[i * 4 + 0] = R * r * std::cos(t);
        s.cpu_buf[i * 4 + 1] = R * ry;
        s.cpu_buf[i * 4 + 2] = R * r * std::sin(t);
        s.cpu_buf[i * 4 + 3] = uni(rng) * 0.5f + 0.5f; // phase
    }
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(s.cpu_buf.size() * sizeof(float)),
                 s.cpu_buf.data(), GL_STATIC_DRAW);
    return s;
}

inline void step_snow(ParticleSystem& s, float dt, glm::vec3 player_pos)
{
    if (!s.enabled || s.kind != ParticleSystem::Kind::Snow) return;
    const float r = s.snow_radius;
    for (std::size_t i = 0; i < s.count; ++i) {
        s.cpu_buf[i * 4 + 0] += s.snow_vel[i].x * dt;
        s.cpu_buf[i * 4 + 1] += s.snow_vel[i].y * dt;
        s.cpu_buf[i * 4 + 2] += s.snow_vel[i].z * dt;
        // Wrap-around: when below feet or too far from the player,
        // respawn above and re-centre.
        const float dx = s.cpu_buf[i * 4 + 0] - player_pos.x;
        const float dz = s.cpu_buf[i * 4 + 2] - player_pos.z;
        const bool below = s.cpu_buf[i * 4 + 1] < player_pos.y - 5.0f;
        if (below || dx * dx + dz * dz > r * r) {
            const float ang = static_cast<float>(i) * 0.5113f;
            const float rad = (static_cast<float>(i % 17) / 17.0f) * r * 0.95f;
            s.cpu_buf[i * 4 + 0] = player_pos.x + std::cos(ang) * rad;
            s.cpu_buf[i * 4 + 1] = player_pos.y + 30.0f + (i % 11) * 2.5f;
            s.cpu_buf[i * 4 + 2] = player_pos.z + std::sin(ang) * rad;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(s.cpu_buf.size() * sizeof(float)),
                 s.cpu_buf.data(), GL_DYNAMIC_DRAW);
}

inline void draw_particles(const ParticleSystem& s,
                           const glm::mat4& mvp,
                           float time_seconds,
                           glm::vec3 tint,
                           float point_size)
{
    if (!s.enabled || !s.prog || s.count == 0) return;
    glUseProgram(s.prog);
    glUniformMatrix4fv(s.u_mvp, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform1f(s.u_time, time_seconds);
    glUniform3f(s.u_tint, tint.x, tint.y, tint.z);
    glUniform1f(s.u_size, point_size);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindVertexArray(s.vao);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(s.count));
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

} // namespace dts_viewer

#endif // DTS_VIEWER_PARTICLES_HPP
