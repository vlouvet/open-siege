#include "lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <variant>

namespace dts_viewer
{

namespace
{

bool finite_color(const std::array<float, 3>& c)
{
    return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
}

float color_magnitude(const std::array<float, 3>& c)
{
    float r = std::isfinite(c[0]) ? std::max(0.0f, c[0]) : 0.0f;
    float g = std::isfinite(c[1]) ? std::max(0.0f, c[1]) : 0.0f;
    float b = std::isfinite(c[2]) ? std::max(0.0f, c[2]) : 0.0f;
    return r + g + b;
}

void walk(
    const studio::content::mission::scene_node& n,
    std::vector<const studio::content::mission::node_sim_light*>& sim_lights,
    std::vector<std::pair<const studio::content::mission::node_planet*, std::string>>& planets,
    const std::string& parent_name)
{
    using namespace studio::content::mission;
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, node_sim_light>) {
            sim_lights.push_back(&p);
        } else if constexpr (std::is_same_v<T, node_planet>) {
            planets.emplace_back(&p, n.instance_name.value_or(parent_name));
        }
    }, n.payload);

    for (const auto& c : n.children) {
        walk(c, sim_lights, planets, n.instance_name.value_or(parent_name));
    }
}

} // anonymous namespace

SceneLighting build_scene_lighting(
    const studio::content::mission::scene_graph& scene)
{
    using namespace studio::content::mission;

    SceneLighting out;
    if (scene.sky && finite_color(scene.sky->ambient_color)) {
        out.ambient_color = scene.sky->ambient_color;
        for (auto& c : out.ambient_color) c = std::clamp(c, 0.0f, 1.0f);
    }

    std::vector<const node_sim_light*> sim_lights;
    std::vector<std::pair<const node_planet*, std::string>> planets;
    walk(scene.root, sim_lights, planets, "");

    // Pick the brightest planet (or the one named "Sun") as the sun.
    const node_planet* sun_planet = nullptr;
    std::string sun_name;
    float best_mag = -1.0f;
    for (auto& [pl, name] : planets) {
        if (name == "Sun") { sun_planet = pl; sun_name = name; break; }
        float m = 1.0f; // planets don't carry a colour in the typed node — use 1
        if (pl->radius > 0.0f) m = pl->radius;
        if (m > best_mag) { best_mag = m; sun_planet = pl; sun_name = name; }
    }

    if (sun_planet) {
        // Use the planet's position vector as the direction towards the sun
        // (Tribes treats the planet as billboarded at infinity in that
        // direction).  Normalise; default to overhead if the magnitude is
        // ~zero or non-finite.
        std::array<float, 3> d = sun_planet->xf.position;
        float len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (std::isfinite(len) && len > 0.001f) {
            out.sun.direction = { d[0] / len, d[1] / len, d[2] / len };
        } else {
            out.sun.direction = { 0.0f, 1.0f, 0.0f };
        }
        out.sun.intensity = 1.0f;
    } else {
        out.sun.direction = { 0.3f, 0.85f, 0.4f };
        out.sun.intensity = 1.0f;
    }

    // SimLight type 2 → point lights.  Cap at 8.
    int dropped = 0;
    for (auto* sl : sim_lights) {
        if (sl->type != 2) continue;
        if (!finite_color(sl->color)) { ++dropped; continue; }
        if (out.point_lights.size() >= 8) { ++dropped; continue; }
        PointLight p;
        p.position = sl->xf.position;
        p.color    = sl->color;
        for (auto& c : p.color) c = std::clamp(c, 0.0f, 1.0f);
        p.radius   = std::max(1.0f, sl->radius);
        out.point_lights.push_back(p);
    }
    if (dropped > 0) {
        std::fprintf(stderr, "lighting: dropped %d SimLight entries (cap=8)\n", dropped);
    }

    return out;
}

void apply_scene_lighting(GLuint program, const SceneLighting& L)
{
    GLint loc;

    loc = glGetUniformLocation(program, "u_ambient_color");
    if (loc >= 0) glUniform3fv(loc, 1, L.ambient_color.data());

    loc = glGetUniformLocation(program, "u_sun_dir");
    if (loc >= 0) glUniform3fv(loc, 1, L.sun.direction.data());

    std::array<float, 3> sc = L.sun.color;
    for (auto& v : sc) v *= L.sun.intensity;
    loc = glGetUniformLocation(program, "u_sun_color");
    if (loc >= 0) glUniform3fv(loc, 1, sc.data());

    const int n = static_cast<int>(L.point_lights.size());
    loc = glGetUniformLocation(program, "u_point_count");
    if (loc >= 0) glUniform1i(loc, n);

    float positions[3 * 8] = {0};
    float colors   [3 * 8] = {0};
    float radii    [    8] = {0};
    for (int i = 0; i < n && i < 8; ++i) {
        positions[i*3+0] = L.point_lights[i].position[0];
        positions[i*3+1] = L.point_lights[i].position[1];
        positions[i*3+2] = L.point_lights[i].position[2];
        colors   [i*3+0] = L.point_lights[i].color[0];
        colors   [i*3+1] = L.point_lights[i].color[1];
        colors   [i*3+2] = L.point_lights[i].color[2];
        radii    [i]     = L.point_lights[i].radius;
    }
    loc = glGetUniformLocation(program, "u_point_pos");
    if (loc >= 0) glUniform3fv(loc, 8, positions);
    loc = glGetUniformLocation(program, "u_point_color");
    if (loc >= 0) glUniform3fv(loc, 8, colors);
    loc = glGetUniformLocation(program, "u_point_radius");
    if (loc >= 0) glUniform1fv(loc, 8, radii);
}

SceneLighting masked_lighting(const SceneLighting& base, LightingMode mode)
{
    SceneLighting m = base;
    if (mode == LightingMode::AmbientOnly) {
        m.sun.intensity = 0.0f;
        m.point_lights.clear();
    } else if (mode == LightingMode::SunOnly) {
        m.point_lights.clear();
    }
    return m;
}

} // namespace dts_viewer
