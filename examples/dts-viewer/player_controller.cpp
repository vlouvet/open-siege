#include "player_controller.hpp"

namespace dts_viewer
{

// Spec 09/01 stub.  Subsequent specs in track 09 fill this in:
//   02-gravity-and-jump.md     -> gravity integration, jump impulse on rising edge
//   03-walking.md              -> WASD acceleration + ground friction
//   04-jetpack.md              -> upward thrust + fuel burn/regen
//   05-collision-terrain.md    -> heightmap clamp + slope_deg
//   06-collision-interiors.md  -> capsule sweep against BSP
//
// Keep this body trivial so the bridge to the existing walk-camera in
// main.cpp is the source of truth until those specs land.
void player_update(PlayerState& /*ps*/,
                   const PlayerTuning& /*t*/,
                   const InputState&   /*in*/,
                   float               /*dt*/)
{
    // Intentionally empty.
}

} // namespace dts_viewer
