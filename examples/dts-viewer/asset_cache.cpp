#include "asset_cache.hpp"

#include "pbmp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "resources/darkstar_volume.hpp"
#include "content/renderable_shape.hpp"
#include "content/dts/renderable_shape_factory.hpp"
#include "content/dts/darkstar.hpp"
#include "content/dts/darkstar_structures.hpp"

namespace dts_viewer
{

namespace
{
namespace fs   = std::filesystem;
namespace dv   = studio::resources::vol::darkstar;
namespace sr   = studio::resources;
namespace sc   = studio::content;
namespace dts  = studio::content::dts::darkstar;

constexpr const char* kVS = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
uniform mat4 u_mvp;
uniform mat3 u_normal_mat;
out vec3 v_normal_ws;
out vec2 v_uv;
void main() {
    v_normal_ws = normalize(u_normal_mat * a_normal);
    v_uv = a_uv;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

constexpr const char* kFS = R"(
#version 410 core
in vec3 v_normal_ws;
in vec2 v_uv;
uniform sampler2D u_tex0;
uniform bool u_has_texture;
uniform vec3  u_ambient_color;
uniform vec3  u_sun_dir;
uniform vec3  u_sun_color;
out vec4 frag;
void main() {
    vec3 N = normalize(v_normal_ws);
    // Tribes meshes carry inconsistent winding (the existing shape-viewer
    // disables face culling for this reason). Use abs() so the back of a
    // surface lights up the same as the front.
    float lambert = abs(dot(N, normalize(u_sun_dir)));
    vec3 lit = u_ambient_color + u_sun_color * lambert;
    if (u_has_texture) {
        vec4 t = texture(u_tex0, v_uv);
        frag = vec4(t.rgb * lit, t.a);
    } else {
        vec3 base = vec3(0.78, 0.80, 0.82);
        frag = vec4(base * lit, 1.0);
    }
}
)";

GLuint compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        std::fprintf(stderr, "asset_cache: shader compile error: %.*s\n",
                     (int)n, log);
    }
    return s;
}

GLuint link(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        std::fprintf(stderr, "asset_cache: program link error: %.*s\n",
                     (int)n, log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

std::string to_lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

std::string normalize_mat_name(const std::string& in)
{
    std::string s = in;
    if (s.size() >= 5 && s.compare(0, 5, "base.") == 0) s.erase(0, 5);
    else if (s.size() >= 5
        && (s[0]=='B'||s[0]=='b') && (s[1]=='A'||s[1]=='a')
        && (s[2]=='S'||s[2]=='s') && (s[3]=='E'||s[3]=='e')
        && s[4]=='.') {
        s.erase(0, 5);
    }
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

std::vector<std::uint8_t> expand_pbmp(
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

// Map an object name -> first-face material index. Same trick the shape
// viewer uses (the high-level renderable_shape callback API drops
// face.material on the floor, so we peek at the low-level shape variant).
std::map<std::string, int>
build_object_to_material(const dts::shape_variant& raw_shape)
{
    return std::visit([](const auto& s) {
        std::map<std::string, int> out;
        for (const auto& obj : s.objects) {
            const auto name_idx = static_cast<std::size_t>(obj.name_index);
            if (name_idx >= s.names.size()) continue;
            std::string name(s.names[name_idx].data(),
                strnlen(s.names[name_idx].data(),
                        s.names[name_idx].size()));
            int mat_idx = -1;
            const auto mesh_idx = static_cast<std::size_t>(obj.mesh_index);
            if (mesh_idx < s.meshes.size()) {
                std::visit([&](const auto& mesh) {
                    if (!mesh.faces.empty()) {
                        mat_idx = static_cast<int>(mesh.faces[0].material);
                    }
                }, s.meshes[mesh_idx]);
            }
            out[name] = mat_idx;
        }
        return out;
    }, raw_shape);
}

struct Bucket
{
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    int  material_index = -1;
    bool has_uvs = false;
    std::string object_name;
};

// Minimal buffering renderer: one bucket per object, position+normal+UV per
// vertex (no skinning, no node tracking).
struct BufferingRenderer final : sc::shape_renderer
{
    std::map<int, Bucket>& buckets;
    int  current_bucket = -1;
    int  next_bucket_id = 0;
    std::string current_object;
    std::vector<sc::vector3f>      cur_pos;
    std::vector<sc::texture_vertex> cur_uv;
    const std::map<std::string, int>* obj_to_mat = nullptr;

    glm::vec3 bbox_min{ 1e30f, 1e30f, 1e30f };
    glm::vec3 bbox_max{-1e30f,-1e30f,-1e30f };

    explicit BufferingRenderer(std::map<int, Bucket>& b) : buckets(b) {}

    void update_node(std::optional<std::string_view>,
                     std::string_view) override {}

    void update_object(std::optional<std::string_view>,
                       std::string_view object_name) override
    {
        current_bucket = next_bucket_id++;
        current_object.assign(object_name.data(), object_name.size());
        auto& b = buckets[current_bucket];
        if (b.object_name.empty()) b.object_name = current_object;
        if (obj_to_mat) {
            auto it = obj_to_mat->find(current_object);
            if (it != obj_to_mat->end()) b.material_index = it->second;
        }
    }

    void new_face(std::size_t) override
    {
        cur_pos.clear();
        cur_uv.clear();
        if (current_bucket < 0) {
            current_bucket = next_bucket_id++;
            auto& b = buckets[current_bucket];
            if (b.object_name.empty()) b.object_name = "<no-object>";
        }
    }

    void emit_vertex(const sc::vector3f& v) override
    {
        cur_pos.push_back(v);
        bbox_min = glm::min(bbox_min, glm::vec3(v.x, v.y, v.z));
        bbox_max = glm::max(bbox_max, glm::vec3(v.x, v.y, v.z));
    }

    void emit_texture_vertex(const sc::texture_vertex& tv) override
    {
        cur_uv.push_back(tv);
    }

    void end_face() override
    {
        if (cur_pos.size() < 3) return;
        const bool face_has_uvs = (cur_uv.size() == cur_pos.size());
        Bucket& b = buckets[current_bucket];
        if (face_has_uvs) b.has_uvs = true;

        glm::vec3 v0(cur_pos[0].x, cur_pos[0].y, cur_pos[0].z);
        sc::texture_vertex t0 = face_has_uvs
            ? cur_uv[0] : sc::texture_vertex{0.0f, 0.0f};
        for (std::size_t i = 1; i + 1 < cur_pos.size(); ++i) {
            glm::vec3 v1(cur_pos[i].x,   cur_pos[i].y,   cur_pos[i].z);
            glm::vec3 v2(cur_pos[i+1].x, cur_pos[i+1].y, cur_pos[i+1].z);
            glm::vec3 n = glm::normalize(glm::cross(v1 - v0, v2 - v0));
            if (!std::isfinite(n.x)) n = glm::vec3(0,0,1);
            sc::texture_vertex t1 = face_has_uvs
                ? cur_uv[i]   : sc::texture_vertex{0.0f, 0.0f};
            sc::texture_vertex t2 = face_has_uvs
                ? cur_uv[i+1] : sc::texture_vertex{0.0f, 0.0f};
            const glm::vec3 verts[3] = { v0, v1, v2 };
            const sc::texture_vertex texs[3] = { t0, t1, t2 };
            for (int k = 0; k < 3; ++k) {
                b.positions.push_back(verts[k].x);
                b.positions.push_back(verts[k].y);
                b.positions.push_back(verts[k].z);
                b.normals.push_back(n.x);
                b.normals.push_back(n.y);
                b.normals.push_back(n.z);
                b.uvs.push_back(texs[k].x);
                b.uvs.push_back(texs[k].y);
            }
        }
    }
};

// Hand-rolled map from common Tribes 1 datablock names to their canonical
// DTS shape filenames. Anything not in the table falls back to a few generic
// transformations (see try_load_for_datablock).
const std::unordered_map<std::string, std::string>& datablock_table()
{
    static const std::unordered_map<std::string, std::string> t = {
        // generators
        {"generator",        "generator.dts"},
        {"portgenerator",    "generator_p.dts"},

        // turrets
        {"turretbasepermanent","chainturret.dts"},
        {"turretbase",       "chainturret.dts"},
        {"aaturret",         "chainturret.dts"},
        {"missileturret",    "missileturret.dts"},
        {"elfturret",        "indoorgun.dts"},
        {"plasmaturret",     "indoorgun.dts"},
        {"mortarturret",     "mortar_turret.dts"},

        // ammo / health / repair items
        {"ammopack",         "ammopack.dts"},
        {"armorpack",        "armorpack.dts"},
        {"energypack",       "ammopack.dts"},
        {"healthpatch",      "armorpatch.dts"},
        {"armorpatch",       "armorpatch.dts"},
        {"repairpatch",      "armorpatch.dts"},
        {"repairpack",       "ammopack.dts"},
        {"repairkit",        "ammopack.dts"},
        {"shieldpack",       "shieldpack.dts"},
        {"jetpack",          "jetpack.dts"},
        {"sensorjampack",    "sensorjampack.dts"},
        {"sensorjammerpack", "sensorjampack.dts"},
        {"mortarpack",       "mortarpack.dts"},
        {"discammo",         "discammo.dts"},
        {"plasammo",         "plasammo.dts"},
        {"plasmaammo",       "plasammo.dts"},
        {"grenammo",         "grenammo.dts"},
        {"grenadeammo",      "grenammo.dts"},
        {"bulletammo",       "ammopack.dts"},
        {"mortarammo",       "mortarammo.dts"},
        {"mineammo",         "mineammo.dts"},

        // stations
        {"inventorystation", "inventory_sta.dts"},
        {"ammostation",      "ammounit.dts"},
        {"ammounit",         "ammounit.dts"},
        {"vehiclestation",   "ammopad.dts"},
        {"commandstation",   "cmdpnl.dts"},
        {"commandpanel",     "cmdpnl.dts"},

        // vehicles
        {"vehiclepad",       "ammopad.dts"},
        {"hover_apc",        "hover_apc.dts"},

        // flags / mission props
        {"flag",             "flag.dts"},
        {"flagstand",        "flagstand.dts"},
        {"towerswitch",      "tower.dts"},
        {"objective",        "tower.dts"},

        // doors
        {"doorfourleft",     "newdoor4_l.dts"},
        {"doorfourright",    "newdoor4_r.dts"},
        {"doortwoleft",      "newdoor2_l.dts"},
        {"doortworight",     "newdoor2_r.dts"},

        // sensors / antennas
        {"sensorlargepulse", "sensor_pulse_med.dts"},
        {"sensorsmallpulse", "sensor_small.dts"},
        {"pulsesensor",      "sensor_pulse_med.dts"},
        {"sensorjammer",     "sensor_jammer.dts"},
        {"antenna",          "anten_med.dts"},
        {"largeantenna",     "anten_lrg.dts"},
        {"smallantenna",     "anten_small.dts"},
        {"rocketturret",     "missileturret.dts"},
    };
    return t;
}

} // anonymous namespace

// --- AssetShape ------------------------------------------------------------

struct AssetShape
{
    struct GLBucket
    {
        GLuint   vao = 0;
        GLuint   vbo_pos = 0;
        GLuint   vbo_nor = 0;
        GLuint   vbo_uv  = 0;
        GLsizei  vertex_count = 0;
        GLuint   texture = 0;
    };

    std::string filename;
    std::vector<GLBucket> buckets;
    glm::vec3 bbox_min { 0.0f };
    glm::vec3 bbox_max { 0.0f };
    std::size_t triangle_count = 0;

    ~AssetShape()
    {
        for (auto& b : buckets) {
            if (b.vbo_pos) glDeleteBuffers(1, &b.vbo_pos);
            if (b.vbo_nor) glDeleteBuffers(1, &b.vbo_nor);
            if (b.vbo_uv ) glDeleteBuffers(1, &b.vbo_uv);
            if (b.vao)     glDeleteVertexArrays(1, &b.vao);
        }
    }
};

// --- AssetCache ------------------------------------------------------------

AssetCache::AssetCache() = default;

AssetCache::~AssetCache()
{
    // Textures are owned by the cache (shapes only borrow them); free them.
    for (auto& kv : texture_cache_) {
        if (kv.second) glDeleteTextures(1, &kv.second);
    }
    if (program_) glDeleteProgram(program_);
}

void AssetCache::init(MaterialResolver* resolver,
                      std::map<std::uint32_t, const Palette*> palette_map,
                      std::vector<std::filesystem::path> mounted_vols)
{
    resolver_     = resolver;
    palette_map_  = std::move(palette_map);
    mounted_vols_ = std::move(mounted_vols);
    initialized_  = true;
}

void AssetCache::ensure_program()
{
    if (program_) return;
    program_ = link(compile(GL_VERTEX_SHADER, kVS),
                    compile(GL_FRAGMENT_SHADER, kFS));
    u_mvp_         = glGetUniformLocation(program_, "u_mvp");
    u_normal_mat_  = glGetUniformLocation(program_, "u_normal_mat");
    u_tex0_        = glGetUniformLocation(program_, "u_tex0");
    u_has_texture_ = glGetUniformLocation(program_, "u_has_texture");
}

bool AssetCache::fetch_dts_bytes(const std::string& filename_lower,
                                 std::vector<char>& out_bytes) const
{
    // Scan mounted VOLs in order; return the first hit. We match on the
    // lowercased basename (the Tribes filename casing varies — most are
    // "Foo.DTS" but a few are "foo.dts").
    for (const auto& vp : mounted_vols_) {
        std::ifstream in(vp, std::ios::binary);
        if (!in) continue;
        dv::vol_file_archive plugin;
        if (!plugin.stream_is_supported(in)) continue;
        in.clear(); in.seekg(0);
        auto all = sr::get_all_content(vp, in, plugin);
        for (auto& entry : all) {
            auto* f = std::get_if<sr::file_info>(&entry);
            if (!f) continue;
            std::string lower = to_lower(f->filename.string());
            if (lower != filename_lower) continue;
            std::stringstream buf;
            in.clear(); in.seekg(0);
            plugin.extract_file_contents(in, *f, buf);
            auto s = buf.str();
            out_bytes.assign(s.begin(), s.end());
            return true;
        }
    }
    return false;
}

const AssetShape* AssetCache::try_load_by_filename(
    const std::string& dts_filename_lower)
{
    if (!initialized_) return nullptr;
    auto it = shapes_.find(dts_filename_lower);
    if (it != shapes_.end()) return it->second.get();
    if (failed_loads_.count(dts_filename_lower)) return nullptr;
    AssetShape* sh = load_shape_internal(dts_filename_lower);
    if (!sh) {
        failed_loads_.insert(dts_filename_lower);
        ++stats_.shapes_missed;
        return nullptr;
    }
    ++stats_.shapes_loaded;
    return sh;
}

const AssetShape* AssetCache::try_load_for_datablock(
    const std::string& datablock_name)
{
    if (!initialized_) return nullptr;
    auto cache_it = datablock_resolution_cache_.find(datablock_name);
    if (cache_it != datablock_resolution_cache_.end()) {
        if (cache_it->second.empty()) return nullptr;
        return try_load_by_filename(cache_it->second);
    }

    std::string key_lower = to_lower(datablock_name);

    auto try_filename = [&](const std::string& fname) -> const AssetShape* {
        const AssetShape* sh = try_load_by_filename(fname);
        if (sh) {
            datablock_resolution_cache_[datablock_name] = fname;
            return sh;
        }
        return nullptr;
    };

    // 1) hand-rolled table
    const auto& tab = datablock_table();
    auto tab_it = tab.find(key_lower);
    if (tab_it != tab.end()) {
        if (const AssetShape* sh = try_filename(tab_it->second)) return sh;
    }

    // 2) lowercased name + ".dts"
    if (const AssetShape* sh = try_filename(key_lower + ".dts")) return sh;

    // 3) strip trailing "data" if present
    if (key_lower.size() > 4
        && key_lower.compare(key_lower.size() - 4, 4, "data") == 0) {
        std::string stem = key_lower.substr(0, key_lower.size() - 4);
        if (!stem.empty()) {
            if (const AssetShape* sh = try_filename(stem + ".dts")) return sh;
        }
    }

    // No mapping — cache the miss and stay quiet.
    datablock_resolution_cache_[datablock_name] = std::string();
    return nullptr;
}

AssetShape* AssetCache::load_shape_internal(const std::string& dts_filename_lower)
{
    std::vector<char> bytes;
    if (!fetch_dts_bytes(dts_filename_lower, bytes) || bytes.empty()) {
        return nullptr;
    }

    // 1) read low-level shape for the object -> material-index table
    std::map<std::string, int> obj_to_mat;
    std::vector<sc::material> materials;
    {
        std::string s(bytes.begin(), bytes.end());
        std::stringstream ss(s);
        try {
            auto raw = dts::read_shape(ss, std::nullopt);
            obj_to_mat = build_object_to_material(raw);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "asset_cache: read_shape '%s' failed: %s\n",
                dts_filename_lower.c_str(), e.what());
            return nullptr;
        }
    }

    // 2) high-level shape -> buffered geometry
    std::map<int, Bucket> buckets;
    glm::vec3 bbox_min { 1e30f, 1e30f, 1e30f };
    glm::vec3 bbox_max {-1e30f,-1e30f,-1e30f };
    try {
        std::string s(bytes.begin(), bytes.end());
        std::stringstream ss(s);
        auto shape = sc::dts::make_shape(ss);
        if (!shape) {
            std::fprintf(stderr,
                "asset_cache: make_shape returned null for '%s'\n",
                dts_filename_lower.c_str());
            return nullptr;
        }
        auto details = shape->get_detail_levels();
        std::vector<std::size_t> detail_indexes;
        if (!details.empty()) detail_indexes.push_back(0);
        auto seqs = shape->get_sequences(detail_indexes);

        BufferingRenderer r(buckets);
        r.obj_to_mat = &obj_to_mat;
        shape->render_shape(r, detail_indexes, seqs);
        materials = shape->get_materials();
        bbox_min = r.bbox_min;
        bbox_max = r.bbox_max;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "asset_cache: make_shape/render '%s' failed: %s\n",
            dts_filename_lower.c_str(), e.what());
        return nullptr;
    }
    if (buckets.empty()) {
        std::fprintf(stderr,
            "asset_cache: '%s' produced no buckets\n",
            dts_filename_lower.c_str());
        return nullptr;
    }

    auto sh = std::make_unique<AssetShape>();
    sh->filename = dts_filename_lower;
    sh->bbox_min = bbox_min;
    sh->bbox_max = bbox_max;

    // 3) per-bucket GL upload + texture resolution
    for (auto& [_, bucket] : buckets) {
        if (bucket.positions.empty()) continue;
        AssetShape::GLBucket gb;
        gb.vertex_count = static_cast<GLsizei>(bucket.positions.size() / 3);
        sh->triangle_count += gb.vertex_count / 3;

        glGenVertexArrays(1, &gb.vao);
        glBindVertexArray(gb.vao);

        glGenBuffers(1, &gb.vbo_pos);
        glBindBuffer(GL_ARRAY_BUFFER, gb.vbo_pos);
        glBufferData(GL_ARRAY_BUFFER,
                     bucket.positions.size() * sizeof(float),
                     bucket.positions.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        glGenBuffers(1, &gb.vbo_nor);
        glBindBuffer(GL_ARRAY_BUFFER, gb.vbo_nor);
        glBufferData(GL_ARRAY_BUFFER,
                     bucket.normals.size() * sizeof(float),
                     bucket.normals.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        glGenBuffers(1, &gb.vbo_uv);
        glBindBuffer(GL_ARRAY_BUFFER, gb.vbo_uv);
        std::vector<float> uv_data = bucket.uvs;
        const std::size_t expected_uvs = bucket.positions.size() / 3 * 2;
        if (uv_data.size() != expected_uvs) uv_data.assign(expected_uvs, 0.0f);
        glBufferData(GL_ARRAY_BUFFER,
                     uv_data.size() * sizeof(float),
                     uv_data.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

        glBindVertexArray(0);

        // Texture resolution
        gb.texture = 0;
        if (resolver_
            && bucket.material_index >= 0
            && bucket.material_index < static_cast<int>(materials.size()))
        {
            const std::string& raw = materials[bucket.material_index].filename;
            if (!raw.empty()) {
                const std::string key = normalize_mat_name(raw);
                if (!key.empty()) {
                    auto cit = texture_cache_.find(key);
                    if (cit != texture_cache_.end()) {
                        gb.texture = cit->second;
                    } else {
                        auto bytes_opt = resolver_->resolve(raw);
                        if (bytes_opt && !bytes_opt->empty()) {
                            try {
                                std::stringstream pss(
                                    std::string(bytes_opt->begin(),
                                                bytes_opt->end()));
                                PbmpImage img = load_pbmp(pss);
                                if (img.width > 0 && img.height > 0) {
                                    auto rgba = expand_pbmp(img, palette_map_);
                                    GLuint tex = 0;
                                    glGenTextures(1, &tex);
                                    glBindTexture(GL_TEXTURE_2D, tex);
                                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                                                 static_cast<GLsizei>(img.width),
                                                 static_cast<GLsizei>(img.height),
                                                 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                                 rgba.data());
                                    glGenerateMipmap(GL_TEXTURE_2D);
                                    glTexParameteri(GL_TEXTURE_2D,
                                        GL_TEXTURE_MIN_FILTER,
                                        GL_LINEAR_MIPMAP_LINEAR);
                                    glTexParameteri(GL_TEXTURE_2D,
                                        GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                                    glTexParameteri(GL_TEXTURE_2D,
                                        GL_TEXTURE_WRAP_S, GL_REPEAT);
                                    glTexParameteri(GL_TEXTURE_2D,
                                        GL_TEXTURE_WRAP_T, GL_REPEAT);
                                    glBindTexture(GL_TEXTURE_2D, 0);
                                    texture_cache_[key] = tex;
                                    gb.texture = tex;
                                    ++stats_.textures_uploaded;
                                }
                            } catch (const std::exception&) {
                                // PBMP parse failed — flat shading.
                            }
                        }
                    }
                }
            }
        }

        sh->buckets.push_back(gb);
    }

    if (sh->buckets.empty()) return nullptr;

    stats_.triangles_total += sh->triangle_count;

    AssetShape* raw = sh.get();
    shapes_.emplace(dts_filename_lower, std::move(sh));
    return raw;
}

void AssetCache::render(const AssetShape& shape,
                        const glm::mat4& model,
                        const glm::mat4& view,
                        const glm::mat4& proj,
                        const SceneLighting& lighting)
{
    ensure_program();
    glUseProgram(program_);

    glm::mat4 mvp = proj * view * model;
    glm::mat3 normal_mat = glm::transpose(glm::inverse(glm::mat3(model)));

    if (u_mvp_ >= 0)
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp));
    if (u_normal_mat_ >= 0)
        glUniformMatrix3fv(u_normal_mat_, 1, GL_FALSE,
                           glm::value_ptr(normal_mat));
    if (u_tex0_ >= 0) glUniform1i(u_tex0_, 0);

    // Lighting: we only need ambient + sun. Point lights are omitted in v1
    // (the existing shape-viewer fragment shader does the same simplification
    // for props).
    GLint loc_amb = glGetUniformLocation(program_, "u_ambient_color");
    GLint loc_dir = glGetUniformLocation(program_, "u_sun_dir");
    GLint loc_col = glGetUniformLocation(program_, "u_sun_color");
    if (loc_amb >= 0) glUniform3fv(loc_amb, 1, lighting.ambient_color.data());
    if (loc_dir >= 0) glUniform3fv(loc_dir, 1, lighting.sun.direction.data());
    if (loc_col >= 0) {
        const float s = lighting.sun.intensity;
        const float col[3] = {
            lighting.sun.color[0] * s,
            lighting.sun.color[1] * s,
            lighting.sun.color[2] * s,
        };
        glUniform3fv(loc_col, 1, col);
    }

    glActiveTexture(GL_TEXTURE0);
    for (const auto& b : shape.buckets) {
        if (b.vertex_count <= 0 || b.vao == 0) continue;
        const bool has_tex = (b.texture != 0);
        if (u_has_texture_ >= 0) glUniform1i(u_has_texture_, has_tex ? 1 : 0);
        glBindTexture(GL_TEXTURE_2D, has_tex ? b.texture : 0);
        glBindVertexArray(b.vao);
        glDrawArrays(GL_TRIANGLES, 0, b.vertex_count);
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void AssetCache::shape_aabb(const AssetShape& shape,
                            glm::vec3& out_min, glm::vec3& out_max) const
{
    out_min = shape.bbox_min;
    out_max = shape.bbox_max;
}

} // namespace dts_viewer
