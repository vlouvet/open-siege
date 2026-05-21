#include "player_controller.hpp"

#include <algorithm>

namespace dts_viewer
{

// Track 09 spec 02 — gravity, ground detection, coyote-time jump.
// Y-up convention (matches the rest of the viewer); the spec text uses
// "vel.z" generically for "vertical" — we use vel.y.  Spec acceptance
// (player falls, jumps, lands) is unchanged by the axis relabel.
//
// Subsequent specs in this track plug in:
//   03 walking.md              -> horizontal acceleration + ground friction
//   04 jetpack.md              -> upward thrust while jet held + fuel
//   05 collision-terrain.md    -> slope_deg + edge cases at the heightmap boundary
//   06 collision-interiors.md  -> capsule sweep against BSP
void player_update(PlayerState&        ps,
                   const PlayerTuning& t,
                   const InputState&   in,
                   const HeightSampler& terrain,
                   float               dt)
{
    if (dt <= 0.0f) return;

    // Sample terrain height beneath the player's feet.  When no terrain
    // is bound (HeightSampler is invalid), use 0 — physics still runs.
    const float terrain_y = terrain.valid()
        ? terrain.sample(ps.pos.x, ps.pos.z)
        : 0.0f;

    // Apply gravity.  Jet thrust replaces this in spec 04; v1 always
    // applies gravity (jet is off this spec).
    ps.vel.y -= t.gravity * dt;

    // Integrate vertical motion first; horizontal comes in spec 03.
    ps.pos.y += ps.vel.y * dt;

    // Ground contact: feet within ground_slop AND falling.  Latch
    // `on_ground`, snap to the surface, kill downward velocity.
    bool was_on_ground = ps.on_ground;
    if (ps.pos.y <= terrain_y + t.ground_slop && ps.vel.y <= 0.0f) {
        ps.pos.y    = terrain_y;
        ps.vel.y    = 0.0f;
        ps.on_ground = true;
    } else {
        ps.on_ground = false;
    }

    // Coyote timer: start countdown the step after we leave the ground.
    if (was_on_ground && !ps.on_ground) {
        ps.coyote_timer = t.coyote_seconds;
    } else if (!ps.on_ground) {
        ps.coyote_timer = std::max(0.0f, ps.coyote_timer - dt);
    } else {
        ps.coyote_timer = 0.0f;
    }

    // Edge-triggered jump.  Holding Space across a landing does NOT
    // auto-rejump (prev_jump latches until release).
    const bool jump_edge = in.jump && !ps.prev_jump;
    ps.prev_jump = in.jump;
    ps.jump_latch = false;
    if (jump_edge && (ps.on_ground || ps.coyote_timer > 0.0f)) {
        ps.vel.y    = t.jump_speed;
        ps.on_ground = false;
        ps.coyote_timer = 0.0f;
        ps.jump_latch = true;
    }
}

} // namespace dts_viewer
