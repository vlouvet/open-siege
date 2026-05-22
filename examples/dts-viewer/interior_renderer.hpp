#ifndef DTS_VIEWER_INTERIOR_RENDERER_HPP
#define DTS_VIEWER_INTERIOR_RENDERER_HPP

// Interior (DIG/DIS) BSP mesh renderer — Spec 05 (06-interiors track).
//
// Loads a parsed dig_file, pairs it with a DML material list and a
// MaterialResolver, triangulates every surface by fan, uploads one
// interleaved VBO per material group, and renders with the same
// textured/flat shader already used by the DTS path.
//
// Vertex layout in the VBO: position (3 floats) + uv (2 floats).
// Index buffer: u32 triangle list produced by fan-triangulating each
// convex polygon (v0,v1,v2), (v0,v2,v3), ...
//
// Material 0 is the engine's "null" slot; surfaces that reference it
// are skipped (never uploaded). Surfaces whose DML material maps to an
// empty filename are also skipped (consistent with the DTS texture
// pipeline: empty-name -> flat-shading fallback, but for interiors we
// just omit them since the geometry is static and the null surfaces are
// internal BSP helpers, not visible faces).

#define GL_SILENCE_DEPRECATION
#include "gl_includes.hpp"
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "content/dig/dig.hpp"
#include "content/dts/darkstar_structures.hpp"
#include "content/interior/dil.hpp"
#include "materials.hpp"
#include "ppl.hpp"

namespace dts_viewer
{

// Per-material draw range within the shared index buffer.
struct InteriorDrawRange
{
    GLsizei offset;     // byte offset into the index buffer
    GLsizei count;      // number of indices (= triangle_count * 3)
    GLuint  texture;    // 0 = flat-shading fallback
};

struct InteriorMesh
{
    GLuint vao = 0;
    GLuint vbo = 0;     // interleaved pos(3) + uv_d(2) + uv_lm(2) per vertex
    GLuint ibo = 0;     // u32 index buffer
    GLsizei index_count = 0;   // total (sum of all ranges)
    std::vector<InteriorDrawRange> ranges;

    // Per-interior lightmap atlas (spec 06-06). 0 when no DIL was supplied
    // or the unpack produced no usable texels.
    GLuint  lightmap_atlas = 0;
    GLsizei lightmap_atlas_w = 0;
    GLsizei lightmap_atlas_h = 0;

    // Bounding box for camera framing.
    glm::vec3 bbox_min{ 1e30f,  1e30f,  1e30f};
    glm::vec3 bbox_max{-1e30f, -1e30f, -1e30f};

    bool valid() const { return vao != 0 && index_count > 0; }
};

// Build a GL-ready InteriorMesh from a parsed DIG file.
// `dml` supplies per-material filenames; `resolver` maps them to PBMP
// bytes + uploads GL textures (identical pipeline as the DTS path).
// `palette_map` should carry the same palettes already loaded for the
// DTS viewer session (Shell.ppl + world .day.ppl).
//
// `dil` (optional) carries the per-surface baked lightmap data; when
// supplied, the function decodes each surface's Huffman bit-stream,
// packs the unpacked 4:4:4:4 IRGB rectangles into a single atlas, and
// adds per-vertex lightmap UVs alongside the diffuse UVs. Pass nullptr
// to fall back to diffuse-only rendering (vertex shader receives zero
// lightmap UV, fragment shader treats lightmap sample as white).
//
// `stock_dil` (optional, only meaningful when `dil` is itself an
// ITRMissionLighting delta) is the parent stock ITRLighting loaded from
// the per-world VOL. Mission DILs are sparse overrides: for surfaces
// listed in the mission's IndexEntry table, the lightmap is sourced
// from `stock_dil->surfaces[dest_index]`; for surfaces the mission
// didn't bake anew it falls back to `stock_dil->surfaces[N]`. See
// docs/clean-room-specs/DIL-INNER.md §7.3.
InteriorMesh build_interior_mesh(
    const studio::content::dig::dig_file& geom,
    const studio::content::dts::darkstar::material_list_variant& dml,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map,
    const studio::content::interior::dil_file* dil = nullptr,
    const studio::content::interior::dil_file* stock_dil = nullptr);

// Draw the interior using a two-sampler shader program. `u_tex0` is the
// diffuse PBMP sampler (texture unit 0), `u_lightmap` is the per-
// interior baked-lighting atlas (texture unit 1). Pass -1 for
// `u_lightmap_loc` / `u_has_lightmap_loc` when the shader has no
// lightmap binding.
void draw_interior(
    const InteriorMesh& mesh,
    GLint u_mvp_loc,
    GLint u_has_texture_loc,
    GLint u_tex0_loc,
    GLint u_lightmap_loc,
    GLint u_has_lightmap_loc,
    const glm::mat4& mvp);

} // namespace dts_viewer

#endif // DTS_VIEWER_INTERIOR_RENDERER_HPP
