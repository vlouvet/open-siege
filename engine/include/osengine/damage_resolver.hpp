#ifndef OSENGINE_DAMAGE_RESOLVER_HPP
#define OSENGINE_DAMAGE_RESOLVER_HPP

// Spec 28/07 — damage application + respawn timer.
//
// HitEvents produced by spec 28/06 land here. We deduct damage from
// the victim's health, transition them to LifeState::Dead when
// health reaches zero, emit a KillEvent for the scoreboard / chat
// to consume, and after ~3 s respawn them at a team spawn point.
//
// v1 keeps the rules simple: flat-damage weapons, no armor scaling,
// no splash radius, no friendly-fire toggle (friendly-fire is always
// off — same-team hits are silently dropped). Spec 31 will wire the
// real Tribes 1 baseProjData.cs values + armor multipliers.

#include <osengine/projectile_world.hpp>

#include <cstdint>
#include <vector>

namespace dts_viewer
{

class SessionTable;
struct SpawnPoint;

struct KillEvent
{
    std::uint16_t killer_slot;     // 0xFFFF = world (fall damage / suicide)
    std::uint16_t victim_slot;
    WeaponType    weapon;
};

struct DamageRules
{
    float respawn_delay_sec = 3.0f;
    float health_max        = 100.0f;
    bool  friendly_fire     = false;
};

// Apply every hit to the matching victim session. Fills out_kills
// with deaths registered this call. now_ms drives died_at_ms so the
// respawn timer is consistent with the listener's clock.
void apply_hits(SessionTable& sessions,
                const std::vector<HitEvent>& hits,
                std::vector<KillEvent>& out_kills,
                std::uint64_t now_ms,
                const DamageRules& rules = {});

// For each dead session whose died_at_ms + rules.respawn_delay_sec
// has elapsed, teleport them to a team spawn point and restore
// health.
void respawn_due(SessionTable& sessions,
                 const std::vector<SpawnPoint>& spawns,
                 std::uint64_t now_ms,
                 const DamageRules& rules = {});

int damage_resolver_selftest();

} // namespace dts_viewer

#endif // OSENGINE_DAMAGE_RESOLVER_HPP
