// open-siege-t1-client — minimal SDL2+GL client (spec 26/06 + 29/01-29/02).
//
// v1 scope today:
//   * Connect to a track-28 server via NetClient::start_live (template-paste
//     handshake + ghost-stream consumer).
//   * Each frame: snapshot the GhostRegistry, render every Player ghost
//     as a colored cube via the shared dts_viewer_render library's
//     render_entity_cube helper.
//   * Hardcoded overhead camera so all spawn-point clusters land in view.
//
// Deferred (spec 29/02b and friends):
//   * Terrain + skybox rendering (need mission_loader + the terrain
//     renderer hooked up — non-trivial; mission name plumbing also
//     needs the server_info VC event from the spec's "Outputs" list).
//   * Local-player camera follow + 3rd-person view.
//   * HUD / chat / scoreboard (29/05+).
//   * Real DTS shape meshes — cubes for v1, real silhouettes when the
//     asset_cache pipeline is wired (also part of 29/02b).

#include <osengine/audio_sink.hpp>
#include <osengine/net_client.hpp>
#include <osengine/ghost_types.hpp>

#include <entity_renderer.hpp>     // from dts_viewer_render

#include <SDL2/SDL.h>

#if defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION
#  include <OpenGL/gl3.h>
#else
#  include <GL/glew.h>
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

std::atomic<bool> g_quit{false};
void on_sigint(int) { g_quit.store(true); }

void print_usage()
{
    std::fputs(
        "open-siege-t1-client [options]\n"
        "  --server <host:port>  Connect to a track-28 server\n"
        "  --groove              Use Groove handshake (default: vanilla)\n"
        "  --width <px>          Window width (default 1024)\n"
        "  --height <px>         Window height (default 768)\n"
        "  --cam <ovr|fwd>       Camera mode: overhead (default) or forward-of-origin\n"
        "  --help                This message\n",
        stderr);
}

bool parse_host_port(const std::string& arg, std::string& host, std::uint16_t& port)
{
    auto colon = arg.find(':');
    if (colon == std::string::npos) return false;
    host = arg.substr(0, colon);
    port = static_cast<std::uint16_t>(std::atoi(arg.substr(colon + 1).c_str()));
    return port != 0;
}

// Minimal flat-color shader sources (mirror of dts-viewer's FLAT_VS/FS).
constexpr const char* kFlatVS = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)";

constexpr const char* kFlatFS = R"(
#version 410 core
uniform vec3 u_color;
out vec4 frag;
void main() { frag = vec4(u_color, 1.0); }
)";

GLuint compile_shader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        std::fprintf(stderr, "[client] shader compile error: %.*s\n", (int)n, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        std::fprintf(stderr, "[client] program link error: %.*s\n", (int)n, log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// Eight distinct colors so each player ghost has a stable hue based on
// its ghost_id slot.
constexpr std::array<std::array<float, 3>, 8> kSlotColors = {{
    {1.0f, 0.20f, 0.20f}, {0.20f, 0.55f, 1.0f}, {0.20f, 1.0f, 0.40f},
    {1.0f, 0.85f, 0.20f}, {0.90f, 0.30f, 1.0f}, {0.20f, 1.0f, 1.0f},
    {1.0f, 0.55f, 0.10f}, {0.70f, 0.70f, 0.70f},
}};

}  // anonymous namespace

int main(int argc, char** argv)
{
    std::string server_arg;
    bool use_groove = false;
    int width = 1024, height = 768;
    std::string cam_mode = "ovr";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { print_usage(); return 0; }
        if (a == "--server" && i + 1 < argc) { server_arg = argv[++i]; continue; }
        if (a == "--groove") { use_groove = true; continue; }
        if (a == "--width"  && i + 1 < argc) { width  = std::atoi(argv[++i]); continue; }
        if (a == "--height" && i + 1 < argc) { height = std::atoi(argv[++i]); continue; }
        if (a == "--cam"    && i + 1 < argc) { cam_mode = argv[++i]; continue; }
        std::fprintf(stderr, "[client] unknown arg: %s\n", a.c_str());
        print_usage();
        return 2;
    }

    std::signal(SIGINT, on_sigint);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[client] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* win = SDL_CreateWindow(
        "open-siege-t1-client",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::fprintf(stderr, "[client] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        std::fprintf(stderr, "[client] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

#if !defined(__APPLE__)
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::fprintf(stderr, "[client] glewInit failed\n");
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
#endif

    // Spec 29/02 — flat-color shader for rendering player ghost cubes.
    const GLuint flat_prog = link_program(
        compile_shader(GL_VERTEX_SHADER,   kFlatVS),
        compile_shader(GL_FRAGMENT_SHADER, kFlatFS));
    if (!flat_prog) {
        std::fputs("[client] failed to build flat shader; aborting\n", stderr);
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    const GLint u_mvp_loc   = glGetUniformLocation(flat_prog, "u_mvp");
    const GLint u_color_loc = glGetUniformLocation(flat_prog, "u_color");

    glEnable(GL_DEPTH_TEST);

    dts_viewer::NetClient net;
    if (!server_arg.empty()) {
        std::string host; std::uint16_t port = 0;
        if (!parse_host_port(server_arg, host, port)) {
            std::fprintf(stderr, "[client] bad --server arg: %s (want host:port)\n",
                         server_arg.c_str());
        } else {
            std::fprintf(stderr, "[client] connecting to %s:%u (%s)\n",
                         host.c_str(), port, use_groove ? "groove" : "vanilla");
            if (!net.start_live(host, port, use_groove)) {
                std::fprintf(stderr, "[client] net.start_live failed: %s\n",
                             net.last_error().c_str());
            }
        }
    } else {
        std::fputs("[client] no --server: rendering idle\n", stderr);
    }

    auto last_log = std::chrono::steady_clock::now();
    int last_render_count = -1;
    while (!g_quit.load()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) g_quit.store(true);
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                g_quit.store(true);
            if (ev.type == SDL_WINDOWEVENT
                && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                width  = ev.window.data1;
                height = ev.window.data2;
            }
        }

        // Camera + projection. Overhead default: high up on +Y, looking
        // straight down with +Z as the "forward" world axis (matches the
        // server's convention from spec 28/03's world_tick).
        const float aspect = (height > 0) ? float(width) / float(height) : 1.0f;
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 1.0f, 5000.0f);
        glm::vec3 eye, look_at, up;
        if (cam_mode == "fwd") {
            // Forward-of-origin camera: stand at (0, 2, -10) looking at +Z.
            eye     = {0.0f, 2.0f, -10.0f};
            look_at = {0.0f, 1.0f,   10.0f};
            up      = {0.0f, 1.0f,    0.0f};
        } else {
            // Overhead: stand 80 m up, slight angle so cubes show depth.
            eye     = {0.0f, 80.0f, -40.0f};
            look_at = {0.0f,  0.0f,  20.0f};
            up      = {0.0f,  0.0f,   1.0f};
        }
        const glm::mat4 view = glm::lookAt(eye, look_at, up);
        const glm::mat4 vp   = proj * view;

        glViewport(0, 0, width, height);
        glClearColor(0.10f, 0.12f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Draw a small reference cube at the world origin so the user
        // always sees SOMETHING even before any ghost arrives.
        glUseProgram(flat_prog);
        dts_viewer::render_entity_cube({0.0f, 0.0f, 0.0f}, 0.5f,
            {0.45f, 0.45f, 0.45f}, u_mvp_loc, u_color_loc, vp);

        // Spec 29/02: render every Player ghost the server has told us about.
        int rendered = 0;
        if (net.running()) {
            const auto reg = net.snapshot_registry();
            for (const auto& kv : reg.players) {
                const auto& p = kv.second;
                const glm::vec3 world_pos{p.pos_x, p.pos_y, p.pos_z};
                const auto& col = kSlotColors[kv.first % kSlotColors.size()];
                dts_viewer::render_entity_cube(world_pos, 1.5f, col,
                    u_mvp_loc, u_color_loc, vp);
                ++rendered;
            }
        }

        SDL_GL_SwapWindow(win);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(2)) {
            if (net.running()) {
                std::fprintf(stderr,
                    "[client] net: %d pkts, %d typed records, rendering %d ghost(s)\n",
                    net.packets_seen(), net.typed_records(), rendered);
            }
            last_log = now;
        }
        if (rendered != last_render_count) {
            std::fprintf(stderr, "[client] ghost count -> %d\n", rendered);
            last_render_count = rendered;
        }
    }

    glDeleteProgram(flat_prog);
    net.stop();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::fputs("[client] shutting down\n", stderr);
    return 0;
}
