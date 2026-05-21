#ifndef DTS_VIEWER_PROJECTILE_HPP
#define DTS_VIEWER_PROJECTILE_HPP

// Projectile simulation — Track 12 specs 01 + 02.
//
// Three archetypes share one update loop:
//   Disc   — flat trajectory (gravity_scale ~ 0), detonates on first contact
//   Grenade — parabolic, bounces with energy loss, fuse-timer detonation
//   ChainBullet — hitscan; resolved at spawn-time, no persistent entity
//
// Collision against the terrain is sampled at each fixed step via the
// HeightSampler; interior collision is deferred behind a no-op stub
// (matches the spec 09/06 pattern — wired up when --mission loads
// interior geometry).  Splash damage applies an impulse to the player
// via `player_apply_impulse` (track 10 spec 03).

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

#include "height_sampler.hpp"

namespace dts_viewer
{

struct PlayerState;

enum class ProjType : std::uint8_t
{
    Disc        = 0,
    Grenade     = 1,
    ChainBullet = 2,
};

struct ProjectileTuning
{
    // Disc
    float disc_init_speed    = 80.0f;
    float disc_gravity_scale = 0.0f;
    float disc_lifetime      = 5.0f;
    float disc_splash_radius = 5.0f;
    float disc_splash_dmg    = 60.0f;
    float disc_splash_impulse = 18.0f;     // delta-v applied at point-blank
    float disc_self_dmg_coef  = 0.5f;
    float disc_fire_interval  = 1.2f;      // seconds between shots

    // Grenade
    float gren_init_speed     = 30.0f;
    float gren_gravity_scale  = 1.0f;
    float gren_drag           = 0.4f;
    float gren_fuse_seconds   = 3.0f;
    float gren_bounce_decay   = 0.45f;     // velocity multiplier on each bounce
    int   gren_max_bounces    = 3;
    float gren_splash_radius  = 6.0f;
    float gren_splash_dmg     = 80.0f;
    float gren_splash_impulse = 20.0f;
    float gren_fire_interval  = 1.0f;

    // ChainBullet
    float chain_range         = 250.0f;
    float chain_dmg           = 8.0f;
    float chain_fire_interval = 0.10f;
};

struct Projectile
{
    ProjType type           = ProjType::Disc;
    glm::vec3 pos           { 0.0f };
    glm::vec3 vel           { 0.0f };
    float    lifetime_left  = 0.0f;
    int      bounces_left   = 0;
    int      owner_id       = -1;
    bool     alive          = true;
};

struct ProjectileSystem
{
    std::vector<Projectile> alive;
    // Cooldowns are per-projectile-type to keep the system zero-state per
    // weapon; weapon-switching in 12/03 doesn't need to copy cooldowns.
    float cooldown_disc    = 0.0f;
    float cooldown_grenade = 0.0f;
    float cooldown_chain   = 0.0f;
};

// Spawn a projectile.  ChainBullet resolves immediately and is NOT added
// to `sys.alive`; instead, the hit point is returned via *out_hit_pos
// (nullptr for misses or non-hitscan calls).  Cooldown is enforced — if
// the relevant timer is still positive, the call is a no-op and returns
// false.
bool projectile_fire(
    ProjectileSystem&         sys,
    const ProjectileTuning&   t,
    ProjType                  type,
    const glm::vec3&          origin,
    const glm::vec3&          aim_dir,
    int                       owner_id,
    glm::vec3*                out_hit_pos = nullptr);

// Step all active projectiles by `dt`.  Resolves terrain collisions and
// applies splash damage / impulse to `player` when the explosion is
// within range.  The caller passes the player pointer because v1 has a
// single player; multi-entity damage routing lands when Track 14 lands.
void projectiles_update(
    ProjectileSystem&        sys,
    const ProjectileTuning&  t,
    const HeightSampler&     terrain,
    PlayerState&             player,
    float                    dt);

} // namespace dts_viewer

#endif // DTS_VIEWER_PROJECTILE_HPP
