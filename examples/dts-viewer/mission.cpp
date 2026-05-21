// Open Siege spec 17/02 — MissionContext implementation.
//
// The lifecycle is intentionally small:
//
//   load(ctx, missions_dir, base_dir, name)
//     1. mission_loader::load_mission() parses .mis -> scene_graph.
//     2. Walk the scene_graph and emit a paren-form `instant SimGroup
//        (MissionGroup) { ... };` containing one `instant <Class>(<id>)`
//        per recognised payload type.
//     3. For each ident in trailer.exec_idents, run
//        `ScriptResolver::runScriptFile(ident)`. Missing files log a
//        warning and append to ctx.unbound_exec_idents.
//
//   tick(ctx, dt)
//     Sim::advanceTime(dt_ms) — drains the script-side schedule() queue.
//
//   unload(ctx)
//     Con::evaluate("MissionGroup.delete();") to cascade-destroy the
//     tree, then Sim::cancelPendingEvents on the root group to drop
//     any events whose target object survived the cascade (shouldn't
//     happen but defends against schedule(0, ...) globals).
//
// The classes referenced in emit_member() are all SimObject subclasses
// defined in entity_bindings.{hpp,cpp} or player_simobject.{hpp,cpp}.
// They get IMPLEMENT_CONOBJECT-anchored via cscript_host.cpp.

#include "mission.hpp"

#include "mission_loader.hpp"

#include "script_resolver.h"

#include "console/console.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/simSet.h"
#include "console/simObject.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <variant>

namespace fs = std::filesystem;
using namespace studio::content::mission;

namespace dts_viewer
{

// ---------------------------------------------------------------------------
// Search-path setup
// ---------------------------------------------------------------------------

void mission_add_script_search_path(const fs::path& dir)
{
    studio::content::cscript::ScriptResolver::addSearchPath(dir.string());
}

// ---------------------------------------------------------------------------
// Member emission
// ---------------------------------------------------------------------------
//
// Unique-name generator. Mission scenes contain multiple objects with the
// same instance_name (e.g. several "catwalkA1"); the cscript dialect
// requires unique names, so we suffix a serial when we detect a clash.

namespace {

struct EmitState
{
    std::set<std::string> taken_names;
    int generated = 0;
    std::vector<std::string>* unbound = nullptr; // ctx.unbound for typed warnings
};

std::string make_unique(EmitState& st, const std::string& want)
{
    if (want.empty() || !st.taken_names.count(want))
    {
        std::string n = want.empty()
            ? std::string("MissionObj_") + std::to_string(st.generated++)
            : want;
        // sanitise: cscript identifiers can't contain dots / spaces / quotes.
        for (char& c : n) {
            if (!(std::isalnum((unsigned char)c) || c == '_')) c = '_';
        }
        if (n.empty() || std::isdigit((unsigned char)n[0])) {
            n = "MissionObj_" + n;
        }
        if (!st.taken_names.count(n)) {
            st.taken_names.insert(n);
            return n;
        }
        // collision after sanitisation — fall through to numeric suffix.
    }
    int n_suffix = 1;
    std::string sanitised = want;
    for (char& c : sanitised) {
        if (!(std::isalnum((unsigned char)c) || c == '_')) c = '_';
    }
    if (sanitised.empty()) sanitised = "MissionObj";
    while (true) {
        std::string candidate = sanitised + "_" + std::to_string(n_suffix++);
        if (!st.taken_names.count(candidate)) {
            st.taken_names.insert(candidate);
            return candidate;
        }
    }
}

// Map scene_node class_name -> the SimObject subclass name we have bound.
// Returns empty string for class_names we don't recognise (a `[unbound]`
// warning is logged and the node is skipped).
//
// Note: T1 SimGroup-of-SimGroup nesting is flattened — we don't preserve
// the hierarchy. Acceptance criteria only require "SimGroup count returns
// to zero", which the cascade-delete of the top-level MissionGroup
// satisfies.
const char* class_binding(const std::string& mis_class)
{
    if (mis_class == "StaticShape")     return "StaticShape";
    if (mis_class == "Item")            return "Item";
    if (mis_class == "InteriorShape")   return nullptr; // not a SimObject in 16/06
    if (mis_class == "Turret")          return "Turret";
    if (mis_class == "Sensor")          return "Sensor";
    if (mis_class == "Marker")          return "MissionMarker";
    if (mis_class == "MissionMarker")   return "MissionMarker";
    if (mis_class == "Moveable")        return "Moveable";
    if (mis_class == "Trigger")         return "Trigger";
    if (mis_class == "SimLight")        return "SimLight";
    if (mis_class == "Planet")          return "Planet";
    if (mis_class == "Sky")             return "Sky";
    if (mis_class == "Generator")       return "Generator";
    if (mis_class == "Door")            return "Door";
    if (mis_class == "VehiclePlaceholder") return "VehiclePlaceholder";
    return nullptr;
}

void emit_member_node(EmitState& st,
                      std::ostringstream& body,
                      const scene_node& node,
                      MissionContext& ctx)
{
    // SimGroup container nodes (monostate payload) are flattened — we
    // recurse into children directly. The top-level caller is the one
    // that opens the outer SimGroup, so the spec 16/02 acceptance
    // "MissionGroup destroyed -> SimGroup count returns to 0" still
    // holds via cascade-delete because every member is registered under
    // MissionGroup.
    if (std::holds_alternative<std::monostate>(node.payload))
    {
        for (const auto& c : node.children) emit_member_node(st, body, c, ctx);
        return;
    }

    const char* bind = class_binding(node.class_name);
    if (!bind)
    {
        // Skip — class isn't bound. Track diagnostic.
        ctx.unbound_exec_idents.push_back(
            std::string("[unbound-class] ") + node.class_name);
        for (const auto& c : node.children) emit_member_node(st, body, c, ctx);
        return;
    }

    // Mission files use instance names like "Sky" / "Item" that collide
    // with the engine-side class registration ("Cannot name object [X]
    // the same name as a script class"). Prefix with an underscore so
    // the instance name is distinct from any registered class name.
    std::string want = node.instance_name.value_or(std::string(bind));
    if (want == bind) want = std::string("_") + want;
    std::string nice = make_unique(st, want);

    // Pull pos / rot / dataBlock out of the payload variants if present.
    std::string pos, rot, datablock;
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, node_static_shape> ||
                      std::is_same_v<T, node_item> ||
                      std::is_same_v<T, node_turret> ||
                      std::is_same_v<T, node_sensor> ||
                      std::is_same_v<T, node_marker> ||
                      std::is_same_v<T, node_moveable>) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%g %g %g",
                p.xf.position[0], p.xf.position[1], p.xf.position[2]);
            pos = buf;
            std::snprintf(buf, sizeof(buf), "%g %g %g %g",
                p.xf.rotation[0], p.xf.rotation[1],
                p.xf.rotation[2], p.xf.rotation[3]);
            rot = buf;
            datablock = p.data_block.name;
        }
    }, node.payload);

    body << "    instant " << bind << "(" << nice << ") {\n";
    if (!pos.empty())       body << "        pos = \"" << pos << "\";\n";
    if (!rot.empty())       body << "        rot = \"" << rot << "\";\n";
    if (!datablock.empty()) body << "        dataBlock = \"" << datablock << "\";\n";
    body << "    };\n";
    ++ctx.built_object_count;

    for (const auto& c : node.children) emit_member_node(st, body, c, ctx);
}

} // namespace

// ---------------------------------------------------------------------------
// Load / unload / tick
// ---------------------------------------------------------------------------

bool mission_load(
    MissionContext& ctx,
    const fs::path& missions_dir,
    const fs::path& base_dir,
    std::string_view short_name)
{
    // Defensive: drop any existing MissionGroup before loading.
    if (ctx.active) mission_unload(ctx);

    auto lm = load_mission(missions_dir, base_dir, short_name);
    if (!lm)
    {
        Con::errorf("mission: failed to load \"%s\"", std::string(short_name).c_str());
        return false;
    }

    ctx.name     = std::string(short_name);
    ctx.mis_path = lm->mis_path;
    ctx.scene    = std::move(lm->scene);
    ctx.built_object_count = 0;
    ctx.unbound_exec_idents.clear();

    // Build the MissionGroup body. Note we emit straight into Con::evaluate
    // — the runtime parses + executes the instant-block immediately.
    std::ostringstream body;
    body << "instant SimGroup(MissionGroup) {\n";
    EmitState st;
    st.taken_names.insert("MissionGroup");
    st.unbound = &ctx.unbound_exec_idents;
    for (const auto& c : ctx.scene.root.children)
        emit_member_node(st, body, c, ctx);
    body << "};\n";

    Con::evaluate(body.str().c_str(), false, "mission.build");

    // Verify MissionGroup actually came up — if the parser rejected the
    // emitted body we want to know early.
    if (Sim::findObject("MissionGroup") == nullptr)
    {
        Con::errorf("mission: MissionGroup did not register — emit body rejected");
        // Best-effort recovery: empty group so unload() can still no-op.
        Con::evaluate("instant SimGroup(MissionGroup) {};", false, "mission.build");
    }

    // Run the trailing exec() idents. Tribes scripts mix idents with and
    // without ".cs"; ScriptResolver tries both forms.
    for (const auto& ident : ctx.scene.trailer.exec_idents)
    {
        bool ok = studio::content::cscript::ScriptResolver::runScriptFile(ident.c_str());
        if (!ok)
        {
            Con::warnf("mission: exec(\"%s\") unresolved [unbound]", ident.c_str());
            ctx.unbound_exec_idents.push_back(ident);
        }
    }

    ctx.active = true;
    Con::printf("mission: loaded \"%s\" — %d objects under MissionGroup, %zu trailing exec()s, %zu unbound",
        ctx.name.c_str(),
        ctx.built_object_count,
        ctx.scene.trailer.exec_idents.size(),
        ctx.unbound_exec_idents.size());
    return true;
}

void mission_tick(MissionContext& ctx, float dt_seconds)
{
    if (!ctx.active) return;
    if (dt_seconds <= 0.0f) return;
    U32 dt_ms = static_cast<U32>(dt_seconds * 1000.0f);
    if (dt_ms == 0) dt_ms = 1;  // forward progress to drain schedule(0, ...) events
    Sim::advanceTime(dt_ms);
}

void mission_unload(MissionContext& ctx)
{
    if (!ctx.active) return;

    // Cancel any events still queued against MissionGroup members. The
    // cascade-delete below would also drop them, but doing it here gives
    // a deterministic teardown order.
    if (SimObject* mg = Sim::findObject("MissionGroup"))
    {
        Sim::cancelPendingEvents(mg);
    }

    Con::evaluate("if (isObject(MissionGroup)) MissionGroup.delete();",
                  false, "mission.unload");

    ctx.active = false;
    ctx.built_object_count = 0;
    ctx.scene = scene_graph{};
    ctx.name.clear();
    ctx.mis_path.clear();
    ctx.unbound_exec_idents.clear();
}

int mission_simgroup_member_count()
{
    SimObject* obj = Sim::findObject("MissionGroup");
    if (!obj) return 0;
    auto* g = dynamic_cast<SimGroup*>(obj);
    if (!g) return 0;
    int count = 0;
    // Iterate the group via SimSet::size + getObject — the recursive walk
    // counts every nested member as one.
    std::function<void(SimGroup*)> walk = [&](SimGroup* group)
    {
        for (SimGroup::iterator it = group->begin(); it != group->end(); ++it)
        {
            ++count;
            if (auto* child_group = dynamic_cast<SimGroup*>(*it))
                walk(child_group);
        }
    };
    walk(g);
    return count;
}

} // namespace dts_viewer
