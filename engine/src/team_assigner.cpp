#include <osengine/team_assigner.hpp>

#include <osengine/mission_loader.hpp>
#include <osengine/session_table.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dts_viewer
{

namespace
{

bool icontains(const std::string& hay, const char* needle)
{
    std::string h, n(needle);
    h.reserve(hay.size());
    for (char c : hay) h.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    for (char& c : n)  c = static_cast<char>(std::tolower((unsigned char)c));
    return h.find(n) != std::string::npos;
}

Team classify_marker(const std::string& datablock_name,
                     const std::string& instance_name)
{
    // The Tribes 1 stock CTF maps use marker datablock names like
    // "SpawnSphereMarker_team1" / "_team2" or sometimes encode the
    // team in the instance name. Be generous.
    auto looks_red = [](const std::string& s) {
        return icontains(s, "team1") || icontains(s, "red");
    };
    auto looks_blue = [](const std::string& s) {
        return icontains(s, "team2") || icontains(s, "blue");
    };
    if (looks_red(datablock_name)  || looks_red(instance_name))  return Team::Red;
    if (looks_blue(datablock_name) || looks_blue(instance_name)) return Team::Blue;
    return Team::Spectator;   // neutral
}

void walk(const studio::content::mission::scene_node& node,
          bool inside_droppoints,
          std::vector<SpawnPoint>& out)
{
    const bool now_inside = inside_droppoints
        || (node.class_name == "DropPoints");
    if (now_inside && node.class_name == "Marker") {
        if (const auto* m = std::get_if<
                studio::content::mission::node_marker>(&node.payload)) {
            SpawnPoint sp;
            sp.pos = {m->xf.position[0], m->xf.position[1], m->xf.position[2]};
            // Tribes .mis transforms store Euler XYZ in the rotation
            // array's first three slots; yaw is rotation[2] (Z-axis
            // turn in world space).
            sp.yaw_radians = m->xf.rotation[2];
            sp.team = classify_marker(
                m->data_block.name,
                node.instance_name.value_or(""));
            sp.marker_name = node.instance_name.value_or(m->data_block.name);
            out.push_back(std::move(sp));
        }
    }
    for (const auto& c : node.children) walk(c, now_inside, out);
}

} // namespace

std::vector<SpawnPoint> extract_spawn_points(const LoadedMission& mission)
{
    std::vector<SpawnPoint> out;
    walk(mission.scene.root, false, out);
    return out;
}

Team pick_team(const SessionTable& sessions, bool round_robin)
{
    if (!round_robin) return Team::Red;
    // SessionTable doesn't expose const access to all sessions yet.
    // Cast away const for a one-shot read of active_sessions(). The
    // call doesn't mutate; the non-const-ness is a side effect of
    // active_sessions() returning Session* (so callers can mutate).
    auto& mut = const_cast<SessionTable&>(sessions);
    std::size_t red = 0, blue = 0;
    for (auto* s : mut.active_sessions()) {
        if (!s) continue;
        if (s->team == Team::Red)  ++red;
        if (s->team == Team::Blue) ++blue;
    }
    return (red <= blue) ? Team::Red : Team::Blue;
}

void place_at_spawn(Session& session,
                    const std::vector<SpawnPoint>& spawns)
{
    const SpawnPoint* picked = nullptr;
    // Pass 1 — strict team match.
    for (const auto& sp : spawns) {
        if (sp.team == session.team) { picked = &sp; break; }
    }
    // Pass 2 — neutral marker.
    if (!picked) {
        for (const auto& sp : spawns) {
            if (sp.team == Team::Spectator) { picked = &sp; break; }
        }
    }
    // Pass 3 — any marker.
    if (!picked && !spawns.empty()) picked = &spawns.front();

    if (picked) {
        session.spawn_pos = picked->pos;
        session.spawn_yaw = picked->yaw_radians;
    } else {
        session.spawn_pos = {0.0f, 0.0f, 0.0f};
        session.spawn_yaw = 0.0f;
    }
    // Teleport the authoritative player state into the spawn. The
    // first world_tick after this call simulates from here.
    session.player_state.pos      = session.spawn_pos;
    session.player_state.vel      = {0.0f, 0.0f, 0.0f};
    session.player_state.yaw      = session.spawn_yaw;
    session.player_state.pitch    = 0.0f;
}

void respawn(Session& session, const std::vector<SpawnPoint>& spawns)
{
    place_at_spawn(session, spawns);
}

int team_assigner_selftest()
{
    // Synthesise 4 spawn points: 2 Red, 2 Blue.
    std::vector<SpawnPoint> spawns = {
        { {10.0f, 0.0f,  0.0f}, 0.0f, Team::Red,  "SpawnRed_A"  },
        { {20.0f, 0.0f,  0.0f}, 0.0f, Team::Red,  "SpawnRed_B"  },
        { {-10.0f, 0.0f, 0.0f}, 0.0f, Team::Blue, "SpawnBlue_A" },
        { {-20.0f, 0.0f, 0.0f}, 0.0f, Team::Blue, "SpawnBlue_B" },
    };

    SessionTable table(8);
    for (int i = 0; i < 4; ++i) {
        studio::content::net::Endpoint peer{"127.0.0.1",
            static_cast<std::uint16_t>(40000 + i)};
        const std::uint8_t nonce[3] = {
            static_cast<std::uint8_t>(i),
            static_cast<std::uint8_t>(i + 1),
            static_cast<std::uint8_t>(i + 2)
        };
        Session* s = table.allocate(peer, nonce, 0);
        if (!s) {
            std::fputs("[team-selftest] allocate failed\n", stderr);
            return 1;
        }
        s->team = pick_team(table, /*round_robin*/ true);
        place_at_spawn(*s, spawns);
    }

    std::size_t red = 0, blue = 0;
    for (auto* s : table.active_sessions()) {
        if (!s) continue;
        if (s->team == Team::Red)  ++red;
        if (s->team == Team::Blue) ++blue;
        // Every session should be at one of the 4 spawn points.
        bool on_a_spawn = false;
        for (const auto& sp : spawns) {
            if (std::fabs(s->player_state.pos.x - sp.pos.x) < 0.01f
                && std::fabs(s->player_state.pos.y - sp.pos.y) < 0.01f
                && std::fabs(s->player_state.pos.z - sp.pos.z) < 0.01f) {
                on_a_spawn = true; break;
            }
        }
        if (!on_a_spawn) {
            std::fprintf(stderr,
                "[team-selftest] slot %u not on any spawn pt: pos=(%.2f,%.2f,%.2f)\n",
                s->player_slot, s->player_state.pos.x,
                s->player_state.pos.y, s->player_state.pos.z);
            return 1;
        }
    }
    if (red != 2 || blue != 2) {
        std::fprintf(stderr,
            "[team-selftest] expected 2/2 red/blue split, got %zu/%zu\n",
            red, blue);
        return 1;
    }

    // FIFO mode: 4 sessions all go Red.
    SessionTable table2(8);
    for (int i = 0; i < 4; ++i) {
        studio::content::net::Endpoint peer{"127.0.0.1",
            static_cast<std::uint16_t>(50000 + i)};
        const std::uint8_t nonce[3] = { 0,0,0 };
        Session* s = table2.allocate(peer, nonce, 0);
        s->team = pick_team(table2, /*round_robin*/ false);
        place_at_spawn(*s, spawns);
    }
    std::size_t red2 = 0;
    for (auto* s : table2.active_sessions()) {
        if (s && s->team == Team::Red) ++red2;
    }
    if (red2 != 4) {
        std::fprintf(stderr, "[team-selftest] FIFO mode: expected 4 red, got %zu\n", red2);
        return 1;
    }

    std::fputs("[team-selftest] OK — 2/2 balance, FIFO fallback, spawn placement\n",
               stderr);
    return 0;
}

} // namespace dts_viewer
