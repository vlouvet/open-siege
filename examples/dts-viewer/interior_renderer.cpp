#include "interior_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "pbmp.hpp"
#include "ppl.hpp"

namespace dts_viewer
{

// ---- helpers -----------------------------------------------------------------

// Extract the null-terminated filename from a 32-char DML array.
static std::string dml_filename(const std::array<char, 32>& arr)
{
    return std::string(arr.data(), strnlen(arr.data(), arr.size()));
}

// Normalize a DML material filename the same way MaterialResolver does:
// strip leading "base." and lowercase everything.
static std::string normalize_mat(const std::string& in)
{
    std::string s = in;
    if (s.size() >= 5 && s.compare(0, 5, "base.") == 0) s.erase(0, 5);
    else if (s.size() >= 5
        && (s[0]=='B'||s[0]=='b') && (s[1]=='A'||s[1]=='a')
        && (s[2]=='S'||s[2]=='s') && (s[3]=='E'||s[3]=='e')
        && s[4]=='.') {
        s.erase(0, 5);
    }
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::vector<std::uint8_t> expand_interior_rgba(
    const PbmpImage& bmp,
    const std::map<std::uint32_t, const Palette*>& palettes)
{
    const std::size_t w = bmp.width;
    const std::size_t h = bmp.height;
    std::vector<std::uint8_t> out(w * h * 4, 0);

    auto it = palettes.find(bmp.palette_index);
    if (it == palettes.end() || it->second == nullptr) {
        // Magenta checker fallback.
        static std::set<std::uint32_t> warned;
        if (warned.insert(bmp.palette_index).second) {
            std::fprintf(stderr,
                "interior: PBMP palette_index=%u not in loaded PPL — magenta fallback\n",
                bmp.palette_index);
        }
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const bool on = ((x >> 3) ^ (y >> 3)) & 1;
                const std::size_t i = (y * w + x) * 4;
                out[i + 0] = on ? 255 : 0;
                out[i + 1] = 0;
                out[i + 2] = on ? 255 : 0;
                out[i + 3] = 255;
            }
        }
        return out;
    }

    const Palette& pal = *it->second;
    const std::size_t pixel_count = std::min<std::size_t>(
        bmp.indexed_pixels.size(), w * h);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::uint8_t idx = bmp.indexed_pixels[i];
        const PaletteEntry& e = pal.colours[idx];
        const std::size_t o = i * 4;
        out[o + 0] = e.r;
        out[o + 1] = e.g;
        out[o + 2] = e.b;
        out[o + 3] = 255;
    }
    return out;
}

// Upload a PBMP (resolved by the MaterialResolver) as a GL texture, or
// return 0 on failure. The texture_cache (key = normalized name) is
// consulted first to avoid re-uploading duplicates.
static GLuint upload_texture(
    const std::string& raw_name,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map,
    std::map<std::string, GLuint>& texture_cache)
{
    const std::string key = normalize_mat(raw_name);
    if (key.empty()) return 0;

    auto cached = texture_cache.find(key);
    if (cached != texture_cache.end()) return cached->second;

    auto bytes = resolver.resolve(raw_name);
    if (!bytes || bytes->empty()) return 0;

    PbmpImage img;
    try {
        std::stringstream ss(std::string(bytes->begin(), bytes->end()));
        img = load_pbmp(ss);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "interior: PBMP parse failed for '%s': %s\n",
            raw_name.c_str(), e.what());
        return 0;
    }
    if (img.width == 0 || img.height == 0) return 0;

    auto rgba = expand_interior_rgba(img, palette_map);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 static_cast<GLsizei>(img.width),
                 static_cast<GLsizei>(img.height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    texture_cache[key] = tex;
    return tex;
}

// ---- main build function -----------------------------------------------------

InteriorMesh build_interior_mesh(
    const studio::content::dig::dig_file& geom,
    const studio::content::dts::darkstar::material_list_variant& dml,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map)
{
    using namespace studio::content::dig;

    InteriorMesh mesh;

    // Extract material filenames from the DML variant (v2/v3/v4 all have
    // a `materials` vector whose elements have a `file_name` array).
    std::vector<std::string> mat_names;
    std::visit([&](const auto& ml) {
        mat_names.reserve(ml.materials.size());
        for (const auto& m : ml.materials) {
            mat_names.push_back(dml_filename(m.file_name));
        }
    }, dml);

    const std::size_t n_mats = mat_names.size();

    // Per-material vertex + index accumulators.
    // Vertex layout: pos.x, pos.y, pos.z, uv.u, uv.v (5 floats, 20 bytes).
    struct MatBuf {
        std::vector<float>          verts;   // 5 floats per vertex
        std::vector<std::uint32_t>  indices; // u32 triangle list
        std::uint32_t               base = 0; // vertex base offset when merged
    };
    std::vector<MatBuf> per_mat(n_mats);

    // Triangulate surfaces, grouped by material index.
    for (const auto& surf : geom.surfaces) {
        const std::size_t mat_idx = static_cast<std::size_t>(surf.material);

        // Skip material 0 (engine null slot) and out-of-range indices.
        if (mat_idx == 0 || mat_idx >= n_mats) continue;

        // Skip surfaces with empty material filenames.
        if (mat_names[mat_idx].empty()) continue;

        const std::size_t n_verts = std::min<std::size_t>(
            surf.vertex_count, surf.point_count);
        if (n_verts < 3) continue;

        // Bounds-check the vertex range.
        const std::size_t v_start = static_cast<std::size_t>(surf.vertex_index);
        if (v_start + n_verts > geom.vertices.size()) continue;

        MatBuf& buf = per_mat[mat_idx];

        // Local vertex index base within this buffer.
        const std::uint32_t local_base = static_cast<std::uint32_t>(
            buf.verts.size() / 5);

        // Emit one vertex per polygon corner.
        for (std::size_t vi = 0; vi < n_verts; ++vi) {
            const packed_vertex& pv = geom.vertices[v_start + vi];
            const std::size_t pi = static_cast<std::size_t>(pv.point_index);
            const std::size_t ti = static_cast<std::size_t>(pv.texture_index);

            float px = 0.0f, py = 0.0f, pz = 0.0f;
            if (pi < geom.points3.size()) {
                px = geom.points3[pi].x;
                py = geom.points3[pi].y;
                pz = geom.points3[pi].z;
            }
            float u = 0.0f, v = 0.0f;
            if (ti < geom.points2.size()) {
                u = geom.points2[ti].x;
                v = geom.points2[ti].y;
            }

            buf.verts.push_back(px);
            buf.verts.push_back(py);
            buf.verts.push_back(pz);
            buf.verts.push_back(u);
            buf.verts.push_back(v);

            // Update mesh bounding box.
            mesh.bbox_min = glm::min(mesh.bbox_min, glm::vec3(px, py, pz));
            mesh.bbox_max = glm::max(mesh.bbox_max, glm::vec3(px, py, pz));
        }

        // Fan-triangulate the polygon: (0,1,2), (0,2,3), ...
        for (std::size_t i = 1; i + 1 < n_verts; ++i) {
            buf.indices.push_back(local_base);
            buf.indices.push_back(local_base + static_cast<std::uint32_t>(i));
            buf.indices.push_back(local_base + static_cast<std::uint32_t>(i + 1));
        }
    }

    // Count total vertices and indices, assign per-buffer base offsets.
    std::uint32_t total_verts   = 0;
    std::uint32_t total_indices = 0;
    for (auto& buf : per_mat) {
        buf.base = total_verts;
        total_verts   += static_cast<std::uint32_t>(buf.verts.size() / 5);
        total_indices += static_cast<std::uint32_t>(buf.indices.size());
    }

    if (total_verts == 0 || total_indices == 0) {
        std::fprintf(stderr, "interior: no renderable surfaces\n");
        return mesh;
    }

    // Merge into a single interleaved VBO and a single IBO.
    // Each per-mat buffer's indices need to be rebased by buf.base.
    std::vector<float>          merged_verts;
    std::vector<std::uint32_t>  merged_idx;
    merged_verts.reserve(static_cast<std::size_t>(total_verts) * 5);
    merged_idx.reserve(static_cast<std::size_t>(total_indices));

    std::map<std::string, GLuint> tex_cache; // local per-build; no cross-call sharing needed
    GLuint tex_err = glGetError(); (void)tex_err;

    for (std::size_t mi = 0; mi < n_mats; ++mi) {
        auto& buf = per_mat[mi];
        if (buf.verts.empty()) continue;

        // Record this draw range before merging.
        InteriorDrawRange range;
        range.offset = static_cast<GLsizei>(merged_idx.size() * sizeof(std::uint32_t));
        range.count  = static_cast<GLsizei>(buf.indices.size());

        // Upload texture for this material.
        if (mi < mat_names.size() && !mat_names[mi].empty()) {
            range.texture = upload_texture(
                mat_names[mi], resolver, palette_map, tex_cache);
        } else {
            range.texture = 0;
        }

        if (range.count == 0) continue;

        // Append vertices to the merged buffer.
        merged_verts.insert(merged_verts.end(),
                            buf.verts.begin(), buf.verts.end());

        // Append indices, rebased by buf.base.
        for (const auto& idx : buf.indices) {
            merged_idx.push_back(idx + buf.base);
        }

        mesh.ranges.push_back(range);
    }

    if (merged_verts.empty() || merged_idx.empty()) {
        std::fprintf(stderr, "interior: all material buffers empty after merge\n");
        return mesh;
    }

    // Upload to GL.
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(merged_verts.size() * sizeof(float)),
                 merged_verts.data(), GL_STATIC_DRAW);

    // layout(location = 0) = position (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          5 * sizeof(float), reinterpret_cast<void*>(0));

    // layout(location = 2) = uv (vec2)  — location 1 is normal; we skip it
    // by uploading zeros for normal in the shader (it uses location 1 but
    // the vertex layout here has no normal). We use a separate "stub" normal
    // VBO to keep the VAO complete.
    //
    // Actually the existing shader accesses location 1 (a_normal) and
    // location 2 (a_uv). Since interiors have no per-vertex normals we bind
    // location 1 to a zero VBO and location 2 to the UV from our interleaved
    // buffer. Simpler approach: just re-use position for normals (flat
    // shading direction wrong, but that's acceptable until a normal spec).
    // Better: bind location 1 to a 1-element VBO of (0,0,1) with divisor=0
    // — but that needs instancing. Simplest correct approach: use a second
    // static VBO of zeros the same size as vertex count.

    // Normal stub: (0, 0, 1) for every vertex.
    GLuint vbo_nor = 0;
    glGenBuffers(1, &vbo_nor);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_nor);
    {
        const std::size_t vcount = merged_verts.size() / 5;
        std::vector<float> norms(vcount * 3, 0.0f);
        for (std::size_t i = 2; i < norms.size(); i += 3) norms[i] = 1.0f;
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(norms.size() * sizeof(float)),
                     norms.data(), GL_STATIC_DRAW);
    }
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Re-bind the interleaved VBO for the UV attribute.
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE,
                          5 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));

    glGenBuffers(1, &mesh.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(merged_idx.size() * sizeof(std::uint32_t)),
                 merged_idx.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // The normal VBO is now referenced by the VAO; we can delete the handle
    // (the GL object stays alive until the VAO is deleted).
    glDeleteBuffers(1, &vbo_nor);

    mesh.index_count = static_cast<GLsizei>(merged_idx.size());

    std::size_t ranges_with_tex = 0;
    for (const auto& r : mesh.ranges) if (r.texture != 0) ++ranges_with_tex;
    std::fprintf(stderr,
        "interior: %u verts, %u indices, %zu draw ranges (%zu textured)\n",
        total_verts, total_indices,
        mesh.ranges.size(), ranges_with_tex);

    return mesh;
}

// ---- draw --------------------------------------------------------------------

void draw_interior(
    const InteriorMesh& mesh,
    GLint u_mvp_loc,
    GLint u_has_texture_loc,
    GLint u_tex0_loc,
    const glm::mat4& mvp)
{
    if (!mesh.valid()) return;

    glUniformMatrix4fv(u_mvp_loc, 1, GL_FALSE, glm::value_ptr(mvp));
    if (u_tex0_loc >= 0) glUniform1i(u_tex0_loc, 0);
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);

    for (const auto& r : mesh.ranges) {
        if (r.count == 0) continue;
        if (r.texture != 0) {
            glBindTexture(GL_TEXTURE_2D, r.texture);
            if (u_has_texture_loc >= 0) glUniform1i(u_has_texture_loc, 1);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
            if (u_has_texture_loc >= 0) glUniform1i(u_has_texture_loc, 0);
        }
        glDrawElements(GL_TRIANGLES, r.count, GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(
                           static_cast<std::uintptr_t>(r.offset)));
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace dts_viewer
