#ifndef DTS_VIEWER_PHYSICS_CAPSULE_HPP
#define DTS_VIEWER_PHYSICS_CAPSULE_HPP

// Capsule-vs-interior collision interface — spec 09/06.
//
// v1 of this spec ships the *interface* only.  --mission mode does not
// load InteriorShape geometry yet (the existing --interior mode loads a
// single DIS at a time), so there is nothing for the capsule to collide
// against during normal player play.  Once a future spec loads interiors
// into --mission mode, the no-op implementation below will be replaced
// with a real BSP sweep.
//
// The 3-iteration slide algorithm (project velocity onto contact plane,
// retry remaining motion, up to 3 contacts per step) is a standard
// Quake-derived pattern; the spec calls it out as the expected approach.
// Step-up: when a sweep hits a near-vertical surface whose top is within
// `step_height` of the player's feet, retry the sweep with the start
// lifted by that amount.
//
// Bots (Track 18) and entity stubs (Track 14 spec 04 elevators) will
// reuse this header.

#include <glm/glm.hpp>

namespace dts_viewer
{

struct CapsuleHitInfo
{
    bool      hit = false;
    glm::vec3 contact_point  { 0.0f };
    glm::vec3 contact_normal { 0.0f, 1.0f, 0.0f };
    float     time = 1.0f;             // 0..1 fraction of the sweep
};

// Move a capsule from `from` to `to` and report the first contact (if
// any) against loaded interior geometry.  When no interiors are loaded
// (--mission mode prior to interior wiring), this always returns
// `hit = false, time = 1.0` — i.e. the sweep completes uninterrupted.
//
//   radius      — capsule cylinder radius (metres)
//   half_height — vertical distance from the capsule centre to either
//                 hemisphere centre (metres)
bool capsule_sweep_interior(
    const glm::vec3& from,
    const glm::vec3& to,
    float            radius,
    float            half_height,
    CapsuleHitInfo&  out_hit);

} // namespace dts_viewer

#endif // DTS_VIEWER_PHYSICS_CAPSULE_HPP
