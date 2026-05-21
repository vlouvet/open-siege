// Spec 25/01 — ImGui glue for SDL2 + OpenGL 4.1 core context.

#include "imgui_layer.hpp"

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_sdl2.h"
#include "third_party/imgui/backends/imgui_impl_opengl3.h"

namespace dts_viewer {

void imgui_init(SDL_Window* window, SDL_GLContext ctx)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't litter cwd with imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, ctx);
    // 4.1 core matches the SDL context dts-viewer creates on macOS.
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
    // The host should ignore events that ImGui consumed. Mouse goes to
    // a panel when WantCaptureMouse; keyboard goes to a widget when
    // WantCaptureKeyboard.
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

    // Spec 25/01 acceptance: demo window visible. Spec 25/02 replaces
    // this with the real menu bar build.
    ImGui::ShowDemoWindow();
}

void imgui_end_frame()
{
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace dts_viewer
