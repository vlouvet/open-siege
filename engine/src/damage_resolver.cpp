#include <osengine/damage_resolver.hpp>

#include <osengine/session_table.hpp>
#include <osengine/team_assigner.hpp>

#include <algorithm>
#include <cstdio>

namespace dts_viewer
{

namespace
{

Session* find_slot(SessionTable& sessions, std::uint16_t slot)
{
    for (auto* s : sessions.active_sessions()) {
        if (s && s->player_slot == slot) return s;
    }
    return nullptr;
}

} // namespace

void apply_hits(SessionTable& sessions,
                const std::vector<HitEvent>& hits,
                std::vector<KillEvent>& out_kills,
                std::uint64_t now_ms,
                const DamageRules& rules)
{
    for (const auto& h : hits) {
        Session* victim  = find_slot(sessions, h.victim_slot);
        Session* shooter = (h.shooter_slot != 0xFFFFu)
                         ? find_slot(sessions, h.shooter_slot)
                         : nullptr;
        if (!victim) continue;
        if (victim->life == Session::LifeState::Dead) continue;

        // Friendly-fire filter: same-team hits drop unless rules allow.
        if (shooter && !rules.friendly_fire
            && shooter->team == victim->team
            && victim->team != Team::Spectator)
        {
            continue;
        }

        victim->player_state.health -= h.damage;
        if (victim->player_state.health > 0.0f) continue;

        victim->player_state.health = 0.0f;
        victim->life               = Session::LifeState::Dead;
        victim->died_at_ms         = now_ms;
        victim->deaths            += 1;
        victim->last_killer        = h.shooter_slot;
        if (shooter && shooter != victim) {
            shooter->kills += 1;
        }
        KillEvent k;
        k.killer_slot = h.shooter_slot;
        k.victim_slot = h.victim_slot;
        k.weapon      = h.weapon;
        out_kills.push_back(k);
        std::fprintf(stderr,
            "[kill] slot %u killed slot %u (weapon %u) — kills=%u deaths=%u\n",
            (unsigned)k.killer_slot, (unsigned)k.victim_slot,
            (unsigned)k.weapon,
            (unsigned)(shooter ? shooter->kills : 0),
            (unsigned)victim->deaths);
    }
}

void respawn_due(SessionTable& sessions,
                 const std::vector<SpawnPoint>& spawns,
                 std::uint64_t now_ms,
                 const DamageRules& rules)
{
    const std::uint64_t delay_ms = static_cast<std::uint64_t>(
        rules.respawn_delay_sec * 1000.0f);
    for (auto* s : sessions.active_sessions()) {
        if (!s) continue;
        if (s->life != Session::LifeState::Dead) continue;
        if (now_ms < s->died_at_ms + delay_ms) continue;
        place_at_spawn(*s, spawns);
        s->player_state.health = rules.health_max;
        s->life                = Session::LifeState::Alive;
        s->pending_moves.clear();
        std::fprintf(stderr,
            "[respawn] slot %u back in @ (%.1f,%.1f,%.1f)\n",
            s->player_slot,
            s->player_state.pos.x, s->player_state.pos.y, s->player_state.pos.z);
    }
}

int damage_resolver_selftest()
{
    SessionTable table(4);
    const std::uint8_t n1[3] = { 1, 2, 3 };
    const std::uint8_t n2[3] = { 4, 5, 6 };
    studio::content::net::Endpoint p1{"127.0.0.1", 61001};
    studio::content::net::Endpoint p2{"127.0.0.1", 61002};
    Session* killer = table.allocate(p1, n1, 0);
    Session* victim = table.allocate(p2, n2, 0);
    if (!killer || !victim) {
        std::fputs("[damage-selftest] allocate failed\n", stderr); return 1;
    }
    killer->team = Team::Red;
    victim->team = Team::Blue;
    killer->player_state.health = 100.0f;
    victim->player_state.health = 100.0f;

    std::vector<HitEvent> hits;
    std::vector<KillEvent> kills;
    HitEvent h;
    h.shooter_slot = killer->player_slot;
    h.victim_slot  = victim->player_slot;
    h.weapon       = WeaponType::Disc;
    h.damage       = 50.0f;

    // 50 dmg → victim still alive.
    hits.push_back(h);
    apply_hits(table, hits, kills, 1000);
    if (victim->player_state.health != 50.0f) {
        std::fprintf(stderr, "[damage-selftest] expected health=50 got %.1f\n",
                     victim->player_state.health);
        return 1;
    }
    if (victim->life != Session::LifeState::Alive) {
        std::fputs("[damage-selftest] victim should still be alive after 50 dmg\n", stderr);
        return 1;
    }
    if (!kills.empty()) {
        std::fputs("[damage-selftest] no kill expected on 50 dmg\n", stderr);
        return 1;
    }
    hits.clear();

    // Another 60 dmg → dead, kill event, killer counters bump.
    h.damage = 60.0f;
    hits.push_back(h);
    apply_hits(table, hits, kills, 1500);
    if (victim->life != Session::LifeState::Dead) {
        std::fputs("[damage-selftest] victim should be dead after 110 dmg\n", stderr);
        return 1;
    }
    if (kills.size() != 1
        || kills[0].killer_slot != killer->player_slot
        || kills[0].victim_slot != victim->player_slot) {
        std::fputs("[damage-selftest] kill event missing/wrong\n", stderr);
        return 1;
    }
    if (killer->kills != 1 || victim->deaths != 1) {
        std::fprintf(stderr,
            "[damage-selftest] scores: killer.kills=%u deaths=%u (expect 1/1)\n",
            (unsigned)killer->kills, (unsigned)victim->deaths);
        return 1;
    }

    // Respawn: not yet (1.5s delay required).
    std::vector<SpawnPoint> spawns = {
        { {0.0f, 0.0f, 0.0f}, 0.0f, Team::Red,  "spawn_red"  },
        { {100.0f, 0.0f, 0.0f}, 0.0f, Team::Blue, "spawn_blue" },
    };
    respawn_due(table, spawns, 2000);          // 500 ms after death, still dead
    if (victim->life != Session::LifeState::Dead) {
        std::fputs("[damage-selftest] respawn fired early\n", stderr);
        return 1;
    }
    respawn_due(table, spawns, 1500 + 3500);   // 3.5s after death, should respawn
    if (victim->life != Session::LifeState::Alive) {
        std::fputs("[damage-selftest] respawn did not fire after delay\n", stderr);
        return 1;
    }
    if (victim->player_state.health != 100.0f) {
        std::fprintf(stderr, "[damage-selftest] respawn health=%.1f (expect 100)\n",
                     victim->player_state.health);
        return 1;
    }
    if (victim->player_state.pos.x != 100.0f) {
        std::fprintf(stderr, "[damage-selftest] respawn pos.x=%.1f (expect 100 blue spawn)\n",
                     victim->player_state.pos.x);
        return 1;
    }

    // Friendly fire is OFF by default — same-team hit drops.
    kills.clear();
    hits.clear();
    Session* victim2 = table.allocate(
        studio::content::net::Endpoint{"127.0.0.1", 61003},
        n1, 0);
    victim2->team = Team::Red;        // same as killer
    victim2->player_state.health = 100.0f;
    h.victim_slot = victim2->player_slot;
    h.damage      = 50.0f;
    hits.push_back(h);
    apply_hits(table, hits, kills, 9999);
    if (victim2->player_state.health != 100.0f) {
        std::fprintf(stderr, "[damage-selftest] FF blocked: expected 100, got %.1f\n",
                     victim2->player_state.health);
        return 1;
    }

    std::fputs("[damage-selftest] OK — damage, kill, respawn, FF block\n", stderr);
    return 0;
}

} // namespace dts_viewer
