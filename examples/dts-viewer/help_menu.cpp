// Spec 25/08 — docs URL resolver + About modal.

#include "help_menu.hpp"
#include "third_party/imgui/imgui.h"

#include <cstdio>
#include <filesystem>
#include <mach-o/dyld.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace dts_viewer {

const char* kOnlineDocsUrl   = "https://github.com/vlouvet/open-siege/blob/macos-arm64/docs/user/index.md";
const char* kReportIssueUrl  = "https://github.com/vlouvet/open-siege/issues/new";

namespace {

fs::path executable_dir()
{
#if defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        return fs::weakly_canonical(fs::path(buf)).parent_path();
    }
#endif
    return fs::current_path();
}

// Search candidates upward from the executable dir for either layout.
fs::path find_docs_root()
{
    const fs::path start = executable_dir();
    fs::path d = start;
    for (int hops = 0; hops < 6; ++hops) {
        const fs::path installer_layout = d / "share" / "open-siege" / "docs";
        if (fs::exists(installer_layout / "index.html"))
            return installer_layout;
        const fs::path dev_layout = d / "docs" / "user" / "_site";
        if (fs::exists(dev_layout / "index.html"))
            return dev_layout;
        // Sourcetree fallback: raw markdown under docs/user/.
        const fs::path src_layout = d / "docs" / "user";
        if (fs::exists(src_layout / "index.md"))
            return src_layout;
        if (!d.has_parent_path() || d == d.parent_path()) break;
        d = d.parent_path();
    }
    return {};
}

bool g_about_open = false;

} // namespace

std::string docs_url_for(const char* page)
{
    fs::path root = find_docs_root();
    if (root.empty()) {
        std::fprintf(stderr,
            "[help] local docs not found; opening online mirror\n");
        return kOnlineDocsUrl;
    }
    fs::path resolved = root / page;
    if (!fs::exists(resolved)) {
        // For the markdown-source fallback, try `.md` instead of `.html`.
        std::string p = page;
        if (auto dot = p.find_last_of('.'); dot != std::string::npos) {
            std::string md = p.substr(0, dot) + ".md";
            if (fs::exists(root / md)) resolved = root / md;
        }
    }
    if (!fs::exists(resolved)) {
        std::fprintf(stderr,
            "[help] %s not found under %s; opening online mirror\n",
            page, root.string().c_str());
        return kOnlineDocsUrl;
    }
    return "file://" + resolved.string();
}

void about_modal_open() { g_about_open = true; }

void about_modal_draw()
{
    if (g_about_open) {
        ImGui::OpenPopup("About Open Siege");
        g_about_open = false;
    }
    if (!ImGui::BeginPopupModal("About Open Siege", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Open Siege");
    ImGui::Text("Native macOS arm64 fork of the Dynamix Darkstar engine,");
    ImGui::Text("running the freeware 1998 release of Starsiege: Tribes.");
    ImGui::Separator();
    ImGui::Text("Version:    dev (track 25 — viewer-toolbar)");
    ImGui::Text("Build date: %s %s", __DATE__, __TIME__);
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Vendored licences")) {
        ImGui::BulletText("3space / Open Siege parsers — MIT");
        ImGui::BulletText("Torque3D cscript core — MIT");
        ImGui::BulletText("Dear ImGui v1.90.4 — MIT");
        ImGui::BulletText("miniaudio v0.11.25 — public domain");
        ImGui::BulletText("nlohmann/json — MIT");
        ImGui::BulletText("SDL2 — zlib");
    }
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace dts_viewer
