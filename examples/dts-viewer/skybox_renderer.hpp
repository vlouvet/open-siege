#ifndef DTS_VIEWER_SKYBOX_RENDERER_HPP
#define DTS_VIEWER_SKYBOX_RENDERER_HPP

// Skybox renderer — Spec 04 (07-mission track).
//
// Builds a GL_TEXTURE_CUBE_MAP from the per-world sky DML referenced by
// `node_sky::dml_name` and the 16-slot textures[] array.  Renders an
// inward-facing cube at "infinity" (view-translation stripped) before
// any other geometry.  v1 just uses textures[0..5] as the six faces in
// (+X, -X, +Y, -Y, +Z, -Z) order; weather variants and StarField layers
// fall back to printing an info line.

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <glm/glm.hpp>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "content/mission/scene.hpp"

#include "materials.hpp"
#include "ppl.hpp"

namespace dts_viewer
{

struct SkyboxResources
{
    GLuint cubemap_texture = 0;
    GLuint vao             = 0;
    GLuint vbo             = 0;
    GLuint program         = 0;
    GLint  u_view          = -1;
    GLint  u_proj          = -1;
    std::array<float, 3> ambient_color { 0.2f, 0.2f, 0.2f };
    bool has_star_field = false;
    std::array<std::array<float, 3>, 3> star_field_colors {};

    bool valid() const { return cubemap_texture != 0 && program != 0; }
};

// Build a cubemap-backed sky from the parsed scene.
//   resolver       — already populated with mounted world VOLs (for the .dml + .bmp lookups)
//   palette_map    — populated from the world's .day.ppl
// Returns nullopt on hard failure (e.g. DML not found).
std::optional<SkyboxResources> build_skybox(
    const studio::content::mission::node_sky& sky,
    const studio::content::mission::node_star_field* star_field_or_null,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map);

// Render the skybox.  Caller supplies the view+proj of the active camera;
// this strips view's translation internally so the cube appears at infinity.
void draw_skybox(
    const SkyboxResources& sky,
    const glm::mat4& view,
    const glm::mat4& projection);

// Tear down GL resources.  Safe to call on an empty/default-constructed value.
void release_skybox(SkyboxResources& sky);

} // namespace dts_viewer

#endif // DTS_VIEWER_SKYBOX_RENDERER_HPP
