// Spec 25/01 — ImGui glue for SDL2 + OpenGL 4.1 core context.
// Spec 25/02 — top-level menu bar (File / Edit / View / Help).

#include "imgui_layer.hpp"

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_sdl2.h"
#include "third_party/imgui/backends/imgui_impl_opengl3.h"

#include <SDL_keycode.h>

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
        if (ImGui::MenuItem("Open Mission\xE2\x80\xA6", kCmdO))
            call_or_log(g_actions.open_mission, "File > Open Mission");
        if (ImGui::MenuItem("Open Shape\xE2\x80\xA6", kCmdShiftO))
            call_or_log(g_actions.open_shape, "File > Open Shape");
        if (ImGui::BeginMenu("Recent")) {
            // Spec 25/03 will populate this; for the skeleton, show an
            // explicit "(empty)" hint so menu navigation works.
            ImGui::MenuItem("(empty)", nullptr, false, false);
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
        if (ImGui::MenuItem("Wireframe", "W", query(g_actions.is_wireframe)))
            call_or_log(g_actions.toggle_wireframe, "View > Wireframe");
        if (ImGui::MenuItem("HUD", "H", query(g_actions.is_hud)))
            call_or_log(g_actions.toggle_hud, "View > HUD");
        ImGui::Separator();
        if (ImGui::MenuItem("Free Camera", "F", query(g_actions.is_free_camera)))
            call_or_log(g_actions.set_free_camera, "View > Free Camera");
        if (ImGui::MenuItem("Walk Camera", "G", query(g_actions.is_walk_camera)))
            call_or_log(g_actions.set_walk_camera, "View > Walk Camera");
        ImGui::Separator();
        if (ImGui::MenuItem("Bounding Boxes", "B", query(g_actions.is_bbox)))
            call_or_log(g_actions.toggle_bbox, "View > Bounding Boxes");
        if (ImGui::MenuItem("Show FPS", "Shift+F", query(g_actions.is_fps)))
            call_or_log(g_actions.toggle_fps, "View > Show FPS");
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

void imgui_begin_frame()
{
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    draw_menu_bar();
}

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
            if (shift) call_or_log(g_actions.open_shape, "File > Open Shape");
            else       call_or_log(g_actions.open_mission, "File > Open Mission");
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
