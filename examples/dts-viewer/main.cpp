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
#include "gl_includes.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include "resources/darkstar_volume.hpp"
#include "content/renderable_shape.hpp"
#include "content/dts/renderable_shape_factory.hpp"
#include "content/dts/darkstar.hpp"
#include "content/dts/darkstar_structures.hpp"
#include "content/dml/dml.hpp"

#include "pbmp.hpp"
#include "ppl.hpp"
#include "materials.hpp"
#include "interior_renderer.hpp"
#include "terrain_renderer.hpp"
#include "content/dig/dig.hpp"
#include "content/dis/dis.hpp"
#include "content/dml/dml.hpp"
#include "content/interior/dil.hpp"
#include "content/terrain/dtf.hpp"
#include "content/terrain/dtb.hpp"
#include "mission_loader.hpp"
#include "camera.hpp"
#include "height_sampler.hpp"
#include "mission_bounds.hpp"
#include "walk_camera.hpp"
#include "skybox_renderer.hpp"
#include "particles.hpp"
#include "lighting.hpp"
#include "terrain_textures.hpp"
#include "hud.hpp"
#include "screenshot.hpp"
#include "mission_switcher.hpp"
#include "player_controller.hpp"
#include "audio.hpp"
#include "mission_sounds.hpp"
#include "projectile.hpp"
#include "inv_station.hpp"
#include "entity_renderer.hpp"
#include "text_renderer.hpp"
#include "vehicle.hpp"
#include "asset_cache.hpp"
#include "cscript_host.hpp"
#include "player_simobject.hpp"
#include "entity_bindings.hpp"
#include "console/console.h"
#include "console/sim.h"
#include "console/simSet.h"
#include "console/script.h"
#include <functional>
#include "mission.hpp"
#include "hud_bindings.hpp"
#include "imgui_layer.hpp"
#include "viewer_state.hpp"
#include "asset_browser.hpp"
#include "inspector.hpp"
#include "open_url.hpp"
#include "help_menu.hpp"
#include "asset_resolver.hpp"
#include "mis_axes.hpp"
#include "content/interior/interior_set.hpp"
#include <set>
#include <unistd.h>
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

// ---------------------- DML metadata view ------------------------------------
//
// DML track spec 06 (renderer integration). The Dynamix Material List metadata
// is parsed by lib3space (studio::content::dts::darkstar::material_list) and
// surfaced per-material via `renderable_shape::get_materials()[i].metadata` —
// an opaque `unordered_map<string_view, variant>` keyed by
// "flags", "alpha", "type", "friction", "elasticity", "useDefaultProperties".
//
// `DmlInfo` is the typed projection of that map for fields this viewer cares
// about. Pulled out into a struct so a future renderer that *does* branch on
// e.g. surface type or alpha mode has a single place to extend.
//
// What we found across the entire Tribes 1.41 corpus (docs/done/03-dml/
// 00-value-semantics.md and the spec-07 wiki page):
//
//   - `flags == 0x00000103` for every valid material (n=4132). The two empty
//     forms (`0x0000F000` and `0x00000000`) always coincide with `name[0] ==
//     '\0'`, so the robust empty-slot test is the name check — already covered
//     by the texture pipeline (spec 08): empty-name materials produce no
//     texture binding and fall through to flat shading.
//   - `type` is a 0-14 surface-category enum for *physics & sound* (footstep,
//     impact spark, friction default). It is NOT a render-mode hint; no
//     consumer in Open Siege selects shaders by type, and no public spec says
//     to. Recorded here but does NOT drive the draw path.
//   - `alpha` is `0.0f` in every shipping record. It is provably not the
//     multiplicative transparency multiplier (that would render everything
//     invisible). Treated as opaque metadata — transparency, if any, is keyed
//     from the bitmap's own 0-magenta / 0-alpha index per PBMP handling.
//   - `friction` is `1.0f` in every record (4229/4229); `elasticity` takes two
//     values (`0.5` for terrain, `1.0` for rigid surfaces). These are physics
//     defaults, not render state.
//   - `use_default_properties` is `1` in every v4 record. The override path
//     (==0) was never exercised in ship content.
//
// Net runtime impact on this viewer: zero visible behavior change. Spec 05
// documented this explicitly. The work in spec 06 is *plumbing*: load the
// metadata onto every BucketGL, log it once at startup, and document that the
// render path is correct-by-construction (empty slots already skip texture
// binding via the name check). A future spec that adds, e.g., footstep audio
// or a per-`type` surface-decal system will consume `DmlInfo.type` here
// without further parser work.
struct DmlInfo
{
    std::uint32_t flags = 0;        // 0x103 valid, 0xF000 / 0 empty slot
    std::int32_t  type  = -1;       // surface category, -1 = unknown / v2 record
    float         alpha = 0.0f;
    float         friction = 0.0f;
    float         elasticity = 0.0f;
    std::uint32_t use_default_properties = 0;
    bool          present = false;  // false = bucket had no material slot at all
    bool          empty_slot = false; // true iff filename was empty
};

// Extract DML fields from a single `sc::material` metadata map. Missing keys
// (v2 records have no `type` / friction / elasticity) leave defaults in place.
// Variant lookup is defensive: the parser is allowed to emit either signed or
// unsigned int for `flags` depending on version, so we accept both.
static DmlInfo extract_dml_info(const sc::material& mat)
{
    DmlInfo info;
    info.present = true;
    info.empty_slot = mat.filename.empty();

    auto get_u32 = [&](const char* key, std::uint32_t& out) {
        auto it = mat.metadata.find(key);
        if (it == mat.metadata.end()) return;
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_integral_v<T>) out = static_cast<std::uint32_t>(v);
        }, it->second);
    };
    auto get_i32 = [&](const char* key, std::int32_t& out) {
        auto it = mat.metadata.find(key);
        if (it == mat.metadata.end()) return;
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_integral_v<T>) out = static_cast<std::int32_t>(v);
        }, it->second);
    };
    auto get_f32 = [&](const char* key, float& out) {
        auto it = mat.metadata.find(key);
        if (it == mat.metadata.end()) return;
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_arithmetic_v<T>) out = static_cast<float>(v);
        }, it->second);
    };

    get_u32("flags", info.flags);
    get_f32("alpha", info.alpha);
    get_i32("type", info.type);
    get_f32("friction", info.friction);
    get_f32("elasticity", info.elasticity);
    get_u32("useDefaultProperties", info.use_default_properties);
    return info;
}

// ---------------------- per-material/object mesh bucket ----------------------

struct MeshBucket
{
    std::vector<float> positions;   // 3 floats per vertex
    std::vector<float> normals;     // 3 floats per vertex, flat per face
    std::vector<float> uvs;         // 2 floats per vertex
    std::vector<int>   vertex_node_index;  // spec 08: per-bucket skinning node index, one per vertex
    std::size_t triangle_count = 0;
    bool has_uvs = false;
    std::string object_name;        // first object that contributed to this bucket
    int material_index = -1;        // spec 06: which loaded.materials[] entry this bucket uses
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

    // spec 06: precomputed object_name -> first-face-material-index map built
    // from the raw shape (the shape_renderer callback doesn't expose
    // face.material). Used to tag each bucket with its material slot so the GL
    // texture cache can be looked up at draw time.
    const std::map<std::string, int>* object_to_material = nullptr;

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
        if (object_to_material) {
            auto it = object_to_material->find(current_object_name);
            if (it != object_to_material->end()) b.material_index = it->second;
        }
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
                b.vertex_node_index.push_back(current_node_index);

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

// Spec 25/03 — enumerate every `.dts` entry in a VOL. Returns the
// shape "short names" (stem only, no extension, lowercased) so the
// picker modal can display them and the relaunch path can pass the
// name as argv[2] to `--match`.
static std::vector<std::string> list_dts_in_vol(const fs::path& vol_path)
{
    std::vector<std::string> out;
    std::ifstream in(vol_path, std::ios::binary);
    if (!in) return out;
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) return out;
    in.clear(); in.seekg(0);
    auto all = sr::get_all_content(vol_path, in, plugin);
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        auto name = f->filename.string();
        auto lower = name;
        for (auto& c : lower) c = std::tolower((unsigned char)c);
        if (lower.size() < 4 || lower.substr(lower.size()-4) != ".dts") continue;
        out.push_back(lower.substr(0, lower.size() - 4));
    }
    return out;
}

struct loaded_shape
{
    buffered_geometry         geom;
    std::vector<sc::material> materials;
    std::vector<Node>         nodes;        // bind-pose hierarchy (spec 02)
    std::vector<glm::mat4>    world_bind;   // accumulated world transforms
    std::vector<Sequence>     sequences;    // named animations (spec 03)
    // spec 06: object_name -> index into `materials` for the first face of that
    // object's mesh. The renderable_shape callback API drops face.material on
    // the floor, so we recover it from the raw shape variant. Empty mesh /
    // missing object falls through as -1 (resolved later as "no texture").
    std::map<std::string, int> object_to_material;
};

// spec 06: walk raw_shape.objects[] and, for each, peek at the first face of
// its referenced mesh to grab the material index. Builds the
// object_name -> material_index table the buffering_renderer uses to tag each
// bucket. Empty / vertex-less / face-less meshes produce a -1 entry.
static std::map<std::string, int> build_object_to_material(
    const dts::shape_variant& raw_shape)
{
    return std::visit([](const auto& s) {
        std::map<std::string, int> out;
        for (const auto& obj : s.objects) {
            const auto name_idx = static_cast<std::size_t>(obj.name_index);
            if (name_idx >= s.names.size()) continue;
            std::string name(s.names[name_idx].data(),
                strnlen(s.names[name_idx].data(), s.names[name_idx].size()));
            int mat_idx = -1;
            const auto mesh_idx = static_cast<std::size_t>(obj.mesh_index);
            if (mesh_idx < s.meshes.size()) {
                std::visit([&](const auto& mesh) {
                    if (!mesh.faces.empty()) {
                        mat_idx = static_cast<int>(mesh.faces[0].material);
                    }
                }, s.meshes[mesh_idx]);
            }
            // Last writer wins on duplicate names — Tribes shapes have one
            // object per name within a detail level; cross-LOD reuse is fine
            // because every LOD's object points at a mesh whose first-face
            // material is consistent.
            out[name] = mat_idx;
        }
        return out;
    }, raw_shape);
}

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
        out.object_to_material = build_object_to_material(raw);
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
    r.object_to_material = &out.object_to_material;
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

static const char* FS_SRC = R"(
#version 410 core
in vec3 v_normal_ws;
in vec2 v_uv;
uniform sampler2D u_tex0;
uniform bool u_has_texture;
out vec4 frag;
void main() {
    vec3 L = normalize(vec3(0.4, 0.8, 0.6));
    float lambert = max(dot(normalize(v_normal_ws), L), 0.0);
    if (u_has_texture) {
        vec4 tex = texture(u_tex0, v_uv);
        frag = vec4(tex.rgb * (0.35 + 0.65 * lambert), tex.a);
    } else {
        vec3 base = vec3(0.75, 0.78, 0.82);
        vec3 col = base * (0.25 + 0.75 * lambert);
        frag = vec4(col, 1.0);
    }
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

// ---------------------- interior shader (shared, used by --interior + --mission) ----------------------
static const char* INT_VS_SRC_SHARED = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec2 a_uv_lm;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec2 v_uv_lm;
void main() {
    v_uv    = a_uv;
    v_uv_lm = a_uv_lm;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

static const char* INT_FS_SRC_SHARED = R"(
#version 410 core
in vec2 v_uv;
in vec2 v_uv_lm;
uniform sampler2D u_tex0;
uniform sampler2D u_lightmap;
uniform bool u_has_texture;
uniform bool u_has_lightmap;
out vec4 frag;
void main() {
    vec4 diffuse = u_has_texture
        ? texture(u_tex0, v_uv)
        : vec4(0.75, 0.78, 0.82, 1.0);
    vec3 lm = u_has_lightmap
        ? texture(u_lightmap, v_uv_lm).rgb
        : vec3(0.5, 0.5, 0.5);
    vec3 lit = diffuse.rgb * lm * 2.0;
    frag = vec4(lit, diffuse.a);
}
)";

// ---------------------- flat-color line shader (debug overlays) ----------------------
static const char* FLAT_VS_SRC = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)";

static const char* FLAT_FS_SRC = R"(
#version 410 core
uniform vec3 u_color;
out vec4 frag;
void main() { frag = vec4(u_color, 1.0); }
)";

// ---------------------- terrain shaders (spec 05-terrain) ----------------------
//
// Per-quad flat coloring: 16 hard-coded distinct colors selected via
// int(mat_idx) % 16. Lambert shading from normal attribute.

static const char* TERRAIN_VS_SRC = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in float a_mat_idx;
layout(location = 2) in vec3 a_normal;
layout(location = 3) in vec4 a_layer_w;
layout(location = 4) in vec4 a_layer_i;
uniform mat4 u_mvp;
out vec3 v_normal_ws;
out vec3 v_pos_ws;
out float v_mat_idx;
out vec4 v_layer_w;
out vec4 v_layer_i;
void main() {
    v_normal_ws = a_normal;
    v_pos_ws    = a_pos;
    v_mat_idx   = a_mat_idx;
    v_layer_w   = a_layer_w;
    v_layer_i   = a_layer_i;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

static const char* TERRAIN_FS_SRC = R"(
#version 410 core
in vec3 v_normal_ws;
in vec3 v_pos_ws;
in float v_mat_idx;
in vec4 v_layer_w;
in vec4 v_layer_i;
out vec4 frag;
uniform vec3  u_ambient_color;
uniform vec3  u_sun_dir;
uniform vec3  u_sun_color;
uniform int   u_point_count;
uniform vec3  u_point_pos[8];
uniform vec3  u_point_color[8];
uniform float u_point_radius[8];
uniform sampler2DArray u_terrain_tex;
uniform int            u_terrain_layers;     // 0 = no texture array bound
uniform float          u_terrain_uv_scale;   // metres-per-quad; tiles per quad
void main() {
    // Fallback palette used when no texture array is bound.
    const vec3 palette[16] = vec3[16](
        vec3(0.90, 0.20, 0.20),
        vec3(0.20, 0.75, 0.20),
        vec3(0.20, 0.40, 0.90),
        vec3(0.90, 0.75, 0.10),
        vec3(0.70, 0.20, 0.80),
        vec3(0.10, 0.80, 0.80),
        vec3(0.90, 0.50, 0.10),
        vec3(0.50, 0.90, 0.20),
        vec3(0.10, 0.20, 0.60),
        vec3(0.80, 0.10, 0.50),
        vec3(0.30, 0.60, 0.10),
        vec3(0.60, 0.35, 0.10),
        vec3(0.20, 0.60, 0.60),
        vec3(0.75, 0.75, 0.20),
        vec3(0.50, 0.10, 0.10),
        vec3(0.70, 0.70, 0.70)
    );
    int mat = int(v_mat_idx);
    vec3 base;
    if (u_terrain_layers > 0) {
        // Spec 05/08 — Tribes splatting: every vertex carries up to 4
        // contributing layer indices + weights derived from its grid
        // neighbours. After rasterisation the weights are bilinearly
        // interpolated. Renormalise to remove drift from interpolation,
        // sample each of the four layers, and blend by weight.
        vec2 uv = fract(v_pos_ws.xz / max(u_terrain_uv_scale, 1.0));
        vec4 w = max(v_layer_w, 0.0);
        float wsum = w.x + w.y + w.z + w.w;
        if (wsum < 1e-4) {
            // Total weight collapsed (very rare interpolation edge case);
            // fall back to the floor/ceil 05/07 path.
            float fl = floor(v_mat_idx);
            float frac = clamp(v_mat_idx - fl, 0.0, 1.0);
            int la = clamp(int(fl),     0, u_terrain_layers - 1);
            int lb = clamp(int(fl) + 1, 0, u_terrain_layers - 1);
            vec3 ca = texture(u_terrain_tex, vec3(uv, float(la))).rgb;
            vec3 cb = texture(u_terrain_tex, vec3(uv, float(lb))).rgb;
            base = mix(ca, cb, frac);
        } else {
            w /= wsum;
            int i0 = clamp(int(v_layer_i.x), 0, u_terrain_layers - 1);
            int i1 = clamp(int(v_layer_i.y), 0, u_terrain_layers - 1);
            int i2 = clamp(int(v_layer_i.z), 0, u_terrain_layers - 1);
            int i3 = clamp(int(v_layer_i.w), 0, u_terrain_layers - 1);
            vec3 c0 = texture(u_terrain_tex, vec3(uv, float(i0))).rgb;
            vec3 c1 = texture(u_terrain_tex, vec3(uv, float(i1))).rgb;
            vec3 c2 = texture(u_terrain_tex, vec3(uv, float(i2))).rgb;
            vec3 c3 = texture(u_terrain_tex, vec3(uv, float(i3))).rgb;
            base = c0 * w.x + c1 * w.y + c2 * w.z + c3 * w.w;
        }
    } else {
        base = palette[mat % 16];
    }
    vec3 N = normalize(v_normal_ws);
    float sun = max(dot(N, normalize(u_sun_dir)), 0.0);
    vec3 lit = base * u_ambient_color + base * u_sun_color * sun;
    for (int i = 0; i < u_point_count && i < 8; ++i) {
        vec3 d = u_point_pos[i] - v_pos_ws;
        float dist = length(d);
        float r = max(u_point_radius[i], 1.0);
        float atten = 1.0 / (1.0 + (dist / r) + (dist * dist) / (r * r));
        float pl = max(dot(N, normalize(d)), 0.0);
        lit += base * u_point_color[i] * pl * atten;
    }
    frag = vec4(lit, 1.0);
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

// ---- terrain mission loader -----------------------------------------------
//
// Extracts the DTF and first DTB from a .ted PVOL archive, parses them, and
// returns the parsed structures.  Returns false on any failure.
static bool load_terrain_from_ted(
    const fs::path& ted_path,
    studio::content::terrain::dtf_file& out_dtf,
    studio::content::terrain::grid_block& out_block)
{
    std::ifstream in(ted_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "terrain: cannot open %s\n", ted_path.string().c_str());
        return false;
    }
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) {
        std::fprintf(stderr, "terrain: %s is not a PVOL archive\n", ted_path.string().c_str());
        return false;
    }
    in.clear(); in.seekg(0);

    auto all = sr::get_all_content(ted_path, in, plugin);

    std::vector<char> dtf_bytes;
    std::vector<char> dtb_bytes;
    std::string dtf_name, dtb_name;

    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        std::string name = f->filename.string();
        std::string lower = name;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);

        if (dtf_bytes.empty() && lower.size() >= 4
            && lower.substr(lower.size() - 4) == ".dtf") {
            std::stringstream buf;
            in.clear(); in.seekg(0);
            plugin.extract_file_contents(in, *f, buf);
            auto s = buf.str();
            dtf_bytes.assign(s.begin(), s.end());
            dtf_name = name;
        }
        if (dtb_bytes.empty() && lower.size() >= 4
            && lower.substr(lower.size() - 4) == ".dtb") {
            std::stringstream buf;
            in.clear(); in.seekg(0);
            plugin.extract_file_contents(in, *f, buf);
            auto s = buf.str();
            dtb_bytes.assign(s.begin(), s.end());
            dtb_name = name;
        }
        if (!dtf_bytes.empty() && !dtb_bytes.empty()) break;
    }

    if (dtf_bytes.empty()) {
        std::fprintf(stderr, "terrain: no .dtf found in %s\n", ted_path.string().c_str());
        return false;
    }
    if (dtb_bytes.empty()) {
        std::fprintf(stderr, "terrain: no .dtb found in %s\n", ted_path.string().c_str());
        return false;
    }

    {
        std::stringstream ss(std::string(dtf_bytes.begin(), dtf_bytes.end()));
        auto opt = studio::content::terrain::parse_dtf(ss);
        if (!opt) {
            std::fprintf(stderr, "terrain: parse_dtf failed for %s\n", dtf_name.c_str());
            return false;
        }
        out_dtf = std::move(*opt);
    }
    {
        std::stringstream ss(std::string(dtb_bytes.begin(), dtb_bytes.end()));
        auto opt = studio::content::terrain::parse_dtb(ss);
        if (!opt) {
            std::fprintf(stderr, "terrain: parse_dtb failed for %s\n", dtb_name.c_str());
            return false;
        }
        out_block = std::move(*opt);
    }

    std::printf("terrain: loaded %s + %s\n", dtf_name.c_str(), dtb_name.c_str());
    std::printf("terrain: scale=%d (%.1f m/quad)  size=%dx%d  heights=[%.1f..%.1f]\n",
        out_dtf.scale, static_cast<float>(1 << out_dtf.scale),
        out_block.size[0], out_block.size[1],
        out_block.height_min, out_block.height_max);
    std::printf("terrain: material_list=%s\n", out_dtf.material_list_name.c_str());
    return true;
}

// Spec 16/10 — script-callback bridge tables. Populated after
// mission_load by matching MissionGroup members back to the entity-state
// arrays via position. The callback function-pointer signatures used by
// entity_renderer can't capture state, so we expose the lookups as file
// statics. Indexing matches the layout of the corresponding ent_*
// vectors in main().
namespace {
std::vector<unsigned int> g_trigger_sim_ids;
std::vector<unsigned int> g_generator_sim_ids;
unsigned int              g_player_sim_id = 0;

void on_trigger_enter_callback(int trigger_idx)
{
    if (trigger_idx < 0 ||
        static_cast<std::size_t>(trigger_idx) >= g_trigger_sim_ids.size())
        return;
    SimObject* trigger = Sim::findObject(g_trigger_sim_ids[trigger_idx]);
    if (!trigger) return;
    SimObject* who = g_player_sim_id
        ? Sim::findObject(g_player_sim_id) : nullptr;
    // Per spec 16/07 callback shape: trigger.onEnter(who).
    Con::executef(trigger, "onEnter", who ? who->getIdString() : "0");
}

void on_generator_destroyed_callback(const dts_viewer::GeneratorState& g)
{
    // Find this generator's SimObject by linear scan — small N (<=32),
    // avoids holding a per-generator pointer-back-to-pointer.
    for (std::size_t i = 0; i < g_generator_sim_ids.size(); ++i) {
        SimObject* obj = Sim::findObject(g_generator_sim_ids[i]);
        if (!obj) continue;
        // Match by pos field equality.
        const char* posField = obj->getDataField(
            StringTable->insert("pos"), nullptr);
        if (!posField) continue;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%g %g %g",
            g.xf.position[0], g.xf.position[1], g.xf.position[2]);
        if (std::strcmp(posField, buf) == 0) {
            Con::executef(obj, "onDestroyed");
            return;
        }
    }
}
} // anonymous namespace

int main(int argc, char** argv)
{
    // Spec 23/03 — strip --tribes-dir before any other arg parsing so it
    // works in every mode.
    dts_viewer::stripTribesDir(argc, argv);

    // When launched with no positional arguments, try the BYO asset resolver
    // to find the Tribes install dir, then launch the mission viewer.
    static std::string s_tribes_dir_storage; // stable backing for injected argv
    static std::string s_default_vol_storage;
    static char* s_injected[4];
    if (argc < 2) {
        fs::path tribes = dts_viewer::resolveTribesDir();
        if (tribes.empty()) {
            std::fprintf(stderr,
                "usage: %s <path-to-vol> [dts-substring] [--grid N] [--screenshot path.ppm]\n"
                "       %s --mission <path-to-.ted> [--screenshot path.ppm]\n"
                "       %s --run-script <path-to-.cs>\n"
                "       %s --tribes-dir <path>   (set + cache Tribes install)\n",
                argv[0], argv[0], argv[0], argv[0]);
            return 1;
        }
        // Find first .ted mission and launch in mission mode.
        fs::path missions_dir = tribes / "base" / "missions";
        fs::path first_ted;
        if (fs::is_directory(missions_dir)) {
            for (const auto& e : fs::directory_iterator(missions_dir)) {
                if (e.path().extension() == ".ted") { first_ted = e.path(); break; }
            }
        }
        if (!first_ted.empty()) {
            s_tribes_dir_storage = tribes.string();
            s_default_vol_storage = first_ted.string();
            s_injected[0] = argv[0];
            s_injected[1] = const_cast<char*>("--mission");
            s_injected[2] = s_default_vol_storage.data();
            s_injected[3] = nullptr;
            argc = 3;
            argv = s_injected;
        } else {
            // No missions found — fall through to VOL mode with Entities.vol.
            s_default_vol_storage = (tribes / "base" / "Entities.vol").string();
            s_injected[0] = argv[0];
            s_injected[1] = s_default_vol_storage.data();
            s_injected[2] = nullptr;
            argc = 2;
            argv = s_injected;
        }
    }

    // Spec 17/06 — global `--verbose-cscript` flag bypasses the
    // benign-warning filter in the cscript console sink. Strip it from
    // argv before mode dispatch so it works in any mode.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--verbose-cscript") {
            dts_viewer::cscript::set_verbose(true);
            // Shift remaining argv left to remove the flag.
            for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
            --argc; --i;
        }
    }

    // 14/12 — `--no-particles` disables ambient snow + star-field effects.
    bool particles_enabled = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-particles") {
            particles_enabled = false;
            for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
            --argc; --i;
        }
    }

    // ---- --run-script <path> mode: bring up cscript VM, eval file, exit
    if (argc >= 3 && std::string(argv[1]) == "--run-script") {
        dts_viewer::cscript::init();
        if (!dts_viewer::cscript::runFile(argv[2])) {
            std::fprintf(stderr, "could not open script: %s\n", argv[2]);
            return 2;
        }
        return 0;
    }

    // ---- --mission <path> mode: terrain heightmap renderer ----
    if (argc >= 3 && std::string(argv[1]) == "--mission") {
        fs::path ted_path = argv[2];
        std::string self_arg0 = argv[0];

        dts_viewer::MissionRegistry mission_reg =
            dts_viewer::scan_missions(ted_path.parent_path());
        for (std::size_t i = 0; i < mission_reg.short_names.size(); ++i) {
            if (mission_reg.ted_paths[i] == ted_path ||
                mission_reg.short_names[i] == ted_path.stem().string()) {
                mission_reg.current_index = i;
                break;
            }
        }
        std::printf("missions: %zu registered, current=%s\n",
            mission_reg.short_names.size(),
            mission_reg.short_names.empty()
                ? "(none)"
                : mission_reg.short_names[mission_reg.current_index].c_str());

        // Spec 25/03 — viewer-state init: load Recent MRU from
        // config.json, populate the mission catalogue for the picker.
        dts_viewer::load_config();
        dts_viewer::set_mission_catalogue(mission_reg.short_names);

        // Spec 25/05 — index every VOL adjacent to the mission's base
        // dir so the Asset Browser sees Entities.vol + Shell.vol +
        // <world>World.vol + Missions.vol + voice/* etc.
        {
            std::vector<dts_viewer::MountedVol> mounts;
            const fs::path base_dir = ted_path.parent_path().parent_path();
            if (!base_dir.empty() && fs::exists(base_dir)) {
                for (const auto& ent : fs::recursive_directory_iterator(base_dir)) {
                    if (!ent.is_regular_file()) continue;
                    auto p = ent.path();
                    auto ext = p.extension().string();
                    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
                    if (ext != ".vol") continue;
                    dts_viewer::MountedVol m;
                    m.path = p;
                    m.short_name = p.filename().string();
                    for (auto& c : m.short_name)
                        c = (char)std::tolower((unsigned char)c);
                    mounts.push_back(std::move(m));
                }
            }
            dts_viewer::asset_browser_init(mounts);
        }


        std::string screenshot_path_ter;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--screenshot" && i + 1 < argc) {
                screenshot_path_ter = argv[i + 1];
                ++i;
            }
        }

        studio::content::terrain::dtf_file dtf;
        studio::content::terrain::grid_block block;
        if (!load_terrain_from_ted(ted_path, dtf, block)) return 1;

        const float metres_per_quad = static_cast<float>(1 << dtf.scale);

        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 3;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

        SDL_Window* win = SDL_CreateWindow("dts-viewer (terrain)",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1920, 1080,
            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
        SDL_GLContext ctx = SDL_GL_CreateContext(win);
        SDL_GL_SetSwapInterval(1);
#if !defined(__APPLE__)
        glewExperimental = GL_TRUE;
        glewInit();
#endif
        std::printf("GL_VERSION: %s\n", glGetString(GL_VERSION));

        // Spec 25/01 — ImGui must init after the GL context but before
        // any host code that consumes SDL events or issues GL draws.
        dts_viewer::imgui_init(win, ctx);

        // Spec 11/02 — bring up the audio backend.  The stub backend
        // logs to stderr and returns true; a future spec swaps in a
        // miniaudio / OpenAL Soft implementation behind the same API.
        dts_viewer::audio_init();

        // Build terrain mesh.
        // Center the terrain on world origin so entities at negative
        // world coords (the typical Tribes mission layout — SimTerrain
        // is at -3072,-3072 but the heightmap wraps modulo block size,
        // so entities sit in the wrap-aligned tile around origin) land
        // on the rendered tile. Without this, missions like Citadels
        // place their bases at world (-600, -650) while the terrain
        // renders at [0, 2048] — entities float in empty space off to
        // the side. See docs/bugfix/citadels-offset for the analysis.
        const float terrain_side_m = static_cast<float>(block.size[0]) * metres_per_quad;
        const glm::vec2 terrain_origin(-0.5f * terrain_side_m, -0.5f * terrain_side_m);
        dts_viewer::TerrainMesh terrain = dts_viewer::build_terrain_mesh(
            block, metres_per_quad, terrain_origin);
        if (!terrain.valid()) {
            std::fprintf(stderr, "terrain: build_terrain_mesh failed\n");
            return 2;
        }
        std::printf("terrain: mesh built — %d triangles, bbox=[%.1f %.1f %.1f]..[%.1f %.1f %.1f]\n",
            terrain.index_count / 3,
            terrain.bbox_min.x, terrain.bbox_min.y, terrain.bbox_min.z,
            terrain.bbox_max.x, terrain.bbox_max.y, terrain.bbox_max.z);

        // Compile terrain program.
        GLuint terrain_prog = link_program(
            compile_shader(GL_VERTEX_SHADER,   TERRAIN_VS_SRC),
            compile_shader(GL_FRAGMENT_SHADER, TERRAIN_FS_SRC));
        GLint u_ter_mvp = glGetUniformLocation(terrain_prog, "u_mvp");

        // Flat-color debug program (bounds + future markers/sky).
        GLuint flat_prog = link_program(
            compile_shader(GL_VERTEX_SHADER,   FLAT_VS_SRC),
            compile_shader(GL_FRAGMENT_SHADER, FLAT_FS_SRC));
        GLint u_flat_mvp   = glGetUniformLocation(flat_prog, "u_mvp");
        GLint u_flat_color = glGetUniformLocation(flat_prog, "u_color");

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_MULTISAMPLE);
        glDisable(GL_CULL_FACE);

        // Camera positioned above terrain centre, looking inward. Use
        // the horizontal (XZ) radius for distance + scale so the
        // camera doesn't end up at a ridiculous Y-altitude on flat-ish
        // missions (terrain Y extent is tiny vs 2048m XZ). Pitch down
        // gently from a height of ~150m so player-scale entities like
        // citadel interiors are visible at spawn instead of dots.
        glm::vec3 ter_center = 0.5f * (terrain.bbox_min + terrain.bbox_max);
        glm::vec3 ter_extent = terrain.bbox_max - terrain.bbox_min;
        float ter_radius_h = 0.5f * std::sqrt(ter_extent.x * ter_extent.x
                                            + ter_extent.z * ter_extent.z);
        if (ter_radius_h < 1.0f) ter_radius_h = 1.0f;
        float ter_radius = ter_radius_h;  // legacy alias used below

        dts_viewer::Camera ter_cam;
        ter_cam.position    = glm::vec3(ter_center.x,
                                        terrain.bbox_max.y + 150.0f,
                                        ter_center.z + ter_radius_h * 0.6f);
        ter_cam.yaw         = glm::pi<float>();  // face -Z toward center
        ter_cam.pitch       = -0.35f;
        ter_cam.move_speed  = ter_radius * 0.05f;
        ter_cam.near_plane  = ter_radius * 0.001f;
        ter_cam.far_plane   = ter_radius * 10.0f;
        dts_viewer::set_mouse_capture(ter_cam, true);

        // Optionally load the matching .mis from the same directory.
        std::optional<dts_viewer::LoadedMission> ter_mission;
        {
            fs::path stem      = ted_path.stem();
            fs::path mis_guess = ted_path.parent_path() / (stem.string() + ".mis");
            if (!fs::exists(mis_guess)) {
                // try case-insensitive search
                if (fs::exists(ted_path.parent_path())) {
                    for (auto& e : fs::directory_iterator(ted_path.parent_path())) {
                        if (e.path().extension() != ".mis") continue;
                        std::string a = e.path().stem().string();
                        std::string b = stem.string();
                        if (a.size() != b.size()) continue;
                        bool ci = true;
                        for (std::size_t i = 0; i < a.size(); ++i) {
                            if (std::tolower((unsigned char)a[i]) !=
                                std::tolower((unsigned char)b[i])) { ci = false; break; }
                        }
                        if (ci) { mis_guess = e.path(); break; }
                    }
                }
            }
            if (fs::exists(mis_guess)) {
                fs::path missions_dir = mis_guess.parent_path();
                fs::path base_dir     = missions_dir.parent_path();
                ter_mission = dts_viewer::load_mission(missions_dir, base_dir, stem.string());
                if (ter_mission) {
                    std::printf("terrain: loaded scene from %s (%zu mounted vols)\n",
                        mis_guess.filename().string().c_str(),
                        ter_mission->mounted_vols.size());
                }
            }
        }

        dts_viewer::HeightSampler height_sampler{
            block.heights.data(),
            static_cast<int>(block.size[0]) + 1,
            metres_per_quad,
            terrain_origin.x,
            terrain_origin.y
        };

        dts_viewer::MissionBounds bounds = dts_viewer::compute_bounds(
            ter_mission ? ter_mission->scene
                        : studio::content::mission::scene_graph{},
            (block.size[0] + 1) * metres_per_quad,
            terrain.bbox_min.y,
            terrain.bbox_max.y,
            terrain_origin.x,
            terrain_origin.y);

        // Stretch the camera's far plane to fit the playable area.
        ter_cam.far_plane = std::max(ter_cam.far_plane, bounds.recommended_far_plane);

        // Build a MaterialResolver over all mounted world VOLs.
        dts_viewer::MaterialResolver ter_resolver;
        std::vector<Palette> ter_palettes;
        std::optional<dts_viewer::SkyboxResources> sky_box;
        if (ter_mission) {
            for (const auto& v : ter_mission->mounted_vols) {
                if (fs::exists(v)) ter_resolver.add_vol(v);
            }
            // Load the per-mission palette (e.g. lush.day.ppl) — needed by
            // skybox + interior textures.  Scan mounted VOLs for the file.
            if (ter_mission->scene.palette) {
                const std::string& ppl_name = ter_mission->scene.palette->ppl_filename;
                auto bytes = ter_resolver.resolve(ppl_name);
                if (bytes && !bytes->empty()) {
                    try {
                        std::stringstream ss(std::string(bytes->begin(), bytes->end()));
                        ter_palettes = load_ppl(ss);
                        std::printf("terrain: loaded %zu palettes from %s\n",
                            ter_palettes.size(), ppl_name.c_str());
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "terrain: PPL parse failed (%s): %s\n",
                            ppl_name.c_str(), e.what());
                    }
                }
            }
            if (ter_mission->scene.sky) {
                auto pal_map = by_index(ter_palettes);
                const studio::content::mission::node_star_field* sf = nullptr;
                // Star field is a sibling node — scan the scene root for it.
                auto find_star = [&](auto& self,
                    const studio::content::mission::scene_node& n) ->
                    const studio::content::mission::node_star_field*
                {
                    if (auto* s = std::get_if<studio::content::mission::node_star_field>(&n.payload))
                        return s;
                    for (auto& c : n.children) {
                        if (auto* r = self(self, c)) return r;
                    }
                    return nullptr;
                };
                sf = find_star(find_star, ter_mission->scene.root);
                sky_box = dts_viewer::build_skybox(
                    *ter_mission->scene.sky, sf, ter_resolver, pal_map);
            }
        }
        bool show_sky = sky_box.has_value();

        // 14/12 — instantiate ambient particle systems from MIS flags.
        std::optional<dts_viewer::ParticleSystem> snow_sys;
        std::optional<dts_viewer::ParticleSystem> star_sys;
        if (particles_enabled && ter_mission) {
            // Walk the scene to find a node_snowfall (if any) and a
            // node_star_field (already used by the skybox path — we
            // additionally render twinkling dots on top).
            bool has_snow = false;
            bool has_stars = false;
            auto scan = [&](auto& self,
                const studio::content::mission::scene_node& n) -> void {
                std::visit([&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T,
                        studio::content::mission::node_snowfall>) {
                        has_snow = true;
                    } else if constexpr (std::is_same_v<T,
                        studio::content::mission::node_star_field>) {
                        has_stars = true;
                    }
                }, n.payload);
                for (auto& c : n.children) self(self, c);
            };
            scan(scan, ter_mission->scene.root);
            if (has_snow) snow_sys = dts_viewer::make_snow();
            if (has_stars) star_sys = dts_viewer::make_stars();
            if (has_snow || has_stars) {
                std::fprintf(stderr,
                    "particles: snow=%d stars=%d\n", has_snow, has_stars);
            }
        }

        dts_viewer::HudState hud;
        hud.show_sky = show_sky;
        std::string hud_mission_name = ted_path.stem().string();

        // Terrain texture array: derive world prefix from the DTF's DML name
        // (e.g. "lush.dml" -> "lush"), then load <prefix>.Terrain.dat.
        dts_viewer::TerrainTextureArray terrain_tex;
        if (ter_mission && !dtf.material_list_name.empty()) {
            std::string dml = dtf.material_list_name;
            auto dot = dml.find('.');
            std::string prefix = (dot == std::string::npos) ? dml : dml.substr(0, dot);
            auto pal_map = by_index(ter_palettes);
            terrain_tex = dts_viewer::build_terrain_textures(prefix, ter_resolver, pal_map);
        }

        // Mission lighting.
        dts_viewer::SceneLighting lighting = ter_mission
            ? dts_viewer::build_scene_lighting(ter_mission->scene)
            : dts_viewer::SceneLighting{};
        dts_viewer::LightingMode lighting_mode = dts_viewer::LightingMode::Full;
        if (ter_mission) {
            std::printf("lighting: ambient=(%.2f,%.2f,%.2f) sun_dir=(%.2f,%.2f,%.2f) points=%zu\n",
                lighting.ambient_color[0], lighting.ambient_color[1], lighting.ambient_color[2],
                lighting.sun.direction[0], lighting.sun.direction[1], lighting.sun.direction[2],
                lighting.point_lights.size());
        }

        dts_viewer::CameraMode cam_mode = dts_viewer::CameraMode::Free;
        bool show_bounds_debug = false;
        bool wireframe = false;
        bool running = true;

        // ---- PlayerState bridge (spec 09/01) ----
        // Mirrors ter_cam each frame; player_update is a stub in spec 01.
        // Spawn at the nearest DropPointMarker if a scene was loaded.
        dts_viewer::PlayerTuning ptune;
        dts_viewer::PlayerState  pstate;
        if (ter_mission) {
            std::array<float, 3> here{ ter_cam.position.x,
                                       ter_cam.position.y,
                                       ter_cam.position.z };
            auto dp = dts_viewer::nearest_drop_point(ter_mission->scene, here);
            if (dp) {
                pstate.pos = glm::vec3{ (*dp)[0], (*dp)[1], (*dp)[2] };
            } else {
                pstate.pos = ter_cam.position
                           - glm::vec3(0.0f, ptune.eye_height, 0.0f);
            }
        } else {
            pstate.pos = ter_cam.position
                       - glm::vec3(0.0f, ptune.eye_height, 0.0f);
        }
        pstate.yaw   = ter_cam.yaw;
        pstate.pitch = ter_cam.pitch;

        const float kFixedStep = 1.0f / 60.0f;
        float       pstep_accumulator = 0.0f;
        int         pstep_count_window = 0;
        Uint32      pstep_log_last_ms  = SDL_GetTicks();

        // Footstep gait (spec 11/06): stride length / horizontal speed
        // gives the seconds between foot strikes; resets when not
        // grounded or below the minimum speed.
        const float kStrideLength    = 1.8f;
        const float kMinFootstepSpeed = 1.5f;
        float footstep_timer = 0.0f;

        // Projectile system + Inventory (Track 12).
        dts_viewer::ProjectileSystem proj_sys;
        dts_viewer::ProjectileTuning proj_tune;
        pstate.inventory = dts_viewer::default_loadout();

        // Spec 14/13 — vehicle driving. -1 means "on foot"; once mounted,
        // points into ent_vehicles. Tuning is shared across all vehicles
        // for v1 (Wildcat / Beowulf share the same arcade hover feel).
        int piloted_vehicle_idx = -1;
        dts_viewer::VehicleTuning vtune;

        // Inventory stations (spec 12/04).
        dts_viewer::InvStationSystem inv_sys;
        if (ter_mission) {
            inv_sys = dts_viewer::inv_stations_load(ter_mission->scene);
        }

        // Shared message feed (specs 13/04 + 14/06).
        std::deque<std::string> message_feed;
        bool show_command_map = false;

        // -----------------------------------------------------------------
        // Spec 17/02 + 17/03 — script-side mission lifecycle + HUD bindings.
        // Brings up cscript_core, points the HUD bindings (notify /
        // centerPrint / addObjective / ...) at the live message_feed +
        // objective HUD state, and asks MissionContext to build a
        // SimGroup("MissionGroup") with one SimObject per scene_graph
        // entity. The tick call later inside the fixed-step loop drains
        // script-side schedule() events.
        // -----------------------------------------------------------------
        dts_viewer::cscript::init();
        dts_viewer::HudBindingsState hud_bindings_state;
        dts_viewer::hud_bindings_set_host(&message_feed, &hud_bindings_state);
        {
            const fs::path scripts_a =
                ted_path.parent_path().parent_path() / "scripts";
            if (fs::is_directory(scripts_a))
                dts_viewer::mission_add_script_search_path(scripts_a);
            if (fs::is_directory("/tmp/scripts"))
                dts_viewer::mission_add_script_search_path("/tmp/scripts");
        }
        dts_viewer::MissionContext mctx;
        if (ter_mission) {
            const fs::path missions_dir = ted_path.parent_path();
            const fs::path base_dir     = missions_dir.parent_path();
            dts_viewer::mission_load(
                mctx, missions_dir, base_dir, ted_path.stem().string());
        }
        // Spec 16/09 — Player SimObject ↔ PlayerState bridge.
        // Constructs `$thePlayer` once (after VM init, after PlayerState
        // is initialised) and binds it to the live gameplay state so
        // script-side reads/writes hit real data. The script-only path
        // (the spec 16/05 unit test) still works because the bridge is
        // opt-in via setLivePlayerState.
        Player* g_thePlayer = nullptr;
        {
            // Constructed via Con::evaluate so the Sim registry tracks
            // it like any other script-side instance.
            Con::evaluate(
                "if (!isObject($thePlayer)) {\n"
                "    new Player(thePlayer) {};\n"
                "    $thePlayer = thePlayer;\n"
                "}\n", false, "spec-16-09-bridge");
            if (SimObject* o = Sim::findObject("thePlayer")) {
                g_thePlayer = dynamic_cast<Player*>(o);
                if (g_thePlayer) {
                    g_thePlayer->setLivePlayerState(&pstate);
                    g_player_sim_id = o->getId();
                }
            }
            if (!g_thePlayer) {
                std::fprintf(stderr,
                    "[cscript] spec 16/09: $thePlayer construction failed\n");
            }
        }

        // Spec 17/06 — print one-line summary of how many benign Tribes
        // warnings were suppressed during script load.
        if (int n = dts_viewer::cscript::flush_suppression_count(); n > 0) {
            std::printf(
                "[cscript] suppressed %d benign Tribes-idiom warnings "
                "(use --verbose-cscript to show)\n", n);
        }

        // ---- Interior meshes (Track 14 follow-up: render placed bases) ----
        // Uses the cross-VOL resolver (3space/interior_set) to pull each
        // node_interior's DIS+DIG+DIL+DML, then build_interior_mesh + the
        // shared interior shader to draw them at the mission's transforms.
        GLuint int_prog_m = 0;
        GLint  int_u_mvp_m = -1, int_u_has_texture_m = -1, int_u_tex0_m = -1;
        GLint  int_u_lightmap_m = -1, int_u_has_lightmap_m = -1;
        struct PlacedInterior {
            dts_viewer::InteriorMesh mesh;
            studio::content::mission::transform xf;
            std::string shape_name;
        };
        std::vector<PlacedInterior> placed_interiors;
        if (ter_mission) {
            int_prog_m = link_program(
                compile_shader(GL_VERTEX_SHADER,   INT_VS_SRC_SHARED),
                compile_shader(GL_FRAGMENT_SHADER, INT_FS_SRC_SHARED));
            int_u_mvp_m          = glGetUniformLocation(int_prog_m, "u_mvp");
            int_u_has_texture_m  = glGetUniformLocation(int_prog_m, "u_has_texture");
            int_u_tex0_m         = glGetUniformLocation(int_prog_m, "u_tex0");
            int_u_lightmap_m     = glGetUniformLocation(int_prog_m, "u_lightmap");
            int_u_has_lightmap_m = glGetUniformLocation(int_prog_m, "u_has_lightmap");

            studio::content::interior::interior_resolver iresolver;
            for (const auto& v : ter_mission->mounted_vols) {
                if (fs::exists(v)) iresolver.add_vol(v);
            }
            auto pal_map_int = by_index(ter_palettes);

            auto walk_int = [&](auto& self,
                const studio::content::mission::scene_node& n) -> void
            {
                std::visit([&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T,
                        studio::content::mission::node_interior>)
                    {
                        auto opt = iresolver.resolve(p.shape_name);
                        if (!opt) {
                            std::fprintf(stderr,
                                "interior: resolve failed for '%s'\n",
                                p.shape_name.c_str());
                            return;
                        }
                        PlacedInterior pi;
                        pi.shape_name = p.shape_name;
                        pi.xf = p.xf;
                        pi.mesh = dts_viewer::build_interior_mesh(
                            opt->dig, opt->material, ter_resolver, pal_map_int,
                            &opt->dil,
                            opt->stock_dil_for_fallback
                                ? &(*opt->stock_dil_for_fallback)
                                : nullptr);
                        if (pi.mesh.valid()) {
                            placed_interiors.push_back(std::move(pi));
                        }
                    }
                }, n.payload);
                for (auto& c : n.children) self(self, c);
            };
            walk_int(walk_int, ter_mission->scene.root);
            std::fprintf(stderr,
                "interior: placed %zu interior meshes\n",
                placed_interiors.size());
        }

        // Track 14 — entity stubs (collect at mission load).
        std::vector<dts_viewer::StaticShapeState>      ent_statics;
        std::vector<dts_viewer::ItemState>             ent_items;
        std::vector<dts_viewer::TurretState>           ent_turrets;
        std::vector<dts_viewer::MoveableState>         ent_moveables;
        std::vector<dts_viewer::GeneratorState>        ent_generators;
        std::vector<dts_viewer::TriggerState>          ent_triggers;
        std::vector<dts_viewer::VehiclePlaceholderState> ent_vehicles;
        if (ter_mission) {
            ent_statics    = dts_viewer::collect_static_shapes(ter_mission->scene);
            ent_items      = dts_viewer::collect_items(ter_mission->scene);
            ent_turrets    = dts_viewer::collect_turrets(ter_mission->scene);
            ent_moveables  = dts_viewer::collect_moveables(ter_mission->scene);
            ent_generators = dts_viewer::collect_generators(ter_mission->scene);
            ent_triggers   = dts_viewer::collect_triggers(ter_mission->scene);
            ent_vehicles   = dts_viewer::collect_vehicle_placeholders(ter_mission->scene);
            std::fprintf(stderr,
                "entities: %zu static, %zu items, %zu turrets, %zu moveables, "
                "%zu generators, %zu triggers, %zu vehicles\n",
                ent_statics.size(), ent_items.size(), ent_turrets.size(),
                ent_moveables.size(), ent_generators.size(),
                ent_triggers.size(), ent_vehicles.size());
        }

        // Spec 16/10 — link mission-spawned SimObjects back to the live
        // entity-state PODs. mission_load() emitted one
        // `instant <Type>(<name>)` per scene entity earlier; we walk
        // MissionGroup and, for every Item / Turret / Generator /
        // Trigger we find, match by position to the corresponding entry
        // in the ent_* vector and call setLive*State(). Sim IDs for
        // triggers + generators are stashed in the file-static tables
        // so the engine-side callbacks (on_trigger_enter /
        // on_generator_destroyed) can route back into the VM.
        {
            g_trigger_sim_ids.assign(ent_triggers.size(), 0);
            g_generator_sim_ids.assign(ent_generators.size(), 0);

            SimObject* mg = Sim::findObject("MissionGroup");
            auto* group = dynamic_cast<SimGroup*>(mg);
            auto positions_match =
                [](const char* a, float bx, float by, float bz) {
                if (!a) return false;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%g %g %g", bx, by, bz);
                return std::strcmp(a, buf) == 0;
            };
            if (group) {
                std::function<void(SimGroup*)> walk = [&](SimGroup* g) {
                    for (auto it = g->begin(); it != g->end(); ++it) {
                        SimObject* obj = *it;
                        if (auto* sub = dynamic_cast<SimGroup*>(obj)) {
                            walk(sub);
                            continue;
                        }
                        const char* posField = obj->getDataField(
                            StringTable->insert("pos"), nullptr);
                        const char* cls = obj->getClassName();
                        if (!cls) continue;
                        if (std::strcmp(cls, "Trigger") == 0) {
                            for (std::size_t i = 0;
                                 i < ent_triggers.size(); ++i) {
                                const auto& tr = ent_triggers[i];
                                if (positions_match(posField,
                                    tr.xf.position[0], tr.xf.position[1],
                                    tr.xf.position[2]))
                                {
                                    if (auto* t = dynamic_cast<Trigger*>(obj))
                                        t->setLiveTriggerState(&ent_triggers[i]);
                                    g_trigger_sim_ids[i] = obj->getId();
                                    break;
                                }
                            }
                        }
                        else if (std::strcmp(cls, "Turret") == 0) {
                            for (auto& tt : ent_turrets) {
                                if (positions_match(posField,
                                    tt.xf.position[0], tt.xf.position[1],
                                    tt.xf.position[2]))
                                {
                                    if (auto* t = dynamic_cast<Turret*>(obj))
                                        t->setLiveTurretState(&tt);
                                    break;
                                }
                            }
                        }
                        else if (std::strcmp(cls, "Generator") == 0) {
                            for (std::size_t i = 0;
                                 i < ent_generators.size(); ++i) {
                                auto& gn = ent_generators[i];
                                if (positions_match(posField,
                                    gn.xf.position[0], gn.xf.position[1],
                                    gn.xf.position[2]))
                                {
                                    if (auto* gobj = dynamic_cast<Generator*>(obj))
                                        gobj->setLiveGeneratorState(&gn);
                                    g_generator_sim_ids[i] = obj->getId();
                                    break;
                                }
                            }
                        }
                        else if (std::strcmp(cls, "Item") == 0) {
                            for (auto& itm : ent_items) {
                                if (positions_match(posField,
                                    itm.xf.position[0], itm.xf.position[1],
                                    itm.xf.position[2]))
                                {
                                    if (auto* iobj = dynamic_cast<Item*>(obj))
                                        iobj->setLiveItemState(&itm);
                                    break;
                                }
                            }
                        }
                    }
                };
                walk(group);
            }
        }

        // Spec 14/09 — DTS prop loading + textured rendering for entities.
        // Resolves each entity's datablock to a DTS shape across the mission's
        // mounted VOLs. Memoised: the 20 turrets sharing one TurretBase load
        // the mesh once.
        dts_viewer::AssetCache asset_cache;
        if (ter_mission) {
            asset_cache.init(&ter_resolver,
                             by_index(ter_palettes),
                             ter_mission->mounted_vols);
            std::size_t resolved = 0, missed = 0;
            auto warm = [&](const std::string& name) {
                if (name.empty()) return;
                if (asset_cache.try_load_for_datablock(name)) ++resolved;
                else ++missed;
            };
            for (const auto& s  : ent_statics)    warm(s.data_block_name);
            for (const auto& it : ent_items)      warm(it.data_block_name);
            for (const auto& t  : ent_turrets)    warm(t.data_block_name);
            for (const auto& m  : ent_moveables)  warm(m.data_block_name);
            for (const auto& g  : ent_generators) warm(g.data_block_name);
            for (const auto& v  : ent_vehicles) { (void)v; warm("vehiclePad"); }
            const auto& cs = asset_cache.stats();
            std::fprintf(stderr,
                "asset_cache: %zu unique shapes loaded, %zu missed datablocks; "
                "%zu textures, %zu tris (per-entity lookups: %zu resolved, "
                "%zu unmapped)\n",
                cs.shapes_loaded, cs.shapes_missed,
                cs.textures_uploaded, cs.triangles_total,
                resolved, missed);

            // Diagnostic: list unique datablock names that fell through.
            // These render as the Tier-B wireframe cube fallback and are
            // candidates for the asset_cache datablock_table().
            std::set<std::string> unmapped;
            auto note_unmapped = [&](const std::string& name) {
                if (name.empty()) return;
                if (!asset_cache.try_load_for_datablock(name)) {
                    unmapped.insert(name);
                }
            };
            for (const auto& s  : ent_statics)    note_unmapped(s.data_block_name);
            for (const auto& it : ent_items)      note_unmapped(it.data_block_name);
            for (const auto& t  : ent_turrets)    note_unmapped(t.data_block_name);
            for (const auto& m  : ent_moveables)  note_unmapped(m.data_block_name);
            for (const auto& g  : ent_generators) note_unmapped(g.data_block_name);
            if (!unmapped.empty()) {
                std::string line;
                for (const auto& n : unmapped) {
                    if (!line.empty()) line += ", ";
                    line += n;
                }
                std::fprintf(stderr,
                    "asset_cache: unmapped datablocks (fall back to cube): %s\n",
                    line.c_str());
            }
        }

        // Pre-collect entity positions for compass/sensor/map (specs
        // 13/02, 13/03, 13/06).  For v1 we just enumerate the scene's
        // generators + inventory stations as static blips.
        std::vector<dts_viewer::MapIcon> map_icons;
        std::vector<dts_viewer::SensorBlip> sensor_blips;
        std::vector<dts_viewer::CompassTick> compass_team_ticks;
        if (ter_mission) {
            auto walk_icons = [&](auto& self,
                const studio::content::mission::scene_node& n) -> void
            {
                std::visit([&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T,
                        studio::content::mission::node_static_shape>)
                    {
                        glm::vec3 wp = dts_viewer::mis_pos_to_gl(p.xf.position);
                        std::array<float,3> col{ 0.4f, 0.7f, 0.4f };
                        if (p.data_block.name == "Generator" ||
                            p.data_block.name == "PortGenerator")
                                col = { 0.9f, 0.2f, 0.2f };
                        else if (p.data_block.name == "InventoryStation")
                                col = { 0.2f, 0.6f, 0.95f };
                        map_icons.push_back({ wp, col });
                        sensor_blips.push_back({ wp, col });
                    }
                }, n.payload);
                for (auto& c : n.children) self(self, c);
            };
            walk_icons(walk_icons, ter_mission->scene.root);
        }

        // Mission ambient sounds (spec 11/05).
        dts_viewer::MissionSoundsState mission_audio;
        if (ter_mission) {
            mission_audio = dts_viewer::mission_sounds_load(
                ter_mission->scene, ted_path.parent_path().parent_path());
            std::fprintf(stderr,
                "audio: registered %zu mission ambient voices%s\n",
                mission_audio.voices.size(),
                mission_audio.wind_active ? " + wind" : "");
        }

        Uint64 fps_last = SDL_GetPerformanceCounter();
        const Uint64 perf_freq  = SDL_GetPerformanceFrequency();
        Uint64 fps_window_start = fps_last;
        int    fps_window_frames = 0;

        int  screenshot_warmup = screenshot_path_ter.empty() ? -1 : 0;
        bool screenshot_done   = false;

        std::printf("keys: F1=wireframe  Tab=walk/free  F=spawn  F3=bounds  "
                    "WASD=fly  mouse=look  Esc=release  Q=quit\n");

        // Spec 25/04 — wire View menu toggles + quit to the local
        // flags. Capture-by-reference so menu clicks mutate the same
        // bools the keys do.
        {
            dts_viewer::MenuActions a;
            a.quit            = [&]{ running = false; };
            a.toggle_edit_mode = [&]{
                const bool to = !dts_viewer::edit_mode_active();
                dts_viewer::set_edit_mode(to);
                // Switch to Free camera so the user can fly freely
                // without the walk-mode terrain clamp fighting them.
                if (to) cam_mode = dts_viewer::CameraMode::Free;
            };
            a.is_wireframe    = [&]{ return wireframe; };
            a.toggle_wireframe= [&]{
                wireframe = !wireframe;
                std::printf("wireframe: %s\n", wireframe ? "on" : "off");
            };
            a.is_hud          = [&]{ return hud.visible; };
            a.toggle_hud      = [&]{ hud.visible = !hud.visible; };
            a.is_bbox         = [&]{ return show_bounds_debug; };
            a.toggle_bbox     = [&]{
                show_bounds_debug = !show_bounds_debug;
                std::printf("mission-bounds: %s\n",
                    show_bounds_debug ? "on" : "off");
            };
            a.is_free_camera  = [&]{ return cam_mode == dts_viewer::CameraMode::Free; };
            a.is_walk_camera  = [&]{ return cam_mode == dts_viewer::CameraMode::Walk; };
            a.set_free_camera = [&]{ cam_mode = dts_viewer::CameraMode::Free; };
            a.set_walk_camera = [&]{ cam_mode = dts_viewer::CameraMode::Walk; };
            a.is_asset_browser     = []{ return dts_viewer::asset_browser_visible_ref(); };
            a.toggle_asset_browser = []{
                bool& v = dts_viewer::asset_browser_visible_ref();
                v = !v;
            };
            a.is_inspector     = []{ return dts_viewer::inspector_visible_ref(); };
            a.toggle_inspector = []{
                bool& v = dts_viewer::inspector_visible_ref();
                v = !v;
            };
            // Spec 25/08 — Help menu wires browser-launch via open_url.
            a.help_user_guide   = []{ dts_viewer::open_url(dts_viewer::docs_url_for("index.html")); };
            a.help_controls     = []{ dts_viewer::open_url(dts_viewer::docs_url_for("controls.html")); };
            a.help_scripting    = []{ dts_viewer::open_url(dts_viewer::docs_url_for("scripting.html")); };
            a.help_online_docs  = []{ dts_viewer::open_url(dts_viewer::kOnlineDocsUrl); };
            a.help_report_issue = []{ dts_viewer::open_url(dts_viewer::kReportIssueUrl); };
            a.help_about        = []{ dts_viewer::about_modal_open(); };
            dts_viewer::set_menu_actions(a);
        }

        if (const char* em = std::getenv("DTS_TEST_EDIT_MODE"); em && *em == '1') {
            dts_viewer::set_edit_mode(true);
            cam_mode = dts_viewer::CameraMode::Free;
        }

        while (running) {
            // Spec 25/03 — drain the deferred-load queue. The picker
            // modal calls request_load_mission(...) on selection; we
            // re-exec into the new mission via the existing execv
            // path that the [ / ] keys use.
            if (auto pl = dts_viewer::take_pending_load();
                pl.kind == dts_viewer::LoadKind::Mission) {
                for (std::size_t i = 0; i < mission_reg.short_names.size(); ++i) {
                    if (mission_reg.short_names[i] != pl.name) continue;
                    fs::path next_ted = mission_reg.ted_paths[i];
                    std::printf("mission-switch: -> %s (menu)\n", pl.name.c_str());
                    std::fflush(stdout);
                    dts_viewer::imgui_shutdown();
                    SDL_GL_DeleteContext(ctx);
                    SDL_DestroyWindow(win);
                    SDL_Quit();
                    std::string ted_str = next_ted.string();
                    char* args[] = {
                        const_cast<char*>(self_arg0.c_str()),
                        const_cast<char*>("--mission"),
                        const_cast<char*>(ted_str.c_str()),
                        nullptr
                    };
                    execv(self_arg0.c_str(), args);
                    std::fprintf(stderr, "mission-switch: execv failed\n");
                    std::exit(1);
                }
            }
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                // Spec 25/01 — ImGui sees the event first. When it
                // returns true, the host suppresses its own handling
                // (mouse over a panel, keyboard in a text widget).
                bool imgui_consumed = dts_viewer::imgui_process_event(ev);
                if (imgui_consumed && ev.type != SDL_QUIT) continue;
                // Spec 25/02 — menu-shortcut dispatch. Cmd/Ctrl-O,
                // Cmd/Ctrl-Q, Cmd/Ctrl-R, Cmd/Ctrl-E, F1.
                if (dts_viewer::imgui_try_menu_shortcut(ev)) continue;
                switch (ev.type) {
                    case SDL_QUIT: running = false; break;
                    case SDL_KEYDOWN:
                        // Q now cycles weapons (track 12 spec 03); use the
                        // window-close button or Cmd-Q to quit.
                        if (ev.key.keysym.sym == SDLK_ESCAPE)
                            dts_viewer::toggle_mouse_capture(ter_cam);
                        if (ev.key.keysym.sym == SDLK_F1) {
                            hud.visible = !hud.visible;
                            dts_viewer::print_hud_snapshot(hud, ter_cam,
                                cam_mode, hud_mission_name);
                        }
                        if (ev.key.keysym.sym == SDLK_1) {
                            dts_viewer::select_weapon(pstate.inventory, 0);
                            std::fprintf(stderr, "weapon: slot 1\n");
                        }
                        if (ev.key.keysym.sym == SDLK_2) {
                            dts_viewer::select_weapon(pstate.inventory, 1);
                            std::fprintf(stderr, "weapon: slot 2\n");
                        }
                        if (ev.key.keysym.sym == SDLK_3) {
                            dts_viewer::select_weapon(pstate.inventory, 2);
                            std::fprintf(stderr, "weapon: slot 3\n");
                        }
                        if (ev.key.keysym.sym == SDLK_4) {
                            dts_viewer::select_weapon(pstate.inventory, 3);
                            std::fprintf(stderr, "weapon: slot 4\n");
                        }
                        if (ev.key.keysym.sym == SDLK_5) {
                            dts_viewer::select_weapon(pstate.inventory, 4);
                            std::fprintf(stderr, "weapon: slot 5\n");
                        }
                        if (ev.key.keysym.sym == SDLK_6) {
                            dts_viewer::select_weapon(pstate.inventory, 5);
                            std::fprintf(stderr, "weapon: slot 6\n");
                        }
                        if (ev.key.keysym.sym == SDLK_7) {
                            dts_viewer::select_weapon(pstate.inventory, 6);
                            std::fprintf(stderr, "weapon: slot 7\n");
                        }
                        if (ev.key.keysym.sym == SDLK_8) {
                            dts_viewer::select_weapon(pstate.inventory, 7);
                            std::fprintf(stderr, "weapon: slot 8\n");
                        }
                        if (ev.key.keysym.sym == SDLK_m) {
                            // M previously printed mission list; with the
                            // command map (13/06) also bound to M, toggle
                            // the map and also dump the mission list to
                            // stderr for parity with the prior behaviour.
                            show_command_map = !show_command_map;
                        }
                        if (ev.key.keysym.sym == SDLK_q) {
                            dts_viewer::cycle_weapon(pstate.inventory);
                            std::fprintf(stderr, "weapon: cycle -> %d\n",
                                pstate.inventory.active_slot);
                        }
                        if (ev.key.keysym.sym == SDLK_F12) {
                            int sw, sh; SDL_GL_GetDrawableSize(win, &sw, &sh);
                            // Render once without HUD (no extra pass needed —
                            // HUD only modifies title-bar; the screenshot
                            // captures the back buffer as-is.)
                            dts_viewer::capture_screenshot(sw, sh, {},
                                hud_mission_name);
                        }
                        if (ev.key.keysym.sym == SDLK_m) {
                            dts_viewer::print_mission_list(mission_reg);
                        }
                        if (ev.key.keysym.sym == SDLK_LEFTBRACKET ||
                            ev.key.keysym.sym == SDLK_RIGHTBRACKET) {
                            const int d = (ev.key.keysym.sym == SDLK_LEFTBRACKET) ? -1 : 1;
                            if (!mission_reg.short_names.empty()) {
                                long long idx = static_cast<long long>(mission_reg.current_index) + d;
                                long long n = static_cast<long long>(mission_reg.short_names.size());
                                while (idx < 0)  idx += n;
                                while (idx >= n) idx -= n;
                                fs::path next_ted = mission_reg.ted_paths[idx];
                                std::printf("mission-switch: -> %s\n",
                                    mission_reg.short_names[idx].c_str());
                                std::fflush(stdout);
                                dts_viewer::imgui_shutdown();
                                SDL_GL_DeleteContext(ctx);
                                SDL_DestroyWindow(win);
                                SDL_Quit();
                                std::string ted_str = next_ted.string();
                                char* args[] = {
                                    const_cast<char*>(self_arg0.c_str()),
                                    const_cast<char*>("--mission"),
                                    const_cast<char*>(ted_str.c_str()),
                                    nullptr
                                };
                                execv(self_arg0.c_str(), args);
                                std::fprintf(stderr,
                                    "mission-switch: execv failed\n");
                                std::exit(1);
                            }
                        }
                        if (ev.key.keysym.sym == SDLK_BACKQUOTE) {
                            wireframe = !wireframe;
                            std::printf("wireframe: %s\n", wireframe ? "on" : "off");
                        }
                        if (ev.key.keysym.sym == SDLK_F4) {
                            hud.show_terrain = !hud.show_terrain;
                            std::printf("layer terrain: %s\n",
                                hud.show_terrain ? "on" : "off");
                        }
                        if (ev.key.keysym.sym == SDLK_F5) {
                            hud.show_interiors = !hud.show_interiors;
                            std::printf("layer interiors: %s\n",
                                hud.show_interiors ? "on" : "off");
                        }
                        if (ev.key.keysym.sym == SDLK_F6) {
                            hud.show_markers = !hud.show_markers;
                            std::printf("layer markers: %s\n",
                                hud.show_markers ? "on" : "off");
                        }
                        if (ev.key.keysym.sym == SDLK_F7) {
                            hud.show_sky = !hud.show_sky;
                            show_sky = hud.show_sky;
                            std::printf("layer sky: %s\n",
                                hud.show_sky ? "on" : "off");
                        }
                        if (ev.key.keysym.sym == SDLK_F3) {
                            show_bounds_debug = !show_bounds_debug;
                            std::printf("bounds debug: %s\n",
                                show_bounds_debug ? "on" : "off");
                        }
                        if (ev.key.keysym.sym == SDLK_F2) {
                            int next = (static_cast<int>(lighting_mode) + 1) % 3;
                            lighting_mode = static_cast<dts_viewer::LightingMode>(next);
                            const char* name = (lighting_mode == dts_viewer::LightingMode::AmbientOnly)
                                ? "ambient-only"
                                : (lighting_mode == dts_viewer::LightingMode::SunOnly)
                                    ? "+sun" : "full";
                            std::printf("lighting: %s\n", name);
                        }
                        if (ev.key.keysym.sym == SDLK_TAB) {
                            cam_mode = (cam_mode == dts_viewer::CameraMode::Free)
                                ? dts_viewer::CameraMode::Walk
                                : dts_viewer::CameraMode::Free;
                            if (cam_mode == dts_viewer::CameraMode::Walk) {
                                std::array<float, 3> p{
                                    ter_cam.position.x,
                                    ter_cam.position.y,
                                    ter_cam.position.z };
                                p = dts_viewer::clamp_to_bounds(p, bounds);
                                ter_cam.position.x = p[0];
                                ter_cam.position.z = p[2];
                                // Seed PlayerState at terrain height so
                                // physics doesn't have to fall through
                                // the whole sky to reach the ground.
                                pstate.pos.x = ter_cam.position.x;
                                pstate.pos.z = ter_cam.position.z;
                                pstate.pos.y = height_sampler.valid()
                                    ? height_sampler.sample(pstate.pos.x, pstate.pos.z)
                                    : 0.0f;
                                pstate.vel = glm::vec3(0.0f);
                                pstate.on_ground = true;
                                pstate.prev_jump = false;
                                ter_cam.position.y = pstate.pos.y + ptune.eye_height;
                                std::printf("camera: walk mode (physics-driven)\n");
                            } else {
                                std::printf("camera: free mode\n");
                            }
                        }
                        if (ev.key.keysym.sym == SDLK_f && ter_mission) {
                            std::array<float, 3> here{
                                ter_cam.position.x,
                                ter_cam.position.y,
                                ter_cam.position.z };
                            auto dp = dts_viewer::nearest_drop_point(
                                ter_mission->scene, here);
                            if (dp) {
                                ter_cam.position.x = (*dp)[0];
                                ter_cam.position.z = (*dp)[2];
                                if (cam_mode == dts_viewer::CameraMode::Walk) {
                                    pstate.pos.x = ter_cam.position.x;
                                    pstate.pos.z = ter_cam.position.z;
                                    pstate.pos.y = height_sampler.valid()
                                        ? height_sampler.sample(pstate.pos.x, pstate.pos.z)
                                        : (*dp)[1];
                                    pstate.vel = glm::vec3(0.0f);
                                    pstate.on_ground = true;
                                    ter_cam.position.y = pstate.pos.y + ptune.eye_height;
                                } else {
                                    ter_cam.position.y = (*dp)[1] + 1.8f;
                                }
                                std::printf("teleport: drop point %.1f,%.1f,%.1f\n",
                                    (*dp)[0], (*dp)[1], (*dp)[2]);
                            } else {
                                std::fprintf(stderr,
                                    "teleport: no DropPointMarker found\n");
                            }
                        }
                        // Spec 14/13 — E mounts the nearest vehicle, R ejects.
                        if (ev.key.keysym.sym == SDLK_e) {
                            if (piloted_vehicle_idx >= 0) {
                                std::fprintf(stderr,
                                    "vehicle: already piloting slot %d\n",
                                    piloted_vehicle_idx);
                            } else {
                                glm::vec3 here = dts_viewer::player_eye(pstate, ptune);
                                int idx = dts_viewer::vehicle_find_mountable(
                                    ent_vehicles, here, vtune.mount_radius);
                                if (idx >= 0) {
                                    piloted_vehicle_idx = idx;
                                    ent_vehicles[idx].piloted = true;
                                    // Seed vehicle yaw from the current camera so
                                    // the first tick doesn't snap the chassis.
                                    ent_vehicles[idx].yaw = ter_cam.yaw;
                                    std::fprintf(stderr,
                                        "vehicle: mounted %s (slot %d)\n",
                                        ent_vehicles[idx].vehicle_dts.c_str(),
                                        idx);
                                } else {
                                    std::fprintf(stderr,
                                        "vehicle: no vehicle within %.1fm\n",
                                        vtune.mount_radius);
                                }
                            }
                        }
                        if (ev.key.keysym.sym == SDLK_r &&
                            piloted_vehicle_idx >= 0) {
                            auto& v = ent_vehicles[piloted_vehicle_idx];
                            v.piloted = false;
                            // Drop the player at the vehicle side; clear
                            // velocity so they don't inherit the chassis
                            // momentum (causes hilarious launches).
                            glm::vec3 drop = dts_viewer::vehicle_dismount_pos(v, vtune);
                            pstate.pos = drop - glm::vec3(0.0f, ptune.eye_height, 0.0f);
                            pstate.vel = glm::vec3(0.0f);
                            pstate.on_ground = false;
                            ter_cam.position = drop;
                            std::fprintf(stderr,
                                "vehicle: dismount @ (%.1f,%.1f,%.1f)\n",
                                drop.x, drop.y, drop.z);
                            piloted_vehicle_idx = -1;
                        }
                        break;
                    case SDL_MOUSEBUTTONDOWN:
                        if (!ter_cam.mouse_captured) {
                            dts_viewer::set_mouse_capture(ter_cam, true);
                        } else if (ev.button.button == SDL_BUTTON_LEFT) {
                            auto& w = dts_viewer::active_weapon(pstate.inventory);
                            // Beam weapons are driven by elf_tick / laser_tick
                            // using a per-frame fire-held flag (see below).
                            const bool beam_weapon =
                                (w.projectile == dts_viewer::ProjType::ELF) ||
                                (w.projectile == dts_viewer::ProjType::Laser);
                            if (!beam_weapon && w.equipped &&
                                w.cooldown <= 0.0f && w.ammo > 0) {
                                glm::vec3 origin = dts_viewer::player_eye(pstate, ptune);
                                glm::vec3 aim = dts_viewer::camera_forward(ter_cam);
                                if (dts_viewer::projectile_fire(
                                        proj_sys, proj_tune, w.projectile,
                                        origin, aim, /*owner_id*/0)) {
                                    --w.ammo;
                                    w.cooldown = w.fire_interval;
                                    if (w.ammo == 0) {
                                        dts_viewer::auto_switch_on_empty(
                                            pstate.inventory);
                                    }
                                }
                            }
                        }
                        break;
                    case SDL_MOUSEMOTION:
                        if (ter_cam.mouse_captured)
                            dts_viewer::handle_mouse_motion(ter_cam, ev.motion.xrel, ev.motion.yrel);
                        break;
                }
            }

            Uint64 now_ter = SDL_GetPerformanceCounter();
            float dt_ter = static_cast<float>(
                static_cast<double>(now_ter - fps_last) / static_cast<double>(perf_freq));
            fps_last = now_ter;
            if (cam_mode == dts_viewer::CameraMode::Free) {
                dts_viewer::update_camera_free(ter_cam, dt_ter);
            }
            dts_viewer::update_hud(hud, dt_ter);

            // ---- 60 Hz fixed-step PlayerState update (spec 09/03) ----
            {
                const Uint8* keys = SDL_GetKeyboardState(nullptr);
                dts_viewer::InputState in;
                if (dts_viewer::edit_mode_active()) {
                    // Spec 25/07 — edit mode suspends WASD/jet/jump so
                    // the user can fly the camera freely without the
                    // player controller fighting them.
                    in = {};
                } else {
                    in.fwd    = keys[SDL_SCANCODE_W];
                    in.back   = keys[SDL_SCANCODE_S];
                    in.left   = keys[SDL_SCANCODE_A];
                    in.right  = keys[SDL_SCANCODE_D];
                    // Shared Space key: in.jump triggers a single jump impulse on
                    // rising edge; in.jet activates jet thrust after the
                    // tap-vs-hold window (`jet_tap_seconds`).
                    in.jump   = keys[SDL_SCANCODE_SPACE];
                    in.jet    = keys[SDL_SCANCODE_SPACE];
                    in.sprint = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
                }

                // Yaw/pitch always come from the camera (mouse-look runs
                // per render-frame, never per fixed step).
                pstate.yaw   = ter_cam.yaw;
                pstate.pitch = ter_cam.pitch;

                if (cam_mode != dts_viewer::CameraMode::Walk) {
                    // Free mode: camera is authority, pstate mirrors for debug.
                    pstate.pos = ter_cam.position
                               - glm::vec3(0.0f, ptune.eye_height, 0.0f);
                    pstate.vel = glm::vec3(0.0f);
                    pstate.on_ground = false;
                }

                pstep_accumulator += dt_ter;
                int steps = 0;
                // Mouse-button hold state for continuous-fire weapons
                // (spec 12/07 beam weapons + future full-auto polish).
                // SDL_GetMouseState polls the OS-tracked state directly so
                // we don't need to mirror MOUSEBUTTONDOWN/UP events.
                Uint32 mbuttons = SDL_GetMouseState(nullptr, nullptr);
                const bool fire_held =
                    (mbuttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0 &&
                    ter_cam.mouse_captured;
                while (pstep_accumulator >= kFixedStep && steps < 5) {
                    // Spec 17/02 — drain script schedule() queue.
                    dts_viewer::mission_tick(mctx, kFixedStep);
                    dts_viewer::projectiles_update(
                        proj_sys, proj_tune, height_sampler, pstate, kFixedStep,
                        &ent_generators,
                        &on_generator_destroyed_callback);
                    // Spec 12/07 — beam-weapon ticks. Only one beam type
                    // can be "held" at a time (the active weapon); the
                    // tick calls are no-ops when fire_held is false.
                    {
                        const auto& aw = dts_viewer::active_weapon(pstate.inventory);
                        glm::vec3 origin = dts_viewer::player_eye(pstate, ptune);
                        glm::vec3 aim = dts_viewer::camera_forward(ter_cam);
                        const bool elf_active =
                            fire_held &&
                            aw.projectile == dts_viewer::ProjType::ELF &&
                            aw.equipped;
                        dts_viewer::elf_tick(proj_sys, proj_tune, height_sampler,
                            pstate, elf_active, origin, aim, /*owner_id*/0,
                            kFixedStep);
                        const bool laser_active =
                            aw.projectile == dts_viewer::ProjType::Laser &&
                            aw.equipped;
                        dts_viewer::laser_tick(proj_sys, proj_tune, height_sampler,
                            pstate, laser_active && fire_held,
                            origin, aim, kFixedStep);
                    }
                    // Spec 16/09 — script $thePlayer.fire() triggers a
                    // single shot through the same code path as a left
                    // mouse click. Latch is cleared after consumption.
                    if (pstate.script_fire_latch) {
                        pstate.script_fire_latch = false;
                        auto& w = dts_viewer::active_weapon(pstate.inventory);
                        const bool beam_weapon =
                            (w.projectile == dts_viewer::ProjType::ELF) ||
                            (w.projectile == dts_viewer::ProjType::Laser);
                        if (!beam_weapon && w.equipped &&
                            w.cooldown <= 0.0f && w.ammo > 0) {
                            glm::vec3 origin =
                                dts_viewer::player_eye(pstate, ptune);
                            glm::vec3 aim =
                                dts_viewer::camera_forward(ter_cam);
                            if (dts_viewer::projectile_fire(
                                    proj_sys, proj_tune, w.projectile,
                                    origin, aim, /*owner_id*/0)) {
                                --w.ammo;
                                w.cooldown = w.fire_interval;
                                if (w.ammo == 0) {
                                    dts_viewer::auto_switch_on_empty(
                                        pstate.inventory);
                                }
                            }
                        }
                    }
                    dts_viewer::weapons_tick_cooldowns(
                        pstate.inventory, kFixedStep);
                    dts_viewer::inv_stations_update(
                        inv_sys, pstate, ptune, kFixedStep);

                    // Track 14 entity ticks.
                    dts_viewer::tick_items(ent_items, pstate, ptune,
                        message_feed, kFixedStep);
                    const bool tp = dts_viewer::team_has_power(0, ent_generators);
                    dts_viewer::tick_turrets(ent_turrets, pstate,
                        message_feed, tp, kFixedStep,
                        &dts_viewer::hud2d_report_damage,
                        height_sampler.valid() ? &height_sampler : nullptr);
                    dts_viewer::tick_moveables(ent_moveables, pstate, kFixedStep);
                    dts_viewer::tick_triggers(ent_triggers, pstate,
                        message_feed, kFixedStep,
                        &on_trigger_enter_callback);

                    // Spec 14/13 — when piloted, the player's WASD/jet
                    // physics are suspended and the vehicle integrator
                    // owns the inputs instead.
                    if (piloted_vehicle_idx >= 0 &&
                        piloted_vehicle_idx <
                            static_cast<int>(ent_vehicles.size()))
                    {
                        auto& v = ent_vehicles[piloted_vehicle_idx];
                        dts_viewer::VehicleInput vin;
                        vin.fwd   = in.fwd;
                        vin.back  = in.back;
                        vin.left  = in.left;
                        vin.right = in.right;
                        vin.up    = in.jump;     // Space ascends
                        vin.down  = in.sprint;   // Shift descends
                        vin.target_yaw = ter_cam.yaw;
                        dts_viewer::vehicle_tick(
                            v, vtune, vin, height_sampler, kFixedStep);
                        // Pin the player to the chassis so the rest of
                        // the world (audio listener, splash damage)
                        // tracks the vehicle without separate plumbing.
                        pstate.pos = v.dyn_pos_gl
                            - glm::vec3(0.0f, ptune.eye_height, 0.0f);
                        pstate.vel = v.vel;
                        pstate.on_ground = false;
                    }
                    else if (cam_mode == dts_viewer::CameraMode::Walk) {
                        dts_viewer::player_update(
                            pstate, ptune, in, height_sampler, &bounds, kFixedStep);

                        // Footstep gait (spec 11/06).
                        const float h_speed = std::sqrt(
                            pstate.vel.x * pstate.vel.x +
                            pstate.vel.z * pstate.vel.z);
                        if (pstate.on_ground && !pstate.skiing &&
                            h_speed > kMinFootstepSpeed)
                        {
                            footstep_timer -= kFixedStep;
                            if (footstep_timer <= 0.0f) {
                                dts_viewer::audio_play_at(
                                    dts_viewer::WavSample{},  // placeholder
                                    pstate.pos, 4.0f, 30.0f, 1.0f, false);
                                footstep_timer = kStrideLength / h_speed;
                            }
                        } else {
                            footstep_timer = 0.0f;
                        }
                    }
                    pstep_accumulator -= kFixedStep;
                    ++steps;
                    ++pstep_count_window;
                }

                // Walk mode: pstate is authoritative — sync camera FROM it.
                if (cam_mode == dts_viewer::CameraMode::Walk) {
                    ter_cam.position.x = pstate.pos.x;
                    ter_cam.position.y = pstate.pos.y + ptune.eye_height;
                    ter_cam.position.z = pstate.pos.z;
                }
                // Spec 14/13 — piloting overrides the camera position
                // (first-person from the chassis centre). Camera yaw/pitch
                // are still mouse-driven; vehicle yaw tracks the camera.
                if (piloted_vehicle_idx >= 0 &&
                    piloted_vehicle_idx < static_cast<int>(ent_vehicles.size()))
                {
                    const auto& v = ent_vehicles[piloted_vehicle_idx];
                    ter_cam.position = v.dyn_pos_gl + glm::vec3(0.0f, 0.8f, 0.0f);
                }

                // Audio listener tracks the camera every frame (spec 11/03).
                {
                    glm::vec3 fwd = dts_viewer::camera_forward(ter_cam);
                    dts_viewer::audio_set_listener(
                        ter_cam.position, fwd, glm::vec3{0, 1, 0});
                }

                Uint32 now_ms = SDL_GetTicks();
                if (now_ms - pstep_log_last_ms >= 1000) {
                    std::fprintf(stderr,
                        "player: %d steps/s | pos=(%.1f,%.1f,%.1f) "
                        "vel=(%.1f,%.1f,%.1f) on_ground=%d coyote=%.2f "
                        "jet=%d fuel=%.0f lockout=%.2f\n",
                        pstep_count_window,
                        pstate.pos.x, pstate.pos.y, pstate.pos.z,
                        pstate.vel.x, pstate.vel.y, pstate.vel.z,
                        pstate.on_ground ? 1 : 0,
                        pstate.coyote_timer,
                        pstate.jet_active ? 1 : 0,
                        pstate.jet_fuel,
                        pstate.jet_lockout);
                    pstep_count_window = 0;
                    pstep_log_last_ms  = now_ms;
                }
            }

            int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.10f, 0.14f, 0.20f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glm::mat4 V   = dts_viewer::camera_view(ter_cam);
            glm::mat4 P   = dts_viewer::camera_projection(ter_cam, (float)w / (float)h);
            glm::mat4 MVP = P * V;

            if (sky_box && show_sky && hud.show_sky) {
                dts_viewer::draw_skybox(*sky_box, V, P);
            }

            // 14/12 — stars sit on a sphere; draw with view matrix that
            // ignores translation so they stay fixed to the sky dome.
            if (star_sys) {
                glm::mat4 Vt = V;
                Vt[3] = glm::vec4(0, 0, 0, 1);
                const float t = SDL_GetTicks() * 0.001f;
                dts_viewer::draw_particles(*star_sys, P * Vt, t,
                                           glm::vec3(1.0f, 1.0f, 0.95f), 3.0f);
            }

            if (hud.show_terrain) {
                glUseProgram(terrain_prog);
                glUniformMatrix4fv(u_ter_mvp, 1, GL_FALSE, glm::value_ptr(MVP));
                dts_viewer::apply_scene_lighting(terrain_prog,
                    dts_viewer::masked_lighting(lighting, lighting_mode));
                GLint u_layers   = glGetUniformLocation(terrain_prog, "u_terrain_layers");
                GLint u_uv_scale = glGetUniformLocation(terrain_prog, "u_terrain_uv_scale");
                GLint u_tex      = glGetUniformLocation(terrain_prog, "u_terrain_tex");
                if (terrain_tex.valid()) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_tex.texture);
                    if (u_tex >= 0)      glUniform1i(u_tex, 0);
                    if (u_layers >= 0)   glUniform1i(u_layers, terrain_tex.layer_count);
                    if (u_uv_scale >= 0) glUniform1f(u_uv_scale, metres_per_quad);
                } else {
                    if (u_layers >= 0)   glUniform1i(u_layers, 0);
                }

                if (wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                dts_viewer::draw_terrain(terrain, u_ter_mvp);
                if (wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            }

            if (show_bounds_debug) {
                glUseProgram(flat_prog);
                dts_viewer::draw_bounds_debug(bounds, u_flat_mvp, u_flat_color, MVP);
            }

            if (hud.show_markers && ter_mission) {
                glUseProgram(flat_prog);
                glDisable(GL_DEPTH_TEST);
                dts_viewer::draw_markers_debug(
                    ter_mission->scene, u_flat_mvp, u_flat_color, MVP);
                glEnable(GL_DEPTH_TEST);
            }

            // Render placed interiors (the cross-VOL resolver bundled
            // DIS/DIG/DIL/DML for each node_interior; we draw them at
            // their authored transforms).
            if (ter_mission && hud.show_interiors && !placed_interiors.empty()) {
                glUseProgram(int_prog_m);
                for (auto& pi : placed_interiors) {
                    // Interior meshes carry vertex data in Tribes Z-up
                    // coords; the MIS transform is also Tribes coords.
                    // mis_world_matrix builds M = P_gl * T * R so the
                    // building stands upright at the right yaw under
                    // GL Y-up.
                    glm::mat4 M = dts_viewer::mis_world_matrix(
                        pi.xf.position, pi.xf.rotation);
                    glm::mat4 MVP_pi = MVP * M;
                    dts_viewer::draw_interior(pi.mesh,
                        int_u_mvp_m, int_u_has_texture_m, int_u_tex0_m,
                        int_u_lightmap_m, int_u_has_lightmap_m,
                        MVP_pi);
                }
            }

            // Spec 14/09 — render entity DTS shapes when the datablock
            // resolves to a known shape in the mission's mounted VOLs;
            // otherwise fall back to the Tier-B coloured wireframe cube so
            // missing-asset cases stay visible rather than vanishing.
            if (ter_mission) {
                auto lit = dts_viewer::masked_lighting(lighting, lighting_mode);

                auto draw_shape_or_cube =
                    [&](const studio::content::mission::transform& xf,
                        const std::string& datablock,
                        float fallback_size,
                        const std::array<float,3>& fallback_col,
                        const glm::vec3* override_pos = nullptr)
                {
                    const dts_viewer::AssetShape* sh =
                        asset_cache.try_load_for_datablock(datablock);
                    if (sh) {
                        std::array<float,3> pos = xf.position;
                        if (override_pos) {
                            // moveable: position is in GL coords already, but
                            // mis_world_matrix wants Tribes coords. Convert
                            // back: GL (x, y, z) <- Tribes (x, z, y).
                            pos = { override_pos->x,
                                    override_pos->z,
                                    override_pos->y };
                        }
                        glm::mat4 M = dts_viewer::mis_world_matrix(pos, xf.rotation);
                        asset_cache.render(*sh, M, V, P, lit);
                    } else {
                        glm::vec3 wp = override_pos
                            ? *override_pos
                            : dts_viewer::mis_pos_to_gl(xf.position);
                        glUseProgram(flat_prog);
                        dts_viewer::render_entity_cube(wp, fallback_size,
                            fallback_col, u_flat_mvp, u_flat_color, MVP);
                    }
                };

                for (auto& ss : ent_statics) {
                    std::array<float,3> col{ 0.6f, 0.6f, 0.6f };
                    if (ss.data_block_name == "Generator" ||
                        ss.data_block_name == "PortGenerator")
                        col = { 0.9f, 0.2f, 0.2f };
                    else if (ss.data_block_name == "InventoryStation")
                        col = { 0.2f, 0.6f, 0.95f };
                    else if (ss.data_block_name == "vehiclePad")
                        col = { 0.9f, 0.7f, 0.1f };
                    draw_shape_or_cube(ss.xf, ss.data_block_name, 3.0f, col);
                }
                for (auto& it : ent_items) {
                    if (!it.active) continue;
                    draw_shape_or_cube(it.xf, it.data_block_name, 1.0f,
                        { 1.0f, 0.4f, 1.0f });
                }
                for (auto& tu : ent_turrets) {
                    if (tu.destroyed) continue;
                    draw_shape_or_cube(tu.xf, tu.data_block_name, 2.0f,
                        { 0.95f, 0.1f, 0.1f });
                }
                for (auto& m : ent_moveables) {
                    glm::vec3 wp = dts_viewer::moveable_position(m);
                    draw_shape_or_cube(m.xf, m.data_block_name, 4.0f,
                        { 0.4f, 0.7f, 1.0f }, &wp);
                }
                for (auto& v : ent_vehicles) {
                    if (!v.visible) continue;
                    // Vehicle pad first (the ground patch under the vehicle).
                    draw_shape_or_cube(v.pad_xf, v.pad_data_block, 4.0f,
                        { 0.9f, 0.7f, 0.1f });
                    // Spec 14/13 — once the vehicle has been mounted at
                    // least once it's `dynamic`: render at its live
                    // position + yaw instead of the static pad transform.
                    const dts_viewer::AssetShape* sh =
                        asset_cache.try_load_by_filename(v.vehicle_dts);
                    if (v.dynamic) {
                        glm::mat4 M = glm::translate(glm::mat4(1.0f),
                            v.dyn_pos_gl);
                        M = glm::rotate(M, v.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                        if (sh) {
                            asset_cache.render(*sh, M, V, P, lit);
                        } else {
                            glUseProgram(flat_prog);
                            dts_viewer::render_entity_cube(v.dyn_pos_gl, 4.0f,
                                { 0.1f, 0.8f, 0.9f },
                                u_flat_mvp, u_flat_color, MVP);
                        }
                    } else {
                        std::array<float,3> vpos = v.pad_xf.position;
                        vpos[2] += 1.5f;  // Tribes Z is up
                        studio::content::mission::transform vxf = v.pad_xf;
                        vxf.position = vpos;
                        if (sh) {
                            glm::mat4 M = dts_viewer::mis_world_matrix(
                                vxf.position, vxf.rotation);
                            asset_cache.render(*sh, M, V, P, lit);
                        } else {
                            glm::vec3 wp = dts_viewer::mis_pos_to_gl(vxf.position);
                            glUseProgram(flat_prog);
                            dts_viewer::render_entity_cube(wp, 4.0f,
                                { 0.1f, 0.8f, 0.9f },
                                u_flat_mvp, u_flat_color, MVP);
                        }
                    }
                }
            }

            // Spec 12/07 — beam-weapon world-space visuals.
            // ELF: a coloured line from the muzzle to the beam end.
            // Laser: a single-pixel marker cube where the player painted.
            // Both ride flat_prog (used for marker / bounds debug).
            {
                static GLuint beam_vao = 0, beam_vbo = 0;
                if (beam_vao == 0) {
                    glGenVertexArrays(1, &beam_vao);
                    glGenBuffers(1, &beam_vbo);
                    glBindVertexArray(beam_vao);
                    glBindBuffer(GL_ARRAY_BUFFER, beam_vbo);
                    glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(float),
                                 nullptr, GL_DYNAMIC_DRAW);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                          3 * sizeof(float), nullptr);
                }
                if (proj_sys.elf.active) {
                    glUseProgram(flat_prog);
                    if (u_flat_mvp >= 0)
                        glUniformMatrix4fv(u_flat_mvp, 1, GL_FALSE,
                                           glm::value_ptr(MVP));
                    if (u_flat_color >= 0) {
                        // Bright cyan when the raycast hits a dynamic target,
                        // pale blue otherwise. Helps the player see contact.
                        const float r = proj_sys.elf.hit_target ? 0.4f : 0.6f;
                        const float g = proj_sys.elf.hit_target ? 1.0f : 0.85f;
                        const float b = 1.0f;
                        glUniform3f(u_flat_color, r, g, b);
                    }
                    const float verts[] = {
                        proj_sys.elf.start.x, proj_sys.elf.start.y, proj_sys.elf.start.z,
                        proj_sys.elf.end.x,   proj_sys.elf.end.y,   proj_sys.elf.end.z,
                    };
                    glBindVertexArray(beam_vao);
                    glBindBuffer(GL_ARRAY_BUFFER, beam_vbo);
                    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
                    glLineWidth(3.0f);
                    glDrawArrays(GL_LINES, 0, 2);
                    glLineWidth(1.0f);
                    glBindVertexArray(0);
                }
                if (proj_sys.laser.marker_active) {
                    glUseProgram(flat_prog);
                    // Fade the marker as it ages so the player sees it expire.
                    const float lt = proj_tune.laser_marker_lifetime;
                    const float fade = (lt > 0.0f)
                        ? std::min(1.0f, proj_sys.laser.marker_remaining / lt)
                        : 1.0f;
                    glm::mat4 M = glm::translate(glm::mat4(1.0f),
                        proj_sys.laser.marker_pos);
                    M = glm::scale(M, glm::vec3(0.6f));
                    glm::mat4 MVP_m = MVP * M;
                    if (u_flat_mvp >= 0)
                        glUniformMatrix4fv(u_flat_mvp, 1, GL_FALSE,
                                           glm::value_ptr(MVP_m));
                    if (u_flat_color >= 0)
                        glUniform3f(u_flat_color,
                                    1.0f, 0.3f * fade, 0.3f * fade);
                    // Reuse the simple line endpoints as a tiny + in 3D
                    // along world Y: cheap and unmissable.
                    const float cross[] = {
                        -1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
                         0.0f,-1.0f, 0.0f,   0.0f, 1.0f, 0.0f,
                         0.0f, 0.0f,-1.0f,   0.0f, 0.0f, 1.0f,
                    };
                    glBindVertexArray(beam_vao);
                    glBindBuffer(GL_ARRAY_BUFFER, beam_vbo);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(cross),
                                 cross, GL_DYNAMIC_DRAW);
                    glLineWidth(2.0f);
                    glDrawArrays(GL_LINES, 0, 6);
                    glLineWidth(1.0f);
                    // Resize the buffer back to the 2-vertex line layout
                    // for the next ELF draw.
                    glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(float),
                                 nullptr, GL_DYNAMIC_DRAW);
                    glBindVertexArray(0);
                }
            }

            // 14/12 — snow steps + draws after world geometry so flakes
            // occlude properly.
            if (snow_sys) {
                dts_viewer::step_snow(*snow_sys,
                    1.0f / 60.0f, ter_cam.position);
                const float t = SDL_GetTicks() * 0.001f;
                dts_viewer::draw_particles(*snow_sys, MVP, t,
                    glm::vec3(0.95f, 0.97f, 1.0f), 4.0f);
            }

            // Spec 13/07 — text overlays on the HUD route through ImGui's
            // foreground draw list, which only accepts AddText calls while
            // a frame is in progress. Bracket the HUD block with
            // imgui_begin_frame so text_draw() lands in the live frame;
            // imgui_end_frame runs after to flush both menus and HUD text.
            dts_viewer::imgui_begin_frame();
            if (hud.visible) {
                {
                    const float lcf = (proj_tune.laser_charge_seconds > 0.0f)
                        ? (proj_sys.laser.charge_t / proj_tune.laser_charge_seconds)
                        : 0.0f;
                    dts_viewer::hud2d_render(pstate, ptune, w, h, lcf);
                }
                // Spec 14/13 — top-center vehicle speed readout while piloting.
                if (piloted_vehicle_idx >= 0 &&
                    piloted_vehicle_idx < static_cast<int>(ent_vehicles.size()))
                {
                    const auto& v = ent_vehicles[piloted_vehicle_idx];
                    const float h_speed = std::sqrt(
                        v.vel.x * v.vel.x + v.vel.z * v.vel.z);
                    const float v_speed = v.vel.y;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                        "PILOTING %s | %.0f m/s | vert %+.0f m/s | R: eject",
                        v.vehicle_dts.c_str(), h_speed, v_speed);
                    dts_viewer::text_draw(
                        16.0f, 36.0f, buf, {1.0f, 0.95f, 0.5f}, 16.0f);
                }
                dts_viewer::hud2d_render_compass(
                    pstate.yaw, compass_team_ticks, pstate.pos, w, h);
                dts_viewer::hud2d_render_sensor(
                    pstate.yaw, pstate.pos, sensor_blips, 200.0f, w, h);
                dts_viewer::hud2d_render_message_feed(message_feed, w, h);
                dts_viewer::hud2d_render_script_text(hud_bindings_state, w, h);
                dts_viewer::hud2d_render_damage_reticle(w, h);
                if (show_command_map) {
                    dts_viewer::hud2d_render_command_map(
                        block.heights.data(),
                        static_cast<int>(block.size[0]) + 1,
                        metres_per_quad,
                        map_icons, pstate.pos, pstate.yaw, w, h,
                        terrain_origin.x, terrain_origin.y);
                }
            }
            dts_viewer::hud2d_tick(dt_ter);

            // Update window-title HUD once a second.
            {
                Uint32 now_ms = SDL_GetTicks();
                static Uint32 last_title = 0;
                if (now_ms - last_title >= 500) {
                    dts_viewer::refresh_hud_window_title(win, hud, ter_cam,
                        cam_mode, hud_mission_name);
                    last_title = now_ms;
                }
            }

            // Spec 25/01 — flush the ImGui frame (begin lives up above with
            // the HUD block so spec 13/07 text_draw() lands in this frame).
            dts_viewer::imgui_end_frame();

            SDL_GL_SwapWindow(win);

            // Screenshot capture.
            if (screenshot_warmup >= 0 && !screenshot_done) {
                ++screenshot_warmup;
                if (screenshot_warmup >= 5) {
                    int sw, sh; SDL_GL_GetDrawableSize(win, &sw, &sh);
                    std::vector<std::uint8_t> rgb((std::size_t)sw * sh * 3);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadBuffer(GL_BACK);
                    glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
                    // Flip rows (GL is bottom-up, PPM is top-down).
                    std::vector<std::uint8_t> flipped(rgb.size());
                    const std::size_t row_bytes = (std::size_t)sw * 3;
                    for (int ry = 0; ry < sh; ++ry) {
                        std::memcpy(&flipped[(sh - 1 - ry) * row_bytes],
                                    &rgb[(std::size_t)ry * row_bytes], row_bytes);
                    }
                    std::ofstream f(screenshot_path_ter, std::ios::binary);
                    if (f) {
                        f << "P6\n" << sw << " " << sh << "\n255\n";
                        f.write(reinterpret_cast<const char*>(flipped.data()),
                                flipped.size());
                        std::fprintf(stderr, "screenshot: wrote %s (%dx%d)\n",
                            screenshot_path_ter.c_str(), sw, sh);
                    } else {
                        std::fprintf(stderr, "screenshot: failed to open %s\n",
                            screenshot_path_ter.c_str());
                    }
                    screenshot_done = true;
                    running = false;
                }
            }

            // FPS counter.
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

        dts_viewer::mission_unload(mctx);
        dts_viewer::mission_sounds_unload(mission_audio);
        dts_viewer::audio_shutdown();
        dts_viewer::hud2d_shutdown();
        dts_viewer::imgui_shutdown();
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 0;
    }

    // ---- --mission-info <name> mode: parse MIS + print summary ----
    if (argc >= 3 && std::string(argv[1]) == "--mission-info") {
        const std::string name = argv[2];
        const fs::path missions_dir = "/Users/v/code/tribes-emscripten/tribes-game/base/missions";
        const fs::path base_dir     = "/Users/v/code/tribes-emscripten/tribes-game/base";

        // Support direct .mis path too.
        fs::path direct(name);
        std::optional<dts_viewer::LoadedMission> lm;
        if (direct.extension() == ".mis" && fs::exists(direct)) {
            std::ifstream f(direct);
            if (f) {
                try {
                    auto parsed = studio::content::mission::read_mis_file(f);
                    dts_viewer::LoadedMission m;
                    m.mis_path = direct;
                    m.scene = studio::content::mission::build_scene(parsed);
                    m.mission_type = m.scene.trailer.game_mission_type.value_or("");
                    for (const auto& sv : m.scene.volumes_in_order) {
                        fs::path resolved;
                        for (const auto& dir : { base_dir, missions_dir }) {
                            for (const auto& ent : fs::directory_iterator(dir)) {
                                if (!ent.is_regular_file()) continue;
                                auto p = ent.path();
                                auto lo = p.filename().string();
                                for (auto& c : lo) c = std::tolower((unsigned char)c);
                                auto svlo = sv.file_name;
                                for (auto& c : svlo) c = std::tolower((unsigned char)c);
                                if (lo == svlo) { resolved = p; break; }
                            }
                            if (!resolved.empty()) break;
                        }
                        m.mounted_vols.push_back(
                            resolved.empty() ? missions_dir / sv.file_name : resolved);
                    }
                    lm = std::move(m);
                } catch (...) {}
            }
        } else {
            lm = dts_viewer::load_mission(missions_dir, base_dir, name);
        }

        if (!lm) {
            std::fprintf(stderr, "mission-info: cannot load mission '%s'\n", name.c_str());
            // Suggest similar names.
            std::fprintf(stderr, "Available missions:\n");
            if (fs::is_directory(missions_dir)) {
                for (const auto& ent : fs::directory_iterator(missions_dir)) {
                    if (ent.path().extension() == ".mis")
                        std::fprintf(stderr, "  %s\n", ent.path().stem().string().c_str());
                }
            }
            return 1;
        }
        dts_viewer::print_mission_summary(*lm, std::cout);
        return 0;
    }

    fs::path vol_path = argv[1];

    // spec 09: --grid N renders N*N instances on a grid with desync'd phase.
    // --screenshot <path.ppm> captures one frame to PPM after auto-playing
    // the first animated sequence for ~1.5s, then exits (for the acceptance
    // image). Both flags are optional and order-independent after argv[1].
    int  grid_n = 1;
    std::string screenshot_path;
    std::string interior_match; // non-empty when --interior <name> was given
    // Re-parse argv for these flags AFTER the existing positional dts-match
    // logic runs; we strip them out by replacing with empty strings so the
    // legacy code path (which looks at argv[2]) ignores them. Cleaner than
    // refactoring the whole argv handling.
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--grid" && i + 1 < argc) {
            grid_n = std::max(1, std::atoi(argv[i + 1]));
            argv[i] = (char*)"";
            argv[i + 1] = (char*)"";
            ++i;
        } else if (a == "--screenshot" && i + 1 < argc) {
            screenshot_path = argv[i + 1];
            argv[i] = (char*)"";
            argv[i + 1] = (char*)"";
            ++i;
        } else if (a == "--interior" && i + 1 < argc) {
            interior_match = argv[i + 1];
            for (auto& c : interior_match) c = (char)std::tolower((unsigned char)c);
            argv[i] = (char*)"";
            argv[i + 1] = (char*)"";
            ++i;
        }
    }

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

    // ---- --interior mode: DIS/DIG interior renderer --------------------------------
    if (!interior_match.empty()) {
        // Collect sibling VOLs.
        std::vector<fs::path> int_vols;
        int_vols.push_back(vol_path);
        {
            const fs::path dir = vol_path.parent_path();
            if (!dir.empty() && fs::exists(dir)) {
                for (const auto& ent : fs::directory_iterator(dir)) {
                    if (!ent.is_regular_file()) continue;
                    auto p = ent.path();
                    auto ext = p.extension().string();
                    for (auto& c : ext) c = std::tolower((unsigned char)c);
                    if (ext != ".vol") continue;
                    if (fs::equivalent(p, vol_path)) continue;
                    int_vols.push_back(p);
                }
            }
        }

        // Find DIS file.
        std::vector<char> dis_bytes;
        std::string dis_name;
        for (const auto& v : int_vols) {
            if (read_named_file_from_vol(v, interior_match, ".dis", dis_bytes, dis_name)) break;
        }
        if (dis_bytes.empty()) {
            std::fprintf(stderr, "interior: no DIS matching '%s' found\n", interior_match.c_str());
            return 1;
        }

        // Parse DIS.
        std::stringstream dis_ss(std::string(dis_bytes.begin(), dis_bytes.end()));
        auto dis_opt = studio::content::dis::parse_dis(dis_ss);
        if (!dis_opt) {
            std::fprintf(stderr, "interior: failed to parse DIS '%s'\n", dis_name.c_str());
            return 1;
        }
        const auto& dis = *dis_opt;
        std::printf("interior: DIS '%s' — %zu LODs, DML='%s'\n",
            dis_name.c_str(), dis.lods.size(), dis.material_list_file.c_str());

        // Pick highest-detail LOD (last = largest min_pixels = most detail).
        const std::string dig_name = dis.lods.empty() ? "" : dis.lods.back().geometry_file;
        if (dig_name.empty()) {
            std::fprintf(stderr, "interior: DIS has no LOD records\n");
            return 1;
        }
        std::string dig_lower = dig_name;
        for (auto& c : dig_lower) c = std::tolower((unsigned char)c);

        // Find and parse DIG.
        std::vector<char> dig_bytes;
        std::string dig_found;
        for (const auto& v : int_vols) {
            if (read_named_file_from_vol(v, dig_lower, ".dig", dig_bytes, dig_found)) break;
        }
        if (dig_bytes.empty()) {
            std::fprintf(stderr, "interior: DIG '%s' not found\n", dig_name.c_str());
            return 1;
        }
        std::stringstream dig_ss(std::string(dig_bytes.begin(), dig_bytes.end()));
        auto dig_opt = studio::content::dig::read_dig_file(dig_ss);
        if (!dig_opt) {
            std::fprintf(stderr, "interior: failed to parse DIG '%s'\n", dig_found.c_str());
            return 1;
        }
        std::printf("interior: DIG '%s' — %zu surfaces, %zu pts3, %zu pts2\n",
            dig_found.c_str(),
            dig_opt->surfaces.size(), dig_opt->points3.size(), dig_opt->points2.size());

        // Find and parse DML.
        std::string dml_lower = dis.material_list_file;
        for (auto& c : dml_lower) c = std::tolower((unsigned char)c);
        std::vector<char> dml_bytes;
        std::string dml_found;
        for (const auto& v : int_vols) {
            if (read_named_file_from_vol(v, dml_lower, ".dml", dml_bytes, dml_found)) break;
        }
        if (dml_bytes.empty()) {
            std::fprintf(stderr, "interior: DML '%s' not found\n",
                dis.material_list_file.c_str());
            return 1;
        }
        std::stringstream dml_ss(std::string(dml_bytes.begin(), dml_bytes.end()));
        auto dml_opt = studio::content::dml::get_dml_data(dml_ss);
        if (!dml_opt) {
            std::fprintf(stderr, "interior: failed to parse DML '%s'\n", dml_found.c_str());
            return 1;
        }

        // ---- Spec 06-06: load the stock DIL lightmap, if available. ----
        // The DIS lists one DIL filename per light state; each LOD has a
        // `light_state_index` selecting its DIL (each LOD has its own
        // matching DIL since the surface count differs per LOD). Mission
        // -specific DILs and the IndexEntry remap are a later spec —
        // v1 uses the per-LOD stock DIL only.
        std::optional<studio::content::interior::dil_file> dil_opt;
        std::string dil_name;
        if (!dis.lods.empty() && !dis.lightmap_files.empty()) {
            const std::uint32_t lsi = dis.lods.back().light_state_index;
            if (lsi < dis.lightmap_files.size()) {
                dil_name = dis.lightmap_files[lsi];
            } else {
                dil_name = dis.lightmap_files.front();
            }
        }
        if (!dil_name.empty()) {
            std::string dil_lower = dil_name;
            for (auto& c : dil_lower) c = std::tolower((unsigned char)c);
            std::vector<char> dil_bytes;
            std::string dil_found;
            for (const auto& v : int_vols) {
                if (read_named_file_from_vol(v, dil_lower, ".dil", dil_bytes, dil_found)) break;
            }
            if (!dil_bytes.empty()) {
                std::stringstream dil_ss(std::string(dil_bytes.begin(), dil_bytes.end()));
                dil_opt = studio::content::interior::parse_dil(dil_ss);
                if (dil_opt) {
                    std::printf("interior: DIL '%s' — %zu surfaces, %zu states, "
                                "%zu lights, %zu mapData bytes, light_scale_shift=%d\n",
                        dil_found.c_str(),
                        dil_opt->surfaces.size(), dil_opt->states.size(),
                        dil_opt->lights.size(), dil_opt->map_data.size(),
                        dil_opt->light_scale_shift);
                } else {
                    std::fprintf(stderr,
                        "interior: failed to parse DIL '%s' — rendering without lightmap\n",
                        dil_found.c_str());
                }
            } else {
                std::fprintf(stderr,
                    "interior: DIL '%s' not found — rendering without lightmap\n",
                    dil_name.c_str());
            }
        }

        // MaterialResolver.
        dts_viewer::MaterialResolver int_resolver;
        for (const auto& v : int_vols) int_resolver.add_vol(v);

        // SDL + GL.
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 3;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

        SDL_Window* win_i = SDL_CreateWindow("dts-viewer (interior)",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
        SDL_GLContext ctx_i = SDL_GL_CreateContext(win_i);
        SDL_GL_SetSwapInterval(1);
#if !defined(__APPLE__)
        glewExperimental = GL_TRUE;
        glewInit();
#endif
        std::printf("GL_VERSION: %s\n", glGetString(GL_VERSION));

        // Load palettes for texture expansion.
        std::vector<Palette> int_pals;
        auto try_int_ppl = [&](const std::string& match, const std::string& ext) {
            std::vector<char> b; std::string fn;
            for (const auto& v : int_vols) {
                if (read_named_file_from_vol(v, match, ext, b, fn)) {
                    std::stringstream ss(std::string(b.begin(), b.end()));
                    auto pals = load_ppl(ss);
                    for (auto& p : pals) int_pals.push_back(std::move(p));
                    return true;
                }
            }
            return false;
        };
        try_int_ppl("shell.ppl", ".ppl");
        const char* world_ppls[] = {
            "lush.day.ppl", "ice.day.ppl", "desert.day.ppl",
            "mars.day.ppl", "mud.day.ppl", "alien.day.ppl"
        };
        for (const char* wp : world_ppls) { if (try_int_ppl(wp, ".ppl")) break; }
        auto int_pal_map = by_index(int_pals);

        // Build mesh (with lightmap atlas if DIL parsed).
        dts_viewer::InteriorMesh int_mesh = dts_viewer::build_interior_mesh(
            *dig_opt, *dml_opt, int_resolver, int_pal_map,
            dil_opt ? &(*dil_opt) : nullptr);
        if (!int_mesh.valid()) {
            std::fprintf(stderr, "interior: no renderable geometry\n");
            SDL_GL_DeleteContext(ctx_i);
            SDL_DestroyWindow(win_i);
            SDL_Quit();
            return 2;
        }
        std::printf("interior: %d indices, %zu ranges\n",
            int_mesh.index_count, int_mesh.ranges.size());

        // Spec 06-06 two-sampler interior shader. Vertex layout:
        //   loc 0 = position    (vec3)
        //   loc 1 = normal      (vec3) — unused; kept for VAO completeness
        //   loc 2 = diffuse UV  (vec2)
        //   loc 3 = lightmap UV (vec2)
        //
        // Fragment combine:
        //   final = diffuse * lightmap * 2.0
        // The *2.0 compensates for the 4-bit precision of the DIL pixel
        // channels (per spec note). With no lightmap bound the sample is
        // forced to (0.5,0.5,0.5) so the multiplier reproduces an unlit
        // baseline.
        static const char* INT_VS_SRC = R"(
#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec2 a_uv_lm;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec2 v_uv_lm;
void main() {
    v_uv    = a_uv;
    v_uv_lm = a_uv_lm;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";
        static const char* INT_FS_SRC = R"(
#version 410 core
in vec2 v_uv;
in vec2 v_uv_lm;
uniform sampler2D u_tex0;
uniform sampler2D u_lightmap;
uniform bool u_has_texture;
uniform bool u_has_lightmap;
out vec4 frag;
void main() {
    vec4 diffuse = u_has_texture
        ? texture(u_tex0, v_uv)
        : vec4(0.75, 0.78, 0.82, 1.0);
    vec3 lm = u_has_lightmap
        ? texture(u_lightmap, v_uv_lm).rgb
        : vec3(0.5, 0.5, 0.5);
    vec3 lit = diffuse.rgb * lm * 2.0;
    frag = vec4(lit, diffuse.a);
}
)";

        GLuint int_prog = link_program(
            compile_shader(GL_VERTEX_SHADER,   INT_VS_SRC),
            compile_shader(GL_FRAGMENT_SHADER, INT_FS_SRC));
        GLint int_u_mvp          = glGetUniformLocation(int_prog, "u_mvp");
        GLint int_u_has_texture  = glGetUniformLocation(int_prog, "u_has_texture");
        GLint int_u_tex0         = glGetUniformLocation(int_prog, "u_tex0");
        GLint int_u_lightmap     = glGetUniformLocation(int_prog, "u_lightmap");
        GLint int_u_has_lightmap = glGetUniformLocation(int_prog, "u_has_lightmap");

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_MULTISAMPLE);
        glDisable(GL_CULL_FACE);

        glm::vec3 ic = 0.5f * (int_mesh.bbox_min + int_mesh.bbox_max);
        float     ir = 0.5f * glm::length(int_mesh.bbox_max - int_mesh.bbox_min);
        if (ir < 1.0f) ir = 1.0f;

        float yaw_i = 0.6f, pitch_i = 0.4f, dist_i = ir * 2.5f;
        bool drag_i = false; int lx_i = 0, ly_i = 0;
        bool wf_i = false, run_i = true;
        int sc_warm_i = screenshot_path.empty() ? -1 : 0;
        bool sc_done_i = false;

        std::printf("keys: F1=wireframe  drag=orbit  scroll=zoom  Q/Esc=quit\n");

        while (run_i) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                    case SDL_QUIT: run_i = false; break;
                    case SDL_KEYDOWN:
                        if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q)
                            run_i = false;
                        if (ev.key.keysym.sym == SDLK_F1) {
                            wf_i = !wf_i;
                            std::printf("wireframe: %s\n", wf_i ? "on" : "off");
                        }
                        break;
                    case SDL_MOUSEBUTTONDOWN:
                        if (ev.button.button == SDL_BUTTON_LEFT) {
                            drag_i = true; lx_i = ev.button.x; ly_i = ev.button.y;
                        }
                        break;
                    case SDL_MOUSEBUTTONUP:
                        if (ev.button.button == SDL_BUTTON_LEFT) drag_i = false;
                        break;
                    case SDL_MOUSEMOTION:
                        if (drag_i) {
                            yaw_i   += (ev.motion.x - lx_i) * 0.005f;
                            pitch_i += (ev.motion.y - ly_i) * 0.005f;
                            pitch_i = glm::clamp(pitch_i, -1.5f, 1.5f);
                            lx_i = ev.motion.x; ly_i = ev.motion.y;
                        }
                        break;
                    case SDL_MOUSEWHEEL:
                        dist_i *= (ev.wheel.y > 0) ? 0.9f : 1.1f;
                        dist_i = glm::clamp(dist_i, ir * 0.05f, ir * 10.0f);
                        break;
                }
            }

            int wi, hi; SDL_GL_GetDrawableSize(win_i, &wi, &hi);
            glViewport(0, 0, wi, hi);
            glClearColor(0.10f, 0.14f, 0.20f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glm::vec3 eye_i = ic + dist_i * glm::vec3(
                std::cos(pitch_i) * std::sin(yaw_i),
                std::sin(pitch_i),
                std::cos(pitch_i) * std::cos(yaw_i));
            glm::mat4 V_i = glm::lookAt(eye_i, ic, glm::vec3(0, 1, 0));
            glm::mat4 P_i = glm::perspective(glm::radians(60.0f),
                (float)wi / (float)hi, ir * 0.001f, ir * 20.0f);
            glm::mat4 MVP_i = P_i * V_i;

            glUseProgram(int_prog);

            if (wf_i) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            dts_viewer::draw_interior(int_mesh, int_u_mvp, int_u_has_texture, int_u_tex0,
                int_u_lightmap, int_u_has_lightmap, MVP_i);
            if (wf_i) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            SDL_GL_SwapWindow(win_i);

            if (sc_warm_i >= 0 && !sc_done_i) {
                ++sc_warm_i;
                if (sc_warm_i >= 5) {
                    int sw, sh; SDL_GL_GetDrawableSize(win_i, &sw, &sh);
                    std::vector<std::uint8_t> rgb((std::size_t)sw * sh * 3);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadBuffer(GL_BACK);
                    glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
                    std::vector<std::uint8_t> flipped(rgb.size());
                    const std::size_t rb = (std::size_t)sw * 3;
                    for (int ry = 0; ry < sh; ++ry)
                        std::memcpy(&flipped[(sh-1-ry)*rb], &rgb[(std::size_t)ry*rb], rb);
                    std::ofstream fsc(screenshot_path, std::ios::binary);
                    if (fsc) {
                        fsc << "P6\n" << sw << " " << sh << "\n255\n";
                        fsc.write(reinterpret_cast<const char*>(flipped.data()), flipped.size());
                        std::fprintf(stderr, "screenshot: wrote %s\n", screenshot_path.c_str());
                    }
                    sc_done_i = true;
                    run_i = false;
                }
            }
        }

        SDL_GL_DeleteContext(ctx_i);
        SDL_DestroyWindow(win_i);
        SDL_Quit();
        return 0;
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

    // ---- spec 05 material-VOL resolution ----
    //
    // Build a MaterialResolver seeded with `argv[1]` + every sibling .vol in
    // the same directory. Most Tribes meshes resolve entirely from
    // Entities.vol, but some (UI atlases, per-world variants) need Shell.vol
    // or `<world>World.vol`, so a single-VOL scan would under-count.
    // Acceptance: log "<dts>: <N> materials, <M> resolved, <K> missing".
    //
    // Hoisted to function scope so spec 06 (GL upload) can reuse the same
    // resolver + sibling-VOL list for PPL discovery.
    dts_viewer::MaterialResolver resolver;
    resolver.add_vol(vol_path);
    std::vector<fs::path> sibling_vols;
    sibling_vols.push_back(vol_path);
    {
        const fs::path dir = vol_path.parent_path();
        if (!dir.empty() && fs::exists(dir)) {
            for (const auto& ent : fs::directory_iterator(dir)) {
                if (!ent.is_regular_file()) continue;
                auto p = ent.path();
                auto ext = p.extension().string();
                for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext != ".vol") continue;
                if (fs::equivalent(p, vol_path)) continue;
                resolver.add_vol(p);
                sibling_vols.push_back(p);
            }
        }

        std::string dts_label = dts_match.empty() ? std::string("dts") : dts_match;
        std::size_t n_mat = loaded.materials.size();
        std::size_t n_resolved = 0;
        std::size_t n_missing  = 0;
        for (const auto& m : loaded.materials) {
            // Empty material strings are valid "no material" sentinels — they
            // don't count toward resolved OR missing per the spec; skip them
            // and shrink the denominator so the counts add up to a meaningful
            // total.
            if (m.filename.empty()) { --n_mat; continue; }
            auto bytes = resolver.resolve(m.filename);
            if (bytes && !bytes->empty()) ++n_resolved;
            else                          ++n_missing;
        }
        std::fprintf(stderr, "%s: %zu materials, %zu resolved, %zu missing\n",
            dts_label.c_str(), n_mat, n_resolved, n_missing);
    }

    // ---- spec 06 (dml track) DML wrapper smoke test ----
    //
    // Tribes DTS files in the 1.41 corpus carry their material list as an
    // inline PERS trailer parsed by lib3space, so the primary path for
    // metadata is already exercised via `loaded.materials[i].metadata` above.
    // This block additionally calls the standalone `dml::get_dml_data()`
    // wrapper against the first sibling `.dml` (if any) found across the
    // loaded VOLs, so the discoverability wrapper added in commit 0e4724f is
    // exercised at least once per viewer launch. Pure validation; output is
    // logged but does not affect rendering.
    {
        bool probed = false;
        for (const auto& vp : sibling_vols) {
            if (probed) break;
            std::ifstream vin(vp, std::ios::binary);
            if (!vin) continue;
            dv::vol_file_archive plugin;
            auto listing = plugin.get_content_listing(vin, { vp, vp });
            for (const auto& entry : listing) {
                if (probed) break;
                auto* f = std::get_if<sr::file_info>(&entry);
                if (!f) continue;
                std::string name = f->filename.string();
                std::string lower = name;
                for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower.size() < 4 || lower.substr(lower.size()-4) != ".dml") continue;

                std::stringstream buf;
                vin.clear(); vin.seekg(0);
                plugin.extract_file_contents(vin, *f, buf);
                auto data = buf.str();
                std::stringstream ss(data);
                if (!sc::dml::is_dml(ss)) continue;
                auto opt = sc::dml::get_dml_data(ss);
                if (!opt) continue;
                std::size_t rec_count = std::visit([](const auto& ml) {
                    return ml.materials.size();
                }, *opt);
                std::fprintf(stderr,
                    "dml: probed %s/%s via dml::get_dml_data() -> %zu records\n",
                    vp.filename().string().c_str(), name.c_str(), rec_count);
                probed = true;
            }
        }
        if (!probed) {
            std::fprintf(stderr,
                "dml: no standalone .dml found across %zu sibling VOLs (DTS-inline path covers metadata)\n",
                sibling_vols.size());
        }
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
            // Static-but-non-empty sequences exist (e.g. chainturret's
            // "visibility" — duration > 0, tracks present, but all keyframes
            // hold the same transform). Demoted from assert to warning so
            // other meshes' downstream init isn't blocked.
            if (max_motion == 0.0f) {
                std::fprintf(stderr,
                    "spec06 selftest: WARNING — chosen probe sequence '%s' "
                    "produced no vertex motion (try a different sequence to "
                    "exercise skinning)\n", sq.name.c_str());
            }
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
#if !defined(__APPLE__)
    glewExperimental = GL_TRUE;
    glewInit();
#endif

    std::printf("GL_VERSION: %s\n", glGetString(GL_VERSION));

    // Spec 25/01 — wire ImGui for the shape-viewer mode too.
    dts_viewer::imgui_init(win, ctx);

    // Spec 25/03 — viewer-state init for the shape-viewer mode.
    dts_viewer::load_config();
    dts_viewer::set_shape_catalogue(list_dts_in_vol(vol_path));

    // Spec 25/05 — index every adjacent VOL for the Asset Browser.
    {
        std::vector<dts_viewer::MountedVol> mounts;
        for (const auto& v : sibling_vols) {
            dts_viewer::MountedVol m;
            m.path = v;
            m.short_name = v.filename().string();
            for (auto& c : m.short_name)
                c = (char)std::tolower((unsigned char)c);
            mounts.push_back(std::move(m));
        }
        dts_viewer::asset_browser_init(mounts);
    }

    // Smoke-test hook (env DTS_TEST_HELP=1) — invoke the docs URL
    // resolver and print what would launch. Skips the actual browser.
    if (const char* th = std::getenv("DTS_TEST_HELP"); th && *th == '1') {
        std::printf("[help-test] index -> %s\n",
            dts_viewer::docs_url_for("index.html").c_str());
        std::printf("[help-test] controls -> %s\n",
            dts_viewer::docs_url_for("controls.html").c_str());
    }

    // Smoke-test hook (env DTS_TEST_INSPECT=<entry>) — preselect an
    // asset and open the Inspector at startup. Used by spec-acceptance
    // screenshots only.
    if (const char* iv = std::getenv("DTS_TEST_INSPECT"); iv && *iv) {
        dts_viewer::Selection sel;
        sel.kind = dts_viewer::SelectionKind::AssetEntry;
        sel.vol_path = vol_path;
        sel.entry_name = iv;
        dts_viewer::inspector_set_selection(sel);
        dts_viewer::inspector_visible_ref() = true;
    }

    // ---- spec 06: texture cache + upload ---------------------------------
    //
    // Two-tier cache (per the spec's Outputs section):
    //
    //   texture_cache[normalized_name]  -> GLuint   (one per unique PBMP file)
    //   bucket_to_texture[bucket_idx]   -> GLuint   (per-bucket lookup; many
    //                                                 buckets may share a tex)
    //
    // The normalized name is the same key the MaterialResolver would lookup
    // against (strip `base.`, lowercase) — so two materials whose filenames
    // differ only in case or `base.` prefix de-dup correctly.
    //
    // Palette discovery: we load Shell.ppl (Tribes armor palette 1136 lives
    // here) and the first sibling `<world>.day.ppl` (structures use per-world
    // palettes like 1135). by_index() returns a non-owning map<uint, const
    // Palette*>, so we keep the Palette vectors alive in `loaded_palettes`
    // and rebuild the unified `palette_map` once after all PPLs are loaded.
    //
    // Per the spec: GL_LINEAR_MIPMAP_LINEAR / GL_LINEAR / GL_REPEAT. Mips via
    // glGenerateMipmap (PBMP DETL chain ignored for v1). OpenGL 4.1 core has
    // no glTextureStorage2D, so we use glTexImage2D + glTexParameteri.
    std::map<std::string, GLuint> texture_cache;
    std::map<int, GLuint>         bucket_to_texture;
    {
        // Local copy of the resolver's normalize() rules — `base.` strip +
        // lowercase. Kept inline so the GL call site is self-contained.
        auto normalize_mat_name = [](const std::string& in) -> std::string {
            std::string s = in;
            if (s.size() >= 5 && s.compare(0, 5, "base.") == 0) s.erase(0, 5);
            else if (s.size() >= 5
                && (s[0]=='B'||s[0]=='b') && (s[1]=='A'||s[1]=='a')
                && (s[2]=='S'||s[2]=='s') && (s[3]=='E'||s[3]=='e')
                && s[4]=='.') {
                s.erase(0, 5);
            }
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };

        // Load Shell.ppl first — covers armor / shell-UI palettes (1136, 4294,
        // 5150). Then pick the first sibling `<world>.day.ppl` to cover
        // structure / vehicle palettes (1135 etc). Missing PPLs are not fatal
        // — expand_to_rgba8() falls back to magenta checker so misses are
        // visible in-viewer.
        std::vector<Palette> loaded_palettes; // owns Palette storage
        auto try_load_ppl = [&](const std::string& match_lower,
                                const std::string& ext) {
            std::vector<char> bytes;
            std::string filename;
            for (const auto& v : sibling_vols) {
                if (read_named_file_from_vol(v, match_lower, ext, bytes, filename)) {
                    std::stringstream ss(std::string(bytes.begin(), bytes.end()));
                    auto pals = load_ppl(ss);
                    std::fprintf(stderr,
                        "spec06: loaded %zu palettes from %s (%s)\n",
                        pals.size(), filename.c_str(), v.filename().string().c_str());
                    for (auto& p : pals) loaded_palettes.push_back(std::move(p));
                    return true;
                }
            }
            return false;
        };
        try_load_ppl("shell.ppl", ".ppl");
        // Pick whichever world PPL we find first — pl0/pl1 difference is
        // negligible for the geometry colours we care about at this stage.
        const char* world_ppl_seeds[] = {
            "lush.day.ppl", "lush.dawn.ppl", "lush.dusk.ppl", "lush.night.ppl",
            "ice.day.ppl",  "desert.day.ppl", "mars.day.ppl",  "mud.day.ppl",
            "alien.day.ppl"
        };
        bool world_loaded = false;
        for (const char* seed : world_ppl_seeds) {
            if (try_load_ppl(seed, ".ppl")) { world_loaded = true; break; }
        }
        if (!world_loaded) {
            std::fprintf(stderr,
                "spec06: no world .day.ppl found in sibling VOLs — structure "
                "materials may render as magenta\n");
        }
        auto palette_map = by_index(loaded_palettes);

        // Walk each bucket; resolve its material_index -> filename -> bytes ->
        // RGBA -> GL texture. De-dup on the *normalized* filename so the two
        // material slots that both point at `base.larmor.BMP` share one texture.
        for (const auto& [bucket_idx, bucket] : geom.buckets) {
            if (bucket.material_index < 0
                || bucket.material_index >= (int)loaded.materials.size()) {
                continue; // unattached bucket — no texture, flat shading
            }
            const std::string& raw_name =
                loaded.materials[bucket.material_index].filename;
            if (raw_name.empty()) continue; // valid "no texture" sentinel

            const std::string key = normalize_mat_name(raw_name);
            if (key.empty()) continue;

            // Already uploaded? Just point the bucket at the existing GLuint.
            auto cached = texture_cache.find(key);
            if (cached != texture_cache.end()) {
                bucket_to_texture[bucket_idx] = cached->second;
                continue;
            }

            auto bytes = resolver.resolve(raw_name);
            if (!bytes || bytes->empty()) continue; // resolver already warned

            PbmpImage img;
            try {
                std::stringstream ss(std::string(bytes->begin(), bytes->end()));
                img = load_pbmp(ss);
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                    "spec06: PBMP parse failed for '%s': %s\n",
                    raw_name.c_str(), e.what());
                continue;
            }
            if (img.width == 0 || img.height == 0) continue;

            auto rgba = expand_to_rgba8(img, palette_map);

            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            // Default unpack alignment is 4 — fine for RGBA8 width-aligned
            // buffers, but set explicitly for safety.
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                         (GLsizei)img.width, (GLsizei)img.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D, 0);

            texture_cache[key] = tex;
            bucket_to_texture[bucket_idx] = tex;
        }

        GLenum err = glGetError();
        std::fprintf(stderr, "uploaded %zu textures (glGetError=0x%04X)\n",
                     texture_cache.size(), err);
        std::fprintf(stderr, "bucket_to_texture covers %zu / %zu buckets\n",
                     bucket_to_texture.size(), geom.buckets.size());
    }

    // ---- spec 06/08 CPU skinning prep ----------------------------------
    // `bucket.positions` was emitted by the renderer in WORLD bind-pose
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

    // Spec 08: per-bucket GL state. Each bucket gets its own VAO + 3 VBOs
    // (positions stream, normals static, UVs static) + a precomputed bind-local
    // positions buffer + a per-frame skinned destination buffer. Positions are
    // STREAM_DRAW because we re-upload every frame; normals + UVs stay STATIC
    // (animated normals are out of scope for this spec; lighting is locked to
    // bind pose, which is visually acceptable for rigid 1-bone Tribes meshes).
    struct BucketGL
    {
        GLuint vao = 0;
        GLuint vbo_pos = 0;
        GLuint vbo_nor = 0;
        GLuint vbo_uv  = 0;
        GLsizei vertex_count = 0;          // = bucket.positions.size() / 3
        std::vector<float> bind_local_pos; // same size as bucket.positions
        std::vector<float> skinned_pos;    // per-frame target buffer
        const std::vector<int>* node_index = nullptr; // borrow from bucket
        GLuint texture = 0;                // 0 = no texture, use flat shading
        int    bucket_id = -1;             // for HUD / logging
        std::string object_name;
        int    material_index = -1;
        DmlInfo dml = {};                  // spec 06: DML metadata for this bucket's material
    };
    std::vector<BucketGL> bucket_gl;
    bucket_gl.reserve(geom.buckets.size());

    for (auto& [bucket_idx, bucket] : geom.buckets) {
        if (bucket.positions.empty()) continue;
        BucketGL bg;
        bg.bucket_id = bucket_idx;
        bg.object_name = bucket.object_name;
        bg.material_index = bucket.material_index;
        bg.vertex_count = (GLsizei)(bucket.positions.size() / 3);
        bg.node_index = &bucket.vertex_node_index;

        // spec 06 (dml track): plumb the per-material DML metadata onto the
        // bucket. Multiple buckets may share a single material_index (and via
        // texture_cache the same GLuint), so the DmlInfo on different
        // buckets may carry identical values — that's intentional, the
        // bookkeeping is per-draw because future code may want to set GL
        // state (blend / alpha-test) per draw, not per texture.
        if (bg.material_index >= 0
            && bg.material_index < (int)loaded.materials.size()) {
            bg.dml = extract_dml_info(loaded.materials[bg.material_index]);
        }

        // Precompute bind-local positions for this bucket.
        bg.bind_local_pos.resize(bucket.positions.size());
        const std::size_t vcount = bucket.positions.size() / 3;
        for (std::size_t v = 0; v < vcount; ++v) {
            glm::vec4 p(bucket.positions[v*3+0],
                        bucket.positions[v*3+1],
                        bucket.positions[v*3+2], 1.0f);
            int ni = (v < bucket.vertex_node_index.size())
                ? bucket.vertex_node_index[v] : -1;
            if (ni >= 0 && ni < (int)inv_world_bind.size()) {
                p = inv_world_bind[ni] * p;
            }
            bg.bind_local_pos[v*3+0] = p.x;
            bg.bind_local_pos[v*3+1] = p.y;
            bg.bind_local_pos[v*3+2] = p.z;
        }
        bg.skinned_pos.assign(bucket.positions.size(), 0.0f);

        // GL upload.
        glGenVertexArrays(1, &bg.vao);
        glBindVertexArray(bg.vao);

        glGenBuffers(1, &bg.vbo_pos);
        glBindBuffer(GL_ARRAY_BUFFER, bg.vbo_pos);
        glBufferData(GL_ARRAY_BUFFER,
                     bucket.positions.size() * sizeof(float),
                     bucket.positions.data(), GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        glGenBuffers(1, &bg.vbo_nor);
        glBindBuffer(GL_ARRAY_BUFFER, bg.vbo_nor);
        glBufferData(GL_ARRAY_BUFFER,
                     bucket.normals.size() * sizeof(float),
                     bucket.normals.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

        glGenBuffers(1, &bg.vbo_uv);
        glBindBuffer(GL_ARRAY_BUFFER, bg.vbo_uv);
        // Even buckets without UVs get a buffer of zeros so location 2 is
        // always valid — simpler than toggling enableVertexAttribArray
        // per-bucket. (Fallback path zeroes u_has_texture so the UVs aren't
        // sampled anyway.)
        std::vector<float> uv_data = bucket.uvs;
        if (uv_data.size() != bucket.positions.size() / 3 * 2) {
            uv_data.assign(bucket.positions.size() / 3 * 2, 0.0f);
        }
        glBufferData(GL_ARRAY_BUFFER,
                     uv_data.size() * sizeof(float),
                     uv_data.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

        glBindVertexArray(0);

        // Texture lookup. Missing texture -> 0 -> flat fallback.
        auto it = bucket_to_texture.find(bucket_idx);
        if (it != bucket_to_texture.end()) bg.texture = it->second;

        bucket_gl.push_back(bg);
    }
    std::fprintf(stderr, "render: %zu draws/frame\n", bucket_gl.size());

    // Log per-bucket material binding + DML metadata (spec 06, dml track).
    // One line per bucket: bucket id, object name, material slot, raw
    // filename, texture-bound flag, then DML fields (flags / type /
    // elasticity / use_default_properties). Empty slots are flagged so it's
    // obvious why they fall through to flat shading.
    std::size_t dml_empty_slot_count = 0;
    for (const auto& bg : bucket_gl) {
        const char* matname = "<none>";
        if (bg.material_index >= 0
            && bg.material_index < (int)loaded.materials.size()) {
            matname = loaded.materials[bg.material_index].filename.c_str();
            if (*matname == '\0') matname = "<empty>";
        }
        if (bg.dml.present && bg.dml.empty_slot) ++dml_empty_slot_count;
        if (bg.dml.present) {
            std::fprintf(stderr,
                "  draw[%d] obj='%s' mat[%d]='%s' tex=%s "
                "dml{flags=0x%04x type=%d elast=%.2f udp=%u%s}\n",
                bg.bucket_id, bg.object_name.c_str(), bg.material_index,
                matname, bg.texture ? "yes" : "no(flat)",
                bg.dml.flags, bg.dml.type, bg.dml.elasticity,
                bg.dml.use_default_properties,
                bg.dml.empty_slot ? " EMPTY-SLOT" : "");
        } else {
            std::fprintf(stderr,
                "  draw[%d] obj='%s' mat[%d]='%s' tex=%s dml{absent}\n",
                bg.bucket_id, bg.object_name.c_str(), bg.material_index,
                matname, bg.texture ? "yes" : "no(flat)");
        }
    }
    std::fprintf(stderr,
        "dml: %zu / %zu buckets carry metadata, %zu empty-slot (skip-render via name check)\n",
        std::count_if(bucket_gl.begin(), bucket_gl.end(),
                      [](const BucketGL& b) { return b.dml.present; }),
        bucket_gl.size(), dml_empty_slot_count);

    GLuint prog = link_program(
        compile_shader(GL_VERTEX_SHADER,   VS_SRC),
        compile_shader(GL_FRAGMENT_SHADER, FS_SRC));
    GLint u_mvp         = glGetUniformLocation(prog, "u_mvp");
    GLint u_normal_mat  = glGetUniformLocation(prog, "u_normal_mat");
    GLint u_tex0        = glGetUniformLocation(prog, "u_tex0");
    GLint u_has_texture = glGetUniformLocation(prog, "u_has_texture");

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

    std::printf("keys: WASD=fly  mouse=look  Shift=sprint  Space/Ctrl=up/dn  Esc=release cursor\n"
                "      Space=play/pause  Left/Right=scrub  R=reset  Tab=next seq  L=loop mode  B=bones  Q=quit\n");
    bool show_bones = false;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    // Tribes models use left-handed coords; we don't cull faces to avoid winding surprises.
    glDisable(GL_CULL_FACE);

    // Camera setup centered on bbox.
    //
    // spec 09: when --grid N is set, instances are tiled around the mesh
    // center on the camera's ground plane (world XZ — the lookAt up axis
    // is +Y). Spacing is `max(extent.x, extent.z) * 1.3` so each instance
    // gets clear room without big gaps; for larmor (X:0.9, Z:2.2) that's
    // ~2.86 units. The camera distance is scaled with grid size so all
    // instances fit in the default framing.
    glm::vec3 center = 0.5f * (geom.bbox_min + geom.bbox_max);
    glm::vec3 extent = geom.bbox_max - geom.bbox_min;
    float radius = 0.5f * glm::length(extent);
    if (radius < 0.001f) radius = 1.0f;
    const float grid_spacing = std::max(extent.x, extent.z) * 1.3f;
    const float grid_half_span = grid_spacing * (grid_n - 1) * 0.5f;

    float yaw = 0.6f, pitch = 0.35f;
    // Zoom out enough to frame the whole grid. The +sqrt(2)*half_span term
    // accounts for the worst-case diagonal from grid center to a corner
    // instance, and we multiply the whole thing by 2.5 (instead of the
    // single-instance 3.0) so the perspective FOV of 45 degrees actually
    // captures the corners. For grid_n == 1 this collapses to radius * 2.5
    // — slightly tighter framing than before but the prior 3.0x was
    // chosen empirically for the smallest meshes; tested fine for larmor.
    float grid_radius = radius
        + grid_half_span * std::sqrt(2.0f);

    // Free-fly camera (spec 08/01). Start behind and above the mesh.
    dts_viewer::Camera dts_cam;
    dts_cam.position   = center + glm::vec3(0, radius * 0.5f, grid_radius * 2.5f);
    dts_cam.yaw        = glm::pi<float>();  // face -Z toward origin
    dts_cam.pitch      = -0.2f;
    dts_cam.move_speed = std::max(1.0f, radius * 0.05f);
    dts_cam.near_plane = radius * 0.05f;
    dts_cam.far_plane  = radius * 200.0f;
    dts_viewer::set_mouse_capture(dts_cam, true);

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

    // spec 09: for the --grid screenshot capture we want a real running
    // animation in the frame, not the bind-pose `root` no-op. Auto-pick the
    // first sequence named "run" (Tribes convention), else the first
    // sequence with non-zero duration and non-empty tracks. The user can
    // still tab away after capture.
    if (grid_n > 1 || !screenshot_path.empty()) {
        for (std::size_t i = 0; i < loaded.sequences.size(); ++i) {
            const auto& s = loaded.sequences[i];
            if (s.duration_seconds > 0.0f && !s.tracks.empty()
                && s.name == "run") {
                current_sequence = (int)i;
                break;
            }
        }
        if (current_sequence == 0) {
            for (std::size_t i = 0; i < loaded.sequences.size(); ++i) {
                const auto& s = loaded.sequences[i];
                if (s.duration_seconds > 0.0f && !s.tracks.empty()) {
                    current_sequence = (int)i;
                    break;
                }
            }
        }
        playing = true;
    }

    // ---- spec 08 per-sequence loop modes ----
    //
    // Three behaviours when the play head reaches `duration`:
    //   Loop     — wrap to 0 (current behaviour; right for run/walk/idle)
    //   Hold     — clamp at duration, pause implicitly (right for death/jet)
    //   PingPong — reverse direction at endpoints
    //
    // The DTS file encodes `cyclic` per raw sequence (spec 03 / spec 01
    // findings), surfaced on our `Sequence` struct. Treat `cyclic == true`
    // as the Loop default and `cyclic == false` as Hold; the user can
    // override either with `L` to cycle Loop -> Hold -> PingPong -> Loop.
    // Mode state is stored per-sequence so toggling on one doesn't reset
    // another; size matches loaded.sequences.size().
    //
    // PingPong needs a per-sequence direction sign (+1 forward, -1 back),
    // independent of `playing`. Reset to +1 whenever the sequence changes
    // or `R` is pressed so the next playback starts moving forward.
    enum class LoopMode : int { Loop = 0, Hold = 1, PingPong = 2 };
    std::vector<LoopMode> seq_mode(loaded.sequences.size(), LoopMode::Hold);
    for (std::size_t i = 0; i < loaded.sequences.size(); ++i) {
        seq_mode[i] = loaded.sequences[i].cyclic ? LoopMode::Loop : LoopMode::Hold;
    }
    int playback_dir = 1; // for PingPong; +1 forward, -1 reverse
    auto mode_name = [](LoopMode m) {
        switch (m) {
            case LoopMode::Loop:     return "loop";
            case LoopMode::Hold:     return "hold";
            case LoopMode::PingPong: return "pingpong";
        }
        return "?";
    };
    auto current_mode = [&]() -> LoopMode {
        if (current_sequence < 0
            || current_sequence >= (int)seq_mode.size()) return LoopMode::Loop;
        return seq_mode[current_sequence];
    };

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
        std::fprintf(stderr, "seq[%d]: %s  t: %.3fs / %.3fs  mode=%s  %s\n",
            current_sequence, seq_name(current_sequence),
            current_time, seq_duration(current_sequence),
            mode_name(current_mode()),
            playing ? "[play]" : "[pause]");
    };
    print_hud();

    // ---- spec 06 FPS counter -------------------------------------------
    // Counts frames per real second so we can verify the >60fps acceptance
    // criterion. Printed to stderr once per second.
    Uint64 fps_window_start = SDL_GetPerformanceCounter();
    int    fps_window_frames = 0;

    // spec 09: --screenshot path.ppm captures one frame to PPM (P6, no
    // alpha) after waiting for the animation to advance ~1.5s so the grid
    // has a chance to desynchronize visually. After capture, exits.
    // Negative = no screenshot pending; counts frames once we start playing.
    int  screenshot_warmup_frames = screenshot_path.empty() ? -1 : 0;
    bool screenshot_done = false;

    bool running = true;
    std::string self_arg0_shape = argv[0];

    // Spec 25/04 — shape mode has no wireframe/HUD/bbox/cam toggles
    // to wire (those are mission-mode concepts). Just bind Quit + the
    // asset browser; the rest stay disabled with unset callbacks.
    {
        dts_viewer::MenuActions a;
        a.quit = [&]{ running = false; };
        a.is_asset_browser     = []{ return dts_viewer::asset_browser_visible_ref(); };
        a.toggle_asset_browser = []{
            bool& v = dts_viewer::asset_browser_visible_ref();
            v = !v;
        };
        a.is_inspector     = []{ return dts_viewer::inspector_visible_ref(); };
        a.toggle_inspector = []{
            bool& v = dts_viewer::inspector_visible_ref();
            v = !v;
        };
        a.help_user_guide   = []{ dts_viewer::open_url(dts_viewer::docs_url_for("index.html")); };
        a.help_controls     = []{ dts_viewer::open_url(dts_viewer::docs_url_for("controls.html")); };
        a.help_scripting    = []{ dts_viewer::open_url(dts_viewer::docs_url_for("scripting.html")); };
        a.help_online_docs  = []{ dts_viewer::open_url(dts_viewer::kOnlineDocsUrl); };
        a.help_report_issue = []{ dts_viewer::open_url(dts_viewer::kReportIssueUrl); };
        a.help_about        = []{ dts_viewer::about_modal_open(); };
        dts_viewer::set_menu_actions(a);
    }

    while (running) {
        // Spec 25/03 — drain deferred-load. For the shape viewer mode
        // we re-exec ourselves with `<vol> <shape>`.
        if (auto pl = dts_viewer::take_pending_load();
            pl.kind == dts_viewer::LoadKind::Shape) {
            std::printf("shape-switch: -> %s (menu)\n", pl.name.c_str());
            std::fflush(stdout);
            dts_viewer::imgui_shutdown();
            SDL_GL_DeleteContext(ctx);
            SDL_DestroyWindow(win);
            SDL_Quit();
            std::string vol_str   = vol_path.string();
            std::string match_str = pl.name;
            char* args[] = {
                const_cast<char*>(self_arg0_shape.c_str()),
                const_cast<char*>(vol_str.c_str()),
                const_cast<char*>(match_str.c_str()),
                nullptr
            };
            execv(self_arg0_shape.c_str(), args);
            std::fprintf(stderr, "shape-switch: execv failed\n");
            std::exit(1);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            bool imgui_consumed = dts_viewer::imgui_process_event(ev);
            if (imgui_consumed && ev.type != SDL_QUIT) continue;
            if (dts_viewer::imgui_try_menu_shortcut(ev)) continue;
            switch (ev.type) {
                case SDL_QUIT: running = false; break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_q) running = false;
                    if (ev.key.keysym.sym == SDLK_ESCAPE)
                        dts_viewer::toggle_mouse_capture(dts_cam);
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
                        playback_dir = 1;
                        print_hud();
                    }
                    if (ev.key.keysym.sym == SDLK_TAB) {
                        if (!loaded.sequences.empty()) {
                            int step = (ev.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                            int n = (int)loaded.sequences.size();
                            current_sequence = (current_sequence + step + n) % n;
                            current_time = 0.0f;
                            playback_dir = 1;
                            print_hud();
                        }
                    }
                    // spec 08: `L` cycles the current sequence's loop mode
                    // (Loop -> Hold -> PingPong -> Loop). State is per
                    // sequence so toggling on one sequence doesn't reset
                    // others. Doesn't reset time/dir — switching mode
                    // mid-playback should let the user see the effect
                    // immediately (e.g. a paused-at-end Hold animation
                    // resumes forward when switched to Loop).
                    if (ev.key.keysym.sym == SDLK_l) {
                        if (current_sequence >= 0
                            && current_sequence < (int)seq_mode.size()) {
                            LoopMode& m = seq_mode[current_sequence];
                            m = static_cast<LoopMode>(
                                (static_cast<int>(m) + 1) % 3);
                            // After a mode change, a backward PingPong
                            // direction makes no sense if the user just
                            // entered PingPong mode; reset to forward so
                            // the bounce starts from wherever t currently is.
                            playback_dir = 1;
                            print_hud();
                        }
                    }
                    // spec 07: number keys 1-9 pick sequences[0..8] by index.
                    // Index-based (not name-based) is mandatory: LOD fan-out
                    // produces duplicate sequence names (e.g. larmor has `wave`
                    // at index 39 AND 42), and a name lookup would make the
                    // higher-LOD copy unreachable. SDL exposes the row keys as
                    // SDLK_1..SDLK_9 (contiguous), so a direct subtraction
                    // gives the target index; out-of-range presses are
                    // silently ignored so meshes with <9 sequences don't
                    // crash or wrap surprisingly.
                    if (ev.key.keysym.sym >= SDLK_1 && ev.key.keysym.sym <= SDLK_9) {
                        int idx = ev.key.keysym.sym - SDLK_1;
                        if (idx < (int)loaded.sequences.size()) {
                            current_sequence = idx;
                            current_time = 0.0f;
                            playback_dir = 1;
                            print_hud();
                        }
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (!dts_cam.mouse_captured)
                        dts_viewer::set_mouse_capture(dts_cam, true);
                    break;
                case SDL_MOUSEMOTION:
                    if (dts_cam.mouse_captured)
                        dts_viewer::handle_mouse_motion(dts_cam, ev.motion.xrel, ev.motion.yrel);
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
                // spec 08: per-mode endpoint behaviour. PingPong needs a
                // signed dt; Loop/Hold always advance forward.
                LoopMode m = current_mode();
                if (m == LoopMode::PingPong) {
                    current_time += dt * (float)playback_dir;
                    // Bounce — if we'd cross either endpoint this frame,
                    // reflect the overshoot. Loop here so a very large dt
                    // bouncing multiple times still settles in-range
                    // (paranoid edge case but cheap).
                    int guard = 0;
                    while (guard++ < 8
                           && (current_time > dur || current_time < 0.0f)) {
                        if (current_time > dur) {
                            current_time = dur - (current_time - dur);
                            playback_dir = -1;
                        } else if (current_time < 0.0f) {
                            current_time = -current_time;
                            playback_dir = +1;
                        }
                    }
                    if (current_time < 0.0f) current_time = 0.0f;
                    if (current_time > dur) current_time = dur;
                } else {
                    current_time += dt;
                    if (m == LoopMode::Loop) {
                        // wrap; fmod-equivalent loop handles long-pause
                        // catchups without leaving a residual offset.
                        while (current_time >= dur) current_time -= dur;
                    } else { // Hold
                        // Clamp at the end; freeze on the last frame. We
                        // intentionally do NOT clear `playing`, so toggling
                        // mode back to Loop / PingPong resumes immediately
                        // without requiring Space.
                        if (current_time > dur) current_time = dur;
                    }
                }
            }
            // Once per second of playback, drop a HUD line so the user can
            // sanity-check elapsed time without scrubbing.
            if (last_hud_print_t < 0.0f
                || std::floor(current_time) != std::floor(last_hud_print_t)) {
                print_hud();
            }
            last_hud_print_t = current_time;
        }

        dts_viewer::update_camera_free(dts_cam, dt);

        int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.12f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 V   = dts_viewer::camera_view(dts_cam);
        glm::mat4 P   = dts_viewer::camera_projection(dts_cam, (float)w / (float)h);
        glm::mat4 MVP = P * V;
        glm::mat3 N   = glm::mat3(glm::transpose(glm::inverse(V)));

        // ---- spec 06/08/09: CPU skinning + per-bucket draws, NxN grid ----
        // 1) For each grid instance (i, j) compute a per-instance time
        //    offset and translation. The phase offset desynchronises
        //    motion across the grid; the translation lays the army out on
        //    a center-anchored grid in XZ.
        // 2) evaluate per-node local transforms at the instance's time. If
        //    the sequence has no tracks / zero duration (e.g. the `root`
        //    no-op marker), evaluate() falls back to bind_local — t=0
        //    reproduces the bind pose exactly, which is the sanity check.
        // 3) accumulate to world via the same parent-walk used at load.
        // 4) per-vertex bone matrix B[n] = world_anim[n] * inverse(world_bind[n]).
        //    We pre-baked the inverse and we *already* expressed each vertex
        //    in node-local space (bind_local_pos), so the skin step is
        //    just `world_anim[n] * bind_local_vertex` — saving the multiply
        //    by inverse(world_bind) per frame.
        // 5) re-upload each bucket's position VBO via glBufferData.
        //
        // For grid_n == 1 this collapses to a single instance at the
        // origin, identical pixel-for-pixel to the pre-spec-09 path.
        const Sequence* sq = (current_sequence >= 0
            && current_sequence < (int)loaded.sequences.size())
            ? &loaded.sequences[current_sequence] : nullptr;

        glUseProgram(prog);
        glUniformMatrix3fv(u_normal_mat, 1, GL_FALSE, glm::value_ptr(N));
        // Texture unit 0 is the sampler the fragment shader reads from; bind
        // every per-bucket texture to GL_TEXTURE0.
        if (u_tex0 >= 0) glUniform1i(u_tex0, 0);
        glActiveTexture(GL_TEXTURE0);

        for (int j = 0; j < grid_n; ++j) {
            for (int i = 0; i < grid_n; ++i) {
                // Per-instance time offset: deterministic, wraps inside
                // the sequence duration so all instances stay in-bounds.
                // 0.1 * linear index matches the spec wording exactly.
                float dur = sq ? sq->duration_seconds : 0.0f;
                float inst_t = current_time;
                if (dur > 0.0f) {
                    inst_t = current_time + (i + j * grid_n) * 0.1f;
                    inst_t = std::fmod(inst_t, dur);
                    if (inst_t < 0.0f) inst_t += dur;
                }
                std::vector<glm::mat4> local_anim;
                if (sq) {
                    local_anim = evaluate(*sq, inst_t, loaded.nodes);
                } else {
                    local_anim.resize(loaded.nodes.size());
                    for (std::size_t k = 0; k < loaded.nodes.size(); ++k)
                        local_anim[k] = loaded.nodes[k].bind_local;
                }
                std::vector<glm::mat4> world_anim_global =
                    compute_world_from_locals(loaded.nodes, local_anim);

                // Instance translation on the camera's ground plane
                // (world XZ; lookAt up axis is +Y). The center vec3 from
                // camera setup already bakes in the bind-pose Y offset;
                // we add the grid offset in world space directly into
                // the MVP.
                glm::vec3 offset(
                    (i - (grid_n - 1) * 0.5f) * grid_spacing,
                    0.0f,
                    (j - (grid_n - 1) * 0.5f) * grid_spacing);
                glm::mat4 model = glm::translate(glm::mat4(1.0f), offset);
                glm::mat4 instance_mvp = MVP * model;
                glUniformMatrix4fv(u_mvp, 1, GL_FALSE,
                                   glm::value_ptr(instance_mvp));

                for (auto& bg : bucket_gl) {
                    // spec 06 (dml track): DML-driven render state.
                    //
                    // Per the value-semantics survey in docs/done/03-dml/
                    // 00-value-semantics.md the shipping Tribes corpus
                    // exposes no field that can legitimately drive
                    // blending or alpha-cutout:
                    //   - `alpha` is 0.0f everywhere (cannot be a multiplier);
                    //   - `type` is a physics/sound surface category, not a
                    //     render-mode hint;
                    //   - `flags` distinguishes only "valid" (0x103) from
                    //     "empty slot" (0x0000 or 0xF000), with the latter
                    //     always coinciding with empty filename.
                    // So the only DML-driven decision here is the
                    // empty-slot skip, and even that is redundant because
                    // spec 08's texture pipeline already binds 0 (-> flat
                    // shading) when the filename is empty. We keep the
                    // check explicit so a future spec that *does* gain a
                    // DML-driven blend/discard path has the obvious hook.
                    if (bg.dml.present && bg.dml.empty_slot) {
                        // Treat as no-texture: nothing to bind, but we
                        // still draw geometry in flat shading to preserve
                        // mesh silhouette. (No-op vs the current path;
                        // documented for clarity.)
                    }

                    // Skin this bucket into bg.skinned_pos.
                    const auto& nidx = *bg.node_index;
                    const std::size_t vcount = bg.bind_local_pos.size() / 3;
                    for (std::size_t v = 0; v < vcount; ++v) {
                        int ni = (v < nidx.size()) ? nidx[v] : -1;
                        glm::vec4 lp(bg.bind_local_pos[v*3+0],
                                     bg.bind_local_pos[v*3+1],
                                     bg.bind_local_pos[v*3+2], 1.0f);
                        glm::vec4 wp;
                        if (ni >= 0 && ni < (int)world_anim_global.size()) {
                            wp = world_anim_global[ni] * lp;
                        } else {
                            wp = lp;
                        }
                        bg.skinned_pos[v*3+0] = wp.x;
                        bg.skinned_pos[v*3+1] = wp.y;
                        bg.skinned_pos[v*3+2] = wp.z;
                    }
                    glBindBuffer(GL_ARRAY_BUFFER, bg.vbo_pos);
                    glBufferData(GL_ARRAY_BUFFER,
                                 bg.skinned_pos.size() * sizeof(float),
                                 bg.skinned_pos.data(), GL_STREAM_DRAW);

                    // Per-bucket texture bind + flag.
                    if (bg.texture != 0) {
                        glBindTexture(GL_TEXTURE_2D, bg.texture);
                        if (u_has_texture >= 0) glUniform1i(u_has_texture, 1);
                    } else {
                        glBindTexture(GL_TEXTURE_2D, 0);
                        if (u_has_texture >= 0) glUniform1i(u_has_texture, 0);
                    }

                    glBindVertexArray(bg.vao);
                    glDrawArrays(GL_TRIANGLES, 0, bg.vertex_count);
                }
            }
        }
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);

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

        dts_viewer::imgui_begin_frame();
        dts_viewer::imgui_end_frame();

        SDL_GL_SwapWindow(win);

        // spec 09: one-shot screenshot capture. We need a few frames of
        // playback to elapse first so the grid is visibly desync'd —
        // capturing on frame 0 freezes all instances at t = 0.1 * idx
        // which still looks varied but a settled mid-anim frame is
        // clearer. Wait ~90 frames (~1.5s at 60fps) before grabbing.
        if (screenshot_warmup_frames >= 0 && !screenshot_done) {
            ++screenshot_warmup_frames;
            if (screenshot_warmup_frames >= 90) {
                int sw, sh; SDL_GL_GetDrawableSize(win, &sw, &sh);
                std::vector<std::uint8_t> rgb(sw * sh * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadBuffer(GL_BACK);
                glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE,
                             rgb.data());
                // glReadPixels returns bottom-up; PPM is top-down. Flip
                // rows in-place via a single scratch buffer (PPM viewers
                // would otherwise show the grid upside-down).
                std::vector<std::uint8_t> flipped(rgb.size());
                const std::size_t row_bytes = (std::size_t)sw * 3;
                for (int y = 0; y < sh; ++y) {
                    std::memcpy(&flipped[(sh - 1 - y) * row_bytes],
                                &rgb[y * row_bytes], row_bytes);
                }
                std::ofstream f(screenshot_path, std::ios::binary);
                if (f) {
                    f << "P6\n" << sw << " " << sh << "\n255\n";
                    f.write(reinterpret_cast<const char*>(flipped.data()),
                            flipped.size());
                    std::fprintf(stderr,
                        "screenshot: wrote %s (%dx%d)\n",
                        screenshot_path.c_str(), sw, sh);
                } else {
                    std::fprintf(stderr,
                        "screenshot: failed to open %s\n",
                        screenshot_path.c_str());
                }
                screenshot_done = true;
                running = false;
            }
        }

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

    dts_viewer::imgui_shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
