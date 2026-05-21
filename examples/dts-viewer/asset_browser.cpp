// Spec 25/05 — VOL asset browser. Builds a per-VOL entry tree once at
// init; renders + handles selection per frame.

#include "asset_browser.hpp"
#include "viewer_state.hpp"
#include "inspector.hpp"
#include "imgui_layer.hpp"

#include "third_party/imgui/imgui.h"

#include "resources/darkstar_volume.hpp"
#include "resources/resource_explorer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <variant>

namespace fs = std::filesystem;
namespace dv = studio::resources::vol::darkstar;
namespace sr = studio::resources;

namespace dts_viewer {

namespace {

struct VolEntry
{
    std::string name;        // full filename inside the VOL (lowercased)
    std::string ext;         // extension only, no dot
    std::size_t size = 0;
};

struct VolTree
{
    std::string           short_name;  // filename of the VOL
    fs::path              path;        // absolute
    std::vector<VolEntry> entries;     // sorted alphabetically
};

std::vector<VolTree> g_tree;
char g_filter[128] = {};

std::vector<VolEntry> list_vol_entries(const fs::path& vol_path)
{
    std::vector<VolEntry> out;
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
        for (auto& c : name) c = std::tolower((unsigned char)c);
        std::string ext;
        const auto dot = name.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < name.size())
            ext = name.substr(dot + 1);
        out.push_back({ name, ext, f->size });
    }
    std::sort(out.begin(), out.end(),
        [](const VolEntry& a, const VolEntry& b){ return a.name < b.name; });
    return out;
}

bool matches_filter(const std::string& s)
{
    if (g_filter[0] == '\0') return true;
    return std::strstr(s.c_str(), g_filter) != nullptr;
}

void extract_to_disk(const VolTree& vt, const VolEntry& e)
{
    std::ifstream in(vt.path, std::ios::binary);
    if (!in) return;
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) return;
    in.clear(); in.seekg(0);
    auto all = sr::get_all_content(vt.path, in, plugin);
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        std::string n = f->filename.string();
        for (auto& c : n) c = std::tolower((unsigned char)c);
        if (n != e.name) continue;
        fs::path out_dir = fs::current_path() / "extracted";
        std::error_code ec; fs::create_directories(out_dir, ec);
        fs::path out_path = out_dir / e.name;
        std::stringstream buf;
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, buf);
        std::ofstream of(out_path, std::ios::binary);
        of << buf.str();
        std::fprintf(stdout, "[asset] extracted %s -> %s\n",
            e.name.c_str(), out_path.string().c_str());
        return;
    }
}

void hex_dump(const VolTree& vt, const VolEntry& e)
{
    std::ifstream in(vt.path, std::ios::binary);
    if (!in) return;
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) return;
    in.clear(); in.seekg(0);
    auto all = sr::get_all_content(vt.path, in, plugin);
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        std::string n = f->filename.string();
        for (auto& c : n) c = std::tolower((unsigned char)c);
        if (n != e.name) continue;
        std::stringstream buf;
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, buf);
        auto s = buf.str();
        const std::size_t n_bytes = std::min<std::size_t>(s.size(), 256);
        std::fprintf(stdout, "[asset] hex %s (first %zu of %zu bytes):\n",
            e.name.c_str(), n_bytes, s.size());
        for (std::size_t i = 0; i < n_bytes; i += 16) {
            std::fprintf(stdout, "  %04zx: ", i);
            for (std::size_t j = 0; j < 16 && i + j < n_bytes; ++j)
                std::fprintf(stdout, "%02x ", (unsigned char)s[i + j]);
            std::fprintf(stdout, "\n");
        }
        return;
    }
}

void on_double_click(const VolEntry& e)
{
    if (e.ext == "mis") {
        // .mis short-name = filename minus .mis. Mission catalogue is
        // populated from .ted stems so we strip both.
        std::string stem = e.name;
        if (stem.size() > 4) stem.resize(stem.size() - 4);
        request_load_mission(stem);
        add_recent_mission(stem);
    } else if (e.ext == "ted") {
        std::string stem = e.name;
        if (stem.size() > 4) stem.resize(stem.size() - 4);
        request_load_mission(stem);
        add_recent_mission(stem);
    } else if (e.ext == "dts") {
        std::string stem = e.name;
        if (stem.size() > 4) stem.resize(stem.size() - 4);
        request_load_shape(stem);
        add_recent_shape(stem);
    } else {
        std::fprintf(stdout, "[asset] no double-click action for .%s\n",
            e.ext.c_str());
    }
}

} // namespace

void asset_browser_init(const std::vector<MountedVol>& mounts)
{
    g_tree.clear();
    g_tree.reserve(mounts.size());
    for (const auto& m : mounts) {
        VolTree vt;
        vt.short_name = m.short_name;
        vt.path       = m.path;
        vt.entries    = list_vol_entries(m.path);
        g_tree.push_back(std::move(vt));
    }
    std::sort(g_tree.begin(), g_tree.end(),
        [](const VolTree& a, const VolTree& b){
            return a.short_name < b.short_name;
        });
    std::fprintf(stdout, "[asset] indexed %zu VOLs\n", g_tree.size());
}

void asset_browser_draw(bool& visible)
{
    if (!visible) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    // Offset by the main menu bar height so the panel sits below it.
    const float menu_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + menu_h),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, vp->WorkSize.y - menu_h),
                            ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asset Browser", &visible)) {
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##abfilter", "filter\xE2\x80\xA6",
        g_filter, sizeof(g_filter));

    for (const auto& vt : g_tree) {
        if (!ImGui::CollapsingHeader(vt.short_name.c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        for (const auto& e : vt.entries) {
            if (!matches_filter(e.name)) continue;
            ImGui::PushID(&e);
            ImGui::Selectable(e.name.c_str(), false,
                ImGuiSelectableFlags_AllowDoubleClick);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                on_double_click(e);
            }
            if (ImGui::BeginPopupContextItem("##ctx")) {
                if (ImGui::MenuItem("Show Info")) {
                    Selection sel;
                    sel.kind       = SelectionKind::AssetEntry;
                    sel.vol_path   = vt.path;
                    sel.entry_name = e.name;
                    sel.size       = e.size;
                    inspector_set_selection(std::move(sel));
                    inspector_visible_ref() = true;
                }
                if (ImGui::MenuItem("Extract to ./extracted/"))
                    extract_to_disk(vt, e);
                if (ImGui::MenuItem("Hex dump (first 256 B)"))
                    hex_dump(vt, e);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace dts_viewer
