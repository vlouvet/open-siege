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
    Plasma      = 3,
    Mortar      = 4,
    // Spec 12/07 — energy / beam weapons. Blaster fires a fast bolt
    // (no splash, impact only). ELF (Lightning) is a sustained beam
    // raycast — no persistent projectile entity. Laser (TargetLaser)
    // is a charge-up release that paints a marker for lock-on weapons.
    Blaster     = 5,
    ELF         = 6,
    Laser       = 7,
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

    // Plasma — BulletData PlasmaBolt (treated as a small-splash projectile)
    float plasma_init_speed    = 55.0f;    // muzzleVelocity
    float plasma_lifetime      = 2.0f;     // liveTime
    float plasma_gravity_scale = 0.0f;
    float plasma_splash_radius = 4.0f;     // explosionRadius
    float plasma_splash_dmg    = 45.0f;    // damageValue 0.45 * 100
    float plasma_splash_impulse = 8.0f;
    float plasma_fire_interval = 0.25f;

    // Mortar — GrenadeData MortarShell
    float mortar_init_speed    = 35.0f;
    float mortar_gravity_scale = 1.0f;
    float mortar_drag          = 0.1f;
    float mortar_fuse_seconds  = 3.0f;
    float mortar_bounce_decay  = 0.10f;    // elasticity
    int   mortar_max_bounces   = 1;
    float mortar_splash_radius = 20.0f;    // explosionRadius
    float mortar_splash_dmg    = 100.0f;   // damageValue 1.0 * 100
    float mortar_splash_impulse = 30.0f;
    float mortar_fire_interval = 3.0f;

    // Blaster — BulletData BlasterBolt (impact-only energy bolt)
    float blaster_init_speed    = 200.0f;  // muzzleVelocity
    float blaster_lifetime      = 1.125f;  // liveTime
    float blaster_dmg           = 12.5f;   // damageValue 0.125 * 100
    float blaster_fire_interval = 0.18f;

    // ELF — LightningData lightningCharge (sustained DoT beam)
    float elf_range             = 40.0f;   // boltLength
    float elf_dps               = 60.0f;   // damagePerSec 0.06 * 100 / engine scale
    float elf_energy_drain_per_s = 60.0f;  // energyDrainPerSec — drains jet_fuel
    float elf_min_energy        = 5.0f;    // beam shuts off below this fuel level

    // Laser (TargetLaser) — charge-up release that paints a marker
    float laser_charge_seconds  = 1.0f;    // time to full charge before release fires
    float laser_marker_lifetime = 6.0f;    // seconds the marker persists post-release
    float laser_range           = 250.0f;  // raycast distance
    float laser_energy_per_shot = 10.0f;   // jet_fuel drained at release
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

// Spec 12/07 — beam-weapon state lives outside the Projectile entity list
// because ELF/Laser don't spawn projectiles. The renderer reads these
// fields each frame to draw the active beam / target marker; the tick
// functions below mutate them.
struct ElfBeamState
{
    bool       active     = false;     // true while beam is being held
    glm::vec3  start      { 0.0f };    // world-space muzzle position
    glm::vec3  end        { 0.0f };    // world-space hit / max-range point
    bool       hit_target = false;     // did the raycast hit something dynamic?
};

struct LaserState
{
    bool       charging         = false;   // true while fire is held
    float      charge_t         = 0.0f;    // seconds of fire held so far
    glm::vec3  marker_pos       { 0.0f };  // world-space painted marker
    float      marker_remaining = 0.0f;    // seconds before marker fades
    bool       marker_active    = false;
};

struct ProjectileSystem
{
    std::vector<Projectile> alive;
    // Cooldowns are per-projectile-type to keep the system zero-state per
    // weapon; weapon-switching in 12/03 doesn't need to copy cooldowns.
    float cooldown_disc    = 0.0f;
    float cooldown_grenade = 0.0f;
    float cooldown_chain   = 0.0f;
    float cooldown_blaster = 0.0f;
    ElfBeamState elf {};
    LaserState   laser {};
};

// Spec 12/07 — sustained-beam tick. `fire_held` is true while the player
// holds the fire button with ELF active. Drains player.jet_fuel, applies
// damage to the (single v1) player when self-hit. Beam endpoints are
// written into `sys.elf` for the renderer.
struct PlayerState;
void elf_tick(ProjectileSystem&       sys,
              const ProjectileTuning& t,
              const HeightSampler&    terrain,
              PlayerState&            player,
              bool                    fire_held,
              const glm::vec3&        origin,
              const glm::vec3&        aim_dir,
              int                     owner_id,
              float                   dt);

// Spec 12/07 — charge / release state machine for the targeting laser.
// While `fire_held`, ramps the charge timer to `laser_charge_seconds`.
// On release (transition true→false), if the charge is complete, places
// a marker at the raycast hit point. Marker decays at `dt` per frame.
void laser_tick(ProjectileSystem&       sys,
                const ProjectileTuning& t,
                const HeightSampler&    terrain,
                PlayerState&            player,
                bool                    fire_held,
                const glm::vec3&        origin,
                const glm::vec3&        aim_dir,
                float                   dt);

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
//
// Spec 12/08 — optional generator-damage routing. When `generators`
// is non-null, every detonation within splash radius applies damage
// via `apply_damage_generator`; `on_generator_destroyed` is invoked
// on the false->true transition (wired by main.cpp to fire the
// `Generator::onDestroyed` script callback).
struct GeneratorState;
void projectiles_update(
    ProjectileSystem&                       sys,
    const ProjectileTuning&                 t,
    const HeightSampler&                    terrain,
    PlayerState&                            player,
    float                                   dt,
    std::vector<GeneratorState>*            generators = nullptr,
    void (*on_generator_destroyed)(const GeneratorState&) = nullptr);

} // namespace dts_viewer

#endif // DTS_VIEWER_PROJECTILE_HPP
