#ifndef DTS_VIEWER_WEAPONS_HPP
#define DTS_VIEWER_WEAPONS_HPP

// Weapon inventory + switching — Track 12 spec 03.
//
// The Inventory lives on PlayerState (a small refactor — see
// player_controller.hpp).  Number keys 1-N select a specific slot;
// Q cycles forward.  Auto-switch on empty: if firing depletes the
// active weapon's ammo, the inventory advances to the next equipped
// slot that still has ammo.

#include "projectile.hpp"

#include <array>
#include <cstdint>

namespace dts_viewer
{

constexpr std::size_t kMaxWeaponSlots = 4;

struct WeaponState
{
    ProjType projectile     = ProjType::Disc;
    int      ammo           = 0;
    int      max_ammo       = 0;
    float    fire_interval  = 1.0f;   // seconds between shots
    float    cooldown       = 0.0f;   // seconds until next fire
    bool     full_auto      = false;
    bool     equipped       = false;
};

struct Inventory
{
    std::array<WeaponState, kMaxWeaponSlots> weapons {};
    int active_slot = 0;
};

// Populate with the canonical default loadout (Disc + Chain + Grenade).
Inventory default_loadout();

// Returns the active weapon (or weapons[0] if active_slot is invalid).
WeaponState& active_weapon(Inventory& inv);
const WeaponState& active_weapon(const Inventory& inv);

// Try to switch to `slot`.  Returns true if the slot is equipped and
// the switch occurred.  Slot is 0-based here; main.cpp maps the SDL
// number-key scancodes to 0..N.
bool select_weapon(Inventory& inv, int slot);

// Cycle forward to the next equipped weapon (Q behaviour).
void cycle_weapon(Inventory& inv);

// Auto-switch to the next equipped weapon that still has ammo.  Called
// after firing consumes the last shot.  No-op if no other equipped
// weapon has ammo (current state preserved).
void auto_switch_on_empty(Inventory& inv);

// Tick all weapon cooldowns by `dt`.
void weapons_tick_cooldowns(Inventory& inv, float dt);

} // namespace dts_viewer

#endif // DTS_VIEWER_WEAPONS_HPP
