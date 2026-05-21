#include "physics_capsule.hpp"

namespace dts_viewer
{

// Stub implementation: --mission mode does not load InteriorShape
// geometry yet, so the capsule has nothing to collide against.  Once a
// future spec (interior loading under --mission) lands, this body will
// be replaced with a real swept-capsule-against-BSP routine.
//
// Returning `hit=false, time=1.0` lets `player_update` proceed with the
// existing terrain-only physics path unchanged.
bool capsule_sweep_interior(
    const glm::vec3& /*from*/,
    const glm::vec3& /*to*/,
    float            /*radius*/,
    float            /*half_height*/,
    CapsuleHitInfo&  out_hit)
{
    out_hit.hit  = false;
    out_hit.time = 1.0f;
    return false;
}

} // namespace dts_viewer
