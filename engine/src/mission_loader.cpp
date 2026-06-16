#include "mission_loader.hpp"

#include "content/mission/mis.hpp"
#include "content/mission/scene.hpp"

#include "paths.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// 14c-I-6 — scope-always object walk + datablock reference collection
// ---------------------------------------------------------------------------

namespace
{

// Case-insensitive substring search for "flag" — CTF flags are the only
// Items the spec marks as scope-always (TRIBES-GHOST-CLASSES.md §2.1:
// "CTF flag is scope-always (so the HUD flag-status indicator always
// works), generic pickup items are distance-scoped").
bool name_indicates_flag(const std::string& s)
{
    if (s.empty()) return false;
    for (std::size_t i = 0; i + 4 <= s.size(); ++i) {
        if ((std::tolower(static_cast<unsigned char>(s[i]))     == 'f') &&
            (std::tolower(static_cast<unsigned char>(s[i + 1])) == 'l') &&
            (std::tolower(static_cast<unsigned char>(s[i + 2])) == 'a') &&
            (std::tolower(static_cast<unsigned char>(s[i + 3])) == 'g')) {
            return true;
        }
    }
    return false;
}

// Walk the team subtree to infer a team id from the enclosing
// TeamGroup. Tribes 1.41 ships missions whose TeamGroup instance_name
// is "Team1" (red) / "Team2" (blue); a fallthrough returns 0 (neutral).
std::uint8_t team_from_group_name(const std::optional<std::string>& nm)
{
    if (!nm.has_value()) return 0;
    const auto& s = *nm;
    // Look for trailing digit. "Team1" → 1, "Team2" → 2.
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (std::isdigit(static_cast<unsigned char>(*it))) {
            return static_cast<std::uint8_t>(*it - '0');
        }
        if (!std::isspace(static_cast<unsigned char>(*it))) break;
    }
    return 0;
}

// Depth-first walker that produces one ScopeAlwaysIntro per recognised
// scope-always node. `team_id` is inherited from the nearest enclosing
// TeamGroup (so a flag inside Team1 inherits team_id = 1).
void walk_scope_always(const scene_node& node,
                       std::uint8_t inherited_team,
                       std::vector<ScopeAlwaysIntro>& out)
{
    // Detect a new TeamGroup → update inherited team id for children.
    std::uint8_t team_for_children = inherited_team;
    if (std::holds_alternative<node_team_group>(node.payload)) {
        const std::uint8_t t = team_from_group_name(node.instance_name);
        if (t != 0) team_for_children = t;
    }

    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, node_static_shape>) {
            ScopeAlwaysIntro intro{};
            intro.kind = ScopeAlwaysIntro::Kind::StaticShape;
            intro.position = p.xf.position;
            intro.rotation = {p.xf.rotation[0], p.xf.rotation[1], p.xf.rotation[2]};
            intro.datablock_name = p.data_block.name;
            intro.instance_name = node.instance_name.value_or("");
            intro.team_id = inherited_team;
            out.push_back(std::move(intro));
        } else if constexpr (std::is_same_v<T, node_turret>) {
            ScopeAlwaysIntro intro{};
            intro.kind = ScopeAlwaysIntro::Kind::Turret;
            intro.position = p.xf.position;
            intro.rotation = {p.xf.rotation[0], p.xf.rotation[1], p.xf.rotation[2]};
            intro.datablock_name = p.data_block.name;
            intro.instance_name = node.instance_name.value_or("");
            intro.team_id = inherited_team;
            out.push_back(std::move(intro));
        } else if constexpr (std::is_same_v<T, node_marker>) {
            ScopeAlwaysIntro intro{};
            intro.kind = ScopeAlwaysIntro::Kind::Marker;
            intro.position = p.xf.position;
            intro.rotation = {p.xf.rotation[0], p.xf.rotation[1], p.xf.rotation[2]};
            intro.datablock_name = p.data_block.name;
            intro.instance_name = node.instance_name.value_or("");
            intro.team_id = inherited_team;
            out.push_back(std::move(intro));
        } else if constexpr (std::is_same_v<T, node_item>) {
            // Only CTF flags ride the scope-always burst — generic
            // pickups are distance-scoped (per §2.1).
            if (name_indicates_flag(p.data_block.name) ||
                name_indicates_flag(node.instance_name.value_or(""))) {
                ScopeAlwaysIntro intro{};
                intro.kind = ScopeAlwaysIntro::Kind::Item;
                intro.position = p.xf.position;
                intro.rotation = {p.xf.rotation[0], p.xf.rotation[1], p.xf.rotation[2]};
                intro.datablock_name = p.data_block.name;
                intro.instance_name = node.instance_name.value_or("");
                intro.team_id = inherited_team;
                out.push_back(std::move(intro));
            }
        } else if constexpr (std::is_same_v<T, node_sensor>) {
            ScopeAlwaysIntro intro{};
            intro.kind = ScopeAlwaysIntro::Kind::Sensor;
            intro.position = p.xf.position;
            intro.rotation = {p.xf.rotation[0], p.xf.rotation[1], p.xf.rotation[2]};
            intro.datablock_name = p.data_block.name;
            intro.instance_name = node.instance_name.value_or("");
            intro.team_id = inherited_team;
            out.push_back(std::move(intro));
        }
        // All other node kinds — Trigger, Moveable, SimLight, Sky, etc.
        // — are NOT emitted as scope-always intros. Triggers in
        // TRIBES-GHOST-CLASSES.md §2.1 are server-only invisible
        // volumes; Moveables would need a path datablock the scene
        // graph doesn't expose; SimLights are not networked.
    }, node.payload);

    for (const auto& child : node.children) {
        walk_scope_always(child, team_for_children, out);
    }
}

}  // namespace

std::vector<ScopeAlwaysIntro>
scope_always_objects(const LoadedMission& m)
{
    std::vector<ScopeAlwaysIntro> out;
    walk_scope_always(m.scene.root, /*inherited_team*/ 0, out);
    return out;
}

std::unordered_set<std::string>
required_datablock_names(const LoadedMission& m)
{
    std::unordered_set<std::string> out;
    const auto intros = scope_always_objects(m);
    for (const auto& intro : intros) {
        if (!intro.datablock_name.empty()) {
            out.insert(intro.datablock_name);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 14c-I-6 selftest — verify 5_CTF parses + yields >= 8 scope-always objects
// ---------------------------------------------------------------------------

int mission_loader_selftest()
{
    // Resolve the missions directory via the same os_paths machinery the
    // server uses at startup.
    const fs::path assets = os_paths::assets_dir();
    fs::path tribes_dir = assets;
    if (tribes_dir.empty()) {
        // Fallback to the in-tree dev location (mac) so this selftest
        // works in CI / local builds without the user having configured
        // an assets dir.
        tribes_dir = "/Users/v/code/tribes-emscripten/tribes-game";
    }
    const fs::path missions_dir = tribes_dir / "base" / "missions";
    const fs::path base_dir     = tribes_dir / "base";

    if (!fs::is_directory(missions_dir)) {
        std::fprintf(stderr,
            "[mission-loader] FAIL: missions dir not found at %s\n",
            missions_dir.string().c_str());
        return 1;
    }

    auto lm = load_mission(missions_dir, base_dir, "5_CTF");
    if (!lm) {
        std::fprintf(stderr,
            "[mission-loader] FAIL: could not load 5_CTF.mis\n");
        return 1;
    }

    const auto intros = scope_always_objects(*lm);
    const auto names  = required_datablock_names(*lm);

    std::fprintf(stderr,
        "[mission-loader] 5_CTF: scope_always_objects=%zu "
        "required_datablock_names=%zu mounted_vols=%zu\n",
        intros.size(), names.size(), lm->mounted_vols.size());

    int failures = 0;

    // Per spec acceptance: at least 8 scope-always objects for 5_CTF.
    if (intros.size() < 8) {
        std::fprintf(stderr,
            "[mission-loader] FAIL: scope_always_objects().size() = %zu "
            "(spec requires >= 8 for 5_CTF)\n", intros.size());
        ++failures;
    }

    // Sanity: every intro has a non-empty datablock name (otherwise the
    // catalogue resolution downstream cannot map them).
    std::size_t missing_db = 0;
    for (const auto& intro : intros) {
        if (intro.datablock_name.empty()) ++missing_db;
    }
    if (missing_db > intros.size() / 2) {
        std::fprintf(stderr,
            "[mission-loader] FAIL: %zu/%zu intros lack a datablock name\n",
            missing_db, intros.size());
        ++failures;
    } else if (missing_db > 0) {
        std::fprintf(stderr,
            "[mission-loader] note: %zu/%zu intros lack a datablock name "
            "(spawn-point Markers are commonly nameless; tolerated)\n",
            missing_db, intros.size());
    }

    if (failures == 0) {
        std::fputs("[mission-loader] selftest OK\n", stderr);
        return 0;
    }
    std::fprintf(stderr,
        "[mission-loader] selftest FAILED (%d failures)\n", failures);
    return 1;
}

} // namespace dts_viewer
