#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <variant>
#include <algorithm>
#include <filesystem>

#include <SDL.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "resources/darkstar_volume.hpp"
#include "content/renderable_shape.hpp"
#include "content/dts/renderable_shape_factory.hpp"

namespace fs = std::filesystem;
namespace dv = studio::resources::vol::darkstar;
namespace sr = studio::resources;
namespace sc = studio::content;

// ---------------------- shape_renderer that buffers triangles ----------------------

struct buffered_geometry
{
    std::vector<float> positions;   // 3 floats per vertex
    std::vector<float> normals;     // 3 floats per vertex, flat per face
    std::size_t triangle_count = 0;

    glm::vec3 bbox_min{ 1e30f}, bbox_max{-1e30f};
};

struct buffering_renderer : sc::shape_renderer
{
    buffered_geometry& g;
    std::vector<sc::vector3f> current_face;

    explicit buffering_renderer(buffered_geometry& g) : g(g) {}

    void update_node(std::optional<std::string_view>, std::string_view) override {}
    void update_object(std::optional<std::string_view>, std::string_view) override {}
    void new_face(std::size_t) override { current_face.clear(); }
    void emit_vertex(const sc::vector3f& v) override {
        current_face.push_back(v);
        g.bbox_min = glm::min(g.bbox_min, glm::vec3(v.x, v.y, v.z));
        g.bbox_max = glm::max(g.bbox_max, glm::vec3(v.x, v.y, v.z));
    }
    void emit_texture_vertex(const sc::texture_vertex&) override {}
    void end_face() override {
        if (current_face.size() < 3) return;
        // fan triangulate (v0,v1,v2), (v0,v2,v3), ...
        glm::vec3 v0(current_face[0].x, current_face[0].y, current_face[0].z);
        for (std::size_t i = 1; i + 1 < current_face.size(); ++i) {
            glm::vec3 v1(current_face[i].x,   current_face[i].y,   current_face[i].z);
            glm::vec3 v2(current_face[i+1].x, current_face[i+1].y, current_face[i+1].z);
            glm::vec3 n = glm::normalize(glm::cross(v1 - v0, v2 - v0));
            if (!std::isfinite(n.x)) n = glm::vec3(0,0,1);
            for (auto& v : {v0, v1, v2}) {
                g.positions.push_back(v.x);
                g.positions.push_back(v.y);
                g.positions.push_back(v.z);
                g.normals.push_back(n.x);
                g.normals.push_back(n.y);
                g.normals.push_back(n.z);
            }
            ++g.triangle_count;
        }
    }
};

// ---------------------- DTS loading ----------------------

static std::vector<char> read_dts_from_vol(const fs::path& vol_path, const std::string& dts_name_lower_match)
{
    std::ifstream in(vol_path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open vol: " + vol_path.string());
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) throw std::runtime_error("not a darkstar vol");
    in.clear(); in.seekg(0);

    auto all = sr::get_all_content(vol_path, in, plugin);
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        auto name = f->filename.string();
        auto lower = name;
        for (auto& c : lower) c = std::tolower((unsigned char)c);
        if (lower.size() < 4 || lower.substr(lower.size()-4) != ".dts") continue;
        if (!dts_name_lower_match.empty() && lower.find(dts_name_lower_match) == std::string::npos) continue;

        std::printf("Loading DTS: %s (%zu bytes)\n", name.c_str(), f->size);
        std::stringstream buf;
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, buf);
        auto s = buf.str();
        return std::vector<char>(s.begin(), s.end());
    }
    throw std::runtime_error("no matching DTS found");
}

static buffered_geometry build_geometry(const std::vector<char>& dts_bytes)
{
    std::string str(dts_bytes.begin(), dts_bytes.end());
    std::stringstream ss(str);
    auto shape = sc::dts::make_shape(ss);
    if (!shape) throw std::runtime_error("make_shape returned null");

    auto details = shape->get_detail_levels();
    // Use only detail level 0 (highest detail)
    std::vector<std::size_t> detail_indexes;
    if (!details.empty()) detail_indexes.push_back(0);

    auto sequences = shape->get_sequences(detail_indexes);

    buffered_geometry geom;
    buffering_renderer r(geom);
    shape->render_shape(r, detail_indexes, sequences);
    return geom;
}

// ---------------------- shader helpers ----------------------

static GLuint compile_shader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        std::fprintf(stderr, "shader compile error: %.*s\n", (int)n, log);
        std::exit(10);
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        std::fprintf(stderr, "program link error: %.*s\n", (int)n, log);
        std::exit(11);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

static const char* VS_SRC = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
uniform mat4 u_mvp;
uniform mat3 u_normal_mat;
out vec3 v_normal_ws;
void main() {
    v_normal_ws = normalize(u_normal_mat * a_normal);
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

static const char* FS_SRC = R"(
#version 410 core
in vec3 v_normal_ws;
out vec4 frag;
void main() {
    vec3 L = normalize(vec3(0.4, 0.8, 0.6));
    float d = max(dot(normalize(v_normal_ws), L), 0.0);
    vec3 base = vec3(0.75, 0.78, 0.82);
    vec3 col = base * (0.25 + 0.75 * d);
    frag = vec4(col, 1.0);
}
)";

// ---------------------- main ----------------------

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <path-to-vol> [dts-substring]\n"
            "  e.g. %s tribes-game/base/Entities.vol chainturret\n",
            argv[0], argv[0]);
        return 1;
    }
    fs::path vol_path = argv[1];
    std::string dts_match = argc >= 3 ? argv[2] : "";
    for (auto& c : dts_match) c = std::tolower((unsigned char)c);

    auto dts_bytes = read_dts_from_vol(vol_path, dts_match);
    auto geom = build_geometry(dts_bytes);

    std::printf("Geometry: %zu triangles, %zu vertices, bbox=[%.1f %.1f %.1f]..[%.1f %.1f %.1f]\n",
        geom.triangle_count, geom.positions.size()/3,
        geom.bbox_min.x, geom.bbox_min.y, geom.bbox_min.z,
        geom.bbox_max.x, geom.bbox_max.y, geom.bbox_max.z);

    if (geom.triangle_count == 0) {
        std::fprintf(stderr, "no geometry to render\n");
        return 2;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr,"SDL_Init: %s\n", SDL_GetError()); return 3; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    SDL_Window* win = SDL_CreateWindow("dts-viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_SetSwapInterval(1);

    std::printf("GL_VERSION: %s\n", glGetString(GL_VERSION));

    // VAO + VBO upload
    GLuint vao = 0, vbo_pos = 0, vbo_nor = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, geom.positions.size()*sizeof(float), geom.positions.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &vbo_nor);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_nor);
    glBufferData(GL_ARRAY_BUFFER, geom.normals.size()*sizeof(float), geom.normals.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    GLuint prog = link_program(
        compile_shader(GL_VERTEX_SHADER,   VS_SRC),
        compile_shader(GL_FRAGMENT_SHADER, FS_SRC));
    GLint u_mvp        = glGetUniformLocation(prog, "u_mvp");
    GLint u_normal_mat = glGetUniformLocation(prog, "u_normal_mat");

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    // Tribes models use left-handed coords; we don't cull faces to avoid winding surprises.
    glDisable(GL_CULL_FACE);

    // Camera setup centered on bbox
    glm::vec3 center = 0.5f * (geom.bbox_min + geom.bbox_max);
    glm::vec3 extent = geom.bbox_max - geom.bbox_min;
    float radius = 0.5f * glm::length(extent);
    if (radius < 0.001f) radius = 1.0f;

    float yaw = 0.6f, pitch = 0.35f;
    float dist = radius * 3.0f;
    bool dragging = false; int last_x = 0, last_y = 0;

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT: running = false; break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) running = false;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) { dragging = true; last_x = ev.button.x; last_y = ev.button.y; }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT) dragging = false;
                    break;
                case SDL_MOUSEMOTION:
                    if (dragging) {
                        yaw   += (ev.motion.x - last_x) * 0.01f;
                        pitch += (ev.motion.y - last_y) * 0.01f;
                        pitch = glm::clamp(pitch, -1.5f, 1.5f);
                        last_x = ev.motion.x; last_y = ev.motion.y;
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    dist *= (ev.wheel.y > 0) ? 0.9f : 1.1f;
                    dist = glm::clamp(dist, radius * 0.2f, radius * 50.0f);
                    break;
            }
        }

        int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.12f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::vec3 eye = center + dist * glm::vec3(
            std::cos(pitch) * std::sin(yaw),
            std::sin(pitch),
            std::cos(pitch) * std::cos(yaw));

        glm::mat4 V = glm::lookAt(eye, center, glm::vec3(0,1,0));
        glm::mat4 P = glm::perspective(glm::radians(45.0f), (float)w / (float)h, radius * 0.05f, radius * 200.0f);
        glm::mat4 MVP = P * V;
        glm::mat3 N = glm::mat3(glm::transpose(glm::inverse(V)));

        glUseProgram(prog);
        glUniformMatrix4fv(u_mvp, 1, GL_FALSE, glm::value_ptr(MVP));
        glUniformMatrix3fv(u_normal_mat, 1, GL_FALSE, glm::value_ptr(N));
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(geom.positions.size() / 3));

        SDL_GL_SwapWindow(win);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
