// open-siege-t1-server — headless dedicated server (spec 26/05).
//
// v1 scope: prove the engine library runs without SDL/GL/audio.
// Loads a mission, ticks at 32 Hz, prints heartbeat, handles SIGINT,
// polls stdin for "quit". No UDP listener yet — that's a follow-up
// once spec 07 wires the engine's ghost_stream to a real socket loop.

#include <osengine/audio_sink.hpp>
#include <osengine/chat_channel.hpp>
#include <osengine/damage_resolver.hpp>
#include <osengine/flag_state.hpp>
#include <osengine/ghost_emitter.hpp>
#include <osengine/ghost_encoder.hpp>
#include <osengine/listen_server.hpp>
#include <osengine/map_cycle.hpp>
#include <osengine/match_state.hpp>
#include <osengine/mission_loader.hpp>
#include <osengine/net_client.hpp>
#include <osengine/mission_sounds.hpp>
#include <osengine/paths.hpp>
#include <osengine/projectile_world.hpp>
#include <osengine/scoreboard.hpp>
#include <osengine/server_listener.hpp>
#include <osengine/session_table.hpp>
#include <osengine/tah_burst_orchestrator.hpp>
#include <osengine/tah_class_encoders.hpp>
#include <osengine/tah_default_catalogue.hpp>
#include <osengine/tah_vc_outbound.hpp>
#include <osengine/team_assigner.hpp>
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
        "  --no-canned-burst         Disable spec 26/11 captured-burst reply\n"
        "  --no-ghost-emit           Disable spec 28/04 OSGB ghost streaming\n"
        "  --team-balance off        Disable round-robin team assignment\n"
        "  --team-assigner-selftest  Run team_assigner selftest and exit\n"
        "  --projectile-selftest     Run projectile_world selftest and exit\n"
        "  --damage-selftest         Run damage_resolver selftest and exit\n"
        "  --flag-selftest           Run flag_state selftest and exit\n"
        "  --match-selftest          Run match_state selftest and exit\n"
        "  --chat-selftest           Run chat_channel selftest and exit\n"
        "  --scoreboard-selftest     Run scoreboard selftest and exit\n"
        "  --mapcycle-selftest       Run map_cycle selftest and exit\n"
        "  --server-info-selftest    Run server_info codec selftest and exit\n"
        "  --map-cycle <n1>,<n2>,..  Comma-separated mission rotation (default: --mission)\n"
        "  --cap-limit <n>           Captures to win the match (default 5)\n"
        "  --time-limit <min>        Match time limit in minutes (default 25)\n"
        "  --listener-selftest       Run server_listener selftest and exit\n"
        "  --groove-handshake-selftest  Run Groove RC + AC selftest (spec 26/10b)\n"
        "  --listen-server-selftest  Run ListenServer thread selftest and exit\n"
        "  --world-tick-selftest     Run world_tick selftest and exit\n"
        "  --ghost-emit-selftest     Run ghost emitter selftest and exit\n"
        "  --ghost-encoder-selftest  Run ghost encoder round-trip selftest and exit\n"
        "  --tah-class-encoders-selftest  Run TAH per-class encoders selftest and exit\n"
        "  --tah-vc-outbound-selftest  Run per-session VC outbound header selftest and exit\n"
        "  --tah-burst-orchestrator-selftest  Run TAH burst orchestrator selftest and exit\n"
        "  --tah-default-catalogue-selftest  Run TAH default catalogue selftest and exit\n"
        "  --mission-loader-selftest  Run mission_loader selftest and exit\n"
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
    bool ghost_emit_selftest = false;
    bool ghost_encoder_selftest = false;
    bool tah_class_encoders_selftest_flag = false;
    bool tah_vc_outbound_selftest_flag = false;
    bool tah_burst_orchestrator_selftest_flag = false;
    bool tah_default_catalogue_selftest_flag = false;
    bool mission_loader_selftest_flag = false;
    bool no_listener = false;
    bool no_canned_burst = false;
    bool no_ghost_emit = false;
    bool team_balance = true;
    bool team_assigner_selftest = false;
    bool projectile_selftest = false;
    bool damage_selftest = false;
    bool flag_selftest = false;
    bool match_selftest = false;
    bool chat_selftest = false;
    bool scoreboard_selftest = false;
    bool mapcycle_selftest = false;
    bool server_info_selftest = false;
    bool groove_handshake_selftest = false;
    std::string map_cycle_csv;
    int  cap_limit  = 5;
    int  time_limit_min = 25;
    bool skip_mission = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { print_usage(); return 0; }
        if (a == "--listen-server-selftest") { listen_server_selftest = true; continue; }
        if (a == "--listener-selftest") { listener_selftest = true; continue; }
        if (a == "--world-tick-selftest") { world_tick_selftest = true; continue; }
        if (a == "--ghost-emit-selftest") { ghost_emit_selftest = true; continue; }
        if (a == "--ghost-encoder-selftest") { ghost_encoder_selftest = true; continue; }
        if (a == "--tah-class-encoders-selftest") { tah_class_encoders_selftest_flag = true; continue; }
        if (a == "--tah-vc-outbound-selftest") { tah_vc_outbound_selftest_flag = true; continue; }
        if (a == "--tah-burst-orchestrator-selftest") { tah_burst_orchestrator_selftest_flag = true; continue; }
        if (a == "--tah-default-catalogue-selftest") { tah_default_catalogue_selftest_flag = true; continue; }
        if (a == "--mission-loader-selftest") { mission_loader_selftest_flag = true; continue; }
        if (a == "--no-listener") { no_listener = true; continue; }
        if (a == "--no-canned-burst") { no_canned_burst = true; continue; }
        if (a == "--no-ghost-emit") { no_ghost_emit = true; continue; }
        if (a == "--team-assigner-selftest") { team_assigner_selftest = true; continue; }
        if (a == "--projectile-selftest") { projectile_selftest = true; continue; }
        if (a == "--damage-selftest") { damage_selftest = true; continue; }
        if (a == "--flag-selftest") { flag_selftest = true; continue; }
        if (a == "--match-selftest") { match_selftest = true; continue; }
        if (a == "--chat-selftest") { chat_selftest = true; continue; }
        if (a == "--scoreboard-selftest") { scoreboard_selftest = true; continue; }
        if (a == "--mapcycle-selftest") { mapcycle_selftest = true; continue; }
        if (a == "--server-info-selftest") { server_info_selftest = true; continue; }
        if (a == "--groove-handshake-selftest") { groove_handshake_selftest = true; continue; }
        if (a == "--map-cycle" && i + 1 < argc) { map_cycle_csv = argv[++i]; continue; }
        if (a == "--cap-limit" && i + 1 < argc) { cap_limit = std::atoi(argv[++i]); continue; }
        if (a == "--time-limit" && i + 1 < argc) { time_limit_min = std::atoi(argv[++i]); continue; }
        if (a == "--team-balance" && i + 1 < argc) {
            const std::string v = argv[++i];
            team_balance = !(v == "off" || v == "false" || v == "0");
            continue;
        }
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

    if (groove_handshake_selftest) {
        return dts_viewer::groove_handshake_selftest();
    }

    if (ghost_emit_selftest) {
        return dts_viewer::ghost_emitter_selftest();
    }

    if (ghost_encoder_selftest) {
        return net20::ghost_encoder_roundtrip_selftest();
    }

    if (tah_class_encoders_selftest_flag) {
        return net20::tah_class_encoders_selftest();
    }

    if (tah_vc_outbound_selftest_flag) {
        return dts_viewer::tah_vc_outbound_selftest();
    }

    if (tah_burst_orchestrator_selftest_flag) {
        return dts_viewer::tah_burst_orchestrator_selftest();
    }

    if (tah_default_catalogue_selftest_flag) {
        return net20::tah_default_catalogue_selftest();
    }

    if (mission_loader_selftest_flag) {
        return dts_viewer::mission_loader_selftest();
    }

    if (team_assigner_selftest) {
        return dts_viewer::team_assigner_selftest();
    }

    if (projectile_selftest) {
        return dts_viewer::ProjectileWorld::selftest();
    }

    if (damage_selftest) {
        return dts_viewer::damage_resolver_selftest();
    }

    if (flag_selftest) {
        return dts_viewer::FlagWorld::selftest();
    }

    if (match_selftest) {
        return dts_viewer::MatchState::selftest();
    }

    if (chat_selftest) {
        return dts_viewer::ChatChannel::selftest();
    }

    if (scoreboard_selftest) {
        return dts_viewer::scoreboard_selftest();
    }

    if (mapcycle_selftest) {
        return dts_viewer::MapCycle::selftest();
    }

    if (server_info_selftest) {
        return dts_viewer::server_info_roundtrip_selftest();
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
        listener_cfg.enable_canned_burst = !no_canned_burst;
        listener_cfg.enable_ghost_emit   = !no_ghost_emit;
        listener_cfg.team_balance        = team_balance;
        listener = std::make_unique<dts_viewer::ServerListener>(listener_cfg);
        listener->set_mission_name(mission_name);   // spec 29/02b
        if (mission) {
            auto spawns = dts_viewer::extract_spawn_points(*mission);
            std::fprintf(stderr,
                "[server] extracted %zu spawn points from mission\n",
                spawns.size());
            listener->set_spawn_points(std::move(spawns));
            listener->set_loaded_mission(&*mission);
        }
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
    dts_viewer::ProjectileWorld projectiles;
    std::vector<dts_viewer::HitEvent>     tick_hits;
    std::vector<dts_viewer::KillEvent>    tick_kills;
    std::vector<dts_viewer::CaptureEvent> tick_captures;
    dts_viewer::DamageRules               damage_rules;
    dts_viewer::FlagWorld                 flags;
    if (mission) flags.load_from_mission(*mission);
    dts_viewer::MatchConfig match_cfg;
    match_cfg.cap_limit     = static_cast<std::uint16_t>(std::max(1, cap_limit));
    match_cfg.time_limit_ms = static_cast<std::uint32_t>(
        std::max(1, time_limit_min)) * 60u * 1000u;
    dts_viewer::MatchState match(match_cfg);
    dts_viewer::ChatChannel chat;
    auto prev_phase = match.phase();
    // Spec 28/11 — assemble the map cycle. CSV overrides --mission.
    std::vector<std::string> cycle_list;
    if (!map_cycle_csv.empty()) {
        std::size_t start = 0;
        while (start < map_cycle_csv.size()) {
            const auto comma = map_cycle_csv.find(',', start);
            const auto end = (comma == std::string::npos) ? map_cycle_csv.size() : comma;
            std::string name = map_cycle_csv.substr(start, end - start);
            if (!name.empty()) cycle_list.push_back(std::move(name));
            start = end + 1;
        }
    }
    if (cycle_list.empty()) cycle_list.push_back(mission_name);
    dts_viewer::MapCycle map_cycle(cycle_list);
    std::fprintf(stderr, "[server] map cycle: %zu map(s), current=%s\n",
                 cycle_list.size(), map_cycle.current().c_str());
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
            // Spec 28/06 — projectile sim + hit detection.
            tick_hits.clear();
            projectiles.tick_fires(listener->sessions(), dt_sec);
            projectiles.tick_motion(listener->sessions(), dt_sec, tick_hits);
            // Spec 28/07 — damage + respawn.
            tick_kills.clear();
            const std::uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            dts_viewer::apply_hits(listener->sessions(), tick_hits, tick_kills,
                                   now_ms, damage_rules);
            // Spec 28/08 — flags must hear about kills before respawn moves
            // the corpse (so the drop position matches the death position).
            for (const auto& k : tick_kills) {
                // The session's player_state still holds the death pos
                // because respawn_due hasn't fired yet this tick.
                for (auto* s : listener->sessions().active_sessions()) {
                    if (s && s->player_slot == k.victim_slot) {
                        flags.on_player_died(k.victim_slot, s->player_state.pos, now_ms);
                        break;
                    }
                }
            }
            dts_viewer::respawn_due(listener->sessions(), listener->spawn_points(),
                                    now_ms, damage_rules);
            tick_captures.clear();
            flags.tick(listener->sessions(), tick_captures, now_ms);
            // Spec 28/09 — score captures and drive match phase.
            for (const auto& cap : tick_captures) {
                match.on_capture(cap, now_ms);
            }
            match.tick(listener->sessions(), now_ms);
            // Spec 28/10 — feed system events into the chat channel.
            for (const auto& k : tick_kills) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "Slot %u killed Slot %u",
                    (unsigned)k.killer_slot, (unsigned)k.victim_slot);
                chat.emit_system(buf, now_ms);
            }
            for (const auto& cap : tick_captures) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "Slot %u captured %s flag",
                    (unsigned)cap.capturer_slot,
                    (cap.flag_taken == dts_viewer::Team::Red ? "Red" : "Blue"));
                chat.emit_system(buf, now_ms);
            }
            if (match.phase() != prev_phase) {
                if (match.phase() == dts_viewer::MatchPhase::Live) {
                    chat.emit_system("Match begins!", now_ms);
                } else if (match.phase() == dts_viewer::MatchPhase::EndHold) {
                    char buf[128];
                    const auto w = match.winner();
                    std::snprintf(buf, sizeof(buf),
                        "Match over — winner: %s (Red %u | Blue %u)",
                        (w == dts_viewer::Team::Red  ? "Red"  :
                         w == dts_viewer::Team::Blue ? "Blue" : "Draw"),
                        (unsigned)match.red_score(),
                        (unsigned)match.blue_score());
                    chat.emit_system(buf, now_ms);
                } else if (prev_phase == dts_viewer::MatchPhase::EndHold
                           && match.phase() == dts_viewer::MatchPhase::Warmup
                           && !skip_mission) {
                    // Spec 28/11 — cycle to the next map.
                    map_cycle.advance();
                    const auto& next = map_cycle.current();
                    std::fprintf(stderr, "[server] cycling to map: %s\n", next.c_str());
                    chat.emit_system(std::string("Map changing: ") + next, now_ms);
                    const auto missions_dir = tribes_dir / "base" / "missions";
                    const auto base_dir     = tribes_dir / "base";
                    auto reloaded = dts_viewer::load_mission(missions_dir, base_dir, next);
                    if (reloaded) {
                        if (!skip_mission) {
                            dts_viewer::mission_sounds_unload(mission_audio, sink);
                        }
                        mission       = std::move(reloaded);
                        mission_audio = dts_viewer::mission_sounds_load(
                            mission->scene, base_dir, sink);
                        listener->set_spawn_points(
                            dts_viewer::extract_spawn_points(*mission));
                        listener->set_loaded_mission(&*mission);
                        flags.load_from_mission(*mission);
                        // Reset every active session's authoritative state.
                        for (auto* s : listener->sessions().active_sessions()) {
                            if (!s) continue;
                            s->kills  = 0;
                            s->deaths = 0;
                            s->player_state.health = 100.0f;
                            s->life   = dts_viewer::Session::LifeState::Alive;
                            s->pending_moves.clear();
                            dts_viewer::place_at_spawn(*s, listener->spawn_points());
                        }
                    } else {
                        std::fprintf(stderr,
                            "[server] map cycle: failed to load %s — staying on current\n",
                            next.c_str());
                    }
                }
                prev_phase = match.phase();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            if (listener) {
                const auto s = listener->stats();
                std::fprintf(stderr,
                    "[server] tick %llu  net: req=%llu acc=%llu rej=%llu data=%llu ghost=%llu unk=%llu  moves=%llu (bad=%llu)  osgb=%llup/%llur/%lluB  sessions=%llu (dropped=%llu)\n",
                    (unsigned long long)tick,
                    (unsigned long long)s.request_connects_received,
                    (unsigned long long)s.accept_connects_sent,
                    (unsigned long long)s.reject_connects_sent,
                    (unsigned long long)s.data_packets_received,
                    (unsigned long long)s.ghost_bursts_sent,
                    (unsigned long long)s.unknown_packets_received,
                    (unsigned long long)s.movecommands_received,
                    (unsigned long long)s.malformed_movecommands,
                    (unsigned long long)s.ghost_emit_packets,
                    (unsigned long long)s.ghost_emit_records,
                    (unsigned long long)s.ghost_emit_bytes,
                    (unsigned long long)s.sessions_active,
                    (unsigned long long)s.sessions_dropped);
                auto active = listener->sessions().active_sessions();
                if (!active.empty()) {
                    const auto& ps = active.front()->player_state;
                    std::fprintf(stderr,
                        "[server]   slot %u pos=(%.2f,%.2f,%.2f) yaw=%.2f pitch=%.2f q=%zu\n",
                        active.front()->player_slot,
                        ps.pos.x, ps.pos.y, ps.pos.z,
                        ps.yaw, ps.pitch,
                        active.front()->pending_moves.size());
                }
                const auto& pjs = projectiles.stats();
                std::fprintf(stderr,
                    "[server]   proj: fired=%llu hits=%llu expired=%llu active=%llu\n",
                    (unsigned long long)pjs.fired,
                    (unsigned long long)pjs.hits,
                    (unsigned long long)pjs.expired,
                    (unsigned long long)pjs.active);
                const char* phn = (match.phase() == dts_viewer::MatchPhase::Warmup)  ? "WARMUP"
                                : (match.phase() == dts_viewer::MatchPhase::Live)    ? "LIVE"
                                : "ENDHOLD";
                std::fprintf(stderr,
                    "[server]   match: %s  R=%u  B=%u\n",
                    phn,
                    (unsigned)match.red_score(),
                    (unsigned)match.blue_score());
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
