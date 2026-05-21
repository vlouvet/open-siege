#ifndef DTS_VIEWER_LIGHTING_HPP
#define DTS_VIEWER_LIGHTING_HPP

// Mission lighting — Spec 05 (07-mission track).
//
// Walks the scene graph to extract a single directional "sun" (from the
// brightest Planet) plus up to eight PointLight entries (from SimLight
// type == 2).  Pushes the result into a shader's uniforms.

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

#include <array>
#include <cstdint>
#include <vector>

#include "content/mission/scene.hpp"

namespace dts_viewer
{

struct DirectionalLight
{
    std::array<float, 3> direction { 0.0f, 1.0f, 0.0f };   // points TO the sun
    std::array<float, 3> color     { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
};

struct PointLight
{
    std::array<float, 3> position { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> color    { 1.0f, 1.0f, 1.0f };
    float radius   = 50.0f;
};

struct SceneLighting
{
    std::array<float, 3>     ambient_color { 0.20f, 0.20f, 0.20f };
    DirectionalLight         sun {};
    std::vector<PointLight>  point_lights;     // capped at 8
};

// Build from a parsed scene.  Handles NaN / negative input by sanitising.
SceneLighting build_scene_lighting(
    const studio::content::mission::scene_graph& scene);

// Push the lighting block into the supplied program's uniforms.  Caller
// must have already bound the program.
//   uniform vec3  u_ambient_color;
//   uniform vec3  u_sun_dir;
//   uniform vec3  u_sun_color;
//   uniform int   u_point_count;
//   uniform vec3  u_point_pos[8];
//   uniform vec3  u_point_color[8];
//   uniform float u_point_radius[8];
void apply_scene_lighting(GLuint program, const SceneLighting& lighting);

// Debug toggle: cycles ambient_only / +sun / +points.
enum class LightingMode : int
{
    AmbientOnly = 0,
    SunOnly     = 1,
    Full        = 2,
};

// Returns a copy of `base` with disabled contributions zeroed out
// (so the same shader can be used for all three modes).
SceneLighting masked_lighting(const SceneLighting& base, LightingMode mode);

} // namespace dts_viewer

#endif // DTS_VIEWER_LIGHTING_HPP
