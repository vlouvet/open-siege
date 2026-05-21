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
    // Canonical values cross-checked against `baseProjData.cs` from
    // scripts.vol (open community data per CLAUDE.md).  Source labels:
    //   muzzleVelocity / terminalVelocity / explosionRadius / damageValue
    //   / kickBackStrength / liveTime / elasticity.  damageValue is on
    //   the engine's 0..1 HP scale; we multiply by 100 for HP units.

    // Disc — RocketData DiscShell
    float disc_init_speed    = 80.0f;      // ~ terminalVelocity 80 (muzzle 65; accel 5)
    float disc_gravity_scale = 0.0f;       // RocketData ignores gravity in Tribes
    float disc_lifetime      = 5.0f;
    float disc_splash_radius = 7.5f;       // explosionRadius
    float disc_splash_dmg    = 50.0f;      // damageValue 0.5 * 100
    float disc_splash_impulse = 22.0f;     // m/s point-blank; tuned from kickBackStrength 150
    float disc_self_dmg_coef  = 0.5f;
    float disc_fire_interval  = 1.2f;

    // Grenade — GrenadeData GrenadeShell
    float gren_init_speed     = 30.0f;
    float gren_gravity_scale  = 1.0f;
    float gren_drag           = 0.4f;
    float gren_fuse_seconds   = 1.0f;      // liveTime
    float gren_bounce_decay   = 0.45f;     // elasticity (matches existing value)
    int   gren_max_bounces    = 3;
    float gren_splash_radius  = 15.0f;     // explosionRadius
    float gren_splash_dmg     = 40.0f;     // damageValue 0.4 * 100
    float gren_splash_impulse = 22.0f;
    float gren_fire_interval  = 1.0f;

    // Chain — BulletData ChaingunBullet
    float chain_range         = 250.0f;    // muzzleVelocity 425 * totalTime 1.5 ~= 637 — clamped
    float chain_dmg           = 11.0f;     // damageValue 0.11 * 100
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
