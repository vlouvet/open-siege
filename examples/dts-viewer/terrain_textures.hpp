#ifndef DTS_VIEWER_TERRAIN_TEXTURES_HPP
#define DTS_VIEWER_TERRAIN_TEXTURES_HPP

// Terrain texture splatting — Spec 06 (05-terrain track).
//
// Loads a world's <world>.Terrain.dat from a mounted VOL, then resolves each
// per-texture BMP via the MaterialResolver and uploads the lot into a single
// GL_TEXTURE_2D_ARRAY.  The terrain fragment shader samples the array using
// the per-vertex material index; v1 has no per-corner blending — every
// quad is flat-textured by the dominant material at its corners.

#define GL_SILENCE_DEPRECATION
#include "gl_includes.hpp"

#include <map>
#include <string>
#include <vector>

#include "content/terrain/world_dats.hpp"

#include "materials.hpp"
#include "ppl.hpp"

namespace dts_viewer
{

struct TerrainTextureArray
{
    GLuint texture = 0;          // GL_TEXTURE_2D_ARRAY
    int    layer_count = 0;
    int    layer_size  = 0;      // square; resolves to first BMP's width
    std::vector<std::string> layer_names;

    bool valid() const { return texture != 0 && layer_count > 0; }
};

// Build the texture array from the given world prefix (e.g. "lush").  Looks
// up "<prefix>.Terrain.dat" via the resolver, parses it, then resolves each
// listed BMP and uploads it as a layer.  All BMPs are square-resampled to
// `target_size` (default 64) to fit the texture-array's same-size constraint.
TerrainTextureArray build_terrain_textures(
    const std::string& world_prefix,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map,
    int target_size = 64);

void release_terrain_textures(TerrainTextureArray& arr);

} // namespace dts_viewer

#endif // DTS_VIEWER_TERRAIN_TEXTURES_HPP
