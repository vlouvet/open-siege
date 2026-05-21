#include "terrain_textures.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

#include "pbmp.hpp"

namespace dts_viewer
{

namespace
{

std::vector<std::uint8_t> resample_nearest(
    const std::vector<std::uint8_t>& src,
    int src_w, int src_h,
    int dst_w, int dst_h)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(dst_w) * dst_h * 4, 0);
    if (src_w <= 0 || src_h <= 0) return out;
    for (int y = 0; y < dst_h; ++y) {
        int sy = std::min(src_h - 1, (y * src_h) / dst_h);
        for (int x = 0; x < dst_w; ++x) {
            int sx = std::min(src_w - 1, (x * src_w) / dst_w);
            const std::size_t si = (static_cast<std::size_t>(sy) * src_w + sx) * 4;
            const std::size_t di = (static_cast<std::size_t>(y)  * dst_w + x ) * 4;
            out[di + 0] = src[si + 0];
            out[di + 1] = src[si + 1];
            out[di + 2] = src[si + 2];
            out[di + 3] = src[si + 3];
        }
    }
    return out;
}

std::vector<std::uint8_t> rgba_from_pbmp_terrain(
    const PbmpImage& bmp,
    const std::map<std::uint32_t, const Palette*>& palettes)
{
    const std::size_t w = bmp.width;
    const std::size_t h = bmp.height;
    std::vector<std::uint8_t> out(w * h * 4, 0);
    auto it = palettes.find(bmp.palette_index);
    if (it == palettes.end() || it->second == nullptr) {
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t i = (y * w + x) * 4;
                const bool on = ((x >> 3) ^ (y >> 3)) & 1;
                out[i + 0] = on ? 200 : 60;
                out[i + 1] = on ? 200 : 60;
                out[i + 2] = on ? 200 : 60;
                out[i + 3] = 255;
            }
        }
        return out;
    }
    const Palette& pal = *it->second;
    const std::size_t n = std::min<std::size_t>(bmp.indexed_pixels.size(), w * h);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t idx = bmp.indexed_pixels[i];
        const PaletteEntry& e = pal.colours[idx];
        const std::size_t o = i * 4;
        out[o + 0] = e.r; out[o + 1] = e.g; out[o + 2] = e.b; out[o + 3] = 255;
    }
    return out;
}

} // anonymous namespace

TerrainTextureArray build_terrain_textures(
    const std::string& world_prefix,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map,
    int target_size)
{
    TerrainTextureArray out;
    if (world_prefix.empty() || target_size <= 0) return out;

    const std::string dat_name = world_prefix + ".Terrain.dat";
    auto bytes = resolver.resolve(dat_name);
    if (!bytes || bytes->empty()) {
        std::fprintf(stderr, "terrain-tex: cannot resolve %s\n", dat_name.c_str());
        return out;
    }

    std::stringstream ss(std::string(bytes->begin(), bytes->end()));
    auto td_opt = studio::content::terrain::parse_terrain_dat(ss);
    if (!td_opt) {
        std::fprintf(stderr, "terrain-tex: parse_terrain_dat failed for %s\n",
            dat_name.c_str());
        return out;
    }
    const auto& td = *td_opt;
    if (td.records.empty()) return out;

    glGenTextures(1, &out.texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, out.texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                 target_size, target_size,
                 static_cast<GLsizei>(td.records.size()),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

    int loaded = 0;
    for (std::size_t i = 0; i < td.records.size(); ++i) {
        const std::string name = td.records[i].bitmap_name;
        out.layer_names.push_back(name);
        if (name.empty()) continue;

        auto bmp_bytes = resolver.resolve(name);
        if (!bmp_bytes || bmp_bytes->empty()) continue;

        PbmpImage img;
        try {
            std::stringstream is(std::string(bmp_bytes->begin(), bmp_bytes->end()));
            img = load_pbmp(is);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "terrain-tex: PBMP parse %s failed: %s\n",
                name.c_str(), e.what());
            continue;
        }
        if (img.width == 0 || img.height == 0) continue;

        auto rgba = rgba_from_pbmp_terrain(img, palette_map);
        if (static_cast<int>(img.width) != target_size ||
            static_cast<int>(img.height) != target_size) {
            rgba = resample_nearest(rgba,
                static_cast<int>(img.width), static_cast<int>(img.height),
                target_size, target_size);
        }

        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                        0, 0, static_cast<GLint>(i),
                        target_size, target_size, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        ++loaded;
    }

    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    out.layer_count = static_cast<int>(td.records.size());
    out.layer_size  = target_size;
    std::printf("terrain-tex: %d/%d layers loaded from %s (%dx%d)\n",
        loaded, out.layer_count, dat_name.c_str(),
        target_size, target_size);

    return out;
}

void release_terrain_textures(TerrainTextureArray& arr)
{
    if (arr.texture) glDeleteTextures(1, &arr.texture);
    arr = TerrainTextureArray{};
}

} // namespace dts_viewer
