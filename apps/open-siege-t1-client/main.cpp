// open-siege-t1-client — minimal SDL2+GL client (spec 26/06).
//
// v1 scope: open a window, hand control to osengine::NetClient when a
// --server is given (spectator path — same capability dts-viewer has via
// --net-host), display a connection-status overlay. The full renderer
// integration (entities, terrain, interiors) is shared with dts-viewer
// via the source tree in apps/dts-viewer/ — that integration is a
// follow-up spec since the dts-viewer renderers still live in
// examples/dts-viewer/ alongside the editor UI.

#include <osengine/audio_sink.hpp>
#include <osengine/net_client.hpp>

#include <SDL2/SDL.h>

#if defined(__APPLE__)
#  include <OpenGL/gl3.h>
#else
#  include <GL/glew.h>
#endif

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
        "  --server <host:port>  Connect spectator-mode to a Tribes 1 server\n"
        "  --groove              Use Groove handshake (default: vanilla)\n"
        "  --width <px>          Window width (default 1024)\n"
        "  --height <px>         Window height (default 768)\n"
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

} // anonymous namespace

int main(int argc, char** argv)
{
    std::string server_arg;
    bool use_groove = false;
    int width = 1024, height = 768;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { print_usage(); return 0; }
        if (a == "--server" && i + 1 < argc) { server_arg = argv[++i]; continue; }
        if (a == "--groove") { use_groove = true; continue; }
        if (a == "--width"  && i + 1 < argc) { width  = std::atoi(argv[++i]); continue; }
        if (a == "--height" && i + 1 < argc) { height = std::atoi(argv[++i]); continue; }
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
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
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
    while (!g_quit.load()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) g_quit.store(true);
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                g_quit.store(true);
        }

        // Background colour cycles slowly so the user can tell the loop
        // is alive even before the renderer is wired up.
        const auto t = std::chrono::steady_clock::now().time_since_epoch();
        const auto secs = std::chrono::duration<float>(t).count();
        const float r = 0.08f + 0.05f * (0.5f + 0.5f * SDL_sinf(secs * 0.5f));
        glClearColor(r, r * 1.2f, r * 1.6f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        SDL_GL_SwapWindow(win);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(2)) {
            if (net.running()) {
                std::fprintf(stderr, "[client] net: %d pkts, %d typed records\n",
                             net.packets_seen(), net.typed_records());
            }
            last_log = now;
        }
    }

    net.stop();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::fputs("[client] shutting down\n", stderr);
    return 0;
}
