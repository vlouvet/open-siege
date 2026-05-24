#include <osengine/flag_state.hpp>

#include <osengine/mission_loader.hpp>
#include <osengine/session_table.hpp>
#include <osengine/team_assigner.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <glm/geometric.hpp>

namespace dts_viewer
{

namespace
{

bool iname_contains(const std::string& hay, const char* needle)
{
    std::string h, n(needle);
    h.reserve(hay.size());
    for (char c : hay) h.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    for (char& c : n)  c = static_cast<char>(std::tolower((unsigned char)c));
    return h.find(n) != std::string::npos;
}

Team classify_flag(const std::string& datablock, const std::string& instance)
{
    auto red  = [](const std::string& s) {
        return iname_contains(s, "team1") || iname_contains(s, "red");
    };
    auto blue = [](const std::string& s) {
        return iname_contains(s, "team2") || iname_contains(s, "blue");
    };
    if (red(datablock)  || red(instance))  return Team::Red;
    if (blue(datablock) || blue(instance)) return Team::Blue;
    return Team::Spectator;
}

void walk_for_flags(const studio::content::mission::scene_node& node,
                    Flag& red, Flag& blue,
                    bool& red_seen, bool& blue_seen)
{
    if (iname_contains(node.class_name, "Item")
        || iname_contains(node.class_name, "StaticShape")
        || iname_contains(node.class_name, "Marker"))
    {
        if (const auto* m = std::get_if<
                studio::content::mission::node_marker>(&node.payload)) {
            const std::string db = m->data_block.name;
            if (iname_contains(db, "flagstand") || iname_contains(db, "flag")) {
                const Team t = classify_flag(db, node.instance_name.value_or(""));
                const glm::vec3 pos{m->xf.position[0], m->xf.position[1], m->xf.position[2]};
                if (t == Team::Red && !red_seen) {
                    red.home_position = pos; red.position = pos;
                    red.team = Team::Red; red_seen = true;
                } else if (t == Team::Blue && !blue_seen) {
                    blue.home_position = pos; blue.position = pos;
                    blue.team = Team::Blue; blue_seen = true;
                }
            }
        }
        if (const auto* si = std::get_if<
                studio::content::mission::node_item>(&node.payload)) {
            const std::string db = si->data_block.name;
            if (iname_contains(db, "flag")) {
                const Team t = classify_flag(db, node.instance_name.value_or(""));
                const glm::vec3 pos{si->xf.position[0], si->xf.position[1], si->xf.position[2]};
                if (t == Team::Red && !red_seen) {
                    red.home_position = pos; red.position = pos;
                    red.team = Team::Red; red_seen = true;
                } else if (t == Team::Blue && !blue_seen) {
                    blue.home_position = pos; blue.position = pos;
                    blue.team = Team::Blue; blue_seen = true;
                }
            }
        }
    }
    for (const auto& c : node.children) {
        walk_for_flags(c, red, blue, red_seen, blue_seen);
    }
}

} // namespace

Flag& FlagWorld::flag_for(Team t)
{
    return (t == Team::Red) ? red_ : blue_;
}

const Flag& FlagWorld::flag_for(Team t) const
{
    return (t == Team::Red) ? red_ : blue_;
}

void FlagWorld::reset_to_home(Flag& f)
{
    f.phase         = FlagPhase::AtHome;
    f.position      = f.home_position;
    f.carrier_slot  = 0xFFFFu;
    f.dropped_at_ms = 0;
}

void FlagWorld::load_from_mission(const LoadedMission& mission)
{
    bool red_seen = false, blue_seen = false;
    walk_for_flags(mission.scene.root, red_, blue_, red_seen, blue_seen);
    // Fallback: if no FlagStand entities found, place flags 100 m apart
    // along +Z so a mini-CTF demo still runs on a non-CTF map.
    if (!red_seen) {
        red_.home_position = {-50.0f, 0.0f, 0.0f};
        red_.position      = red_.home_position;
        red_.team          = Team::Red;
    }
    if (!blue_seen) {
        blue_.home_position = {50.0f, 0.0f, 0.0f};
        blue_.position      = blue_.home_position;
        blue_.team          = Team::Blue;
    }
    reset_to_home(red_);
    reset_to_home(blue_);
    loaded_ = true;
    std::fprintf(stderr,
        "[flags] loaded: Red @ (%.1f,%.1f,%.1f) Blue @ (%.1f,%.1f,%.1f)\n",
        red_.home_position.x, red_.home_position.y, red_.home_position.z,
        blue_.home_position.x, blue_.home_position.y, blue_.home_position.z);
}

void FlagWorld::seed_for_test(const glm::vec3& red_home,
                              const glm::vec3& blue_home)
{
    red_.team           = Team::Red;
    red_.home_position  = red_home;
    blue_.team          = Team::Blue;
    blue_.home_position = blue_home;
    reset_to_home(red_);
    reset_to_home(blue_);
    loaded_ = true;
}

void FlagWorld::on_player_died(std::uint16_t slot,
                               const glm::vec3& death_pos,
                               std::uint64_t now_ms)
{
    for (Flag* f : { &red_, &blue_ }) {
        if (f->phase == FlagPhase::Carried && f->carrier_slot == slot) {
            f->phase         = FlagPhase::Dropped;
            f->position      = death_pos;
            f->carrier_slot  = 0xFFFFu;
            f->dropped_at_ms = now_ms;
            std::fprintf(stderr,
                "[flag-drop] %s flag dropped by slot %u at (%.1f,%.1f,%.1f)\n",
                (f->team == Team::Red ? "Red" : "Blue"),
                slot, death_pos.x, death_pos.y, death_pos.z);
        }
    }
}

void FlagWorld::tick(SessionTable& sessions,
                     std::vector<CaptureEvent>& out_caps,
                     std::uint64_t now_ms)
{
    if (!loaded_) return;

    auto active = sessions.active_sessions();

    // Phase 1: carriers — keep flag glued to player; check capture.
    for (Flag* f : { &red_, &blue_ }) {
        if (f->phase != FlagPhase::Carried) continue;
        Session* carrier = nullptr;
        for (auto* s : active) {
            if (s && s->player_slot == f->carrier_slot) { carrier = s; break; }
        }
        if (!carrier) {
            // Carrier vanished (disconnect mid-carry). Drop the flag
            // where it last was.
            f->phase         = FlagPhase::Dropped;
            f->carrier_slot  = 0xFFFFu;
            f->dropped_at_ms = now_ms;
            continue;
        }
        if (carrier->life != Session::LifeState::Alive) continue;
        f->position = carrier->player_state.pos;

        // Capture check: carrier is bringing the ENEMY flag (f) home
        // when they reach their OWN team's flag stand AND their own
        // flag is at home.
        const Flag& own_flag = (carrier->team == Team::Red) ? red_ : blue_;
        const Flag* enemy_flag = (carrier->team == Team::Red) ? &blue_ : &red_;
        if (enemy_flag != f) continue;   // carrier doesn't have enemy flag

        const float d = glm::length(carrier->player_state.pos - own_flag.home_position);
        if (d <= kPickupRadius && own_flag.phase == FlagPhase::AtHome) {
            CaptureEvent ev;
            ev.capturer_slot = carrier->player_slot;
            ev.flag_taken    = f->team;
            out_caps.push_back(ev);
            std::fprintf(stderr,
                "[capture] slot %u (%s) captured %s flag\n",
                carrier->player_slot,
                (carrier->team == Team::Red ? "Red" : "Blue"),
                (f->team == Team::Red ? "Red" : "Blue"));
            reset_to_home(red_);
            reset_to_home(blue_);
        }
    }

    // Phase 2: pickup / return on AtHome / Dropped flags.
    for (Flag* f : { &red_, &blue_ }) {
        if (f->phase == FlagPhase::Carried) continue;
        // Deterministic order: scan by ascending slot id so two
        // simultaneous touches resolve to the lower-slot player.
        Session* best = nullptr;
        for (auto* s : active) {
            if (!s) continue;
            if (s->life != Session::LifeState::Alive) continue;
            if (s->team == Team::Spectator) continue;
            const float d = glm::length(s->player_state.pos - f->position);
            if (d > kPickupRadius) continue;
            if (!best || s->player_slot < best->player_slot) best = s;
        }
        if (!best) {
            // Phase 3: auto-return for stale drops.
            if (f->phase == FlagPhase::Dropped
                && now_ms >= f->dropped_at_ms + kAutoReturnMs)
            {
                std::fprintf(stderr,
                    "[flag-return] %s flag auto-returned after timeout\n",
                    (f->team == Team::Red ? "Red" : "Blue"));
                reset_to_home(*f);
            }
            continue;
        }

        // Own-team touch on a Dropped flag → instant return.
        if (best->team == f->team && f->phase == FlagPhase::Dropped) {
            std::fprintf(stderr,
                "[flag-return] %s flag returned by slot %u\n",
                (f->team == Team::Red ? "Red" : "Blue"), best->player_slot);
            reset_to_home(*f);
            continue;
        }

        // Enemy-team touch → pickup.
        if (best->team != f->team) {
            f->phase        = FlagPhase::Carried;
            f->carrier_slot = best->player_slot;
            f->dropped_at_ms = 0;
            std::fprintf(stderr,
                "[flag-pickup] slot %u (%s) picked up %s flag\n",
                best->player_slot,
                (best->team == Team::Red ? "Red" : "Blue"),
                (f->team == Team::Red ? "Red" : "Blue"));
        }
    }
}

int FlagWorld::selftest()
{
    FlagWorld fw;
    fw.seed_for_test({-100.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f});
    if (!fw.red() || !fw.blue()) {
        std::fputs("[flag-selftest] flags not loaded\n", stderr); return 1;
    }
    if (fw.red()->phase != FlagPhase::AtHome
        || fw.blue()->phase != FlagPhase::AtHome) {
        std::fputs("[flag-selftest] flags not AtHome after seed\n", stderr);
        return 1;
    }

    SessionTable table(4);
    const std::uint8_t n[3] = { 0xaa, 0xbb, 0xcc };
    studio::content::net::Endpoint p1{"127.0.0.1", 62001};
    studio::content::net::Endpoint p2{"127.0.0.1", 62002};
    Session* red_player  = table.allocate(p1, n, 0);
    Session* blue_player = table.allocate(p2, n, 0);
    red_player->team  = Team::Red;
    blue_player->team = Team::Blue;
    red_player->life  = Session::LifeState::Alive;
    blue_player->life = Session::LifeState::Alive;

    // 1) Blue player walks onto Red flag stand → pickup of Red flag.
    blue_player->player_state.pos = {-100.0f, 0.0f, 0.0f};
    std::vector<CaptureEvent> caps;
    fw.tick(table, caps, 1000);
    if (fw.red()->phase != FlagPhase::Carried
        || fw.red()->carrier_slot != blue_player->player_slot) {
        std::fputs("[flag-selftest] Red flag should be carried by blue player\n", stderr);
        return 1;
    }
    // Position should now track the carrier.
    if (glm::length(fw.red()->position - blue_player->player_state.pos) > 0.01f) {
        std::fputs("[flag-selftest] Red flag pos != carrier pos\n", stderr);
        return 1;
    }

    // 2) Blue player carries it home to Blue flag stand → capture event.
    blue_player->player_state.pos = {100.0f, 0.0f, 0.0f};
    fw.tick(table, caps, 1100);
    if (caps.size() != 1
        || caps[0].capturer_slot != blue_player->player_slot
        || caps[0].flag_taken != Team::Red) {
        std::fprintf(stderr, "[flag-selftest] capture event missing/wrong (got %zu)\n",
                     caps.size());
        return 1;
    }
    if (fw.red()->phase != FlagPhase::AtHome
        || fw.blue()->phase != FlagPhase::AtHome) {
        std::fputs("[flag-selftest] flags not reset to home after capture\n", stderr);
        return 1;
    }

    // 3) Carrier death drops flag.
    caps.clear();
    blue_player->player_state.pos = {-100.0f, 0.0f, 0.0f};
    fw.tick(table, caps, 2000);   // re-pickup
    if (fw.red()->phase != FlagPhase::Carried) {
        std::fputs("[flag-selftest] re-pickup failed\n", stderr); return 1;
    }
    fw.on_player_died(blue_player->player_slot, {10.0f, 0.0f, 10.0f}, 3000);
    if (fw.red()->phase != FlagPhase::Dropped) {
        std::fputs("[flag-selftest] flag should be Dropped after carrier death\n", stderr);
        return 1;
    }
    if (glm::length(fw.red()->position - glm::vec3{10.0f, 0.0f, 10.0f}) > 0.01f) {
        std::fputs("[flag-selftest] dropped flag pos != death pos\n", stderr);
        return 1;
    }

    // 4) Same-team (Red) player touches dropped Red flag → instant return.
    red_player->player_state.pos = {10.0f, 0.0f, 10.0f};
    fw.tick(table, caps, 3100);
    if (fw.red()->phase != FlagPhase::AtHome) {
        std::fputs("[flag-selftest] dropped flag should return on own-team touch\n", stderr);
        return 1;
    }

    // 5) Drop again, wait 30+ seconds, auto-return.
    fw.tick(table, caps, 4000);   // dummy tick
    blue_player->life = Session::LifeState::Alive;
    blue_player->player_state.pos = {-100.0f, 0.0f, 0.0f};
    red_player->player_state.pos  = {-999.0f, 0.0f, -999.0f};  // out of pickup
    fw.tick(table, caps, 4100);   // blue picks up red
    if (fw.red()->phase != FlagPhase::Carried) {
        std::fputs("[flag-selftest] second pickup failed\n", stderr); return 1;
    }
    fw.on_player_died(blue_player->player_slot, {5.0f, 0.0f, 5.0f}, 5000);
    // Tick at exactly +30s to trigger auto-return. With nobody nearby
    // the auto-return path runs.
    fw.tick(table, caps, 5000 + FlagWorld::kAutoReturnMs);
    if (fw.red()->phase != FlagPhase::AtHome) {
        std::fputs("[flag-selftest] auto-return did not fire\n", stderr);
        return 1;
    }

    std::fputs("[flag-selftest] OK — pickup, capture, drop, own-team return, auto-return\n",
               stderr);
    return 0;
}

} // namespace dts_viewer
