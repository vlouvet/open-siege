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
#include <OpenGL/gl3.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "content/dig/dig.hpp"
#include "content/dts/darkstar_structures.hpp"
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
    GLuint vbo = 0;     // interleaved pos(3) + uv(2) per vertex
    GLuint ibo = 0;     // u32 index buffer
    GLsizei index_count = 0;   // total (sum of all ranges)
    std::vector<InteriorDrawRange> ranges;

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
InteriorMesh build_interior_mesh(
    const studio::content::dig::dig_file& geom,
    const studio::content::dts::darkstar::material_list_variant& dml,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map);

// Draw the interior using a shader program that already has u_mvp and
// u_has_texture / u_tex0 uniforms set up (identical to the DTS shader).
void draw_interior(
    const InteriorMesh& mesh,
    GLint u_mvp_loc,
    GLint u_has_texture_loc,
    GLint u_tex0_loc,
    const glm::mat4& mvp);

} // namespace dts_viewer

#endif // DTS_VIEWER_INTERIOR_RENDERER_HPP
