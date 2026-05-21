// Spec 25/06 — Inspector panel impl. For an asset entry: shows path,
// size, extension, first 64 bytes of raw content (so binary headers
// like DTS magic are visible).

#include "inspector.hpp"

#include "third_party/imgui/imgui.h"

#include "resources/darkstar_volume.hpp"
#include "resources/resource_explorer.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <variant>

namespace dv = studio::resources::vol::darkstar;
namespace sr = studio::resources;

namespace dts_viewer {

namespace {

Selection                g_sel;
std::vector<std::uint8_t> g_header_bytes;
std::size_t              g_total_size = 0;

void cache_header_bytes()
{
    g_header_bytes.clear();
    g_total_size = 0;
    if (g_sel.kind != SelectionKind::AssetEntry) return;
    std::ifstream in(g_sel.vol_path, std::ios::binary);
    if (!in) return;
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) return;
    in.clear(); in.seekg(0);
    auto all = sr::get_all_content(g_sel.vol_path, in, plugin);
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        std::string n = f->filename.string();
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (n != g_sel.entry_name) continue;
        g_total_size = f->size;
        std::stringstream buf;
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, buf);
        auto s = buf.str();
        const std::size_t n_bytes = std::min<std::size_t>(s.size(), 64);
        g_header_bytes.assign(s.begin(), s.begin() + (std::ptrdiff_t)n_bytes);
        return;
    }
}

} // namespace

void inspector_set_selection(Selection sel)
{
    g_sel = std::move(sel);
    cache_header_bytes();
}

void inspector_clear_selection()
{
    g_sel = {};
    g_header_bytes.clear();
    g_total_size = 0;
}

void inspector_draw(bool& visible)
{
    if (!visible) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float menu_h = ImGui::GetFrameHeight();
    const float width  = 360.0f;
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - width,
               vp->WorkPos.y + menu_h),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(width, vp->WorkSize.y - menu_h),
                            ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", &visible)) {
        ImGui::End();
        return;
    }

    if (g_sel.kind == SelectionKind::None) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextWrapped("Right-click an entry in the Asset Browser "
                           "and choose Show Info to inspect it.");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Asset");
    ImGui::Separator();
    ImGui::Text("VOL:   %s",  g_sel.vol_path.filename().string().c_str());
    ImGui::Text("Name:  %s",  g_sel.entry_name.c_str());
    ImGui::Text("Size:  %zu bytes", g_total_size);
    auto dot = g_sel.entry_name.find_last_of('.');
    if (dot != std::string::npos)
        ImGui::Text("Ext:   .%s", g_sel.entry_name.c_str() + dot + 1);
    ImGui::Separator();
    ImGui::TextDisabled("Header (first %zu bytes)", g_header_bytes.size());
    if (!g_header_bytes.empty()) {
        ImGui::BeginChild("##hex", ImVec2(0, 220), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (std::size_t i = 0; i < g_header_bytes.size(); i += 16) {
            char hex[64] = {};
            char ascii[20] = {};
            char* h = hex;
            char* a = ascii;
            for (std::size_t j = 0; j < 16 && i + j < g_header_bytes.size(); ++j) {
                std::uint8_t b = g_header_bytes[i + j];
                h += std::snprintf(h, hex + sizeof(hex) - h, "%02x ", b);
                *a++ = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
            }
            *a = '\0';
            ImGui::Text("%04zx  %-48s  %s", i, hex, ascii);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace dts_viewer
