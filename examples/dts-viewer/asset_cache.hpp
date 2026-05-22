#ifndef DTS_VIEWER_ASSET_CACHE_HPP
#define DTS_VIEWER_ASSET_CACHE_HPP

// Spec 14/09 — per-datablock DTS prop loading.
//
// Mission mode walks every entity (StaticShape / Item / Turret / Generator /
// Trigger / Moveable / Vehicle / etc.) and resolves its datablock name to a
// DTS shape. The shape's geometry + materials + per-bucket textures are
// loaded once and memoised; subsequent entities sharing the same datablock
// reuse the cached upload.
//
// The renderer is intentionally a subset of the shape-viewer pipeline:
//   * detail-level 0 only
//   * static geometry — no skinning, no animation playback
//   * flat normals computed per face (matches shape-viewer behaviour)
//   * one draw per bucket; texture sampling falls back to flat shading when
//     no PBMP material is bound
//
// Entities whose datablock cannot be mapped to a DTS or whose DTS fails to
// load fall through to the existing wireframe-cube path in entity_renderer.

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "lighting.hpp"
#include "materials.hpp"
#include "ppl.hpp"

namespace dts_viewer
{

struct AssetCacheStats
{
    std::size_t shapes_loaded = 0;
    std::size_t shapes_missed = 0;
    std::size_t textures_uploaded = 0;
    std::size_t triangles_total = 0;
};

struct AssetShape;   // opaque

class AssetCache
{
public:
    AssetCache();
    ~AssetCache();

    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    // Bind data sources. Must be called once before any try_load() call.
    // `palette_map` is borrowed — keep the underlying Palette storage alive
    // for the lifetime of the cache.
    void init(MaterialResolver* resolver,
              std::map<std::uint32_t, const Palette*> palette_map,
              std::vector<std::filesystem::path> mounted_vols);

    bool initialized() const { return initialized_; }

    // Look up a shape by DTS filename ("generator.dts"). Returns nullptr if
    // not resolvable. The first call loads + uploads the geometry; later
    // calls return the cached handle.
    const AssetShape* try_load_by_filename(const std::string& dts_filename_lower);

    // Look up via a Tribes datablock name (e.g. "Generator", "AATurret").
    // Falls back to several common transformations of the name before giving
    // up. Returns null on miss.
    const AssetShape* try_load_for_datablock(const std::string& datablock_name);

    // Draw `shape` at `model`. `view`/`proj` are caller-supplied. Lighting is
    // pushed each call so the same shape can render under different mission
    // lighting (lighting mode toggles via F2).
    void render(const AssetShape& shape,
                const glm::mat4& model,
                const glm::mat4& view,
                const glm::mat4& proj,
                const SceneLighting& lighting);

    // AABB in DTS-local space (Tribes axis convention). Caller is responsible
    // for the MIS->GL axis swap if it wants to draw debug overlays.
    void shape_aabb(const AssetShape& shape,
                    glm::vec3& out_min, glm::vec3& out_max) const;

    const AssetCacheStats& stats() const { return stats_; }

private:
    void ensure_program();
    AssetShape* load_shape_internal(const std::string& dts_filename_lower);
    bool fetch_dts_bytes(const std::string& filename_lower,
                         std::vector<char>& out_bytes) const;

    bool initialized_ = false;
    MaterialResolver* resolver_ = nullptr;
    std::map<std::uint32_t, const Palette*> palette_map_;
    std::vector<std::filesystem::path> mounted_vols_;

    // shape filename (lower, ".dts" included) -> loaded shape
    std::unordered_map<std::string, std::unique_ptr<AssetShape>> shapes_;
    // failures so we don't retry/log every frame
    std::unordered_set<std::string> failed_loads_;
    // datablock name -> mapped filename (mostly heuristic). Empty value
    // means "no DTS for this datablock".
    std::unordered_map<std::string, std::string> datablock_resolution_cache_;
    // texture filename (normalized) -> GL texture, shared across shapes.
    std::unordered_map<std::string, GLuint> texture_cache_;

    GLuint program_ = 0;
    GLint  u_mvp_ = -1;
    GLint  u_normal_mat_ = -1;
    GLint  u_tex0_ = -1;
    GLint  u_has_texture_ = -1;

    AssetCacheStats stats_;
};

} // namespace dts_viewer

#endif // DTS_VIEWER_ASSET_CACHE_HPP
