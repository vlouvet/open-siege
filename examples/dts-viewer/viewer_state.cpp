// Spec 25/03 — viewer-state implementation: deferred-load queue, MRU,
// JSON persistence.

#include "viewer_state.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace dts_viewer {
namespace {

PendingLoad g_pending;

std::vector<std::string> g_recent_missions;
std::vector<std::string> g_recent_shapes;

std::vector<std::string> g_mission_catalogue;
std::vector<std::string> g_shape_catalogue;

constexpr std::size_t kMruMax = 10;

fs::path config_dir()
{
    // macOS:  ~/Library/Application Support/open-siege/
    // Linux:  $XDG_CONFIG_HOME/open-siege/ (or ~/.config/open-siege/)
    // Windows %APPDATA%/open-siege/
#if defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return fs::path(home) / "Library" / "Application Support" / "open-siege";
#elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) return {};
    return fs::path(appdata) / "open-siege";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "open-siege";
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return fs::path(home) / ".config" / "open-siege";
#endif
}

fs::path config_file() { return config_dir() / "config.json"; }

void mru_push(std::vector<std::string>& v, const std::string& s)
{
    if (s.empty()) return;
    auto it = std::find(v.begin(), v.end(), s);
    if (it != v.end()) v.erase(it);
    v.insert(v.begin(), s);
    if (v.size() > kMruMax) v.resize(kMruMax);
}

} // namespace

void request_load_mission(const std::string& name)
{
    g_pending = { LoadKind::Mission, name };
}

void request_load_shape(const std::string& name)
{
    g_pending = { LoadKind::Shape, name };
}

PendingLoad take_pending_load()
{
    PendingLoad out = g_pending;
    g_pending = {};
    return out;
}

const std::vector<std::string>& recent_missions() { return g_recent_missions; }
const std::vector<std::string>& recent_shapes()   { return g_recent_shapes; }

void add_recent_mission(const std::string& name)
{
    mru_push(g_recent_missions, name);
    save_config();
}

void add_recent_shape(const std::string& name)
{
    mru_push(g_recent_shapes, name);
    save_config();
}

void load_config()
{
    std::ifstream f(config_file());
    if (!f) return;
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("recent_missions") && j["recent_missions"].is_array()) {
            for (auto& v : j["recent_missions"])
                if (v.is_string()) g_recent_missions.push_back(v.get<std::string>());
        }
        if (j.contains("recent_shapes") && j["recent_shapes"].is_array()) {
            for (auto& v : j["recent_shapes"])
                if (v.is_string()) g_recent_shapes.push_back(v.get<std::string>());
        }
        if (g_recent_missions.size() > kMruMax) g_recent_missions.resize(kMruMax);
        if (g_recent_shapes.size()   > kMruMax) g_recent_shapes.resize(kMruMax);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[config] parse error: %s\n", e.what());
    }
}

void save_config()
{
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    nlohmann::json j;
    j["recent_missions"] = g_recent_missions;
    j["recent_shapes"]   = g_recent_shapes;
    std::ofstream f(config_file());
    if (!f) {
        std::fprintf(stderr, "[config] could not write %s\n",
            config_file().string().c_str());
        return;
    }
    f << j.dump(2);
}

void set_mission_catalogue(std::vector<std::string> names)
{
    g_mission_catalogue = std::move(names);
    std::sort(g_mission_catalogue.begin(), g_mission_catalogue.end());
}

void set_shape_catalogue(std::vector<std::string> names)
{
    g_shape_catalogue = std::move(names);
    std::sort(g_shape_catalogue.begin(), g_shape_catalogue.end());
}

const std::vector<std::string>& mission_catalogue() { return g_mission_catalogue; }
const std::vector<std::string>& shape_catalogue()   { return g_shape_catalogue; }

} // namespace dts_viewer
