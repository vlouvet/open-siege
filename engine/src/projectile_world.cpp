#include <osengine/projectile_world.hpp>

#include <osengine/session_table.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/gtc/constants.hpp>
#include <glm/geometric.hpp>

namespace dts_viewer
{

namespace
{

constexpr float kEyeOffsetY      = 1.6f;  // metres above feet
constexpr float kPlayerCapsuleR  = 0.4f;
constexpr float kPlayerCapsuleH  = 1.8f;  // total capsule height
constexpr float kProjectileR     = 0.15f;

struct WeaponSpec {
    float muzzle_speed_mps;
    float damage;
    float ttl_sec;
    float cooldown_sec;
};

WeaponSpec spec_for(WeaponType w)
{
    switch (w) {
        case WeaponType::Disc:  return { 200.0f, 30.0f, 5.0f, 0.250f };
        case WeaponType::Chain: return { 600.0f,  8.0f, 1.0f, 0.080f };
    }
    return { 200.0f, 0.0f, 1.0f, 1.0f };
}

// Closest-distance from point P to line segment A->B.
float dist_point_segment(const glm::vec3& p, const glm::vec3& a,
                         const glm::vec3& b)
{
    const glm::vec3 ab = b - a;
    const float t_denom = glm::dot(ab, ab);
    if (t_denom <= 1e-6f) return glm::length(p - a);
    float t = glm::dot(p - a, ab) / t_denom;
    t = glm::clamp(t, 0.0f, 1.0f);
    return glm::length(p - (a + t * ab));
}

// Distance between two line segments (capsule axis vs projectile path).
// Simplified: project P (capsule axis midpoint) onto AB; if within
// segment, use the perpendicular distance; otherwise clamp endpoints.
// For v1's collision filter this is a good-enough capsule sweep.
float dist_capsule_segment(const glm::vec3& cap_bottom,
                           const glm::vec3& cap_top,
                           const glm::vec3& seg_a,
                           const glm::vec3& seg_b)
{
    // Test 4 representative distances: projectile-end vs cap-axis (twice),
    // cap-end vs projectile (twice). Take the minimum.
    const float d1 = dist_point_segment(seg_a, cap_bottom, cap_top);
    const float d2 = dist_point_segment(seg_b, cap_bottom, cap_top);
    const float d3 = dist_point_segment(cap_bottom, seg_a, seg_b);
    const float d4 = dist_point_segment(cap_top,    seg_a, seg_b);
    return std::min(std::min(d1, d2), std::min(d3, d4));
}

glm::vec3 forward_from_yaw_pitch(float yaw, float pitch)
{
    // yaw=0 -> +Z; positive pitch tilts up. Matches PlayerController v3.
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    return glm::vec3(sy * cp, sp, cy * cp);
}

} // namespace

ProjectileWorld::ProjectileWorld() = default;

void ProjectileWorld::tick_fires(SessionTable& sessions, float dt_sec)
{
    auto active = sessions.active_sessions();

    // Reap cooldown entries for sessions no longer present.
    cooldowns_.erase(std::remove_if(cooldowns_.begin(), cooldowns_.end(),
        [&](const SessionFireCooldown& c) {
            for (auto* s : active) if (s && s->player_slot == c.slot) return false;
            return true;
        }), cooldowns_.end());

    for (auto& c : cooldowns_) {
        c.remaining_sec = std::max(0.0f, c.remaining_sec - dt_sec);
    }

    for (auto* s : active) {
        if (!s) continue;
        if (s->team == Team::Spectator) continue;
        if (s->pending_moves.empty()) {
            // Check the latest applied input — for v1 we look at the next
            // queued move only (consumed by world_tick same tick). The
            // fire bit is sticky as long as the client keeps sending
            // trigger=true.
            continue;
        }
        bool wants_fire = false;
        for (const auto& m : s->pending_moves) {
            if (m.trigger) { wants_fire = true; break; }
        }
        if (!wants_fire) continue;

        auto it = std::find_if(cooldowns_.begin(), cooldowns_.end(),
            [&](const SessionFireCooldown& c) { return c.slot == s->player_slot; });
        if (it == cooldowns_.end()) {
            cooldowns_.push_back({s->player_slot, 0.0f});
            it = cooldowns_.end() - 1;
        }
        if (it->remaining_sec > 0.0f) continue;

        const WeaponType w = WeaponType::Disc;       // v1 always disc
        const WeaponSpec spec = spec_for(w);

        ActiveProjectile p;
        p.id            = next_id_++;
        p.owner_slot    = s->player_slot;
        p.weapon        = w;
        const glm::vec3 dir = forward_from_yaw_pitch(s->player_state.yaw,
                                                     s->player_state.pitch);
        p.pos           = s->player_state.pos + glm::vec3(0.0f, kEyeOffsetY, 0.0f)
                          + dir * 0.8f;
        p.vel           = dir * spec.muzzle_speed_mps;
        p.ttl_remaining = spec.ttl_sec;
        p.damage        = spec.damage;
        projectiles_.push_back(p);
        it->remaining_sec = spec.cooldown_sec;
        ++stats_.fired;
        std::fprintf(stderr,
            "[fire] slot %u disc id=%u from (%.1f,%.1f,%.1f) dir=(%.2f,%.2f,%.2f)\n",
            s->player_slot, p.id, p.pos.x, p.pos.y, p.pos.z,
            dir.x, dir.y, dir.z);
    }
}

void ProjectileWorld::tick_motion(SessionTable& sessions, float dt_sec,
                                  std::vector<HitEvent>& out_hits)
{
    auto active = sessions.active_sessions();

    for (auto it = projectiles_.begin(); it != projectiles_.end(); ) {
        const glm::vec3 seg_a = it->pos;
        const glm::vec3 seg_b = it->pos + it->vel * dt_sec;
        bool hit = false;
        for (auto* s : active) {
            if (!s) continue;
            if (s->player_slot == it->owner_slot) continue;
            if (s->team == Team::Spectator) continue;
            const glm::vec3 cap_bottom = s->player_state.pos;
            const glm::vec3 cap_top    = s->player_state.pos
                                       + glm::vec3(0.0f, kPlayerCapsuleH, 0.0f);
            const float d = dist_capsule_segment(cap_bottom, cap_top, seg_a, seg_b);
            if (d <= (kPlayerCapsuleR + kProjectileR)) {
                HitEvent ev;
                ev.shooter_slot  = it->owner_slot;
                ev.victim_slot   = s->player_slot;
                ev.weapon        = it->weapon;
                ev.impact_pos    = seg_b;
                ev.damage        = it->damage;
                ev.projectile_id = it->id;
                out_hits.push_back(ev);
                ++stats_.hits;
                std::fprintf(stderr,
                    "[hit] proj %u: slot %u -> slot %u damage=%.1f at (%.1f,%.1f,%.1f)\n",
                    ev.projectile_id, ev.shooter_slot, ev.victim_slot,
                    ev.damage, ev.impact_pos.x, ev.impact_pos.y, ev.impact_pos.z);
                hit = true;
                break;
            }
        }

        if (hit) {
            it = projectiles_.erase(it);
            continue;
        }

        it->pos = seg_b;
        it->ttl_remaining -= dt_sec;
        if (it->ttl_remaining <= 0.0f) {
            ++stats_.expired;
            it = projectiles_.erase(it);
        } else {
            ++it;
        }
    }
    stats_.active = projectiles_.size();
}

std::vector<HitEvent> ProjectileWorld::tick(SessionTable& sessions,
                                             float dt_sec)
{
    std::vector<HitEvent> hits;
    tick_fires(sessions, dt_sec);
    tick_motion(sessions, dt_sec, hits);
    return hits;
}

int ProjectileWorld::selftest()
{
    // Two sessions facing each other at 10 m on the +Z axis.
    SessionTable table(4);
    const std::uint8_t n1[3] = { 1, 2, 3 };
    const std::uint8_t n2[3] = { 4, 5, 6 };
    studio::content::net::Endpoint p1{"127.0.0.1", 60001};
    studio::content::net::Endpoint p2{"127.0.0.1", 60002};
    Session* shooter = table.allocate(p1, n1, 0);
    Session* victim  = table.allocate(p2, n2, 0);
    if (!shooter || !victim) {
        std::fputs("[projectile-selftest] allocate failed\n", stderr);
        return 1;
    }
    shooter->team = Team::Red;
    victim->team  = Team::Blue;
    shooter->player_state.pos = { 0.0f, 0.0f, 0.0f };
    shooter->player_state.yaw = 0.0f;            // faces +Z
    victim->player_state.pos  = { 0.0f, 0.0f, 10.0f };

    // Queue a fire-bit move for the shooter.
    net20::MoveInput m{};
    m.trigger = true;
    shooter->pending_moves.push_back(m);

    ProjectileWorld pw;
    std::vector<HitEvent> hits_all;
    constexpr float dt = 1.0f / 32.0f;   // 32 Hz
    for (int t = 0; t < 32; ++t) {       // up to 1 second
        auto hits = pw.tick(table, dt);
        for (auto& h : hits) hits_all.push_back(h);
        if (!hits_all.empty()) break;
        // After the first tick the cooldown will block retries; clear
        // the pending move queue so we don't accidentally double-fire.
        shooter->pending_moves.clear();
    }

    if (hits_all.size() != 1) {
        std::fprintf(stderr,
            "[projectile-selftest] expected 1 hit, got %zu\n", hits_all.size());
        return 1;
    }
    if (hits_all[0].shooter_slot != shooter->player_slot
        || hits_all[0].victim_slot != victim->player_slot) {
        std::fprintf(stderr,
            "[projectile-selftest] hit slot mismatch: shooter=%u (expect %u) victim=%u (expect %u)\n",
            hits_all[0].shooter_slot, shooter->player_slot,
            hits_all[0].victim_slot,  victim->player_slot);
        return 1;
    }
    if (hits_all[0].damage <= 0.0f) {
        std::fputs("[projectile-selftest] hit damage is zero\n", stderr);
        return 1;
    }

    // Self-fire protection: shooter aims straight up (pitch +π/2), no
    // hit should ever be reported on themselves.
    SessionTable t2(4);
    Session* solo = t2.allocate(p1, n1, 0);
    solo->team = Team::Red;
    solo->player_state.pos = {0.0f, 0.0f, 0.0f};
    solo->player_state.pitch = glm::half_pi<float>();
    net20::MoveInput mfire{};
    mfire.trigger = true;
    solo->pending_moves.push_back(mfire);
    ProjectileWorld pw2;
    for (int t = 0; t < 32; ++t) {
        auto hits = pw2.tick(t2, dt);
        if (!hits.empty()) {
            std::fputs("[projectile-selftest] self-fire produced a hit\n", stderr);
            return 1;
        }
        solo->pending_moves.clear();
    }

    // TTL: fire into empty world, project should expire silently.
    SessionTable t3(4);
    Session* lonely = t3.allocate(p1, n1, 0);
    lonely->team = Team::Red;
    lonely->player_state.pos = {0.0f, 0.0f, 0.0f};
    lonely->player_state.pitch = 0.05f;        // slight upward angle
    net20::MoveInput mf3{};
    mf3.trigger = true;
    lonely->pending_moves.push_back(mf3);
    ProjectileWorld pw3;
    pw3.tick(t3, dt);     // spawn one
    lonely->pending_moves.clear();
    const auto active_before = pw3.projectiles().size();
    if (active_before == 0) {
        std::fputs("[projectile-selftest] TTL: no projectile spawned\n", stderr);
        return 1;
    }
    // Tick for 6 seconds.
    for (int t = 0; t < 6 * 32; ++t) {
        pw3.tick(t3, dt);
    }
    if (!pw3.projectiles().empty()) {
        std::fprintf(stderr,
            "[projectile-selftest] TTL: expected 0 active after 6 s, got %zu\n",
            pw3.projectiles().size());
        return 1;
    }
    if (pw3.stats().expired == 0) {
        std::fputs("[projectile-selftest] TTL: expired counter not incremented\n", stderr);
        return 1;
    }

    std::fputs("[projectile-selftest] OK — hit/self-protection/TTL\n", stderr);
    return 0;
}

} // namespace dts_viewer
