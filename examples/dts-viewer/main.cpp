#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <variant>
#include <algorithm>
#include <filesystem>
#include <map>
#include <limits>

#include <SDL.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include "resources/darkstar_volume.hpp"
#include "content/renderable_shape.hpp"
#include "content/dts/renderable_shape_factory.hpp"
#include "content/dts/darkstar.hpp"
#include "content/dts/darkstar_structures.hpp"

#include "pbmp.hpp"

namespace fs = std::filesystem;
namespace dv = studio::resources::vol::darkstar;
namespace sr = studio::resources;
namespace sc = studio::content;
namespace dts = studio::content::dts::darkstar;

// ---------------------- DTS node hierarchy (bind pose) ----------------------
//
// Spec 02-node-hierarchy: capture the static skeleton from the raw shape_variant.
// The shape_renderer visitor callbacks only carry node *names*, not matrices, so
// we must call `dts::read_shape()` directly and walk `nodes` + `transforms`.

struct Node
{
    std::string name;
    int         parent_index;   // -1 for root
    glm::mat4   bind_local;     // local-to-parent at bind pose
};

// Convert a raw DTS transform (v2/v3/v5/v6/v7/v8) into a local glm::mat4 using
// the same recipe `dts_renderable_shape.cpp:328` uses:
//   T * transpose(toMat4(quat(w,x,y,z))) * S
// v2/v3/v5/v6 use quaternion4f + scale; v7 uses quaternion4s + scale;
// v8 uses quaternion4s and lacks a scale field (treat as (1,1,1)).
static glm::mat4 make_local_mat(const sc::quaternion4f& rot,
                                const sc::vector3f& trans,
                                const glm::vec3& scale)
{
    glm::mat4 T = glm::translate(glm::mat4(1.0f), glm::vec3(trans.x, trans.y, trans.z));
    glm::mat4 R = glm::transpose(glm::toMat4(glm::quat(rot.w, rot.x, rot.y, rot.z)));
    glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
    return T * R * S;
}

static glm::mat4 transform_to_local_mat(const dts::shape::v2::transform& t)
{
    return make_local_mat(sc::to_float(t.rotation), t.translation,
                          glm::vec3(t.scale.x, t.scale.y, t.scale.z));
}
static glm::mat4 transform_to_local_mat(const dts::shape::v7::transform& t)
{
    return make_local_mat(sc::to_float(t.rotation), t.translation,
                          glm::vec3(t.scale.x, t.scale.y, t.scale.z));
}
static glm::mat4 transform_to_local_mat(const dts::shape::v8::transform& t)
{
    return make_local_mat(sc::to_float(t.rotation), t.translation,
                          glm::vec3(1.0f, 1.0f, 1.0f));
}

static std::vector<Node> build_nodes(const dts::shape_variant& raw_shape)
{
    return std::visit([](const auto& s) {
        std::vector<Node> out;
        out.reserve(s.nodes.size());
        for (const auto& n : s.nodes) {
            Node out_node;
            out_node.name = std::string(s.names[n.name_index].data(),
                strnlen(s.names[n.name_index].data(), s.names[n.name_index].size()));
            out_node.parent_index = static_cast<int>(n.parent_node_index);
            const auto tx_idx = static_cast<std::size_t>(n.default_transform_index);
            if (tx_idx < s.transforms.size()) {
                out_node.bind_local = transform_to_local_mat(s.transforms[tx_idx]);
            } else {
                out_node.bind_local = glm::mat4(1.0f);
            }
            out.push_back(std::move(out_node));
        }
        return out;
    }, raw_shape);
}

// Accumulate parent transforms to get world-space bind matrices, one per node.
// Roots (parent_index == -1) use their local matrix directly.
static std::vector<glm::mat4> compute_world_bind(const std::vector<Node>& nodes)
{
    std::vector<glm::mat4> world(nodes.size(), glm::mat4(1.0f));
    // Nodes appear in declaration order in the DTS; parents typically precede
    // children, but make no assumption — resolve iteratively up to N passes.
    std::vector<bool> done(nodes.size(), false);
    bool progress = true;
    std::size_t passes = 0;
    while (progress && passes++ <= nodes.size() + 1) {
        progress = false;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (done[i]) continue;
            int p = nodes[i].parent_index;
            if (p < 0) {
                world[i] = nodes[i].bind_local;
                done[i] = true;
                progress = true;
            } else if (p < (int)nodes.size() && done[p]) {
                world[i] = world[p] * nodes[i].bind_local;
                done[i] = true;
                progress = true;
            }
        }
    }
    return world;
}

// ---------------------- per-material/object mesh bucket ----------------------

struct MeshBucket
{
    std::vector<float> positions;   // 3 floats per vertex
    std::vector<float> normals;     // 3 floats per vertex, flat per face
    std::vector<float> uvs;         // 2 floats per vertex
    std::size_t triangle_count = 0;
    bool has_uvs = false;
    std::string object_name;        // first object that contributed to this bucket
    // running uv range, only meaningful if has_uvs
    float u_min = std::numeric_limits<float>::infinity();
    float u_max = -std::numeric_limits<float>::infinity();
    float v_min = std::numeric_limits<float>::infinity();
    float v_max = -std::numeric_limits<float>::infinity();
};

// ---------------------- shape_renderer that buffers triangles ----------------------

struct buffered_geometry
{
    // legacy flat buffers (kept so the existing single-draw render path still works
    // — replaced by per-bucket draws in a later spec).
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<int>   vertex_node_index;  // one entry per emitted vertex (3 per tri)
    std::size_t triangle_count = 0;

    glm::vec3 bbox_min{ 1e30f}, bbox_max{-1e30f};

    // per-material/object grouping
    std::map<int, MeshBucket> buckets;
};

struct buffering_renderer : sc::shape_renderer
{
    buffered_geometry& g;
    std::vector<sc::vector3f>      current_positions;
    std::vector<sc::texture_vertex> current_uvs;
    int  current_bucket = -1;       // assigned when first object starts
    int  next_bucket_id = 0;
    std::string current_object_name;

    // For spec 02: track which node the current vertices belong to. The
    // shape_renderer visitor only carries node *names*, so we map name -> index
    // by consulting the Node vector built from the raw shape. Duplicate names
    // in the DTS name pool are rare for Tribes meshes; if collisions appear we
    // fall back to the first match.
    const std::vector<Node>* nodes_ref = nullptr;
    int current_node_index = -1;

    explicit buffering_renderer(buffered_geometry& g) : g(g) {}

    void update_node(std::optional<std::string_view>, std::string_view name) override {
        current_node_index = -1;
        if (nodes_ref) {
            for (std::size_t i = 0; i < nodes_ref->size(); ++i) {
                if ((*nodes_ref)[i].name == std::string(name)) {
                    current_node_index = (int)i;
                    break;
                }
            }
        }
    }

    void update_object(std::optional<std::string_view>, std::string_view object_name) override {
        // Each object becomes its own bucket (one mesh per object, typically one
        // material). The shape_renderer callback API doesn't expose face.material
        // directly, so per-object is the finest granularity available here.
        current_bucket = next_bucket_id++;
        current_object_name.assign(object_name.data(), object_name.size());
        auto& b = g.buckets[current_bucket];
        if (b.object_name.empty()) b.object_name = current_object_name;
    }

    void new_face(std::size_t) override {
        current_positions.clear();
        current_uvs.clear();
        if (current_bucket < 0) {
            // some shapes never call update_object before the first face — give them
            // a default bucket so geometry is not dropped.
            current_bucket = next_bucket_id++;
            auto& b = g.buckets[current_bucket];
            if (b.object_name.empty()) b.object_name = "<no-object>";
        }
    }

    void emit_vertex(const sc::vector3f& v) override {
        current_positions.push_back(v);
        g.bbox_min = glm::min(g.bbox_min, glm::vec3(v.x, v.y, v.z));
        g.bbox_max = glm::max(g.bbox_max, glm::vec3(v.x, v.y, v.z));
    }

    void emit_texture_vertex(const sc::texture_vertex& tv) override {
        current_uvs.push_back(tv);
    }

    void end_face() override {
        if (current_positions.size() < 3) return;

        const bool face_has_uvs = current_uvs.size() == current_positions.size();
        MeshBucket& b = g.buckets[current_bucket];
        if (face_has_uvs) b.has_uvs = true;

        // fan triangulate (v0,v1,v2), (v0,v2,v3), ...
        glm::vec3 v0(current_positions[0].x, current_positions[0].y, current_positions[0].z);
        sc::texture_vertex t0 = face_has_uvs ? current_uvs[0] : sc::texture_vertex{0.0f, 0.0f};

        for (std::size_t i = 1; i + 1 < current_positions.size(); ++i) {
            glm::vec3 v1(current_positions[i].x,   current_positions[i].y,   current_positions[i].z);
            glm::vec3 v2(current_positions[i+1].x, current_positions[i+1].y, current_positions[i+1].z);
            glm::vec3 n = glm::normalize(glm::cross(v1 - v0, v2 - v0));
            if (!std::isfinite(n.x)) n = glm::vec3(0,0,1);

            sc::texture_vertex t1 = face_has_uvs ? current_uvs[i]   : sc::texture_vertex{0.0f, 0.0f};
            sc::texture_vertex t2 = face_has_uvs ? current_uvs[i+1] : sc::texture_vertex{0.0f, 0.0f};

            const glm::vec3 verts[3] = { v0, v1, v2 };
            const sc::texture_vertex tex[3] = { t0, t1, t2 };

            for (int k = 0; k < 3; ++k) {
                // legacy flat buffer
                g.positions.push_back(verts[k].x);
                g.positions.push_back(verts[k].y);
                g.positions.push_back(verts[k].z);
                g.normals.push_back(n.x);
                g.normals.push_back(n.y);
                g.normals.push_back(n.z);
                g.vertex_node_index.push_back(current_node_index);

                // per-bucket buffers
                b.positions.push_back(verts[k].x);
                b.positions.push_back(verts[k].y);
                b.positions.push_back(verts[k].z);
                b.normals.push_back(n.x);
                b.normals.push_back(n.y);
                b.normals.push_back(n.z);
                b.uvs.push_back(tex[k].x);
                b.uvs.push_back(tex[k].y);

                if (face_has_uvs) {
                    b.u_min = std::min(b.u_min, tex[k].x);
                    b.u_max = std::max(b.u_max, tex[k].x);
                    b.v_min = std::min(b.v_min, tex[k].y);
                    b.v_max = std::max(b.v_max, tex[k].y);
                }
            }
            ++g.triangle_count;
            ++b.triangle_count;
        }
    }
};

// ---------------------- DTS loading ----------------------

static std::vector<char> read_dts_from_vol(const fs::path& vol_path, const std::string& dts_name_lower_match)
{
    std::ifstream in(vol_path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open vol: " + vol_path.string());
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) throw std::runtime_error("not a darkstar vol");
    in.clear(); in.seekg(0);

    auto all = sr::get_all_content(vol_path, in, plugin);
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        auto name = f->filename.string();
        auto lower = name;
        for (auto& c : lower) c = std::tolower((unsigned char)c);
        if (lower.size() < 4 || lower.substr(lower.size()-4) != ".dts") continue;
        if (!dts_name_lower_match.empty() && lower.find(dts_name_lower_match) == std::string::npos) continue;

        std::printf("Loading DTS: %s (%zu bytes)\n", name.c_str(), f->size);
        std::stringstream buf;
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, buf);
        auto s = buf.str();
        return std::vector<char>(s.begin(), s.end());
    }
    throw std::runtime_error("no matching DTS found");
}

struct loaded_shape
{
    buffered_geometry         geom;
    std::vector<sc::material> materials;
    std::vector<Node>         nodes;        // bind-pose hierarchy (spec 02)
    std::vector<glm::mat4>    world_bind;   // accumulated world transforms
};

static loaded_shape build_geometry(const std::vector<char>& dts_bytes)
{
    // 1) Raw shape via read_shape() — this is where the bind pose lives.
    //    The shape_renderer callbacks do NOT carry matrices, so we MUST
    //    bypass `make_shape` for node-hierarchy data.
    loaded_shape out;
    {
        std::string str(dts_bytes.begin(), dts_bytes.end());
        std::stringstream ss(str);
        auto raw = dts::read_shape(ss, std::nullopt);
        out.nodes = build_nodes(raw);
        out.world_bind = compute_world_bind(out.nodes);
    }

    // 2) The high-level renderable_shape, for geometry buffering (unchanged path).
    std::string str(dts_bytes.begin(), dts_bytes.end());
    std::stringstream ss(str);
    auto shape = sc::dts::make_shape(ss);
    if (!shape) throw std::runtime_error("make_shape returned null");

    auto details = shape->get_detail_levels();
    // Use only detail level 0 (highest detail)
    std::vector<std::size_t> detail_indexes;
    if (!details.empty()) detail_indexes.push_back(0);

    auto sequences = shape->get_sequences(detail_indexes);

    buffering_renderer r(out.geom);
    r.nodes_ref = &out.nodes;
    shape->render_shape(r, detail_indexes, sequences);
    out.materials = shape->get_materials();
    return out;
}

// ---------------------- shader helpers ----------------------

static GLuint compile_shader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        std::fprintf(stderr, "shader compile error: %.*s\n", (int)n, log);
        std::exit(10);
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        std::fprintf(stderr, "program link error: %.*s\n", (int)n, log);
        std::exit(11);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

static const char* VS_SRC = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
uniform mat4 u_mvp;
uniform mat3 u_normal_mat;
out vec3 v_normal_ws;
void main() {
    v_normal_ws = normalize(u_normal_mat * a_normal);
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

static const char* FS_SRC = R"(
#version 410 core
in vec3 v_normal_ws;
out vec4 frag;
void main() {
    vec3 L = normalize(vec3(0.4, 0.8, 0.6));
    float d = max(dot(normalize(v_normal_ws), L), 0.0);
    vec3 base = vec3(0.75, 0.78, 0.82);
    vec3 col = base * (0.25 + 0.75 * d);
    frag = vec4(col, 1.0);
}
)";

// ---------------------- bone overlay shader (spec 02) ----------------------

static const char* BONE_VS_SRC = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_color;
uniform mat4 u_mvp;
out vec3 v_color;
void main() {
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

static const char* BONE_FS_SRC = R"(
#version 410 core
in  vec3 v_color;
out vec4 frag;
void main() {
    frag = vec4(v_color, 1.0);
}
)";

// ---------------------- PBMP smoke test (spec 02) ----------------------
//
// Pull a single .bmp out of a VOL by filename substring, run it through
// load_pbmp(), and print the head/PiDX/DETL fields. Used by the
// `--dump-bmp <name>` CLI flag.

static int dump_pbmp_from_vol(const fs::path& vol_path, const std::string& bmp_match_lower)
{
    std::ifstream in(vol_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open vol: %s\n", vol_path.string().c_str());
        return 1;
    }
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) {
        std::fprintf(stderr, "not a darkstar vol: %s\n", vol_path.string().c_str());
        return 1;
    }
    in.clear(); in.seekg(0);

    auto all = sr::get_all_content(vol_path, in, plugin);
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        auto name = f->filename.string();
        auto lower = name;
        for (auto& c : lower) c = std::tolower((unsigned char)c);
        if (lower.size() < 4 || lower.substr(lower.size()-4) != ".bmp") continue;
        if (!bmp_match_lower.empty() && lower.find(bmp_match_lower) == std::string::npos) continue;

        std::stringstream buf;
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, buf);
        buf.seekg(0);
        PbmpImage img = load_pbmp(buf);
        std::printf("PBMP %s:\n", name.c_str());
        std::printf("  width         = %u\n", img.width);
        std::printf("  height        = %u\n", img.height);
        std::printf("  bit_depth     = %u\n", img.bit_depth);
        std::printf("  palette_index = %u\n", img.palette_index);
        std::printf("  mip_count     = %u\n", img.mip_count);
        std::printf("  pixels        = %zu bytes (expected %u for 8bpp primary level)\n",
            img.indexed_pixels.size(),
            img.width * img.height);
        return 0;
    }
    std::fprintf(stderr, "no .bmp matching '%s' in %s\n",
        bmp_match_lower.c_str(), vol_path.string().c_str());
    return 2;
}

// ---------------------- main ----------------------

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <path-to-vol> [dts-substring]\n"
            "       %s <path-to-vol> --dump-bmp <bmp-substring>\n"
            "  e.g. %s tribes-game/base/Entities.vol chainturret\n"
            "       %s tribes-game/base/Entities.vol --dump-bmp ammo\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    fs::path vol_path = argv[1];

    // Spec 02 smoke test: dump the parsed fields of one PBMP and exit, without
    // opening a window. Accepts `--dump-bmp <substring>`.
    if (argc >= 4 && std::string(argv[2]) == "--dump-bmp") {
        std::string bmp_match = argv[3];
        for (auto& c : bmp_match) c = std::tolower((unsigned char)c);
        return dump_pbmp_from_vol(vol_path, bmp_match);
    }

    std::string dts_match = argc >= 3 ? argv[2] : "";
    for (auto& c : dts_match) c = std::tolower((unsigned char)c);

    auto dts_bytes = read_dts_from_vol(vol_path, dts_match);
    auto loaded = build_geometry(dts_bytes);
    auto& geom = loaded.geom;

    std::printf("Geometry: %zu triangles, %zu vertices, bbox=[%.1f %.1f %.1f]..[%.1f %.1f %.1f]\n",
        geom.triangle_count, geom.positions.size()/3,
        geom.bbox_min.x, geom.bbox_min.y, geom.bbox_min.z,
        geom.bbox_max.x, geom.bbox_max.y, geom.bbox_max.z);

    // ---- spec 01-uv-capture: summary + per-bucket UV ranges to stderr ----
    std::fprintf(stderr, "materials: %zu, total tris: %zu\n",
        loaded.materials.size(), geom.triangle_count);
    std::fprintf(stderr, "buckets: %zu\n", geom.buckets.size());
    for (const auto& [idx, b] : geom.buckets) {
        if (b.has_uvs) {
            std::fprintf(stderr,
                "  bucket %d (%s): tris=%zu uvs=yes  u=[%.3f..%.3f] v=[%.3f..%.3f]\n",
                idx, b.object_name.c_str(), b.triangle_count,
                b.u_min, b.u_max, b.v_min, b.v_max);
        } else {
            std::fprintf(stderr,
                "  bucket %d (%s): tris=%zu uvs=no\n",
                idx, b.object_name.c_str(), b.triangle_count);
        }
    }
    for (std::size_t i = 0; i < loaded.materials.size(); ++i) {
        std::fprintf(stderr, "  material[%zu]: %s\n",
            i, loaded.materials[i].filename.c_str());
    }

    // ---- spec 02 node hierarchy summary ----
    {
        const auto& nodes = loaded.nodes;
        std::string dts_label = dts_match.empty() ? std::string("dts") : dts_match;
        std::printf("%s: %zu nodes\n", dts_label.c_str(), nodes.size());
        // Print every root node (parent_index == -1) — almost always exactly one.
        int root_count = 0;
        for (const auto& n : nodes) {
            if (n.parent_index < 0) {
                std::printf("  root: %s\n", n.name.c_str());
                ++root_count;
            }
        }
        if (root_count == 0) std::printf("  (no root found)\n");
        // Also dump the hierarchy to stderr for debugging.
        std::fprintf(stderr, "nodes (%zu):\n", nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            std::fprintf(stderr, "  [%zu] %s (parent=%d)\n",
                i, nodes[i].name.c_str(), nodes[i].parent_index);
        }
    }

    if (geom.triangle_count == 0) {
        std::fprintf(stderr, "no geometry to render\n");
        return 2;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr,"SDL_Init: %s\n", SDL_GetError()); return 3; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    SDL_Window* win = SDL_CreateWindow("dts-viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_SetSwapInterval(1);

    std::printf("GL_VERSION: %s\n", glGetString(GL_VERSION));

    // VAO + VBO upload
    GLuint vao = 0, vbo_pos = 0, vbo_nor = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, geom.positions.size()*sizeof(float), geom.positions.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &vbo_nor);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_nor);
    glBufferData(GL_ARRAY_BUFFER, geom.normals.size()*sizeof(float), geom.normals.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    GLuint prog = link_program(
        compile_shader(GL_VERTEX_SHADER,   VS_SRC),
        compile_shader(GL_FRAGMENT_SHADER, FS_SRC));
    GLint u_mvp        = glGetUniformLocation(prog, "u_mvp");
    GLint u_normal_mat = glGetUniformLocation(prog, "u_normal_mat");

    // ---- spec 02 bone overlay: one line per (parent->child) bone ----
    // Build a packed [pos.xyz | color.rgb] buffer at load time, using the
    // bind-pose world transforms. Each non-root node contributes one segment:
    //   start = parent.world.translation
    //   end   = node.world.translation
    GLuint bone_vao = 0, bone_vbo = 0;
    GLsizei bone_vertex_count = 0;
    GLuint bone_prog = link_program(
        compile_shader(GL_VERTEX_SHADER,   BONE_VS_SRC),
        compile_shader(GL_FRAGMENT_SHADER, BONE_FS_SRC));
    GLint u_bone_mvp = glGetUniformLocation(bone_prog, "u_mvp");
    {
        std::vector<float> bone_buf; // x,y,z, r,g,b per vertex
        const auto& nodes = loaded.nodes;
        const auto& world = loaded.world_bind;
        auto color_for = [](int idx) {
            // simple deterministic palette
            float h = (idx * 0.6180339887f);
            h = h - std::floor(h);
            // HSV->RGB with s=1, v=1
            float r=0, g=0, b=0;
            float hh = h * 6.0f;
            int i = (int)std::floor(hh);
            float f = hh - i;
            float q = 1.0f - f, t = f;
            switch (i % 6) {
                case 0: r=1; g=t; b=0; break;
                case 1: r=q; g=1; b=0; break;
                case 2: r=0; g=1; b=t; break;
                case 3: r=0; g=q; b=1; break;
                case 4: r=t; g=0; b=1; break;
                case 5: r=1; g=0; b=q; break;
            }
            return glm::vec3(r, g, b);
        };
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            int p = nodes[i].parent_index;
            if (p < 0 || p >= (int)nodes.size()) continue;
            glm::vec3 a = glm::vec3(world[p] * glm::vec4(0,0,0,1));
            glm::vec3 b = glm::vec3(world[i] * glm::vec4(0,0,0,1));
            glm::vec3 col = color_for((int)i);
            bone_buf.insert(bone_buf.end(), { a.x, a.y, a.z, col.r, col.g, col.b,
                                              b.x, b.y, b.z, col.r, col.g, col.b });
        }
        bone_vertex_count = (GLsizei)(bone_buf.size() / 6);

        glGenVertexArrays(1, &bone_vao);
        glBindVertexArray(bone_vao);
        glGenBuffers(1, &bone_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, bone_vbo);
        glBufferData(GL_ARRAY_BUFFER, bone_buf.size() * sizeof(float),
                     bone_buf.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);
        std::printf("bone overlay: %d bones (toggle with B)\n",
                    (int)(bone_vertex_count / 2));
    }
    bool show_bones = false;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    // Tribes models use left-handed coords; we don't cull faces to avoid winding surprises.
    glDisable(GL_CULL_FACE);

    // Camera setup centered on bbox
    glm::vec3 center = 0.5f * (geom.bbox_min + geom.bbox_max);
    glm::vec3 extent = geom.bbox_max - geom.bbox_min;
    float radius = 0.5f * glm::length(extent);
    if (radius < 0.001f) radius = 1.0f;

    float yaw = 0.6f, pitch = 0.35f;
    float dist = radius * 3.0f;
    bool dragging = false; int last_x = 0, last_y = 0;

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT: running = false; break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) running = false;
                    if (ev.key.keysym.sym == SDLK_b) {
                        show_bones = !show_bones;
                        std::printf("bones: %s\n", show_bones ? "on" : "off");
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) { dragging = true; last_x = ev.button.x; last_y = ev.button.y; }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT) dragging = false;
                    break;
                case SDL_MOUSEMOTION:
                    if (dragging) {
                        yaw   += (ev.motion.x - last_x) * 0.01f;
                        pitch += (ev.motion.y - last_y) * 0.01f;
                        pitch = glm::clamp(pitch, -1.5f, 1.5f);
                        last_x = ev.motion.x; last_y = ev.motion.y;
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    dist *= (ev.wheel.y > 0) ? 0.9f : 1.1f;
                    dist = glm::clamp(dist, radius * 0.2f, radius * 50.0f);
                    break;
            }
        }

        int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.12f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::vec3 eye = center + dist * glm::vec3(
            std::cos(pitch) * std::sin(yaw),
            std::sin(pitch),
            std::cos(pitch) * std::cos(yaw));

        glm::mat4 V = glm::lookAt(eye, center, glm::vec3(0,1,0));
        glm::mat4 P = glm::perspective(glm::radians(45.0f), (float)w / (float)h, radius * 0.05f, radius * 200.0f);
        glm::mat4 MVP = P * V;
        glm::mat3 N = glm::mat3(glm::transpose(glm::inverse(V)));

        glUseProgram(prog);
        glUniformMatrix4fv(u_mvp, 1, GL_FALSE, glm::value_ptr(MVP));
        glUniformMatrix3fv(u_normal_mat, 1, GL_FALSE, glm::value_ptr(N));
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(geom.positions.size() / 3));

        if (show_bones && bone_vertex_count > 0) {
            // Disable depth test so the skeleton is visible through the mesh.
            glDisable(GL_DEPTH_TEST);
            glUseProgram(bone_prog);
            glUniformMatrix4fv(u_bone_mvp, 1, GL_FALSE, glm::value_ptr(MVP));
            glBindVertexArray(bone_vao);
            glLineWidth(2.0f);
            glDrawArrays(GL_LINES, 0, bone_vertex_count);
            glEnable(GL_DEPTH_TEST);
        }

        SDL_GL_SwapWindow(win);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
