#ifndef DTS_VIEWER_TERRAIN_RENDERER_HPP
#define DTS_VIEWER_TERRAIN_RENDERER_HPP

// Terrain heightmap renderer — Spec 05 (05-terrain track).
//
// Accepts a parsed grid_block (heights + materials) from the DTB parser plus
// the scale factor from the DTF, and emits a GL-ready vertex + index buffer
// for the 257×257 heightmap grid.
//
// Vertex layout: position (vec3), material_index (float), normal (vec3) —
// packed interleaved in one VBO (stride = 7 floats = 28 bytes).
// Index layout:  u32 triangle list; 256*256*2 triangles = 131 072 triangles.
//
// Normals are computed from central differences (clamped at boundary):
//   dh/dx = (h[x+1,y] - h[x-1,y]) / (2 * metres_per_quad)   (or one-sided at edge)
//   dh/dy = (h[x,y+1] - h[x,y-1]) / (2 * metres_per_quad)
//   normal = normalize(vec3(-dh/dx, 1, -dh/dy))
//
// Per-quad coloring: the material_index float carries the raw
// material_cell::index value from the materialmap. The fragment shader
// maps it to one of 16 hard-coded distinct colors via modulo.
//
// World position of vertex (x, y):
//   world_x = x * metres_per_quad
//   world_y = heights[y * (grid_x+1) + x]
//   world_z = y * metres_per_quad
// (Y-up, consistent with the existing dts-viewer convention.)
//
// No leaked engine source was consulted.

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdint>
#include <vector>

#include "content/terrain/dtb.hpp"

namespace dts_viewer
{

struct TerrainMesh
{
    GLuint vao         = 0;
    GLuint vbo         = 0;   // interleaved: pos(3) + mat_idx(1) + normal(3)
    GLuint ibo         = 0;   // u32 index buffer
    GLsizei index_count = 0;

    // Bounding box for camera framing.
    glm::vec3 bbox_min{ 1e30f,  1e30f,  1e30f};
    glm::vec3 bbox_max{-1e30f, -1e30f, -1e30f};

    bool valid() const { return vao != 0 && index_count > 0; }
};

// Build a GL-ready TerrainMesh from a parsed grid_block.
// metres_per_quad = (1 << dtf.scale), typically 8 for shipped Tribes missions.
TerrainMesh build_terrain_mesh(
    const studio::content::terrain::grid_block& block,
    float metres_per_quad);

// Draw the terrain.  The caller must have a program active with:
//   uniform mat4 u_mvp_terrain;
// and (optionally) wireframe via glPolygonMode before the call.
void draw_terrain(
    const TerrainMesh& mesh,
    GLint u_mvp_loc);

} // namespace dts_viewer

// ---- inline implementation ----
// Kept here (header-only) so no separate .cpp is needed in CMakeLists.

#include <algorithm>
#include <cmath>

namespace dts_viewer
{

inline TerrainMesh build_terrain_mesh(
    const studio::content::terrain::grid_block& block,
    float metres_per_quad)
{
    const int sx = block.size[0];  // 256
    const int sy = block.size[1];  // 256
    const int vx = sx + 1;        // 257
    const int vy = sy + 1;        // 257

    // Height lookup with bounds check.
    auto H = [&](int x, int y) -> float {
        x = std::max(0, std::min(x, vx - 1));
        y = std::max(0, std::min(y, vy - 1));
        return block.heights[static_cast<std::size_t>(y) * vx + x];
    };

    const std::size_t vertex_count = static_cast<std::size_t>(vx) * vy;
    // 7 floats per vertex: pos.xyz, mat_idx, normal.xyz
    std::vector<float> vdata;
    vdata.reserve(vertex_count * 7);

    TerrainMesh mesh;

    for (int y = 0; y < vy; ++y) {
        for (int x = 0; x < vx; ++x) {
            float wx = x * metres_per_quad;
            float wy = H(x, y);
            float wz = y * metres_per_quad;

            mesh.bbox_min = glm::min(mesh.bbox_min, glm::vec3(wx, wy, wz));
            mesh.bbox_max = glm::max(mesh.bbox_max, glm::vec3(wx, wy, wz));

            // Central-difference normal.
            float hL = H(x - 1, y);
            float hR = H(x + 1, y);
            float hD = H(x, y - 1);
            float hU = H(x, y + 1);

            // dx step is metres_per_quad; use actual spacing.
            float dHdx = (hR - hL) / (2.0f * metres_per_quad);
            float dHdz = (hU - hD) / (2.0f * metres_per_quad);
            glm::vec3 n = glm::normalize(glm::vec3(-dHdx, 1.0f, -dHdz));

            // Material index from materialmap (per-vertex cell, same array).
            float mat_idx = 0.0f;
            if (!block.materials.empty()) {
                std::size_t mi = static_cast<std::size_t>(y) * vx + x;
                if (mi < block.materials.size()) {
                    mat_idx = static_cast<float>(block.materials[mi].index);
                }
            }

            vdata.push_back(wx);
            vdata.push_back(wy);
            vdata.push_back(wz);
            vdata.push_back(mat_idx);
            vdata.push_back(n.x);
            vdata.push_back(n.y);
            vdata.push_back(n.z);
        }
    }

    // Index buffer: two triangles per quad.
    // Quad (x, y) uses corners:
    //   BL = y*vx + x, BR = y*vx + (x+1),
    //   TL = (y+1)*vx + x, TR = (y+1)*vx + (x+1)
    const std::size_t tri_count = static_cast<std::size_t>(sx) * sy * 2;
    std::vector<std::uint32_t> idata;
    idata.reserve(tri_count * 3);

    for (int y = 0; y < sy; ++y) {
        for (int x = 0; x < sx; ++x) {
            std::uint32_t BL = static_cast<std::uint32_t>(y * vx + x);
            std::uint32_t BR = BL + 1;
            std::uint32_t TL = static_cast<std::uint32_t>((y + 1) * vx + x);
            std::uint32_t TR = TL + 1;
            // Triangle 1: BL, BR, TL
            idata.push_back(BL); idata.push_back(BR); idata.push_back(TL);
            // Triangle 2: BR, TR, TL
            idata.push_back(BR); idata.push_back(TR); idata.push_back(TL);
        }
    }

    mesh.index_count = static_cast<GLsizei>(idata.size());

    // Upload to GPU.
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 vdata.size() * sizeof(float),
                 vdata.data(), GL_STATIC_DRAW);

    constexpr GLsizei stride = 7 * sizeof(float);
    // location 0: position (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(0));
    // location 1: mat_idx (float, read as float)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    // location 2: normal (vec3)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(4 * sizeof(float)));

    glGenBuffers(1, &mesh.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 idata.size() * sizeof(std::uint32_t),
                 idata.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);
    return mesh;
}

inline void draw_terrain(const TerrainMesh& mesh, GLint u_mvp_loc)
{
    if (!mesh.valid()) return;
    glBindVertexArray(mesh.vao);
    // u_mvp_loc already set by caller via glUniformMatrix4fv before this call.
    (void)u_mvp_loc;
    glDrawElements(GL_TRIANGLES, mesh.index_count, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace dts_viewer

#endif // DTS_VIEWER_TERRAIN_RENDERER_HPP
