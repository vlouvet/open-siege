#pragma once

// Tribes -> GL axis conversion for mission-file (.mis) transforms.
//
// Tribes is Z-up: a vector (X, Y, Z) means X forward, Y horizontal,
// Z = altitude. Gravity is "0 0 -20" along negative Z and SimTerrain
// position[2] = 0 (block sits at z=0).
//
// Our viewer is Y-up (OpenGL convention). The terrain mesh already
// builds with `wy = H(x, y)` so the heightmap's altitude lives on GL Y.
// Entity positions read from the MIS, however, are stored verbatim in
// node_*.xf.position[]. Without converting them at the consumption
// site, a building at MIS (289, -35, 142) lands at GL (289, -35, 142)
// — Y=-35 puts it below the ground.
//
// Convention used here:
//   GL X = Tribes X   (forward axis, unchanged)
//   GL Y = Tribes Z   (vertical / altitude)
//   GL Z = Tribes Y   (horizontal-perpendicular)
//
// This matches build_terrain_mesh's mapping (`wx=x*q, wy=H, wz=y*q`).
// Both spaces are right-handed; this permutation flips handedness but
// the terrain code uses the same permutation so visualisation is
// consistent. UI features that care about handedness (e.g. compass) are
// already aligned with the terrain.

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace dts_viewer {

inline glm::vec3 mis_pos_to_gl(const std::array<float, 3>& p)
{
    return glm::vec3(p[0], p[2], p[1]);
}

// Permutation matrix mapping Tribes-local (Z-up) to GL-world (Y-up).
//   (Tx, Ty, Tz) -> (Tx, Tz, Ty)
// Same swap the terrain mesh applies; both spaces are right-handed.
inline glm::mat4 tribes_to_gl_basis()
{
    // glm is column-major. Column N is where the N-th basis vector of
    // the source space lands in the target space. We want source-Y
    // (Tribes horizontal) to land on target-Z (GL horizontal) and
    // source-Z (Tribes up) to land on target-Y (GL up). X stays X.
    glm::mat4 P(1.0f);
    P[1] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f); // src Y -> dst Z
    P[2] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f); // src Z -> dst Y
    return P;
}

// Full world matrix for a mesh whose local vertices are in Tribes-Z-up
// coords, placed at the MIS transform (also in Tribes coords). The
// resulting matrix lives in GL-Y-up world space:
//   M = P * T_tribes * R_tribes
// so a local vertex `v_local` (Tribes coords) -> GL world position.
inline glm::mat4 mis_world_matrix(const std::array<float, 3>& pos,
                                  const std::array<float, 4>& rot)
{
    glm::mat4 T = glm::translate(glm::mat4(1.0f),
        glm::vec3(pos[0], pos[1], pos[2]));
    glm::mat4 R(1.0f);
    if (std::abs(rot[3] - 1.0f) < 1e-3f) {
        // Apply in XYZ order around Tribes axes (matches the corpus
        // convention; yaw-only rotations are by far the common case).
        R = glm::rotate(R, rot[0], glm::vec3(1, 0, 0));
        R = glm::rotate(R, rot[1], glm::vec3(0, 1, 0));
        R = glm::rotate(R, rot[2], glm::vec3(0, 0, 1));
    } else {
        glm::quat q(rot[3], rot[0], rot[1], rot[2]);
        R = glm::mat4_cast(q);
    }
    return tribes_to_gl_basis() * T * R;
}

} // namespace dts_viewer
