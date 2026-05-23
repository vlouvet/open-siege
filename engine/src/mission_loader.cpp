#include "mission_loader.hpp"

#include "content/mission/mis.hpp"
#include "content/mission/scene.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ostream>
#include <stdexcept>
#include <variant>

namespace fs = std::filesystem;
using namespace studio::content::mission;

namespace dts_viewer
{

// ---------------------------------------------------------------------------
// Case-insensitive path scan
// ---------------------------------------------------------------------------

static std::string lower(const std::string& s)
{
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

fs::path find_mis(const fs::path& missions_dir, std::string_view name_sv)
{
    if (!fs::is_directory(missions_dir)) return {};
    const std::string name_lo = lower(std::string(name_sv));
    for (const auto& ent : fs::directory_iterator(missions_dir)) {
        if (!ent.is_regular_file()) continue;
        const fs::path p = ent.path();
        if (lower(p.extension().string()) != ".mis") continue;
        if (lower(p.stem().string()) == name_lo) return p;
    }
    return {};
}

// Resolve a VOL filename (bare name from SimVolume) to an absolute path.
// Search order: base_dir first, then missions_dir.  Case-insensitive.
static fs::path resolve_vol(
    const std::string& vol_name,
    const fs::path& base_dir,
    const fs::path& missions_dir)
{
    const std::string lo = lower(vol_name);
    for (const auto& dir : { base_dir, missions_dir }) {
        if (!fs::is_directory(dir)) continue;
        for (const auto& ent : fs::directory_iterator(dir)) {
            if (!ent.is_regular_file()) continue;
            auto p = ent.path();
            if (lower(p.extension().string()) != ".vol") continue;
            if (lower(p.filename().string()) == lo) return p;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// load_mission
// ---------------------------------------------------------------------------

std::optional<LoadedMission> load_mission(
    const fs::path& missions_dir,
    const fs::path& base_dir,
    std::string_view mission_short_name)
{
    const fs::path mis_path = find_mis(missions_dir, mission_short_name);
    if (mis_path.empty()) return std::nullopt;

    std::ifstream f(mis_path);
    if (!f) return std::nullopt;

    mis_file parsed;
    try { parsed = read_mis_file(f); }
    catch (const std::exception&) { return std::nullopt; }

    LoadedMission lm;
    lm.mis_path = mis_path;
    lm.scene = build_scene(parsed);
    lm.mission_type = lm.scene.trailer.game_mission_type.value_or("");

    // Mount volumes in declaration order.
    for (const auto& sv : lm.scene.volumes_in_order) {
        const fs::path resolved = resolve_vol(sv.file_name, base_dir, missions_dir);
        if (!resolved.empty()) {
            lm.mounted_vols.push_back(resolved);
        } else {
            // Not found — record as-is so callers can warn.
            lm.mounted_vols.push_back(missions_dir / sv.file_name);
        }
    }

    return lm;
}

// ---------------------------------------------------------------------------
// print_mission_summary
// ---------------------------------------------------------------------------

static void count_nodes(const scene_node& node,
                         int& statics, int& items, int& interiors,
                         int& markers, int& turrets, int& triggers,
                         int& teams, int& others)
{
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, node_static_shape>)  ++statics;
        else if constexpr (std::is_same_v<T, node_item>)      ++items;
        else if constexpr (std::is_same_v<T, node_interior>)  ++interiors;
        else if constexpr (std::is_same_v<T, node_marker>)    ++markers;
        else if constexpr (std::is_same_v<T, node_turret>)    ++turrets;
        else if constexpr (std::is_same_v<T, node_trigger>)   ++triggers;
        else if constexpr (std::is_same_v<T, node_team_group>)++teams;
        else if constexpr (!std::is_same_v<T, std::monostate>) ++others;
    }, node.payload);

    for (const auto& child : node.children) {
        count_nodes(child, statics, items, interiors, markers, turrets, triggers, teams, others);
    }
}

void print_mission_summary(const LoadedMission& m, std::ostream& os)
{
    os << "mission: " << m.mis_path.filename() << "\n";
    os << "  type: " << (m.mission_type.empty() ? "(none)" : m.mission_type) << "\n";

    int statics=0, items=0, interiors=0, markers=0, turrets=0, triggers=0, teams=0, others=0;
    count_nodes(m.scene.root, statics, items, interiors, markers, turrets, triggers, teams, others);

    os << "  static_shapes: " << statics << "\n";
    os << "  items: "         << items << "\n";
    os << "  interiors: "     << interiors << "\n";
    os << "  markers: "       << markers << "\n";
    os << "  turrets: "       << turrets << "\n";
    os << "  triggers: "      << triggers << "\n";
    os << "  teams: "         << teams << "\n";
    os << "  other_typed: "   << others << "\n";

    if (m.scene.palette) {
        os << "  palette: " << m.scene.palette->ppl_filename << "\n";
    }
    if (m.scene.sky) {
        os << "  sky_dml: " << m.scene.sky->dml_name << "\n";
    }
    if (m.scene.terrain) {
        os << "  terrain_ted: " << m.scene.terrain->ted_filename << "\n";
    }

    os << "  mounted_vols (" << m.mounted_vols.size() << "):\n";
    for (const auto& v : m.mounted_vols) {
        const bool exists = fs::exists(v);
        os << "    " << v.filename() << (exists ? "" : " [MISSING]") << "\n";
    }
}

} // namespace dts_viewer
