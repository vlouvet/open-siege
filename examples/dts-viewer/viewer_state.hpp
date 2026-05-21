#pragma once

// Spec 25/03 — global viewer state shared between the ImGui layer and
// the host. Holds deferred-load requests (so menu callbacks don't fire
// load logic mid-ImGui-frame), the Recent MRU, and the relaunch
// callbacks each mode provides at startup.

#include <functional>
#include <string>
#include <vector>

namespace dts_viewer {

enum class LoadKind { None, Mission, Shape };

struct PendingLoad
{
    LoadKind kind = LoadKind::None;
    std::string name;
};

// Queue a load. Called by modal/menu/asset-browser code; consumed by
// the host's main loop at frame start.
void request_load_mission(const std::string& name);
void request_load_shape(const std::string& name);
PendingLoad take_pending_load();

// Recent MRU. push_front + dedup + truncate at 10. Auto-saves to
// config.json on each add.
const std::vector<std::string>& recent_missions();
const std::vector<std::string>& recent_shapes();
void add_recent_mission(const std::string& name);
void add_recent_shape(const std::string& name);

// Load/save MRU from disk. Call load_config() once at startup;
// save_config() runs automatically when MRU mutates.
void load_config();
void save_config();

// Catalogues populated by the host at startup. The Open Mission /
// Open Shape modals filter from these lists.
void set_mission_catalogue(std::vector<std::string> names);
void set_shape_catalogue(std::vector<std::string> names);
const std::vector<std::string>& mission_catalogue();
const std::vector<std::string>& shape_catalogue();

// Spec 25/07 — edit mode global. When true the host suspends player
// input; the ImGui layer paints an "EDIT MODE" overlay.
bool edit_mode_active();
void set_edit_mode(bool active);

} // namespace dts_viewer
