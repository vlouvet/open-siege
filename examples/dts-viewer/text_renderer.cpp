#include "text_renderer.hpp"

#include "third_party/imgui/imgui.h"

namespace dts_viewer
{

void text_draw(float x_px, float y_px,
               const std::string& text,
               TextColor color,
               float size_px)
{
    if (text.empty()) return;
    // GetForegroundDrawList is rendered after every ImGui window for the
    // current frame, so this lands on top of our 3D scene + HUD bars.
    auto* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const auto to_u8 = [](float c){
        const int v = static_cast<int>(c * 255.0f + 0.5f);
        return static_cast<unsigned int>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    const ImU32 col = IM_COL32(to_u8(color.r), to_u8(color.g),
                               to_u8(color.b), to_u8(color.a));
    dl->AddText(ImGui::GetFont(), size_px, ImVec2(x_px, y_px), col, text.c_str());
}

float text_measure_width(const std::string& text, float size_px)
{
    ImFont* f = ImGui::GetFont();
    if (!f || text.empty()) return 0.0f;
    const ImVec2 sz = f->CalcTextSizeA(size_px, FLT_MAX, 0.0f, text.c_str());
    return sz.x;
}

} // namespace dts_viewer
