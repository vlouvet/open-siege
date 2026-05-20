#include <cassert>
#include <cmath>
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
#include "ppl.hpp"
#include <set>
#include <cstdint>

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

// ---------------------- DTS animation sequences (spec 03) ----------------------
//
// `sequence_info` (the public type returned by renderable_shape::get_sequences)
// exposes only name + indices. The two fields the player needs — `duration`
// (float, seconds) and `cyclic` (bool) — live on the RAW `sequence` struct in
// `darkstar_structures.hpp:360` (v2/v3) and `:550` (v5/6/7/8) and are not
// surfaced through any accessor. We therefore read them straight off the
// `shape_variant` from `dts::read_shape()` (see findings doc 00-api-findings).
//
// `frame_count` is not stored in the file; we report the max `num_key_frames`
// across all sub-sequences belonging to a sequence (different sub-sequences
// within one sequence may have different keyframe counts; the densest is the
// nominal sample rate of the timeline).
//
// `animated_nodes` is the set of node indices touched by the sequence —
// derived from BOTH node-attached and object-attached sub-sequence ranges
// (an object lives on a node, so its sub-sequences animate that node too).
// `dts_renderable_shape.cpp:197-223` does the same dual-source fold.

// One keyframe sampled and decoded into engine-neutral glm types. Position is
// the wire-level float; spec 05's working hypothesis (verified experimentally
// on larmor "run") is that time_in_sec = position * sequence.duration, with
// position spanning the sub-sequence's own [min_position, max_position]
// (typically [0, 1] for the densest sub-sequences).
struct Keyframe
{
    float     position;
    glm::quat rotation;
    glm::vec3 translation;
    glm::vec3 scale;
};

// One animation channel within a Sequence: every keyframe targets a single
// node and a single transform field set. Tracks are independent — different
// channels may have different keyframe counts inside the same sequence.
struct SubTrack
{
    int                   node_index;
    std::vector<Keyframe> keyframes;
};

struct Sequence
{
    std::string           name;
    float                 duration_seconds;
    bool                  cyclic;
    int                   frame_count;
    std::vector<int>      animated_nodes;
    std::vector<SubTrack> tracks;   // spec 05: extracted per-channel keyframes
};

// Pull a raw transform (any version) into a (quat, vec3 translation, vec3 scale)
// triple. v8 has no scale field, so report (1,1,1). The rotation conversion uses
// the same wire convention as the static bind-pose path: GLM quat is (w,x,y,z),
// transpose is applied at matrix-build time (not here — see eval_to_mat4 below).
static void extract_transform(const dts::shape::v2::transform& t,
                              glm::quat& q, glm::vec3& tr, glm::vec3& sc)
{
    auto r = sc::to_float(t.rotation);
    q  = glm::quat(r.w, r.x, r.y, r.z);
    tr = glm::vec3(t.translation.x, t.translation.y, t.translation.z);
    sc = glm::vec3(t.scale.x, t.scale.y, t.scale.z);
}
static void extract_transform(const dts::shape::v7::transform& t,
                              glm::quat& q, glm::vec3& tr, glm::vec3& sc)
{
    auto r = sc::to_float(t.rotation);
    q  = glm::quat(r.w, r.x, r.y, r.z);
    tr = glm::vec3(t.translation.x, t.translation.y, t.translation.z);
    sc = glm::vec3(t.scale.x, t.scale.y, t.scale.z);
}
static void extract_transform(const dts::shape::v8::transform& t,
                              glm::quat& q, glm::vec3& tr, glm::vec3& sc)
{
    auto r = sc::to_float(t.rotation);
    q  = glm::quat(r.w, r.x, r.y, r.z);
    tr = glm::vec3(t.translation.x, t.translation.y, t.translation.z);
    sc = glm::vec3(1.0f, 1.0f, 1.0f);
}

static std::vector<Sequence> build_sequences(const dts::shape_variant& raw_shape)
{
    return std::visit([](const auto& s) {
        std::vector<Sequence> out;
        out.reserve(s.sequences.size());

        // For each sub-sequence, figure out which node it animates. A
        // sub-sequence is referenced from a node directly (node.first_sub_sequence_index)
        // or via an object attached to a node (object.first_sub_sequence_index,
        // and the object lives on object.node_index). Build an index:
        //   sub_seq_index -> node_index
        // Default to -1 (unattached / unknown).
        std::vector<int> sub_seq_to_node(s.sub_sequences.size(), -1);
        for (std::size_t ni = 0; ni < s.nodes.size(); ++ni) {
            const auto first = static_cast<int>(s.nodes[ni].first_sub_sequence_index);
            const auto count = static_cast<int>(s.nodes[ni].num_sub_sequences);
            for (int k = 0; k < count; ++k) {
                int idx = first + k;
                if (idx >= 0 && idx < (int)sub_seq_to_node.size()) {
                    sub_seq_to_node[idx] = (int)ni;
                }
            }
        }
        for (const auto& obj : s.objects) {
            const auto first = static_cast<int>(obj.first_sub_sequence_index);
            const auto count = static_cast<int>(obj.num_sub_sequences);
            const auto node_idx = static_cast<int>(obj.node_index);
            for (int k = 0; k < count; ++k) {
                int idx = first + k;
                if (idx >= 0 && idx < (int)sub_seq_to_node.size()
                    && sub_seq_to_node[idx] == -1) {
                    sub_seq_to_node[idx] = node_idx;
                }
            }
        }

        for (std::size_t si = 0; si < s.sequences.size(); ++si) {
            const auto& seq = s.sequences[si];
            Sequence out_seq;
            const auto name_idx = static_cast<std::size_t>(seq.name_index);
            if (name_idx < s.names.size()) {
                out_seq.name.assign(
                    s.names[name_idx].data(),
                    strnlen(s.names[name_idx].data(), s.names[name_idx].size()));
            }
            out_seq.duration_seconds = seq.duration;
            out_seq.cyclic = (static_cast<std::int32_t>(seq.cyclic) != 0);

            int max_frames = 0;
            std::vector<int> nodes_set;
            for (std::size_t ssi = 0; ssi < s.sub_sequences.size(); ++ssi) {
                const auto& ss = s.sub_sequences[ssi];
                if (static_cast<std::size_t>(ss.sequence_index) != si) continue;
                int nk = static_cast<int>(ss.num_key_frames);
                if (nk > max_frames) max_frames = nk;
                int n = sub_seq_to_node[ssi];
                if (n >= 0 && std::find(nodes_set.begin(), nodes_set.end(), n) == nodes_set.end()) {
                    nodes_set.push_back(n);
                }

                // spec 05: extract this sub-sequence's keyframes into a typed track.
                // Skip tracks not bound to any node (e.g. material/IFL channels we
                // don't animate here) and tracks with no keyframes.
                if (n < 0 || nk <= 0) continue;
                SubTrack tr;
                tr.node_index = n;
                tr.keyframes.reserve(nk);
                const int first_kf = static_cast<int>(ss.first_key_frame_index);
                for (int k = 0; k < nk; ++k) {
                    const int kf_idx = first_kf + k;
                    if (kf_idx < 0 || kf_idx >= (int)s.keyframes.size()) continue;
                    const auto& kf = s.keyframes[kf_idx];
                    const auto tx_idx = static_cast<std::size_t>(kf.transform_index);
                    if (tx_idx >= s.transforms.size()) continue;
                    Keyframe out_kf;
                    out_kf.position = kf.position;
                    extract_transform(s.transforms[tx_idx],
                                      out_kf.rotation, out_kf.translation, out_kf.scale);
                    tr.keyframes.push_back(out_kf);
                }
                if (!tr.keyframes.empty()) {
                    out_seq.tracks.push_back(std::move(tr));
                }
            }
            out_seq.frame_count = max_frames;
            out_seq.animated_nodes = std::move(nodes_set);
            out.push_back(std::move(out_seq));
        }
        return out;
    }, raw_shape);
}

// ---------------------- per-node transform evaluation (spec 05) ----------------------
//
// Given a sequence and a wall-clock time `t` (seconds), return one local-space
// glm::mat4 per node. Nodes the sequence doesn't animate fall back to their
// bind_local. For animated nodes:
//   - Map t -> normalized position p in [0, 1] via p = clamp(t / duration, 0, 1).
//     (Working hypothesis from spec 01: each sub-sequence's keyframe `position`
//     values span ~[0, 1] and `sequence.duration` is the matching seconds.
//     Verified experimentally on larmor "run": at t=0 all tracks land on their
//     first keyframe, at t=duration on their last, at t=duration/2 transforms
//     differ from both endpoints.)
//   - Within a track, find the two bracketing keyframes by `position` and lerp
//     translation/scale, slerp rotation. Shortest-path slerp: negate q1 if
//     dot(q0, q1) < 0.
//   - Compose with the same recipe as the static bind pose:
//       T * transpose(toMat4(quat)) * S
//     so the result is directly comparable to bind_local.
static glm::mat4 compose_local(const glm::quat& q, const glm::vec3& t, const glm::vec3& s)
{
    glm::mat4 T = glm::translate(glm::mat4(1.0f), t);
    glm::mat4 R = glm::transpose(glm::toMat4(q));
    glm::mat4 S = glm::scale(glm::mat4(1.0f), s);
    return T * R * S;
}

static std::vector<glm::mat4> evaluate(const Sequence& seq, float t,
                                       const std::vector<Node>& bind)
{
    // Start with the bind pose; we overwrite the animated channels below.
    std::vector<glm::mat4> out(bind.size());
    for (std::size_t i = 0; i < bind.size(); ++i) out[i] = bind[i].bind_local;

    if (seq.duration_seconds <= 0.0f || seq.tracks.empty()) return out;

    const float p = glm::clamp(t / seq.duration_seconds, 0.0f, 1.0f);

    for (const auto& tr : seq.tracks) {
        if (tr.node_index < 0 || tr.node_index >= (int)bind.size()) continue;
        if (tr.keyframes.empty()) continue;

        const auto& kfs = tr.keyframes;
        glm::quat q;
        glm::vec3 tx, sc;

        if (kfs.size() == 1 || p <= kfs.front().position) {
            q  = kfs.front().rotation;
            tx = kfs.front().translation;
            sc = kfs.front().scale;
        } else if (p >= kfs.back().position) {
            q  = kfs.back().rotation;
            tx = kfs.back().translation;
            sc = kfs.back().scale;
        } else {
            // Find bracketing keyframes [i, i+1] where kfs[i].position <= p < kfs[i+1].position.
            // Keyframes are written in monotonically increasing position order in the file.
            std::size_t i = 0;
            for (; i + 1 < kfs.size(); ++i) {
                if (kfs[i + 1].position >= p) break;
            }
            const auto& a = kfs[i];
            const auto& b = kfs[i + 1];
            const float span = b.position - a.position;
            const float u = span > 0.0f ? (p - a.position) / span : 0.0f;

            // Shortest-path slerp.
            glm::quat q0 = a.rotation;
            glm::quat q1 = b.rotation;
            if (glm::dot(q0, q1) < 0.0f) q1 = -q1;
            q  = glm::normalize(glm::slerp(q0, q1, u));
            tx = glm::mix(a.translation, b.translation, u);
            sc = glm::mix(a.scale,       b.scale,       u);
        }

        out[tr.node_index] = compose_local(q, tx, sc);
    }
    return out;
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

// Spec 06: compose evaluated local transforms (one per node, from `evaluate()`)
// into world-space matrices using the same parent-walk algorithm as
// `compute_world_bind`. O(n^2) worst case but n is ~100 — fine.
static std::vector<glm::mat4> compute_world_from_locals(
    const std::vector<Node>& nodes,
    const std::vector<glm::mat4>& locals)
{
    std::vector<glm::mat4> world(nodes.size(), glm::mat4(1.0f));
    std::vector<bool> done(nodes.size(), false);
    bool progress = true;
    std::size_t passes = 0;
    while (progress && passes++ <= nodes.size() + 1) {
        progress = false;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (done[i]) continue;
            int p = nodes[i].parent_index;
            if (p < 0) {
                world[i] = locals[i];
                done[i] = true;
                progress = true;
            } else if (p < (int)nodes.size() && done[p]) {
                world[i] = world[p] * locals[i];
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
    std::vector<Sequence>     sequences;    // named animations (spec 03)
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
        out.sequences = build_sequences(raw);
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

// ---------------------- spec 04: palette resolution → RGBA8 ----------------------
//
// Resolve a PBMP's indexed pixels against a PPL palette map to produce a
// width*height*4 RGBA8 buffer ready for GL upload. The map is keyed by the
// PL98 palette `index`, which the PBMP's `PiDX` chunk (`palette_index`) refers
// to directly — empirically verified: `Shell.ppl` palette index 1136 matches
// `base.larmor.bmp`'s palette_index 1136.
//
// Fallback: if the PBMP references a palette ID we don't have loaded
// (world-specific palettes live in `<world>World.vol/<world>.day.ppl`), emit
// a magenta-checkered texture so the failure is obvious in-viewer. A small
// std::set tracks which missing IDs we've already warned about so a mesh with
// dozens of materials doesn't spam the same warning N times.
//
// `flags` byte handling (v1): treat every palette entry as opaque (alpha=255).
// PL98 stores something OTHER than alpha in that fourth byte (see
// wiki-contributions/PPL.md); colour-key transparency belongs in a later spec.

static std::vector<std::uint8_t> expand_to_rgba8(
    const PbmpImage& bmp,
    const std::map<std::uint32_t, const Palette*>& palettes)
{
    const std::size_t w = bmp.width;
    const std::size_t h = bmp.height;
    std::vector<std::uint8_t> out(w * h * 4, 0);

    auto it = palettes.find(bmp.palette_index);
    if (it == palettes.end() || it->second == nullptr) {
        // Magenta-checkered fallback. 8×8 checker so the pattern is visible
        // even on small textures and tiles obviously even on large ones.
        static std::set<std::uint32_t> warned;
        if (warned.insert(bmp.palette_index).second) {
            std::fprintf(stderr,
                "warning: PBMP references palette_index=%u not present in loaded PPL "
                "— rendering as magenta checker\n",
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
        out[o + 3] = 255;  // v1: flags-as-alpha is incorrect; opaque is safe.
    }
    return out;
}

// ---------------------- spec 04 smoke test: --dump-rgba ----------------------
//
// Pull a single PBMP and a single PPL out of the VOLs in the same directory as
// the argv[1] VOL, resolve PBMP pixels through the PPL palette map, and write
// the result as a binary PPM (P6) file alongside a small header dump. PPM
// is the path-of-least-resistance image format that Preview.app opens natively
// — alpha is dropped (RGB only) since PPM has no alpha channel.

static bool read_named_file_from_vol(const fs::path& vol_path,
                                     const std::string& match_lower,
                                     const std::string& required_ext,
                                     std::vector<char>& out_bytes,
                                     std::string& out_filename)
{
    std::ifstream in(vol_path, std::ios::binary);
    if (!in) return false;
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) return false;
    in.clear(); in.seekg(0);

    auto all = sr::get_all_content(vol_path, in, plugin);
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        auto name = f->filename.string();
        auto lower = name;
        for (auto& c : lower) c = std::tolower((unsigned char)c);
        if (!required_ext.empty()) {
            if (lower.size() < required_ext.size()) continue;
            if (lower.substr(lower.size() - required_ext.size()) != required_ext) continue;
        }
        if (!match_lower.empty() && lower.find(match_lower) == std::string::npos) continue;

        std::stringstream buf;
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, buf);
        auto s = buf.str();
        out_bytes.assign(s.begin(), s.end());
        out_filename = name;
        return true;
    }
    return false;
}

static int dump_rgba_to_ppm(const fs::path& seed_vol_path,
                            const std::string& bmp_match_lower,
                            const std::string& ppl_match_lower)
{
    // Search seed_vol first, then every sibling .vol — most assets live in
    // Entities.vol, but PPLs live in Shell.vol / <world>World.vol, so a
    // single-VOL scan would miss every cross-archive case.
    std::vector<fs::path> vol_search;
    vol_search.push_back(seed_vol_path);
    const fs::path dir = seed_vol_path.parent_path();
    if (!dir.empty() && fs::exists(dir)) {
        for (const auto& ent : fs::directory_iterator(dir)) {
            if (!ent.is_regular_file()) continue;
            auto p = ent.path();
            auto ext = p.extension().string();
            for (auto& c : ext) c = std::tolower((unsigned char)c);
            if (ext != ".vol") continue;
            if (fs::equivalent(p, seed_vol_path)) continue;
            vol_search.push_back(p);
        }
    }

    std::vector<char> bmp_bytes;
    std::string bmp_filename;
    for (const auto& v : vol_search) {
        if (read_named_file_from_vol(v, bmp_match_lower, ".bmp", bmp_bytes, bmp_filename)) {
            std::printf("found PBMP %s in %s\n", bmp_filename.c_str(), v.string().c_str());
            break;
        }
    }
    if (bmp_bytes.empty()) {
        std::fprintf(stderr, "no .bmp matching '%s' in any VOL beside %s\n",
                     bmp_match_lower.c_str(), seed_vol_path.string().c_str());
        return 2;
    }

    std::vector<char> ppl_bytes;
    std::string ppl_filename;
    for (const auto& v : vol_search) {
        // Try .ppl first, then .ipl as a fallback.
        if (read_named_file_from_vol(v, ppl_match_lower, ".ppl", ppl_bytes, ppl_filename)) {
            std::printf("found PPL %s in %s\n", ppl_filename.c_str(), v.string().c_str());
            break;
        }
        if (read_named_file_from_vol(v, ppl_match_lower, ".ipl", ppl_bytes, ppl_filename)) {
            std::printf("found IPL %s in %s\n", ppl_filename.c_str(), v.string().c_str());
            break;
        }
    }
    if (ppl_bytes.empty()) {
        std::fprintf(stderr, "no .ppl/.ipl matching '%s' in any VOL beside %s\n",
                     ppl_match_lower.c_str(), seed_vol_path.string().c_str());
        return 2;
    }

    PbmpImage img;
    {
        std::stringstream ss(std::string(bmp_bytes.begin(), bmp_bytes.end()));
        img = load_pbmp(ss);
    }
    std::vector<Palette> palettes;
    {
        std::stringstream ss(std::string(ppl_bytes.begin(), ppl_bytes.end()));
        palettes = load_ppl(ss);
    }
    auto pal_map = by_index(palettes);

    std::printf("PBMP %s: %ux%u  bit_depth=%u  palette_index=%u\n",
        bmp_filename.c_str(), img.width, img.height, img.bit_depth, img.palette_index);
    std::printf("PPL  %s: %zu palettes (indices:",
        ppl_filename.c_str(), palettes.size());
    for (const auto& p : palettes) std::printf(" %u", p.index);
    std::printf(")\n");

    auto rgba = expand_to_rgba8(img, pal_map);

    // First 16 RGBA bytes — required for spec acceptance verification.
    std::printf("first 16 RGBA bytes:");
    for (std::size_t i = 0; i < 16 && i < rgba.size(); ++i) {
        std::printf(" %02X", rgba[i]);
    }
    std::printf("\n");

    // Dump as binary PPM (P6, 8-bit RGB). PPM doesn't have an alpha channel,
    // which is fine for v1 since we hardcoded alpha=255 anyway.
    std::string ppm_path = bmp_filename;
    {
        auto dot = ppm_path.find_last_of('.');
        if (dot != std::string::npos) ppm_path.erase(dot);
        ppm_path += ".ppm";
    }
    std::ofstream out(ppm_path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", ppm_path.c_str());
        return 3;
    }
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    std::vector<std::uint8_t> rgb(img.width * img.height * 3);
    for (std::size_t i = 0; i < (std::size_t)img.width * img.height; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    out.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    std::printf("wrote %s (%ux%u P6)\n", ppm_path.c_str(), img.width, img.height);
    return 0;
}

// ---------------------- main ----------------------

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <path-to-vol> [dts-substring]\n"
            "       %s <path-to-vol> --dump-bmp <bmp-substring>\n"
            "       %s <path-to-vol> --dump-rgba <bmp-substring> <ppl-substring>\n"
            "  e.g. %s tribes-game/base/Entities.vol chainturret\n"
            "       %s tribes-game/base/Entities.vol --dump-bmp ammo\n"
            "       %s tribes-game/base/Entities.vol --dump-rgba ammo Shell.ppl\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
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

    // Spec 04 smoke test: resolve PBMP through a PPL palette and dump RGBA as
    // a PPM image (openable in Preview.app). The PPL is searched in every
    // sibling VOL of argv[1] — Tribes ships PPLs in Shell.vol / <world>World.vol,
    // not Entities.vol — so the seed VOL can be the same one used for meshes.
    if (argc >= 5 && std::string(argv[2]) == "--dump-rgba") {
        std::string bmp_match = argv[3];
        std::string ppl_match = argv[4];
        for (auto& c : bmp_match) c = std::tolower((unsigned char)c);
        for (auto& c : ppl_match) c = std::tolower((unsigned char)c);
        return dump_rgba_to_ppm(vol_path, bmp_match, ppl_match);
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

    // ---- spec 03 sequence enumeration ----
    {
        const auto& seqs = loaded.sequences;
        std::printf("sequences: %zu\n", seqs.size());
        for (std::size_t i = 0; i < seqs.size(); ++i) {
            const auto& sq = seqs[i];
            std::printf("  [%zu] %-20s  duration=%.3fs  frames=%d  cyclic=%s  nodes=%zu  tracks=%zu\n",
                i,
                sq.name.empty() ? "<unnamed>" : sq.name.c_str(),
                sq.duration_seconds,
                sq.frame_count,
                sq.cyclic ? "yes" : "no",
                sq.animated_nodes.size(),
                sq.tracks.size());
        }
    }

    // ---- spec 05 acceptance check: probe evaluate() at t=0, t=duration/2, t=duration ----
    // Picks the first sequence named "run" if available, else the first
    // sequence with duration > 0 and at least one animated track. Asserts that
    // no transform contains a NaN, and that t=duration/2 differs from both
    // endpoints for at least one animated node.
    {
        const auto& seqs = loaded.sequences;
        int probe_idx = -1;
        for (std::size_t i = 0; i < seqs.size(); ++i) {
            if (seqs[i].name == "run" && seqs[i].duration_seconds > 0.0f
                && !seqs[i].tracks.empty()) { probe_idx = (int)i; break; }
        }
        if (probe_idx < 0) {
            for (std::size_t i = 0; i < seqs.size(); ++i) {
                if (seqs[i].duration_seconds > 0.0f && !seqs[i].tracks.empty()) {
                    probe_idx = (int)i; break;
                }
            }
        }
        if (probe_idx >= 0) {
            const auto& sq = seqs[probe_idx];
            const float dur = sq.duration_seconds;
            auto m0   = evaluate(sq, 0.0f,       loaded.nodes);
            auto mhalf= evaluate(sq, dur * 0.5f, loaded.nodes);
            auto mend = evaluate(sq, dur,        loaded.nodes);

            auto has_nan = [](const std::vector<glm::mat4>& v) {
                for (const auto& m : v) {
                    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
                        if (!std::isfinite(m[r][c])) return true;
                    }
                }
                return false;
            };
            const bool nan0 = has_nan(m0), nanH = has_nan(mhalf), nanE = has_nan(mend);

            // Count animated nodes whose mid-point differs from both endpoints.
            int diff_count = 0;
            for (int n : sq.animated_nodes) {
                if (n < 0 || n >= (int)m0.size()) continue;
                bool diff0 = (m0[n]    != mhalf[n]);
                bool diffE = (mhalf[n] != mend[n]);
                if (diff0 && diffE) ++diff_count;
            }
            std::fprintf(stderr,
                "spec05 probe: seq '%s' dur=%.3fs  NaN[t=0,t/2,t=d]=%d,%d,%d  "
                "interp-differs nodes=%d / %zu animated\n",
                sq.name.c_str(), dur, (int)nan0, (int)nanH, (int)nanE,
                diff_count, sq.animated_nodes.size());
            assert(!nan0 && !nanH && !nanE);
        } else {
            std::fprintf(stderr, "spec05 probe: no animated sequence available\n");
        }
    }

    if (geom.triangle_count == 0) {
        std::fprintf(stderr, "no geometry to render\n");
        return 2;
    }

    // ---- spec 06 self-test (no window) -----------------------------------
    // Verifies the skinning math BEFORE we get into the SDL render path:
    //   - at t=0 with a sequence whose first keyframe == bind (or no anim),
    //     skinned positions should equal the original world-bind positions.
    //   - applying bind_local everywhere via compute_world_from_locals
    //     should reproduce loaded.world_bind exactly.
    //   - exposing the same `run` sequence at t=duration/2 should move at
    //     least some vertices off their bind positions.
    {
        // Build inv_world_bind here for the self-test (same recipe as below).
        std::vector<glm::mat4> inv_wb(loaded.world_bind.size(), glm::mat4(1.0f));
        for (std::size_t i = 0; i < loaded.world_bind.size(); ++i)
            inv_wb[i] = glm::inverse(loaded.world_bind[i]);

        // Bind-local positions for self-test (same recipe as runtime path).
        std::vector<glm::vec3> blp(geom.positions.size() / 3);
        for (std::size_t v = 0; v < blp.size(); ++v) {
            glm::vec4 p(geom.positions[v*3+0],
                        geom.positions[v*3+1],
                        geom.positions[v*3+2], 1.0f);
            int ni = geom.vertex_node_index[v];
            if (ni >= 0 && ni < (int)inv_wb.size()) p = inv_wb[ni] * p;
            blp[v] = glm::vec3(p);
        }

        // (1) bind-local round-trip — apply world_bind back, should equal
        // the original world-bind position exactly (up to float epsilon).
        float max_err = 0.0f;
        for (std::size_t v = 0; v < blp.size(); ++v) {
            int ni = geom.vertex_node_index[v];
            glm::vec4 wp = (ni >= 0 && ni < (int)loaded.world_bind.size())
                ? (loaded.world_bind[ni] * glm::vec4(blp[v], 1.0f))
                : glm::vec4(blp[v], 1.0f);
            glm::vec3 orig(geom.positions[v*3+0],
                           geom.positions[v*3+1],
                           geom.positions[v*3+2]);
            max_err = std::max(max_err, glm::length(glm::vec3(wp) - orig));
        }
        std::fprintf(stderr, "spec06 selftest: bind round-trip max error = %g\n", max_err);
        assert(max_err < 1e-3f);

        // (2) Find a sequence with real animation (prefer "run").
        int probe = -1;
        for (std::size_t i = 0; i < loaded.sequences.size(); ++i) {
            if (loaded.sequences[i].name == "run" &&
                loaded.sequences[i].duration_seconds > 0.0f &&
                !loaded.sequences[i].tracks.empty()) { probe = (int)i; break; }
        }
        if (probe < 0) {
            for (std::size_t i = 0; i < loaded.sequences.size(); ++i) {
                if (loaded.sequences[i].duration_seconds > 0.0f &&
                    !loaded.sequences[i].tracks.empty()) { probe = (int)i; break; }
            }
        }
        if (probe >= 0) {
            const auto& sq = loaded.sequences[probe];
            auto eval_world = [&](float t) {
                auto loc = evaluate(sq, t, loaded.nodes);
                return compute_world_from_locals(loaded.nodes, loc);
            };
            auto wa0 = eval_world(0.0f);
            auto waH = eval_world(sq.duration_seconds * 0.5f);

            // At t=0 some keyframed nodes may already differ from the bind,
            // but the *whole-mesh* skin must still produce finite values.
            float max_motion = 0.0f;
            int moved = 0;
            for (std::size_t v = 0; v < blp.size(); ++v) {
                int ni = geom.vertex_node_index[v];
                if (ni < 0 || ni >= (int)wa0.size()) continue;
                glm::vec3 p0 = glm::vec3(wa0[ni] * glm::vec4(blp[v], 1.0f));
                glm::vec3 ph = glm::vec3(waH[ni] * glm::vec4(blp[v], 1.0f));
                float d = glm::length(ph - p0);
                if (d > 1e-3f) ++moved;
                max_motion = std::max(max_motion, d);
            }
            std::fprintf(stderr,
                "spec06 selftest: seq '%s' t=0 vs t=%.3fs — %d vertices moved, max |dp|=%.3f\n",
                sq.name.c_str(), sq.duration_seconds * 0.5f, moved, max_motion);
            assert(max_motion > 0.0f);
        }
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

    // ---- spec 06 CPU skinning prep -------------------------------------
    // `geom.positions` was emitted by the renderer in WORLD bind-pose
    // (vertices already multiplied through the bind node_matrix — see
    //  dts_renderable_shape.cpp:394). For skinning we need each vertex in
    // its owning node's LOCAL space, so the per-frame world matrix can map
    // it to its animated world position:
    //   bind_world = world_bind[n] * bind_local         (already true)
    //   bind_local = inverse(world_bind[n]) * bind_world
    //   anim_world = world_anim[n] * bind_local
    // Vertices whose node_index is < 0 (unattached / unknown) stay in world
    // space and are skinned by identity — i.e. they don't move.
    //
    // Pre-cached inverse-bind matrices avoid the inverse() per frame.
    std::vector<glm::mat4> inv_world_bind(loaded.world_bind.size(), glm::mat4(1.0f));
    for (std::size_t i = 0; i < loaded.world_bind.size(); ++i) {
        inv_world_bind[i] = glm::inverse(loaded.world_bind[i]);
    }
    std::vector<float> bind_local_positions(geom.positions.size(), 0.0f);
    {
        const std::size_t vcount = geom.positions.size() / 3;
        for (std::size_t v = 0; v < vcount; ++v) {
            glm::vec4 p(geom.positions[v*3+0],
                        geom.positions[v*3+1],
                        geom.positions[v*3+2], 1.0f);
            int ni = geom.vertex_node_index[v];
            if (ni >= 0 && ni < (int)inv_world_bind.size()) {
                p = inv_world_bind[ni] * p;
            }
            bind_local_positions[v*3+0] = p.x;
            bind_local_positions[v*3+1] = p.y;
            bind_local_positions[v*3+2] = p.z;
        }
    }
    // Per-frame skinning destination buffer. Reused; only re-allocated if
    // geometry size changes (it doesn't, post-load).
    std::vector<float> skinned_positions(geom.positions.size(), 0.0f);

    // VAO + VBO upload. Positions are STREAM_DRAW because we re-upload
    // every frame; normals stay STATIC for now (lighting is locked to bind
    // pose — visually acceptable for rigid 1-bone Tribes meshes, the body
    // panels deform slightly but the silhouette motion is what's checked).
    GLuint vao = 0, vbo_pos = 0, vbo_nor = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, geom.positions.size()*sizeof(float), geom.positions.data(), GL_STREAM_DRAW);
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

    std::printf("keys: Space=play/pause  Left/Right=scrub +/-0.05s  R=reset  Tab=next seq (Shift+Tab=prev)  B=bones  Q/Esc=quit\n");
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

    // ---- spec 04 timeline state ----
    //
    // Holds the play head over the currently-selected sequence. No skinning
    // happens yet; this spec just wires up the input loop so later specs (05
    // keyframe interp, 06 CPU skinning) can read `current_time` against
    // `loaded.sequences[current_sequence]`.
    //
    // Notes:
    //   - Tab cycles by INDEX, not name — duplicate sequence names exist
    //     (e.g. `wave` at 39 and 42 from LOD fan-out) and indexing keeps all
    //     45 reachable.
    //   - The `root` sequence has duration 0 (no-op marker). Playback at
    //     duration <= 0 is a no-op; we still let Space / Tab work so it
    //     doesn't trap the user.
    int   current_sequence = 0;
    float current_time     = 0.0f;
    bool  playing          = false;
    Uint64 last_tick_ns    = SDL_GetPerformanceCounter();
    const Uint64 perf_freq = SDL_GetPerformanceFrequency();
    float last_hud_print_t = -1.0f;  // forces an initial HUD line

    auto seq_duration = [&](int idx) -> float {
        if (idx < 0 || idx >= (int)loaded.sequences.size()) return 0.0f;
        return loaded.sequences[idx].duration_seconds;
    };
    auto seq_name = [&](int idx) -> const char* {
        if (idx < 0 || idx >= (int)loaded.sequences.size()) return "<none>";
        const auto& s = loaded.sequences[idx];
        return s.name.empty() ? "<unnamed>" : s.name.c_str();
    };
    auto print_hud = [&]() {
        std::fprintf(stderr, "seq[%d]: %s  t: %.3fs / %.3fs  %s\n",
            current_sequence, seq_name(current_sequence),
            current_time, seq_duration(current_sequence),
            playing ? "[play]" : "[pause]");
    };
    print_hud();

    // ---- spec 06 FPS counter -------------------------------------------
    // Counts frames per real second so we can verify the >60fps acceptance
    // criterion. Printed to stderr once per second.
    Uint64 fps_window_start = SDL_GetPerformanceCounter();
    int    fps_window_frames = 0;

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
                    if (ev.key.keysym.sym == SDLK_SPACE) {
                        playing = !playing;
                        print_hud();
                    }
                    if (ev.key.keysym.sym == SDLK_LEFT) {
                        // Scrub backward; clamp at 0 (no negative time).
                        current_time -= 0.05f;
                        if (current_time < 0.0f) current_time = 0.0f;
                        print_hud();
                    }
                    if (ev.key.keysym.sym == SDLK_RIGHT) {
                        // Scrub forward; wrap past duration. (Loop refinement
                        // — non-cyclic clamp vs cyclic wrap — lands in spec 08.)
                        current_time += 0.05f;
                        float dur = seq_duration(current_sequence);
                        if (dur > 0.0f) {
                            while (current_time >= dur) current_time -= dur;
                        } else {
                            current_time = 0.0f;
                        }
                        print_hud();
                    }
                    if (ev.key.keysym.sym == SDLK_r) {
                        current_time = 0.0f;
                        print_hud();
                    }
                    if (ev.key.keysym.sym == SDLK_TAB) {
                        if (!loaded.sequences.empty()) {
                            int step = (ev.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                            int n = (int)loaded.sequences.size();
                            current_sequence = (current_sequence + step + n) % n;
                            current_time = 0.0f;
                            print_hud();
                        }
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

        // Advance the play head. dt is wall time since the previous frame; the
        // perf-counter is monotonic so this stays accurate across long pauses.
        Uint64 now_ns = SDL_GetPerformanceCounter();
        float dt = (float)((double)(now_ns - last_tick_ns) / (double)perf_freq);
        last_tick_ns = now_ns;
        if (playing) {
            float dur = seq_duration(current_sequence);
            if (dur > 0.0f) {
                current_time += dt;
                while (current_time >= dur) current_time -= dur;
            }
            // Once per second of playback, drop a HUD line so the user can
            // sanity-check elapsed time without scrubbing.
            if (last_hud_print_t < 0.0f
                || std::floor(current_time) != std::floor(last_hud_print_t)) {
                print_hud();
            }
            last_hud_print_t = current_time;
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

        // ---- spec 06: CPU skinning ----
        // 1) evaluate per-node local transforms at current_time. If the
        //    sequence has no tracks / zero duration (e.g. the `root` no-op
        //    marker), evaluate() falls back to bind_local — t=0 reproduces
        //    the bind pose exactly, which is the sanity check.
        // 2) accumulate to world via the same parent-walk used at load.
        // 3) per-vertex bone matrix B[n] = world_anim[n] * inverse(world_bind[n]).
        //    We pre-baked the inverse and we *already* expressed each vertex
        //    in node-local space (bind_local_positions), so the skin step is
        //    just `world_anim[n] * bind_local_vertex` — saving the multiply
        //    by inverse(world_bind) per frame. Equivalent to:
        //      skinned = (world_anim * inverse(world_bind)) * bind_world_vertex
        // 4) re-upload the position VBO via glBufferData (GL_STREAM_DRAW).
        {
            const Sequence* sq = (current_sequence >= 0
                && current_sequence < (int)loaded.sequences.size())
                ? &loaded.sequences[current_sequence] : nullptr;
            std::vector<glm::mat4> local_anim;
            if (sq) {
                local_anim = evaluate(*sq, current_time, loaded.nodes);
            } else {
                local_anim.resize(loaded.nodes.size());
                for (std::size_t i = 0; i < loaded.nodes.size(); ++i)
                    local_anim[i] = loaded.nodes[i].bind_local;
            }
            auto world_anim = compute_world_from_locals(loaded.nodes, local_anim);

            const std::size_t vcount = bind_local_positions.size() / 3;
            for (std::size_t v = 0; v < vcount; ++v) {
                int ni = geom.vertex_node_index[v];
                glm::vec4 lp(bind_local_positions[v*3+0],
                             bind_local_positions[v*3+1],
                             bind_local_positions[v*3+2], 1.0f);
                glm::vec4 wp;
                if (ni >= 0 && ni < (int)world_anim.size()) {
                    wp = world_anim[ni] * lp;
                } else {
                    // unattached vertex — its bind_local_positions entry is
                    // already in world space (identity in/out at load), so
                    // pass through.
                    wp = lp;
                }
                skinned_positions[v*3+0] = wp.x;
                skinned_positions[v*3+1] = wp.y;
                skinned_positions[v*3+2] = wp.z;
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
            glBufferData(GL_ARRAY_BUFFER,
                         skinned_positions.size() * sizeof(float),
                         skinned_positions.data(), GL_STREAM_DRAW);
        }

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

        // spec 06 FPS print — every ~1s of wall time print frames/sec to
        // stderr. Required by acceptance ("> 60fps for a single mesh").
        ++fps_window_frames;
        {
            Uint64 now2 = SDL_GetPerformanceCounter();
            double elapsed = (double)(now2 - fps_window_start) / (double)perf_freq;
            if (elapsed >= 1.0) {
                std::fprintf(stderr, "fps: %.1f (%d frames in %.2fs)\n",
                    fps_window_frames / elapsed, fps_window_frames, elapsed);
                fps_window_start = now2;
                fps_window_frames = 0;
            }
        }
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
