// open-siege-t1-server — headless dedicated server (spec 26/05).
//
// v1 scope: prove the engine library runs without SDL/GL/audio.
// Loads a mission, ticks at 32 Hz, prints heartbeat, handles SIGINT,
// polls stdin for "quit". No UDP listener yet — that's a follow-up
// once spec 07 wires the engine's ghost_stream to a real socket loop.

#include <osengine/audio_sink.hpp>
#include <osengine/mission_loader.hpp>
#include <osengine/mission_sounds.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif

namespace
{

std::atomic<bool> g_quit{false};

void on_sigint(int)
{
    g_quit.store(true);
}

void print_usage()
{
    std::fputs(
        "open-siege-t1-server [options]\n"
        "  --mission <name>    Mission short name (default: 1_Welcome)\n"
        "  --tribes-dir <path> Tribes install dir containing base/missions/\n"
        "  --port <n>          UDP port (placeholder; not yet bound — v1)\n"
        "  --tick-hz <n>       Tick rate (default 32)\n"
        "  --help              This message\n",
        stderr);
}

bool poll_stdin_quit()
{
#ifndef _WIN32
    // Non-blocking peek using select. Pure POSIX; the server can run
    // under nohup or systemd without TTY.
    fd_set rfds; FD_ZERO(&rfds); FD_SET(0, &rfds);
    timeval tv{0, 0};
    if (select(1, &rfds, nullptr, nullptr, &tv) > 0) {
        std::string line;
        if (std::getline(std::cin, line)) {
            if (line == "quit" || line == "exit") return true;
            std::fprintf(stderr, "[server] unknown command: %s\n", line.c_str());
        }
    }
#endif
    return false;
}

} // anonymous namespace

int main(int argc, char** argv)
{
    std::string mission_name = "1_Welcome";
    std::filesystem::path tribes_dir;
    int port = 28000;
    int tick_hz = 32;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { print_usage(); return 0; }
        if (a == "--mission" && i + 1 < argc) { mission_name = argv[++i]; continue; }
        if (a == "--tribes-dir" && i + 1 < argc) { tribes_dir = argv[++i]; continue; }
        if (a == "--port" && i + 1 < argc) { port = std::atoi(argv[++i]); continue; }
        if (a == "--tick-hz" && i + 1 < argc) { tick_hz = std::atoi(argv[++i]); continue; }
        std::fprintf(stderr, "[server] unknown arg: %s\n", a.c_str());
        print_usage();
        return 2;
    }

    if (tribes_dir.empty()) {
        std::fputs("[server] --tribes-dir is required\n", stderr);
        return 2;
    }

    std::signal(SIGINT, on_sigint);
#ifndef _WIN32
    std::signal(SIGTERM, on_sigint);
#endif

    const auto missions_dir = tribes_dir / "base" / "missions";
    const auto base_dir     = tribes_dir / "base";
    auto mission = dts_viewer::load_mission(missions_dir, base_dir, mission_name);
    if (!mission) {
        std::fprintf(stderr, "[server] failed to load mission: %s\n", mission_name.c_str());
        return 1;
    }
    std::fprintf(stderr, "[server] mission loaded: %s (port %d, %d Hz tick)\n",
                 mission_name.c_str(), port, tick_hz);

    auto& sink = dts_viewer::null_audio_sink();
    auto mission_audio = dts_viewer::mission_sounds_load(
        mission->scene, base_dir, sink);
    std::fprintf(stderr, "[server] %zu ambient voices registered (null sink)\n",
                 mission_audio.voices.size());

    const auto period = std::chrono::milliseconds(1000 / std::max(1, tick_hz));
    auto last_log = std::chrono::steady_clock::now();
    std::uint64_t tick = 0;

    while (!g_quit.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        ++tick;
        if (poll_stdin_quit()) g_quit.store(true);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            std::fprintf(stderr, "[server] tick %llu\n", (unsigned long long)tick);
            last_log = now;
        }
        std::this_thread::sleep_for(period - (std::chrono::steady_clock::now() - t0));
    }

    dts_viewer::mission_sounds_unload(mission_audio, sink);
    std::fputs("[server] shutting down\n", stderr);
    return 0;
}
