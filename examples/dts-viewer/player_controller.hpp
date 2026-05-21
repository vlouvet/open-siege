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
    float jet_fuel_max    = 100.0f;
    float jet_fuel_regen  = 25.0f;     // units/s while not jetting
    float jet_fuel_burn   = 40.0f;     // units/s while jetting
    float eye_height      = 1.75f;     // metres above feet
    float jump_impulse    = 9.0f;      // m/s applied upward on jump press
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
};

struct InputState
{
    bool fwd  = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool jump = false;       // held — not edge-triggered
    bool jet  = false;       // held — burns fuel while down
    bool sprint = false;     // shift; respected by spec 09/03
    float mouse_dx = 0.0f;   // raw pixels since last frame
    float mouse_dy = 0.0f;
};

// Stub for spec 01: applies no physics.  Subsequent specs add gravity,
// walking, jetpack, and collision in this order.  Implementations should
// be deterministic for fixed `dt`.
void player_update(PlayerState&   ps,
                   const PlayerTuning& t,
                   const InputState&   in,
                   float               dt);

// Compute the camera eye position (feet + eye_height in world Y).
inline glm::vec3 player_eye(const PlayerState& ps, const PlayerTuning& t)
{
    return ps.pos + glm::vec3(0.0f, t.eye_height, 0.0f);
}

} // namespace dts_viewer

#endif // DTS_VIEWER_PLAYER_CONTROLLER_HPP
