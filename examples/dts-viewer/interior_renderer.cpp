#include "interior_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
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

// ---- spec 06-06: DIL lightmap unpack + atlas -------------------------------
//
// Each DIL surface owns a `map_size_x * map_size_y` rectangle of 4:4:4:4 IRGB
// pixels. The pixels are reachable through one of two encodings on
// `map_index_or_color`:
//   - bit 30 set: low 30 bits = byte offset into `map_data[]`, where the
//     Huffman-compressed bit-stream for this surface starts. MSB-first
//     bit-reads, tree root = `huffman_nodes.back()`. Decode `pixel_count`
//     16-bit colour leaves in row-major order.
//   - bit 30 clear: low 16 bits ARE the single IRGB pixel that fills the
//     whole rectangle (unlit surface — exterior / skybox).
//
// 16-bit pixel layout: II IR R RG GG GB BB B (MSB to LSB), each channel
// 4 bits. Per the spec note, `light_scale_shift = 4` in shipping content
// (so the intensity scales by 2^4 / 15 = 16/15 ≈ 1.067 — applied at
// unpack time as a multiplier on the RGB channels).
//
// Atlas packing: shelf algorithm — sort surfaces by height descending,
// place each into the current row left-to-right; when the row is full,
// start a new row directly below. 1-texel padding around each tile to
// stop bilinear filtering from bleeding neighbours across atlas edges.

namespace
{

// One unpacked surface lightmap. Origin (0,0) is top-left of the
// rectangle; pixels[y * w + x] for (x,y) in [0, w-1] × [0, h-1].
struct UnpackedTile
{
    std::uint32_t surface_index = 0;
    int           w = 0;
    int           h = 0;
    std::vector<std::uint8_t> rgba; // w * h * 4 bytes
    // Atlas-space top-left after packing. -1 = not yet placed / dropped.
    int atlas_x = -1;
    int atlas_y = -1;
};

// Huffman decoder state: MSB-first bit-reader over `map_data`.
struct HuffmanBitReader
{
    const std::byte* data;
    std::size_t      data_size;
    std::size_t      byte_pos;  // next byte to top-up from
    std::uint32_t    bit_buf;   // bits pending, MSB-first in low bits at top of next read
    int              bits_in;   // count of bits in bit_buf

    HuffmanBitReader(const std::byte* d, std::size_t n, std::size_t start_byte)
        : data(d), data_size(n), byte_pos(start_byte), bit_buf(0), bits_in(0)
    {}

    // Read one bit, MSB-first. Returns -1 at EOF.
    int read_bit()
    {
        if (bits_in == 0) {
            if (byte_pos >= data_size) return -1;
            bit_buf = static_cast<std::uint8_t>(data[byte_pos++]);
            bits_in = 8;
        }
        --bits_in;
        return (bit_buf >> bits_in) & 1;
    }
};

// Decode `pixel_count` 16-bit IRGB colours starting at `start_byte`
// of `map_data`, using the file's Huffman tree (root = last node).
// Returns the decoded pixels (size pixel_count) or empty on error.
std::vector<std::uint16_t> decode_huffman_pixels(
    const studio::content::interior::dil_file& dil,
    std::size_t start_byte,
    std::size_t pixel_count)
{
    std::vector<std::uint16_t> out;
    if (pixel_count == 0) return out;

    // Trivial empty-tree case shouldn't happen (parser enforces L=N+1)
    // but guard anyway.
    if (dil.huffman_leaves.empty()) return out;

    const auto& nodes  = dil.huffman_nodes;
    const auto& leaves = dil.huffman_leaves;

    // Root is the last (highest-indexed) internal node. When N==0 the
    // tree is just a single leaf — emit it `pixel_count` times.
    if (nodes.empty()) {
        out.assign(pixel_count, leaves.front().colour);
        return out;
    }

    HuffmanBitReader br(dil.map_data.data(), dil.map_data.size(), start_byte);
    out.reserve(pixel_count);

    const std::int32_t root_idx = static_cast<std::int32_t>(nodes.size() - 1);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        std::int32_t cur = root_idx;
        // Walk until we hit a leaf.
        while (true) {
            int bit = br.read_bit();
            if (bit < 0) return {}; // EOF mid-decode -> abort
            if (cur < 0 || cur >= static_cast<std::int32_t>(nodes.size())) {
                return {}; // malformed tree
            }
            std::int32_t child = (bit == 0)
                ? nodes[cur].index_zero
                : nodes[cur].index_one;
            if (child >= 0) {
                cur = child;
                continue;
            }
            // Leaf: index = ~child = -1 - child.
            std::size_t li = static_cast<std::size_t>(~child);
            if (li >= leaves.size()) return {};
            out.push_back(leaves[li].colour);
            break;
        }
    }
    return out;
}

// Convert a 16-bit IRGB 4:4:4:4 pixel into RGBA8. `light_scale_shift`
// scales intensity: scale = 2^shift / 15.0 (clamped to 1.0 to avoid
// overflow blowouts; shipping content has shift=4 -> 16/15 ≈ 1.067).
inline void irgb4444_to_rgba8(std::uint16_t px,
                              int light_scale_shift,
                              std::uint8_t out[4])
{
    const std::uint8_t mono = static_cast<std::uint8_t>((px >> 12) & 0xF);
    const std::uint8_t r4   = static_cast<std::uint8_t>((px >> 8)  & 0xF);
    const std::uint8_t g4   = static_cast<std::uint8_t>((px >> 4)  & 0xF);
    const std::uint8_t b4   = static_cast<std::uint8_t>( px        & 0xF);

    // 4-bit -> 8-bit: replicate the nibble so 0xF maps to 0xFF.
    auto expand = [](std::uint8_t n) -> std::uint8_t {
        return static_cast<std::uint8_t>((n << 4) | n);
    };

    float r = expand(r4) / 255.0f;
    float g = expand(g4) / 255.0f;
    float b = expand(b4) / 255.0f;
    const float i = expand(mono) / 255.0f;

    // Apply intensity-shift scale. Cap at 1.0 because the shader multiplier
    // (*2.0) already compensates for 4-bit precision; over-bright source
    // pixels would saturate to white otherwise.
    const float scale = (1u << light_scale_shift) / 15.0f;
    const float k = std::min(i * scale, 1.0f);
    r = std::min(r * k, 1.0f);
    g = std::min(g * k, 1.0f);
    b = std::min(b * k, 1.0f);

    out[0] = static_cast<std::uint8_t>(r * 255.0f + 0.5f);
    out[1] = static_cast<std::uint8_t>(g * 255.0f + 0.5f);
    out[2] = static_cast<std::uint8_t>(b * 255.0f + 0.5f);
    out[3] = 255;
}

// Unpack every surface's lightmap rectangle into an UnpackedTile.
// Surfaces with size 0 in either dimension are skipped (no usable
// rectangle to sample).
std::vector<UnpackedTile> unpack_dil_surfaces(
    const studio::content::interior::dil_file& dil)
{
    std::vector<UnpackedTile> tiles;
    tiles.reserve(dil.surfaces.size());

    const int shift = std::clamp(dil.light_scale_shift, 0, 7);

    std::size_t fail_count = 0;
    for (std::size_t si = 0; si < dil.surfaces.size(); ++si) {
        const auto& s = dil.surfaces[si];
        const int w = s.map_size_x;
        const int h = s.map_size_y;
        if (w <= 0 || h <= 0) continue;

        UnpackedTile t;
        t.surface_index = static_cast<std::uint32_t>(si);
        t.w = w; t.h = h;
        t.rgba.assign(static_cast<std::size_t>(w * h) * 4, 0);

        const std::uint32_t enc = s.map_index_or_color;
        const std::size_t pixel_count = static_cast<std::size_t>(w) * h;

        if ((enc & 0x40000000u) != 0) {
            // Huffman-encoded: decode from byte offset.
            const std::size_t off = enc & 0x3FFFFFFFu;
            auto pixels = decode_huffman_pixels(dil, off, pixel_count);
            if (pixels.size() != pixel_count) {
                ++fail_count;
                // Fall through: leave the tile zero-filled (will appear black).
            } else {
                for (std::size_t p = 0; p < pixel_count; ++p) {
                    irgb4444_to_rgba8(pixels[p], shift,
                                      &t.rgba[p * 4]);
                }
            }
        } else {
            // Unlit flat-colour surface: low 16 bits ARE the IRGB pixel.
            const std::uint16_t px = static_cast<std::uint16_t>(enc & 0xFFFFu);
            std::uint8_t rgba[4];
            irgb4444_to_rgba8(px, shift, rgba);
            for (std::size_t p = 0; p < pixel_count; ++p) {
                t.rgba[p * 4 + 0] = rgba[0];
                t.rgba[p * 4 + 1] = rgba[1];
                t.rgba[p * 4 + 2] = rgba[2];
                t.rgba[p * 4 + 3] = rgba[3];
            }
        }

        tiles.push_back(std::move(t));
    }

    // Sanity stat: mean luminance of all unpacked tiles, logged so the
    // operator can verify the lightmap is plausible (acceptance criterion
    // 06-06 #3). Sane range is roughly 0.05..0.9 of full white; 0 means
    // the entire DIL is the static "lights-off" frame (e.g. animated-only
    // interiors before the state machine runs).
    double sum = 0.0;
    std::size_t total_px = 0;
    for (const auto& t : tiles) {
        const std::size_t n = static_cast<std::size_t>(t.w) * t.h;
        for (std::size_t p = 0; p < n; ++p) {
            sum += t.rgba[p * 4 + 0] + t.rgba[p * 4 + 1] + t.rgba[p * 4 + 2];
        }
        total_px += n;
    }
    const double mean = total_px > 0
        ? sum / (3.0 * 255.0 * static_cast<double>(total_px))
        : 0.0;
    std::fprintf(stderr,
        "interior: lightmap unpacked — %zu surfaces, %zu pixels, "
        "mean luminance %.3f%s\n",
        tiles.size(), total_px, mean,
        fail_count > 0 ? " (some surfaces fell back to black on Huffman EOF)" : "");

    return tiles;
}

// Shelf-pack tiles into an atlas, return (atlas_w, atlas_h). Tiles are
// sorted by descending height; one-texel padding between every tile and
// around the edges. Each tile's atlas_x / atlas_y are filled in.
std::pair<int, int> pack_atlas_shelves(std::vector<UnpackedTile>& tiles)
{
    if (tiles.empty()) return {0, 0};

    // Sort indices by descending height (then width). Stable sort to keep
    // surface order broadly intact for debuggability.
    std::vector<std::size_t> order(tiles.size());
    for (std::size_t i = 0; i < tiles.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&](std::size_t a, std::size_t b) {
            if (tiles[a].h != tiles[b].h) return tiles[a].h > tiles[b].h;
            return tiles[a].w > tiles[b].w;
        });

    // Pick an atlas width that comfortably fits the widest tile plus
    // padding; round up to a power of two for sane GL behaviour. Start
    // at 256 and grow if needed.
    int min_w = 1;
    for (const auto& t : tiles) min_w = std::max(min_w, t.w + 2);
    int atlas_w = 256;
    while (atlas_w < min_w) atlas_w *= 2;

    int cursor_x = 1;            // 1-texel left margin
    int cursor_y = 1;            // 1-texel top margin
    int row_h    = 0;
    int max_y    = 0;

    for (std::size_t idx : order) {
        UnpackedTile& t = tiles[idx];
        // Need t.w + 1 to the right (padding) and t.h + 1 below.
        if (cursor_x + t.w + 1 > atlas_w) {
            // Advance to next shelf.
            cursor_y += row_h + 1;
            cursor_x = 1;
            row_h    = 0;
        }
        t.atlas_x = cursor_x;
        t.atlas_y = cursor_y;
        cursor_x += t.w + 1;
        if (t.h > row_h) row_h = t.h;
        if (cursor_y + t.h > max_y) max_y = cursor_y + t.h;
    }

    // Final atlas height = max_y + 1 (bottom padding), rounded up to a
    // multiple of 4 for stride sanity.
    int atlas_h = max_y + 1;
    if (atlas_h < 4) atlas_h = 4;
    atlas_h = (atlas_h + 3) & ~3;

    return {atlas_w, atlas_h};
}

// Compose all unpacked tiles into a single RGBA8 atlas buffer.
std::vector<std::uint8_t> compose_atlas(
    int atlas_w, int atlas_h,
    const std::vector<UnpackedTile>& tiles)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(atlas_w) * atlas_h * 4, 0);
    for (const auto& t : tiles) {
        if (t.atlas_x < 0 || t.atlas_y < 0) continue;
        for (int y = 0; y < t.h; ++y) {
            const int dst_y = t.atlas_y + y;
            if (dst_y < 0 || dst_y >= atlas_h) continue;
            const std::size_t dst_off =
                (static_cast<std::size_t>(dst_y) * atlas_w + t.atlas_x) * 4;
            const std::size_t src_off =
                static_cast<std::size_t>(y) * t.w * 4;
            const std::size_t row_bytes =
                static_cast<std::size_t>(t.w) * 4;
            std::memcpy(&out[dst_off], &t.rgba[src_off], row_bytes);
        }
    }
    return out;
}

} // anonymous namespace

// ---- main build function -----------------------------------------------------

InteriorMesh build_interior_mesh(
    const studio::content::dig::dig_file& geom,
    const studio::content::dts::darkstar::material_list_variant& dml,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map,
    const studio::content::interior::dil_file* dil)
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

    // ---- Lightmap atlas (spec 06-06) ---------------------------------------
    //
    // Unpack the DIL's per-surface IRGB rectangles into RGBA8 tiles, shelf-
    // pack them into one atlas, and remember the per-surface (atlas_x,
    // atlas_y, w, h) so the vertex loop can compute the lightmap UV
    // alongside the diffuse UV.
    int   atlas_w = 0;
    int   atlas_h = 0;
    std::vector<std::uint8_t> atlas_rgba;
    // Per DIG surface index, the atlas rect of its lightmap (or -1 if
    // the surface has no lightmap entry / was dropped).
    struct AtlasRect { int x = -1, y = -1, w = 0, h = 0; };
    std::vector<AtlasRect> surf_to_atlas(geom.surfaces.size());

    if (dil != nullptr && !dil->surfaces.empty()) {
        auto tiles = unpack_dil_surfaces(*dil);
        auto [aw, ah] = pack_atlas_shelves(tiles);
        atlas_w = aw;
        atlas_h = ah;
        if (atlas_w > 0 && atlas_h > 0) {
            atlas_rgba = compose_atlas(atlas_w, atlas_h, tiles);
            // Map back tile -> DIG surface index. DIL.surfaces is parallel
            // to DIG.surfaces in shipping content (one entry per polygon).
            for (const auto& t : tiles) {
                if (t.surface_index < surf_to_atlas.size()) {
                    surf_to_atlas[t.surface_index] =
                        AtlasRect{ t.atlas_x, t.atlas_y, t.w, t.h };
                }
            }
        }
    }

    // Per-material vertex + index accumulators.
    // Vertex layout: pos.xyz, uv_d.uv, uv_lm.uv (7 floats, 28 bytes).
    constexpr int kFloatsPerVertex = 7;
    struct MatBuf {
        std::vector<float>          verts;   // kFloatsPerVertex per vertex
        std::vector<std::uint32_t>  indices; // u32 triangle list
        std::uint32_t               base = 0; // vertex base offset when merged
    };
    std::vector<MatBuf> per_mat(n_mats);

    // Triangulate surfaces, grouped by material index.
    for (std::size_t si = 0; si < geom.surfaces.size(); ++si) {
        const auto& surf = geom.surfaces[si];
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
            buf.verts.size() / kFloatsPerVertex);

        // Per-surface lightmap UV derivation. We compute the lightmap UV
        // by mapping the diffuse UV's fractional position within the
        // surface's texture rect into the surface's lightmap rect, then
        // offsetting by the atlas position.
        //
        // Working hypothesis (per spec note 06-06):
        //   frac      = (points2[ti] - surface.texture_offset) / surface.texture_size
        //   tile_uv   = clamp(frac, 0, 1) * mapSize
        //   atlas_uv  = (atlas_origin + tile_uv + 0.5) / atlas_size
        //
        // For surfaces with no atlas entry, emit (0, 0) — the fragment
        // shader will detect a zero-radius atlas and skip the lightmap
        // sample.
        const AtlasRect ar = (si < surf_to_atlas.size())
            ? surf_to_atlas[si] : AtlasRect{};
        const bool has_lm = (ar.x >= 0 && ar.w > 0 && ar.h > 0
                             && atlas_w > 0 && atlas_h > 0);

        const float tsx = surf.texture_size_x > 0
            ? static_cast<float>(surf.texture_size_x) : 1.0f;
        const float tsy = surf.texture_size_y > 0
            ? static_cast<float>(surf.texture_size_y) : 1.0f;
        const float tox = static_cast<float>(surf.texture_offset_x);
        const float toy = static_cast<float>(surf.texture_offset_y);

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

            // Lightmap UV.
            float lu = 0.0f, lv = 0.0f;
            if (has_lm) {
                // (u, v) here are the surface-local texture coordinates
                // in the same integer-ish space as texture_offset/size
                // (points2 is the float UV pool the diffuse texture is
                // sampled with). The fractional position within the
                // surface footprint is:
                const float frac_u = (u - tox) / tsx;
                const float frac_v = (v - toy) / tsy;
                // Wrap into [0,1] to handle UVs outside the declared
                // rectangle (e.g. tiled diffuse textures).
                auto wrap01 = [](float x) {
                    x = x - std::floor(x);
                    if (x < 0.0f) x += 1.0f;
                    return x;
                };
                const float fu = wrap01(frac_u);
                const float fv = wrap01(frac_v);

                // Convert to atlas-pixel coords with half-texel offset.
                const float ax = static_cast<float>(ar.x) + fu * ar.w + 0.5f;
                const float ay = static_cast<float>(ar.y) + fv * ar.h + 0.5f;
                lu = ax / static_cast<float>(atlas_w);
                lv = ay / static_cast<float>(atlas_h);
            }

            buf.verts.push_back(px);
            buf.verts.push_back(py);
            buf.verts.push_back(pz);
            buf.verts.push_back(u);
            buf.verts.push_back(v);
            buf.verts.push_back(lu);
            buf.verts.push_back(lv);

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
        total_verts   += static_cast<std::uint32_t>(buf.verts.size() / kFloatsPerVertex);
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
    merged_verts.reserve(static_cast<std::size_t>(total_verts) * kFloatsPerVertex);
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

    const GLsizei stride =
        static_cast<GLsizei>(kFloatsPerVertex * sizeof(float));

    // layout(location = 0) = position (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          stride, reinterpret_cast<void*>(0));

    // layout(location = 1) = normal (vec3) — interiors have no per-vertex
    // normals; the new two-sampler shader doesn't use them, but the legacy
    // single-sampler DTS shader (FS_SRC) does. Keep a small stub VBO of
    // (0,0,1) so the VAO is complete if the caller reuses the DTS shader.
    GLuint vbo_nor = 0;
    glGenBuffers(1, &vbo_nor);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_nor);
    {
        const std::size_t vcount = merged_verts.size() / kFloatsPerVertex;
        std::vector<float> norms(vcount * 3, 0.0f);
        for (std::size_t i = 2; i < norms.size(); i += 3) norms[i] = 1.0f;
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(norms.size() * sizeof(float)),
                     norms.data(), GL_STATIC_DRAW);
    }
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Re-bind the interleaved VBO for the UV attributes.
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);

    // layout(location = 2) = diffuse UV (vec2), offset = 3 floats.
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE,
                          stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));

    // layout(location = 3) = lightmap UV (vec2), offset = 5 floats.
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE,
                          stride,
                          reinterpret_cast<void*>(5 * sizeof(float)));

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

    // Upload the lightmap atlas as a GL_RGBA8 texture, linear-filtered,
    // clamped to edge (so the 1-texel padding round each tile keeps
    // bilinear taps inside their own rect). No mipmaps — atlas sampling
    // crosses tile boundaries at lower mip levels.
    if (!atlas_rgba.empty() && atlas_w > 0 && atlas_h > 0) {
        glGenTextures(1, &mesh.lightmap_atlas);
        glBindTexture(GL_TEXTURE_2D, mesh.lightmap_atlas);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     static_cast<GLsizei>(atlas_w),
                     static_cast<GLsizei>(atlas_h),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, atlas_rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        mesh.lightmap_atlas_w = atlas_w;
        mesh.lightmap_atlas_h = atlas_h;
    }

    std::size_t ranges_with_tex = 0;
    for (const auto& r : mesh.ranges) if (r.texture != 0) ++ranges_with_tex;
    std::fprintf(stderr,
        "interior: %u verts, %u indices, %zu draw ranges (%zu textured), "
        "lightmap atlas %dx%d (%s)\n",
        total_verts, total_indices,
        mesh.ranges.size(), ranges_with_tex,
        atlas_w, atlas_h,
        mesh.lightmap_atlas != 0 ? "ok" : "absent");

    return mesh;
}

// ---- draw --------------------------------------------------------------------

void draw_interior(
    const InteriorMesh& mesh,
    GLint u_mvp_loc,
    GLint u_has_texture_loc,
    GLint u_tex0_loc,
    GLint u_lightmap_loc,
    GLint u_has_lightmap_loc,
    const glm::mat4& mvp)
{
    if (!mesh.valid()) return;

    glUniformMatrix4fv(u_mvp_loc, 1, GL_FALSE, glm::value_ptr(mvp));
    if (u_tex0_loc     >= 0) glUniform1i(u_tex0_loc, 0);
    if (u_lightmap_loc >= 0) glUniform1i(u_lightmap_loc, 1);

    // Bind the per-interior lightmap atlas once (unit 1, shared across
    // every draw range).
    if (mesh.lightmap_atlas != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mesh.lightmap_atlas);
        if (u_has_lightmap_loc >= 0) glUniform1i(u_has_lightmap_loc, 1);
    } else {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (u_has_lightmap_loc >= 0) glUniform1i(u_has_lightmap_loc, 0);
    }

    // Diffuse texture changes per range -> activate unit 0 last so the
    // subsequent glBindTexture calls inside the loop hit unit 0.
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
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace dts_viewer
