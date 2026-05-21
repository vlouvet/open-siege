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
    GLuint vbo_blend   = 0;   // interleaved: layer_w(vec4) + layer_idx(vec4)
    GLuint ibo         = 0;   // u32 index buffer
    GLsizei index_count = 0;

    // Bounding box for camera framing.
    glm::vec3 bbox_min{ 1e30f,  1e30f,  1e30f};
    glm::vec3 bbox_max{-1e30f, -1e30f, -1e30f};

    bool valid() const { return vao != 0 && index_count > 0; }
};

// Build a GL-ready TerrainMesh from a parsed grid_block.
// metres_per_quad = (1 << dtf.scale), typically 8 for shipped Tribes missions.
// `world_origin` is added to every output X/Z so the mesh can be placed
// centered on world origin (mission mode passes -half_size so entities
// at negative world coords land on the rendered tile — Tribes' wrap-around
// terrain semantics).
TerrainMesh build_terrain_mesh(
    const studio::content::terrain::grid_block& block,
    float metres_per_quad,
    glm::vec2 world_origin = glm::vec2(0.0f, 0.0f));

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
#include <array>
#include <cmath>
#include <utility>

namespace dts_viewer
{

inline TerrainMesh build_terrain_mesh(
    const studio::content::terrain::grid_block& block,
    float metres_per_quad,
    glm::vec2 world_origin)
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

    // Material lookup with bounds clamp; returns -1 if no materials.
    auto M = [&](int x, int y) -> int {
        if (block.materials.empty()) return -1;
        x = std::max(0, std::min(x, vx - 1));
        y = std::max(0, std::min(y, vy - 1));
        return static_cast<int>(block.materials[
            static_cast<std::size_t>(y) * vx + x].index);
    };

    // 05/08 — derive 4-layer blended weights at every vertex by sampling
    // the per-vertex authored layer + its 4 grid neighbours, counting
    // occurrences, and keeping the top 4 contributors. Pads with zero
    // weights when fewer than 4 distinct layers are present.
    auto compute_blend = [&](int x, int y,
                             float w_out[4], float i_out[4])
    {
        const int samples[5] = {
            M(x, y), M(x - 1, y), M(x + 1, y), M(x, y - 1), M(x, y + 1)
        };
        std::array<std::pair<int, int>, 5> counts{};
        int n = 0;
        for (int s : samples) {
            if (s < 0) continue;
            bool found = false;
            for (int k = 0; k < n; ++k) {
                if (counts[k].first == s) {
                    ++counts[k].second;
                    found = true; break;
                }
            }
            if (!found && n < 5) { counts[n++] = {s, 1}; }
        }
        std::sort(counts.begin(), counts.begin() + n,
            [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
        const int top = std::min(n, 4);
        int total = 0;
        for (int k = 0; k < top; ++k) total += counts[k].second;
        for (int k = 0; k < 4; ++k) { w_out[k] = 0.0f; i_out[k] = 0.0f; }
        if (total > 0) {
            for (int k = 0; k < top; ++k) {
                i_out[k] = static_cast<float>(counts[k].first);
                w_out[k] = static_cast<float>(counts[k].second) /
                           static_cast<float>(total);
            }
        }
    };

    const std::size_t vertex_count = static_cast<std::size_t>(vx) * vy;
    // 7 floats per vertex: pos.xyz, mat_idx, normal.xyz
    std::vector<float> vdata;
    vdata.reserve(vertex_count * 7);
    // 8 floats per vertex for the blend buffer: weights.xyzw + indices.xyzw
    std::vector<float> bdata;
    bdata.reserve(vertex_count * 8);

    TerrainMesh mesh;

    for (int y = 0; y < vy; ++y) {
        for (int x = 0; x < vx; ++x) {
            float wx = x * metres_per_quad + world_origin.x;
            float wy = H(x, y);
            float wz = y * metres_per_quad + world_origin.y;

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

            float bw[4], bi[4];
            compute_blend(x, y, bw, bi);
            bdata.push_back(bw[0]); bdata.push_back(bw[1]);
            bdata.push_back(bw[2]); bdata.push_back(bw[3]);
            bdata.push_back(bi[0]); bdata.push_back(bi[1]);
            bdata.push_back(bi[2]); bdata.push_back(bi[3]);
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

    // Blend buffer (05/08): 4 weights + 4 layer indices per vertex.
    glGenBuffers(1, &mesh.vbo_blend);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_blend);
    glBufferData(GL_ARRAY_BUFFER,
                 bdata.size() * sizeof(float),
                 bdata.data(), GL_STATIC_DRAW);
    constexpr GLsizei blend_stride = 8 * sizeof(float);
    // location 3: layer weights (vec4)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, blend_stride,
                          reinterpret_cast<const void*>(0));
    // location 4: layer indices (vec4, kept as float so 4.1 core path works)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, blend_stride,
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
