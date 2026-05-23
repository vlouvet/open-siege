#ifndef DTS_VIEWER_VEHICLE_HPP
#define DTS_VIEWER_VEHICLE_HPP

// Hover-vehicle driving — Track 14 spec 13.
//
// v1 covers a single hover model used by every mounted vehicle on a
// vehiclePad (Wildcat / Beowulf / etc. all share the same arcade
// thrust+gravity feel). The placeholder state from spec 14/08 is
// extended in-place with `vel / yaw / pilot bookkeeping`; this header
// adds the tick + input routing.
//
// Tribes 1's vehicles are arcade hovercraft, not realistic aircraft —
// thrust counters gravity; lateral input gives bounded translation;
// yaw follows the camera. Don't model aerodynamics.

#include <glm/glm.hpp>
#include "osengine/entity_state.hpp"

namespace dts_viewer
{

struct HeightSampler;

struct VehicleTuning
{
    float gravity        = 20.0f;   // m/s^2 — matches PlayerTuning.gravity
    float max_thrust     = 28.0f;   // m/s^2 ascending acceleration
    float descend_accel  = 18.0f;
    float forward_accel  = 24.0f;
    float lateral_accel  = 14.0f;
    float linear_drag    = 0.9f;    // 1/s — vel *= max(0, 1 - drag*dt) per tick
    float top_speed      = 50.0f;   // m/s horizontal cap
    float top_vertical   = 30.0f;   // m/s vertical cap
    float mount_radius   = 6.0f;    // metres — player must be within to press E
    float dismount_offset = 3.0f;   // metres beside the vehicle to drop the pilot
    float ground_clearance = 1.0f;  // metres above terrain when idling
};

struct VehicleInput
{
    bool  fwd    = false;   // W
    bool  back   = false;   // S
    bool  left   = false;   // A — strafe left
    bool  right  = false;   // D — strafe right
    bool  up     = false;   // Space — ascend
    bool  down   = false;   // LShift — descend
    float target_yaw = 0.0f; // radians, taken from the camera each frame
};

// Find a vehicle the player can mount. Returns the index in `vehs`, or
// -1 if none is within `mount_radius`. `player_pos_gl` is GL world space
// (Y up); the vehicle position is brought into GL space inside.
int vehicle_find_mountable(
    const std::vector<VehiclePlaceholderState>& vehs,
    const glm::vec3& player_pos_gl,
    float mount_radius);

// World-space (GL) position of a vehicle for rendering / cameras.
// Returns dyn_pos_gl once the vehicle has been mounted at least once;
// otherwise computes it from the pad transform (Tribes Z-up -> GL Y-up,
// lifted by 1.5 m to sit on the pad).
glm::vec3 vehicle_world_pos_gl(const VehiclePlaceholderState& v);

// One fixed-step integration. Applies gravity, input thrust, lateral
// strafe, and a soft terrain floor (vehicle won't sink below
// ground_clearance metres above the heightmap). `in.target_yaw` is
// adopted directly so the vehicle faces wherever the camera is
// pointing — simpler than spring-tracking and matches Tribes' feel.
void vehicle_tick(VehiclePlaceholderState& v,
                  const VehicleTuning&     t,
                  const VehicleInput&      in,
                  const HeightSampler&     terrain,
                  float                    dt);

// Pilot dismount: returns a sensible drop-off point in GL coords,
// offset by `dismount_offset` in the vehicle's local +X direction so
// the pilot doesn't clip into the chassis.
glm::vec3 vehicle_dismount_pos(const VehiclePlaceholderState& v,
                               const VehicleTuning& t);

} // namespace dts_viewer

#endif // DTS_VIEWER_VEHICLE_HPP
