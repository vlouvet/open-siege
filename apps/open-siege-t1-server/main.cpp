// open-siege-t1-server — headless dedicated server (spec 26/05).
//
// v1 scope: prove the engine library runs without SDL/GL/audio.
// Loads a mission, ticks at 32 Hz, prints heartbeat, handles SIGINT,
// polls stdin for "quit". No UDP listener yet — that's a follow-up
// once spec 07 wires the engine's ghost_stream to a real socket loop.

#include <osengine/audio_sink.hpp>
#include <osengine/listen_server.hpp>
#include <osengine/mission_loader.hpp>
#include <osengine/mission_sounds.hpp>
#include <osengine/paths.hpp>
#include <osengine/server_listener.hpp>
#include <osengine/session_table.hpp>
#include <osengine/world_tick.hpp>
#include <glm/glm.hpp>
#include <glm/geometric.hpp>
#include "content/net/udp_socket.hpp"
#include <cmath>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
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
        "  --mission <name>          Mission short name (default: 1_Welcome)\n"
        "  --tribes-dir <path>       Tribes install dir containing base/missions/\n"
        "  --port <n>                UDP listen port (default 28000)\n"
        "  --tick-hz <n>             Tick rate (default 32)\n"
        "  --max-players <n>         Max concurrent sessions (default 32)\n"
        "  --no-listener             Skip the UDP bind (server tick-only)\n"
        "  --listener-selftest       Run server_listener selftest and exit\n"
        "  --listen-server-selftest  Run ListenServer thread selftest and exit\n"
        "  --world-tick-selftest     Run world_tick selftest and exit\n"
        "  --help                    This message\n",
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
    int max_players = 32;

    bool listen_server_selftest = false;
    bool listener_selftest = false;
    bool world_tick_selftest = false;
    bool no_listener = false;
    bool skip_mission = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { print_usage(); return 0; }
        if (a == "--listen-server-selftest") { listen_server_selftest = true; continue; }
        if (a == "--listener-selftest") { listener_selftest = true; continue; }
        if (a == "--world-tick-selftest") { world_tick_selftest = true; continue; }
        if (a == "--no-listener") { no_listener = true; continue; }
        if (a == "--skip-mission") { skip_mission = true; continue; }
        if (a == "--mission" && i + 1 < argc) { mission_name = argv[++i]; continue; }
        if (a == "--tribes-dir" && i + 1 < argc) { tribes_dir = argv[++i]; continue; }
        if (a == "--port" && i + 1 < argc) { port = std::atoi(argv[++i]); continue; }
        if (a == "--tick-hz" && i + 1 < argc) { tick_hz = std::atoi(argv[++i]); continue; }
        if (a == "--max-players" && i + 1 < argc) { max_players = std::atoi(argv[++i]); continue; }
        std::fprintf(stderr, "[server] unknown arg: %s\n", a.c_str());
        print_usage();
        return 2;
    }

    if (listen_server_selftest) {
        dts_viewer::ListenServer ls(dts_viewer::ListenServerConfig{200, 6});
        ls.start();
        ls.client_endpoint().send({1,2,3,4});
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        ls.stop();
        if (ls.ticks() < 4) {
            std::fprintf(stderr, "[server] selftest: only %llu ticks (expected >=4)\n",
                         (unsigned long long)ls.ticks());
            return 1;
        }
        std::fprintf(stderr, "[server] listen-server selftest OK (%llu ticks)\n",
                     (unsigned long long)ls.ticks());
        return 0;
    }

    if (listener_selftest) {
        return dts_viewer::server_listener_selftest();
    }

    if (world_tick_selftest) {
        // Spec 28/03 selftest: two sessions with different inputs,
        // 10 ticks each, verify positions diverge as expected.
        dts_viewer::SessionTable table(4);
        const std::uint8_t n1[3] = { 0xa, 0xb, 0xc };
        const std::uint8_t n2[3] = { 0x1, 0x2, 0x3 };
        studio::content::net::Endpoint p1{"127.0.0.1", 50001};
        studio::content::net::Endpoint p2{"127.0.0.1", 50002};
        auto* s1 = table.allocate(p1, n1, 0);
        auto* s2 = table.allocate(p2, n2, 0);
        if (!s1 || !s2) { std::fputs("[world-tick-selftest] allocate failed\n", stderr); return 1; }
        // s1 walks forward (look down +Z which is default yaw=0).
        // s2 stays still.
        for (int i = 0; i < 32; ++i) {
            net20::MoveInput m{};
            m.forward = 1.0f;
            s1->pending_moves.push_back(m);
        }
        dts_viewer::WorldTickContext ctx{};
        ctx.max_moves_per_tick = 32;
        for (int i = 0; i < 32; ++i) {
            dts_viewer::world_tick(table, ctx, 1.0f / 32.0f);
        }
        const float dist_s1 = glm::length(s1->player_state.pos);
        const float dist_s2 = glm::length(s2->player_state.pos);
        std::fprintf(stderr,
            "[world-tick-selftest] s1 pos=(%.2f,%.2f,%.2f) |d|=%.2f  s2 pos=(%.2f,%.2f,%.2f) |d|=%.2f\n",
            s1->player_state.pos.x, s1->player_state.pos.y, s1->player_state.pos.z, dist_s1,
            s2->player_state.pos.x, s2->player_state.pos.y, s2->player_state.pos.z, dist_s2);
        if (dist_s1 < 1.0f) {
            std::fputs("[world-tick-selftest] s1 did not move forward (expected |d| > 1m)\n", stderr);
            return 1;
        }
        if (dist_s2 > 0.5f) {
            // s2 may fall under gravity to y=0 from the spawn — that's fine.
            // Just ensure it doesn't walk horizontally.
            if (std::abs(s2->player_state.pos.x) > 0.1f ||
                std::abs(s2->player_state.pos.z) > 0.1f) {
                std::fputs("[world-tick-selftest] s2 drifted horizontally (expected stationary)\n", stderr);
                return 1;
            }
        }
        std::fputs("[world-tick-selftest] OK\n", stderr);
        return 0;
    }

    if (tribes_dir.empty() && !skip_mission) {
        tribes_dir = os_paths::assets_dir();
        if (tribes_dir.empty()) {
            std::fprintf(stderr,
                "[server] --tribes-dir not given and no %s/tribes-dir.txt found "
                "(use --skip-mission for listener-only mode)\n",
                os_paths::config_dir("shared").string().c_str());
            return 2;
        }
        std::fprintf(stderr, "[server] using assets_dir from %s\n",
                     os_paths::config_dir("shared").string().c_str());
    }

    std::signal(SIGINT, on_sigint);
#ifndef _WIN32
    std::signal(SIGTERM, on_sigint);
#endif

    auto& sink = dts_viewer::null_audio_sink();
    std::optional<dts_viewer::LoadedMission> mission;
    dts_viewer::MissionSoundsState mission_audio;
    if (!skip_mission) {
        const auto missions_dir = tribes_dir / "base" / "missions";
        const auto base_dir     = tribes_dir / "base";
        mission = dts_viewer::load_mission(missions_dir, base_dir, mission_name);
        if (!mission) {
            std::fprintf(stderr, "[server] failed to load mission: %s\n", mission_name.c_str());
            return 1;
        }
        std::fprintf(stderr, "[server] mission loaded: %s (port %d, %d Hz tick)\n",
                     mission_name.c_str(), port, tick_hz);
        mission_audio = dts_viewer::mission_sounds_load(
            mission->scene, base_dir, sink);
        std::fprintf(stderr, "[server] %zu ambient voices registered (null sink)\n",
                     mission_audio.voices.size());
    } else {
        std::fprintf(stderr,
            "[server] --skip-mission: listener-only mode (port %d, %d Hz tick)\n",
            port, tick_hz);
    }

    std::unique_ptr<dts_viewer::ServerListener> listener;
    if (!no_listener) {
        dts_viewer::ServerListenerConfig listener_cfg{};
        listener_cfg.port = static_cast<std::uint16_t>(port);
        listener_cfg.tick_hz = tick_hz;
        listener_cfg.max_players = static_cast<std::uint16_t>(max_players);
        listener = std::make_unique<dts_viewer::ServerListener>(listener_cfg);
        if (!listener->start()) {
            std::fprintf(stderr, "[server] listener failed to start: %s\n",
                         listener->last_error().c_str());
            return 1;
        }
    } else {
        std::fputs("[server] --no-listener: skipping UDP bind\n", stderr);
    }

    const auto period = std::chrono::milliseconds(1000 / std::max(1, tick_hz));
    auto last_log = std::chrono::steady_clock::now();
    std::uint64_t tick = 0;
    const float dt_sec = 1.0f / static_cast<float>(std::max(1, tick_hz));
    dts_viewer::WorldTickContext world_ctx{};
    // Future: populate world_ctx.terrain + world_ctx.bounds from the
    // loaded mission. v1: empty HeightSampler (sample() returns 0) so
    // players free-fall onto the implicit y=0 plane and walk on it.

    while (!g_quit.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        ++tick;
        if (poll_stdin_quit()) g_quit.store(true);

        // Spec 28/03: advance authoritative player state from each
        // session's queued movecommands.
        if (listener) {
            dts_viewer::world_tick(listener->sessions(), world_ctx, dt_sec);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            if (listener) {
                const auto s = listener->stats();
                std::fprintf(stderr,
                    "[server] tick %llu  net: req=%llu acc=%llu rej=%llu data=%llu ghost=%llu unk=%llu  sessions=%llu (dropped=%llu)\n",
                    (unsigned long long)tick,
                    (unsigned long long)s.request_connects_received,
                    (unsigned long long)s.accept_connects_sent,
                    (unsigned long long)s.reject_connects_sent,
                    (unsigned long long)s.data_packets_received,
                    (unsigned long long)s.ghost_bursts_sent,
                    (unsigned long long)s.unknown_packets_received,
                    (unsigned long long)s.sessions_active,
                    (unsigned long long)s.sessions_dropped);
            } else {
                std::fprintf(stderr, "[server] tick %llu\n", (unsigned long long)tick);
            }
            last_log = now;
        }
        std::this_thread::sleep_for(period - (std::chrono::steady_clock::now() - t0));
    }

    if (listener) listener->stop();
    if (!skip_mission) dts_viewer::mission_sounds_unload(mission_audio, sink);
    std::fputs("[server] shutting down\n", stderr);
    return 0;
}
