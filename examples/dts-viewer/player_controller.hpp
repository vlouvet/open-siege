#ifndef DTS_VIEWER_PLAYER_CONTROLLER_HPP
#define DTS_VIEWER_PLAYER_CONTROLLER_HPP

// PlayerState + InputState + PlayerTuning — Track 09 spec 01.
//
// The walk-camera struct (Track 08) supplied position + yaw + pitch only.
// The physics work in track 09 needs velocity, on-ground, jet-fuel, and a
// fixed 60 Hz step.  This header defines the data + a stub `player_update`
// that subsequent specs (02 gravity, 03 walking, 04 jetpack, 05/06 collision)
// fill in.  v1 of this spec leaves the existing walk-camera in control of
// the actual camera; PlayerState is built on top as a parallel data
// structure so the accumulator can be exercised.

#include <glm/glm.hpp>
#include <cstdint>

#include "height_sampler.hpp"

namespace dts_viewer
{

enum class ArmorClass : int { Light = 0, Medium = 1, Heavy = 2 };

struct PlayerTuning
{
    // Default constants reflect Medium-armor.  Spec 01 doesn't act on most
    // of these; later specs do.  Tuned for visual feel; revisited in
    // Track 10 spec 04 against a Wine reference.
    float mass            = 90.0f;     // kg
    float max_walk_speed  = 8.5f;      // m/s; cap when grounded + not skiing
    float gravity         = 20.0f;     // m/s^2 (Tribes-canonical)
    float jet_thrust      = 18.0f;     // upward accel while jet held (m/s^2)
    float jet_thrust_burst = 32.0f;    // afterburner thrust (Shift + Space)
    float jet_fuel_max    = 100.0f;
    float jet_fuel_regen  = 25.0f;     // units/s while not jetting + grounded
    float jet_fuel_burn   = 1.0f;      // units/s base consumption while jetting
    float jet_burst_mult  = 2.5f;      // burst burns this much faster
    float jet_upward_cap  = 25.0f;     // m/s soft cap on vel.y while jet held
    float jet_tap_seconds = 0.15f;     // hold Space < this == tap-jump; > == jet
    float jet_lockout_sec = 0.30f;     // lockout after fuel hits 0
    float jet_resume_fuel = 5.0f;      // fuel must regen above this before resuming
    float eye_height      = 1.75f;     // metres above feet
    float jump_speed      = 7.5f;      // m/s applied upward on jump rising edge
    float ground_slop     = 0.05f;     // metres; on-ground if feet within this of terrain
    float coyote_seconds  = 0.10f;     // grace window after walking off a ledge

    // Walking (spec 09/03)
    float ground_accel    = 60.0f;     // m/s^2 toward wish-velocity while grounded
    float air_control     = 0.3f;      // scales ground_accel while airborne
    float friction        = 8.0f;      // 1/s — vel *= (1 - friction*dt) when no input on ground
    float sprint_mult     = 1.5f;      // shift multiplies the speed cap
    float mouse_sens      = 0.0022f;   // radians per pixel (camera-side, not physics)

    // Terrain collision (spec 09/05)
    float max_walk_slope  = 45.0f;     // degrees; steeper -> can't walk up, slides down
    float slide_friction  = 0.4f;      // damps slide momentum on too-steep slopes

    // Capsule collision (spec 09/06)
    float capsule_radius  = 0.4f;      // metres
    float capsule_half_h  = 0.9f;      // metres from centre to hemisphere centre
    float step_height     = 0.4f;      // max ledge height the capsule auto-mounts
    int   max_slide_iters = 3;         // Quake-style slide iteration cap

    // Skiing (track 10)
    float ski_min_slope   = 5.0f;      // degrees; below this, Space won't ski
    float ski_max_speed   = 80.0f;     // m/s global cap on skiing momentum

    // Disc-jumping (track 10 spec 03; splash code lives in track 12)
    float self_damage_coef = 0.5f;     // self-disc applies this fraction of splash dmg
};

struct PlayerState
{
    glm::vec3 pos   { 0.0f, 0.0f, 0.0f };  // feet position (NOT eye height)
    glm::vec3 vel   { 0.0f, 0.0f, 0.0f };
    float     yaw   = 0.0f;                 // radians, 0 = +Z
    float     pitch = 0.0f;
    bool      on_ground = false;
    float     jet_fuel  = 100.0f;
    ArmorClass armor    = ArmorClass::Medium;
    // Spec 09/05 will populate this with the terrain-normal slope in degrees.
    float     slope_deg = 0.0f;
    // Spec 10/01 flips this when skiing is engaged; spec 01 leaves it false.
    bool      skiing    = false;
    // Spec 09/02 flips this true while a jump impulse is being applied;
    // it's a one-step latch.
    bool      jump_latch = false;
    // Spec 09/02: edge-detect of the jump button (true while held since
    // the last rising edge); cleared by the controller, not by the caller.
    bool      prev_jump  = false;
    // Spec 09/02: counts down each step from `coyote_seconds` after
    // on_ground becomes false; lets a slightly-late jump still fire.
    float     coyote_timer = 0.0f;
    // Spec 09/04: jetpack bookkeeping.
    bool      jet_active     = false;  // true while thrust is being applied
    float     jet_hold_timer = 0.0f;   // how long in.jet has been held continuously
    float     jet_lockout    = 0.0f;   // post-empty cooldown countdown
};

struct InputState
{
    bool fwd  = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool jump = false;       // Space held (raw); edge-triggered for jump impulse
    bool jet  = false;       // Space held past the tap window; same key as jump
    bool sprint = false;     // shift; respected by spec 09/03
    float mouse_dx = 0.0f;   // raw pixels since last frame
    float mouse_dy = 0.0f;
};

// Step the player one fixed dt.  Currently applies gravity + ground
// detection + jump impulse (spec 09/02) and walking (spec 09/03).
// Subsequent specs add jetpack (04) and interior collision (06).
// The HeightSampler is borrowed; pass an empty/default sampler when
// no terrain is loaded.
struct MissionBounds;
void player_update(PlayerState&        ps,
                   const PlayerTuning& t,
                   const InputState&   in,
                   const HeightSampler& terrain,
                   const MissionBounds* bounds,    // may be nullptr
                   float               dt);

// Compute the camera eye position (feet + eye_height in world Y).
inline glm::vec3 player_eye(const PlayerState& ps, const PlayerTuning& t)
{
    return ps.pos + glm::vec3(0.0f, t.eye_height, 0.0f);
}

// Track 10 spec 03 — disc-jumping impulse hook.  Splash code (Track 12
// spec 02) calls this when an explosion includes the player in its
// radius; the impulse is interpreted as a delta-velocity in m/s and
// added directly to `vel`.  Mass scaling is deferred to a future
// refactor since v1 only cares about the player.
inline void player_apply_impulse(PlayerState& ps, const glm::vec3& delta_v)
{
    ps.vel += delta_v;
}

} // namespace dts_viewer

#endif // DTS_VIEWER_PLAYER_CONTROLLER_HPP
