#ifndef DTS_VIEWER_MISSION_HPP
#define DTS_VIEWER_MISSION_HPP

// Open Siege spec 17/02 — MissionContext.
//
// Owns the script-side mission lifetime: parse MIS, build a top-level
// SimGroup ("MissionGroup") populated with one SimObject per scene_graph
// entity, run the trailing `exec(<gameplay-rules>);` invocations, and
// provide a tick() that advances the Sim event queue so script-side
// `schedule()` callbacks fire.
//
// Unload destroys the MissionGroup (the spec 16/02 SimGroup cascade
// destructor reaps all members) and cancels any in-flight events.
//
// This is intentionally a thin shell over libcscript_core. It does NOT
// own any GL state, sound state, or gameplay objects — the dts-viewer
// main loop continues to manage those alongside the MissionContext.

#include "content/mission/scene.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace dts_viewer
{

struct MissionContext
{
    // Resolved on load(); empty when no mission is active.
    std::string                          name;
    std::filesystem::path                mis_path;
    studio::content::mission::scene_graph scene;
    bool                                 active = false;

    // Names that the script-side exec() couldn't resolve (logged
    // `[unbound] <ident>` lines). Diagnostic only.
    std::vector<std::string>             unbound_exec_idents;

    // Count of SimObjects the lifecycle code asked to construct on load.
    // Useful for the load->unload->load test (SimGroup count should return
    // to the pre-load value).
    int                                  built_object_count = 0;
};

// Configure where exec() looks for .cs files. Idempotent; call once at
// startup after `dts_viewer::cscript::init()`.
void mission_add_script_search_path(const std::filesystem::path& dir);

// Parse + build + run trailing exec()s for a single mission.
// `missions_dir` is tribes-game/base/missions/, `base_dir` is the parent
// (tribes-game/base/). Returns true on success.
bool mission_load(
    MissionContext& ctx,
    const std::filesystem::path& missions_dir,
    const std::filesystem::path& base_dir,
    std::string_view short_name);

// Drive the script-side schedule() queue by `dt_seconds`. Safe to call
// every frame; safe to call when no mission is active (no-op).
void mission_tick(MissionContext& ctx, float dt_seconds);

// Destroy MissionGroup. Cascade-deletes every entity built by load();
// also cancels any pending Sim events queued against members. Idempotent.
void mission_unload(MissionContext& ctx);

// Diagnostics — number of SimObjects currently parented (directly or
// transitively) under MissionGroup. Returns 0 when the group is absent.
int mission_simgroup_member_count();

} // namespace dts_viewer

#endif // DTS_VIEWER_MISSION_HPP
