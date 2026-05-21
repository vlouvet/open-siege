// Spec 25/01 — ImGui glue for SDL2 + OpenGL 4.1 core context.
// Spec 25/02 — top-level menu bar (File / Edit / View / Help).
// Spec 25/03 — Open Mission / Open Shape pickers + Recent submenu.

#include "imgui_layer.hpp"
#include "viewer_state.hpp"
#include "asset_browser.hpp"
#include "inspector.hpp"

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_sdl2.h"
#include "third_party/imgui/backends/imgui_impl_opengl3.h"

#include <SDL_keycode.h>

#include <cstring>

namespace dts_viewer {

namespace {

MenuActions g_actions;

// Single source of truth for Cmd/Ctrl. macOS keystrokes use KMOD_GUI;
// Linux/Windows use KMOD_CTRL. Recognise either.
bool is_cmdctrl(const SDL_Event& ev)
{
    Uint16 m = ev.key.keysym.mod;
    return (m & (KMOD_GUI | KMOD_CTRL)) != 0;
}

void call_or_log(const std::function<void()>& fn, const char* label)
{
    std::fprintf(stdout, "[menu] %s\n", label);
    if (fn) fn();
}

void default_quit()
{
    SDL_Event e;
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}

// Spec 25/03 picker-modal state. One bool per modal; the menu sets
// them and the draw pass reads + clears them.
bool   g_open_mission_modal = false;
bool   g_open_shape_modal   = false;
char   g_modal_filter[128]  = {};

void draw_picker_modal(const char* title,
                       const std::vector<std::string>& items,
                       void (*on_select)(const std::string&))
{
    if (!ImGui::BeginPopupModal(title, nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputTextWithHint("##filter", "filter\xE2\x80\xA6",
        g_modal_filter, sizeof(g_modal_filter));

    if (ImGui::BeginListBox("##items", ImVec2(360, 320))) {
        for (const auto& name : items) {
            if (g_modal_filter[0] != '\0' &&
                std::strstr(name.c_str(), g_modal_filter) == nullptr) {
                continue;
            }
            bool dummy = false;
            if (ImGui::Selectable(name.c_str(), dummy,
                    ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    on_select(name);
                    g_modal_filter[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndListBox();
    }
    if (ImGui::Button("Cancel")) {
        g_modal_filter[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void on_pick_mission(const std::string& name)
{
    request_load_mission(name);
    add_recent_mission(name);
}

void on_pick_shape(const std::string& name)
{
    request_load_shape(name);
    add_recent_shape(name);
}

bool query(const std::function<bool()>& q) { return q ? q() : false; }

// Display strings for accelerators. On macOS use "Cmd"; on others
// "Ctrl". Compile-time choice keeps it cheap.
#if defined(__APPLE__)
constexpr const char* kCmdO       = "Cmd+O";
constexpr const char* kCmdShiftO  = "Cmd+Shift+O";
constexpr const char* kCmdQ       = "Cmd+Q";
constexpr const char* kCmdR       = "Cmd+R";
constexpr const char* kCmdE       = "Cmd+E";
#else
constexpr const char* kCmdO       = "Ctrl+O";
constexpr const char* kCmdShiftO  = "Ctrl+Shift+O";
constexpr const char* kCmdQ       = "Ctrl+Q";
constexpr const char* kCmdR       = "Ctrl+R";
constexpr const char* kCmdE       = "Ctrl+E";
#endif

void draw_menu_bar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Mission\xE2\x80\xA6", kCmdO)) {
            std::fprintf(stdout, "[menu] File > Open Mission\n");
            g_open_mission_modal = true;
        }
        if (ImGui::MenuItem("Open Shape\xE2\x80\xA6", kCmdShiftO)) {
            std::fprintf(stdout, "[menu] File > Open Shape\n");
            g_open_shape_modal = true;
        }
        if (ImGui::BeginMenu("Recent")) {
            const auto& rm = recent_missions();
            const auto& rs = recent_shapes();
            if (rm.empty() && rs.empty()) {
                ImGui::MenuItem("(empty)", nullptr, false, false);
            } else {
                if (!rm.empty()) {
                    ImGui::TextDisabled("Missions");
                    for (const auto& m : rm) {
                        if (ImGui::MenuItem(m.c_str()))
                            on_pick_mission(m);
                    }
                }
                if (!rs.empty()) {
                    if (!rm.empty()) ImGui::Separator();
                    ImGui::TextDisabled("Shapes");
                    for (const auto& s : rs) {
                        if (ImGui::MenuItem(s.c_str()))
                            on_pick_shape(s);
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", kCmdQ)) {
            call_or_log(g_actions.quit, "File > Quit");
            if (!g_actions.quit) default_quit();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Reload Scripts", kCmdR))
            call_or_log(g_actions.reload_scripts, "Edit > Reload Scripts");
        if (ImGui::MenuItem("Enter Edit Mode", kCmdE))
            call_or_log(g_actions.toggle_edit_mode, "Edit > Enter Edit Mode");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        const bool has_wf = (bool)g_actions.toggle_wireframe;
        if (ImGui::MenuItem("Wireframe", "`", query(g_actions.is_wireframe),
                            has_wf))
            call_or_log(g_actions.toggle_wireframe, "View > Wireframe");

        const bool has_hud = (bool)g_actions.toggle_hud;
        if (ImGui::MenuItem("HUD", "F1", query(g_actions.is_hud), has_hud))
            call_or_log(g_actions.toggle_hud, "View > HUD");

        ImGui::Separator();
        const bool has_cam = (bool)g_actions.set_free_camera;
        if (ImGui::MenuItem("Free Camera", "Tab",
                            query(g_actions.is_free_camera), has_cam))
            call_or_log(g_actions.set_free_camera, "View > Free Camera");
        if (ImGui::MenuItem("Walk Camera", "Tab",
                            query(g_actions.is_walk_camera), has_cam))
            call_or_log(g_actions.set_walk_camera, "View > Walk Camera");

        ImGui::Separator();
        const bool has_bbox = (bool)g_actions.toggle_bbox;
        if (ImGui::MenuItem("Bounding Boxes", "F3",
                            query(g_actions.is_bbox), has_bbox))
            call_or_log(g_actions.toggle_bbox, "View > Bounding Boxes");
        const bool has_fps = (bool)g_actions.toggle_fps;
        if (ImGui::MenuItem("Show FPS", nullptr, query(g_actions.is_fps),
                            has_fps))
            call_or_log(g_actions.toggle_fps, "View > Show FPS");
        ImGui::Separator();
        const bool has_ab = (bool)g_actions.toggle_asset_browser;
        if (ImGui::MenuItem("Asset Browser", nullptr,
                            query(g_actions.is_asset_browser), has_ab))
            call_or_log(g_actions.toggle_asset_browser, "View > Asset Browser");
        const bool has_ins = (bool)g_actions.toggle_inspector;
        if (ImGui::MenuItem("Inspector", nullptr,
                            query(g_actions.is_inspector), has_ins))
            call_or_log(g_actions.toggle_inspector, "View > Inspector");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("User Guide", "F1"))
            call_or_log(g_actions.help_user_guide, "Help > User Guide");
        if (ImGui::MenuItem("Controls & Keybindings"))
            call_or_log(g_actions.help_controls, "Help > Controls & Keybindings");
        if (ImGui::MenuItem("Scripting Reference"))
            call_or_log(g_actions.help_scripting, "Help > Scripting Reference");
        ImGui::Separator();
        if (ImGui::MenuItem("Online Docs"))
            call_or_log(g_actions.help_online_docs, "Help > Online Docs");
        if (ImGui::MenuItem("Report an Issue"))
            call_or_log(g_actions.help_report_issue, "Help > Report an Issue");
        ImGui::Separator();
        if (ImGui::MenuItem("About Open Siege\xE2\x80\xA6"))
            call_or_log(g_actions.help_about, "Help > About Open Siege");
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    // Spec 25/03 — drive picker modals from the click-set flags. Open
    // here (after the menu bar closed), so the popup stack is
    // independent of the menu hierarchy.
    if (g_open_mission_modal) {
        ImGui::OpenPopup("Open Mission");
        g_open_mission_modal = false;
    }
    if (g_open_shape_modal) {
        ImGui::OpenPopup("Open Shape");
        g_open_shape_modal = false;
    }
    draw_picker_modal("Open Mission", mission_catalogue(), &on_pick_mission);
    draw_picker_modal("Open Shape",   shape_catalogue(),   &on_pick_shape);
}

} // namespace

void imgui_init(SDL_Window* window, SDL_GLContext ctx)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, ctx);
    ImGui_ImplOpenGL3_Init("#version 410 core");
}

void imgui_shutdown()
{
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

bool imgui_process_event(const SDL_Event& ev)
{
    if (ImGui::GetCurrentContext() == nullptr) return false;
    ImGui_ImplSDL2_ProcessEvent(&ev);
    const ImGuiIO& io = ImGui::GetIO();
    switch (ev.type) {
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL:
            return io.WantCaptureMouse;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_TEXTINPUT:
            return io.WantCaptureKeyboard;
        default:
            return false;
    }
}

namespace {
bool g_asset_browser_visible = true;
bool g_inspector_visible     = false;
} // namespace

void imgui_begin_frame()
{
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    draw_menu_bar();
    asset_browser_draw(g_asset_browser_visible);
    inspector_draw(g_inspector_visible);
}

bool& asset_browser_visible_ref() { return g_asset_browser_visible; }
bool& inspector_visible_ref()     { return g_inspector_visible; }

void imgui_end_frame()
{
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void set_menu_actions(const MenuActions& a) { g_actions = a; }

bool imgui_try_menu_shortcut(const SDL_Event& ev)
{
    if (ev.type != SDL_KEYDOWN) return false;
    if (ev.key.repeat) return false;
    const SDL_Keycode k = ev.key.keysym.sym;

    if (k == SDLK_F1) {
        call_or_log(g_actions.help_user_guide, "Help > User Guide (F1)");
        return true;
    }

    if (!is_cmdctrl(ev)) return false;
    const bool shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;

    switch (k) {
        case SDLK_o:
            if (shift) { std::fprintf(stdout, "[menu] File > Open Shape\n");
                         g_open_shape_modal = true; }
            else       { std::fprintf(stdout, "[menu] File > Open Mission\n");
                         g_open_mission_modal = true; }
            return true;
        case SDLK_q:
            call_or_log(g_actions.quit, "File > Quit");
            if (!g_actions.quit) default_quit();
            return true;
        case SDLK_r:
            call_or_log(g_actions.reload_scripts, "Edit > Reload Scripts");
            return true;
        case SDLK_e:
            call_or_log(g_actions.toggle_edit_mode, "Edit > Enter Edit Mode");
            return true;
        default:
            return false;
    }
}

} // namespace dts_viewer
