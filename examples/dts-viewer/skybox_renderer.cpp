#include "skybox_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <variant>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "content/dts/darkstar.hpp"
#include "content/dts/darkstar_structures.hpp"
#include "pbmp.hpp"

namespace dts_viewer
{

namespace
{

const char* SKY_VS = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_view;
uniform mat4 u_proj;
out vec3 v_dir;
void main() {
    v_dir = a_pos;
    vec4 p = u_proj * mat4(mat3(u_view)) * vec4(a_pos, 1.0);
    // Push depth to the far plane so the sky always loses to scene geometry.
    gl_Position = p.xyww;
}
)";

const char* SKY_FS = R"(
#version 410 core
in vec3 v_dir;
uniform samplerCube u_cube;
out vec4 frag;
void main() {
    frag = texture(u_cube, normalize(v_dir));
}
)";

GLuint compile(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log), &n, log);
        std::fprintf(stderr, "skybox shader compile error: %.*s\n", n, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint link(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        std::fprintf(stderr, "skybox program link error: %.*s\n", n, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

std::string dml_file_name(const std::array<char, 32>& arr)
{
    return std::string(arr.data(), strnlen(arr.data(), arr.size()));
}

std::vector<std::string> material_filenames_from_dml_bytes(
    const std::vector<char>& bytes)
{
    std::vector<std::string> out;
    if (bytes.empty()) return out;
    std::stringstream ss(std::string(bytes.begin(), bytes.end()));
    using namespace studio::content::dts::darkstar;
    auto v = read_shape(ss);
    if (auto* ml = std::get_if<material_list_variant>(&v)) {
        std::visit([&](const auto& list) {
            out.reserve(list.materials.size());
            for (const auto& m : list.materials) {
                out.push_back(dml_file_name(m.file_name));
            }
        }, *ml);
    }
    return out;
}

std::vector<std::uint8_t> rgba_from_pbmp(
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
                out[i + 0] = 80;  out[i + 1] = 110; out[i + 2] = 200; out[i + 3] = 255;
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

// Vertices for an inward-facing unit cube (each face's winding is irrelevant
// because we disable culling for the sky pass — but the positions also act
// as the cubemap sample direction).
const float CUBE_VERTS[] = {
    // +X
     1, -1, -1,   1, -1,  1,   1,  1,  1,
     1, -1, -1,   1,  1,  1,   1,  1, -1,
    // -X
    -1, -1,  1,  -1, -1, -1,  -1,  1, -1,
    -1, -1,  1,  -1,  1, -1,  -1,  1,  1,
    // +Y
    -1,  1, -1,   1,  1, -1,   1,  1,  1,
    -1,  1, -1,   1,  1,  1,  -1,  1,  1,
    // -Y
    -1, -1,  1,   1, -1,  1,   1, -1, -1,
    -1, -1,  1,   1, -1, -1,  -1, -1, -1,
    // +Z
     1, -1,  1,  -1, -1,  1,  -1,  1,  1,
     1, -1,  1,  -1,  1,  1,   1,  1,  1,
    // -Z
    -1, -1, -1,   1, -1, -1,   1,  1, -1,
    -1, -1, -1,   1,  1, -1,  -1,  1, -1,
};

} // anonymous namespace

std::optional<SkyboxResources> build_skybox(
    const studio::content::mission::node_sky& sky,
    const studio::content::mission::node_star_field* star_field_or_null,
    const MaterialResolver& resolver,
    const std::map<std::uint32_t, const Palette*>& palette_map)
{
    if (sky.dml_name.empty()) {
        std::fprintf(stderr, "skybox: node_sky has empty dml_name\n");
        return std::nullopt;
    }

    auto dml_bytes = resolver.resolve(sky.dml_name);
    if (!dml_bytes || dml_bytes->empty()) {
        std::fprintf(stderr, "skybox: DML '%s' not found in any mounted VOL\n",
                     sky.dml_name.c_str());
        return std::nullopt;
    }

    std::vector<std::string> mat_names;
    try {
        mat_names = material_filenames_from_dml_bytes(*dml_bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "skybox: DML parse failed: %s\n", e.what());
        return std::nullopt;
    }
    if (mat_names.empty()) {
        std::fprintf(stderr, "skybox: DML '%s' has no materials\n",
                     sky.dml_name.c_str());
        return std::nullopt;
    }

    // Map each cube face to a material name via sky.textures[].  v1: faces
    // 0..5 map to textures[0..5] directly.  Stay-on-track if any face is
    // missing — fall back to a fixed colour later.
    constexpr int kFaces = 6;

    GLuint cube = 0;
    glGenTextures(1, &cube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // Spec 07/07 — Tribes sky is a horizon TUBE, not a cube. The 6+
    // referenced DML textures are vertical strips (sky-on-top,
    // ground-on-bottom, horizon-line through the middle) meant to
    // wrap around the player at the horizon, with sky/haze colour
    // filling the dome above and below. Naively mapping strip 2/3
    // to GL +Y/-Y leaves a horizon strip cutting diagonally across
    // the upward view. The right fix is a cylinder renderer; the v1
    // fix is to synthesise +Y and -Y from skyColor/hazeColor with a
    // sensible default when the MIS gave us all zeroes (Citadels).
    int uploaded = 0;
    int cube_size = 0;
    // Per-pixel sums from the strip's top + bottom rows so the synth
    // top / bottom cube faces can match the lateral texture's edges
    // and the dome-to-side seam fades.
    std::array<float, 3> top_avg{0, 0, 0};
    std::array<float, 3> bot_avg{0, 0, 0};
    int top_avg_samples = 0;
    int bot_avg_samples = 0;
    // Spec 07/07: use the SAME strip texture (textures[0]) on all 4
    // lateral cube faces. Tribes' real renderer wraps 16 strips into a
    // tube; mapping different strips to different cube faces produces
    // seams where neighbouring strips don't tile. Using one strip on
    // all sides removes the seams at the cost of a repeating sky —
    // better than a glitchy panorama for v1. +Y / -Y are synthesised
    // below from skyColor / hazeColor.
    for (int face = 0; face < kFaces; ++face) {
        if (face == 2 || face == 3) continue;     // +Y / -Y handled below
        int mat_idx = sky.textures[0];            // single shared strip
        if (mat_idx < 0 || mat_idx >= static_cast<int>(mat_names.size())) {
            continue;
        }
        const std::string& mat_name = mat_names[mat_idx];
        if (mat_name.empty()) continue;

        auto pbmp_bytes = resolver.resolve(mat_name);
        if (!pbmp_bytes || pbmp_bytes->empty()) continue;

        PbmpImage img;
        try {
            std::stringstream ss(std::string(pbmp_bytes->begin(), pbmp_bytes->end()));
            img = load_pbmp(ss);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "skybox: PBMP parse failed for face %d ('%s'): %s\n",
                face, mat_name.c_str(), e.what());
            continue;
        }
        if (img.width == 0 || img.height == 0) continue;

        if (cube_size == 0) cube_size = static_cast<int>(img.width);

        // GL requires all faces to be square + same size.  If a face mismatches,
        // skip it; v1 ships with a partial cubemap rather than corrupt all faces.
        if (static_cast<int>(img.width) != cube_size ||
            static_cast<int>(img.height) != cube_size) {
            std::fprintf(stderr,
                "skybox: face %d size %ux%u differs from cube size %d — skipped\n",
                face, img.width, img.height, cube_size);
            continue;
        }

        auto rgba = rgba_from_pbmp(img, palette_map);

        // Accumulate top/bottom row averages so the synth +Y/-Y faces
        // can match the lateral texture's vertical edges.
        if (top_avg_samples == 0 && img.width > 0 && img.height > 0) {
            const int rows = std::min<int>(8, static_cast<int>(img.height));
            for (int y = 0; y < rows; ++y) {
                for (int x = 0; x < static_cast<int>(img.width); ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
                    top_avg[0] += rgba[i + 0];
                    top_avg[1] += rgba[i + 1];
                    top_avg[2] += rgba[i + 2];
                    ++top_avg_samples;
                }
            }
            for (int y = static_cast<int>(img.height) - rows; y < static_cast<int>(img.height); ++y) {
                for (int x = 0; x < static_cast<int>(img.width); ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
                    bot_avg[0] += rgba[i + 0];
                    bot_avg[1] += rgba[i + 1];
                    bot_avg[2] += rgba[i + 2];
                    ++bot_avg_samples;
                }
            }
        }

        GLenum target = static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face);
        glTexImage2D(target, 0, GL_RGBA8,
            static_cast<GLsizei>(img.width),
            static_cast<GLsizei>(img.height),
            0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        ++uploaded;

        // Spec 07/07 — when DTS_DEBUG_SKYBOX=1 is set, dump each face's
        // decoded RGBA to /tmp/sky_face_<idx>_<name>.ppm so a broken
        // face can be visually identified.
        if (const char* dbg = std::getenv("DTS_DEBUG_SKYBOX"); dbg && *dbg == '1') {
            char path[256];
            std::snprintf(path, sizeof(path),
                "/tmp/sky_face_%d_%s.ppm", face, mat_name.c_str());
            FILE* f = std::fopen(path, "wb");
            if (f) {
                std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
                for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
                    std::fwrite(&rgba[i], 1, 3, f);
                }
                std::fclose(f);
                std::fprintf(stdout,
                    "skybox: dumped face %d ('%s') -> %s\n",
                    face, mat_name.c_str(), path);
            }
        }
    }

    if (uploaded == 0 || cube_size == 0) {
        std::fprintf(stderr, "skybox: no faces uploaded\n");
        glDeleteTextures(1, &cube);
        return std::nullopt;
    }

    // Spec 07/07 — synthesise +Y (top) and -Y (bottom) cube faces from
    // skyColor / hazeColor. When the MIS authored zeroes (Citadels),
    // fall back to a light-blue / tan default so the dome doesn't
    // become a black void.
    auto avg_to_byte = [](const std::array<float, 3>& sum, int n,
                          float def_r, float def_g, float def_b) {
        if (n <= 0) {
            const auto cl = [](float v) {
                return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            };
            return std::array<std::uint8_t, 3>{ cl(def_r), cl(def_g), cl(def_b) };
        }
        return std::array<std::uint8_t, 3>{
            static_cast<std::uint8_t>(std::clamp(sum[0] / n, 0.0f, 255.0f)),
            static_cast<std::uint8_t>(std::clamp(sum[1] / n, 0.0f, 255.0f)),
            static_cast<std::uint8_t>(std::clamp(sum[2] / n, 0.0f, 255.0f)),
        };
    };
    auto pack_color = [](const std::array<float, 3>& c, float def_r, float def_g, float def_b) {
        const bool zero = (c[0] <= 0.0f && c[1] <= 0.0f && c[2] <= 0.0f);
        const float r = zero ? def_r : c[0];
        const float g = zero ? def_g : c[1];
        const float b = zero ? def_b : c[2];
        const auto cl = [](float v) {
            return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
        };
        return std::array<std::uint8_t, 3>{ cl(r), cl(g), cl(b) };
    };
    // Prefer MIS-authored colours; fall back to the lateral strip's
    // top/bottom-row average; ultimate fallback is a hard-coded sky/tan.
    const bool mis_top    = sky.sky_color[0] > 0.0f || sky.sky_color[1] > 0.0f || sky.sky_color[2] > 0.0f;
    const bool mis_bottom = sky.haze_color[0] > 0.0f || sky.haze_color[1] > 0.0f || sky.haze_color[2] > 0.0f;
    const auto top_col    = mis_top
        ? pack_color(sky.sky_color,  0.55f, 0.70f, 0.90f)
        : avg_to_byte(top_avg, top_avg_samples, 0.55f, 0.70f, 0.90f);
    const auto bottom_col = mis_bottom
        ? pack_color(sky.haze_color, 0.60f, 0.55f, 0.45f)
        : avg_to_byte(bot_avg, bot_avg_samples, 0.60f, 0.55f, 0.45f);
    auto fill_solid = [&](GLenum target, const std::array<std::uint8_t, 3>& c) {
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(cube_size) * cube_size * 4);
        for (std::size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i + 0] = c[0]; rgba[i + 1] = c[1]; rgba[i + 2] = c[2]; rgba[i + 3] = 255;
        }
        glTexImage2D(target, 0, GL_RGBA8, cube_size, cube_size,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    };
    fill_solid(GL_TEXTURE_CUBE_MAP_POSITIVE_Y, top_col);
    fill_solid(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, bottom_col);

    // Backstop: any lateral face that still has no upload (e.g. a
    // missing PBMP) falls back to a mid-tone so sampling doesn't
    // return undefined memory.
    for (int face = 0; face < kFaces; ++face) {
        GLenum target = static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face);
        GLint w = 0;
        glGetTexLevelParameteriv(target, 0, GL_TEXTURE_WIDTH, &w);
        if (w > 0) continue;
        // For a lateral face mid-grey is fine; +Y/-Y handled above.
        const auto mid = (face == 2) ? top_col
                       : (face == 3) ? bottom_col
                       : std::array<std::uint8_t, 3>{ 100, 110, 120 };
        fill_solid(target, mid);
    }

    SkyboxResources sb;
    sb.cubemap_texture = cube;
    sb.ambient_color = sky.ambient_color;
    if (star_field_or_null) {
        sb.has_star_field = true;
        sb.star_field_colors = star_field_or_null->colors;
    }

    // Build cube VBO/VAO.
    glGenVertexArrays(1, &sb.vao);
    glBindVertexArray(sb.vao);
    glGenBuffers(1, &sb.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, sb.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(CUBE_VERTS), CUBE_VERTS, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);

    GLuint vs = compile(GL_VERTEX_SHADER,   SKY_VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, SKY_FS);
    if (vs == 0 || fs == 0) {
        glDeleteTextures(1, &sb.cubemap_texture);
        glDeleteBuffers(1, &sb.vbo);
        glDeleteVertexArrays(1, &sb.vao);
        return std::nullopt;
    }
    sb.program = link(vs, fs);
    if (sb.program == 0) {
        glDeleteTextures(1, &sb.cubemap_texture);
        glDeleteBuffers(1, &sb.vbo);
        glDeleteVertexArrays(1, &sb.vao);
        return std::nullopt;
    }
    sb.u_view = glGetUniformLocation(sb.program, "u_view");
    sb.u_proj = glGetUniformLocation(sb.program, "u_proj");

    std::printf("skybox: built %dx%d cubemap (%d uploaded faces) from %s\n",
                cube_size, cube_size, uploaded, sky.dml_name.c_str());

    return sb;
}

void draw_skybox(
    const SkyboxResources& sky,
    const glm::mat4& view,
    const glm::mat4& projection)
{
    if (!sky.valid()) return;

    GLboolean dt_enabled = GL_FALSE; glGetBooleanv(GL_DEPTH_TEST, &dt_enabled);
    GLint depth_func = GL_LESS;     glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    GLboolean dm = GL_TRUE;          glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    glUseProgram(sky.program);
    glUniformMatrix4fv(sky.u_view, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(sky.u_proj, 1, GL_FALSE, glm::value_ptr(projection));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, sky.cubemap_texture);
    glUniform1i(glGetUniformLocation(sky.program, "u_cube"), 0);

    glBindVertexArray(sky.vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);

    glDepthMask(dm);
    glDepthFunc(depth_func);
    if (!dt_enabled) glDisable(GL_DEPTH_TEST);
}

void release_skybox(SkyboxResources& sky)
{
    if (sky.cubemap_texture) glDeleteTextures(1, &sky.cubemap_texture);
    if (sky.vbo)             glDeleteBuffers(1, &sky.vbo);
    if (sky.vao)             glDeleteVertexArrays(1, &sky.vao);
    if (sky.program)         glDeleteProgram(sky.program);
    sky = SkyboxResources{};
}

} // namespace dts_viewer
