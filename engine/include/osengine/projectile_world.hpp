#ifndef OSENGINE_PROJECTILE_WORLD_HPP
#define OSENGINE_PROJECTILE_WORLD_HPP

// Spec 28/06 — authoritative projectile sim + hit detection, v1.
//
// Server-side projectile lifecycle:
//   * On each world_tick, scan every session for a "fire" intent
//     (any pending MoveInput with trigger=true). Per-session cooldown
//     (~250 ms for disc, ~80 ms for chain) prevents wire-flooded
//     spawns from over-firing.
//   * Each projectile carries a velocity and a TTL. We integrate
//     position each tick and test the swept segment against every
//     OTHER session's player capsule (centred at pos + capsule_half_h).
//   * On hit: record a HitEvent (consumed by spec 28/07 damage code)
//     and remove the projectile.
//   * On TTL expiry: remove silently.
//
// v1 weapons: disc (slow heavy bolt, 200 m/s, 30 dmg, 5 s TTL) and
// chain (fast hitscan-ish, 600 m/s, 8 dmg, 1 s TTL). Both produce
// instantaneous-impact HitEvents; splash damage is deferred.

#include <osengine/movecommand.hpp>

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace dts_viewer
{

struct Session;
class  SessionTable;

enum class WeaponType : std::uint8_t {
    Disc  = 0,
    Chain = 1,
};

struct ActiveProjectile
{
    std::uint32_t id            = 0;     // server-global incrementing
    std::uint16_t owner_slot    = 0;
    WeaponType    weapon        = WeaponType::Disc;
    glm::vec3     pos           {0.0f};
    glm::vec3     vel           {0.0f};
    float         ttl_remaining = 0.0f;  // seconds
    float         damage        = 0.0f;
};

struct HitEvent
{
    std::uint16_t shooter_slot;
    std::uint16_t victim_slot;
    WeaponType    weapon;
    glm::vec3     impact_pos;
    float         damage;
    std::uint32_t projectile_id;
};

struct ProjectileWorldStats
{
    std::uint64_t fired   = 0;
    std::uint64_t hits    = 0;
    std::uint64_t expired = 0;
    std::uint64_t active  = 0;
};

class ProjectileWorld
{
public:
    ProjectileWorld();

    // Per session, advance the firing cooldown by dt and consume the
    // most-recent applied MoveInput.trigger bit. If a fire is permitted,
    // spawn a projectile at the session's eye position aimed along its
    // (yaw, pitch). v1 always fires Disc; weapon selection is 28/06b.
    void tick_fires(SessionTable& sessions, float dt_sec);

    // Step every active projectile by dt, check hits against every
    // session whose slot != owner_slot, append HitEvents.
    void tick_motion(SessionTable& sessions, float dt_sec,
                     std::vector<HitEvent>& out_hits);

    // Combined helper called by world_tick(). Calls tick_fires then
    // tick_motion, returns the hit list collected this tick.
    std::vector<HitEvent> tick(SessionTable& sessions, float dt_sec);

    const std::vector<ActiveProjectile>& projectiles() const noexcept {
        return projectiles_;
    }

    const ProjectileWorldStats& stats() const noexcept { return stats_; }

    // Spec 28/06 selftest: two sessions facing each other at 10 m,
    // one fires; after a few ticks a HitEvent is recorded.
    static int selftest();

private:
    struct SessionFireCooldown {
        std::uint16_t slot;
        float         remaining_sec = 0.0f;
    };
    std::vector<ActiveProjectile>       projectiles_;
    std::vector<SessionFireCooldown>    cooldowns_;
    std::uint32_t                       next_id_ = 1;
    ProjectileWorldStats                stats_;
};

} // namespace dts_viewer

#endif // OSENGINE_PROJECTILE_WORLD_HPP
