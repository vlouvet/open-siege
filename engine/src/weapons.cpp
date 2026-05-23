#include "weapons.hpp"

#include <algorithm>

namespace dts_viewer
{

Inventory default_loadout()
{
    Inventory inv;
    // Slot 0: Blaster (spec 12/07) — energy bolt, plentiful ammo
    inv.weapons[0] = WeaponState{
        ProjType::Blaster, 100, 100, 0.18f, 0.0f, true, true };
    // Slot 1: Disc (semi-auto)
    inv.weapons[1] = WeaponState{
        ProjType::Disc, 25, 25, 1.2f, 0.0f, false, true };
    // Slot 2: Chain (full-auto)
    inv.weapons[2] = WeaponState{
        ProjType::ChainBullet, 200, 200, 0.10f, 0.0f, true, true };
    // Slot 3: Grenade
    inv.weapons[3] = WeaponState{
        ProjType::Grenade, 12, 12, 1.0f, 0.0f, false, true };
    // Slot 4: Plasma (spec 12/06)
    inv.weapons[4] = WeaponState{
        ProjType::Plasma, 60, 60, 0.25f, 0.0f, true, true };
    // Slot 5: Mortar (spec 12/06)
    inv.weapons[5] = WeaponState{
        ProjType::Mortar, 8, 8, 3.0f, 0.0f, false, true };
    // Slot 6: ELF (spec 12/07) — beam weapon, drains energy not ammo.
    // max_ammo=0 + ammo=1 so the firing gate (ammo > 0) lets us select
    // the slot; the actual gate is the energy pool inside elf_tick.
    inv.weapons[6] = WeaponState{
        ProjType::ELF, 1, 1, 0.0f, 0.0f, true, true };
    // Slot 7: Laser (TargetLaser, spec 12/07) — charge-up release
    inv.weapons[7] = WeaponState{
        ProjType::Laser, 1, 1, 0.0f, 0.0f, false, true };
    inv.active_slot = 0;
    return inv;
}

WeaponState& active_weapon(Inventory& inv)
{
    if (inv.active_slot < 0 ||
        inv.active_slot >= static_cast<int>(inv.weapons.size())) {
        inv.active_slot = 0;
    }
    return inv.weapons[inv.active_slot];
}

const WeaponState& active_weapon(const Inventory& inv)
{
    int s = inv.active_slot;
    if (s < 0 || s >= static_cast<int>(inv.weapons.size())) s = 0;
    return inv.weapons[s];
}

bool select_weapon(Inventory& inv, int slot)
{
    if (slot < 0 || slot >= static_cast<int>(inv.weapons.size())) return false;
    if (!inv.weapons[slot].equipped) return false;
    inv.active_slot = slot;
    return true;
}

void cycle_weapon(Inventory& inv)
{
    const int n = static_cast<int>(inv.weapons.size());
    for (int i = 1; i <= n; ++i) {
        int s = (inv.active_slot + i) % n;
        if (inv.weapons[s].equipped) { inv.active_slot = s; return; }
    }
}

void auto_switch_on_empty(Inventory& inv)
{
    const int n = static_cast<int>(inv.weapons.size());
    for (int i = 1; i <= n; ++i) {
        int s = (inv.active_slot + i) % n;
        if (inv.weapons[s].equipped && inv.weapons[s].ammo > 0) {
            inv.active_slot = s;
            return;
        }
    }
}

void weapons_tick_cooldowns(Inventory& inv, float dt)
{
    for (auto& w : inv.weapons) {
        if (w.cooldown > 0.0f) w.cooldown = std::max(0.0f, w.cooldown - dt);
    }
}

} // namespace dts_viewer
