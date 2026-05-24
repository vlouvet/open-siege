#include "chat_ui.hpp"

#include <imgui.h>

#include <cstdio>

namespace open_siege {

namespace {

constexpr std::size_t kChatHardCap = 200;

}  // namespace

bool begin_capture(ChatUiState& s, ChatScope scope)
{
    if (s.capturing) return false;
    s.capturing = true;
    s.scope     = scope;
    s.buffer.clear();
    SDL_StartTextInput();
    return true;
}

void clear_capture(ChatUiState& s)
{
    if (s.capturing) {
        SDL_StopTextInput();
    }
    s.capturing = false;
    s.scope     = ChatScope::None;
    s.buffer.clear();
}

CaptureResult feed_event(ChatUiState& s, const SDL_Event& ev)
{
    if (!s.capturing) return CaptureResult::StillCapturing;

    if (ev.type == SDL_KEYDOWN) {
        switch (ev.key.keysym.sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                return CaptureResult::Submitted;
            case SDLK_ESCAPE:
                return CaptureResult::Aborted;
            case SDLK_BACKSPACE:
                if (!s.buffer.empty()) {
                    // Pop trailing utf-8 continuation bytes too.
                    while (!s.buffer.empty()
                           && (static_cast<unsigned char>(s.buffer.back()) & 0xC0u) == 0x80u) {
                        s.buffer.pop_back();
                    }
                    if (!s.buffer.empty()) s.buffer.pop_back();
                }
                return CaptureResult::StillCapturing;
            default:
                break;
        }
    }
    if (ev.type == SDL_TEXTINPUT) {
        const std::size_t add = std::char_traits<char>::length(ev.text.text);
        if (s.buffer.size() + add <= kChatHardCap) {
            s.buffer.append(ev.text.text, add);
        }
    }
    return CaptureResult::StillCapturing;
}

void draw_input_box(const ChatUiState& s,
                    int viewport_w,
                    int viewport_h)
{
    if (!s.capturing) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float vw = float(viewport_w);
    const float vh = float(viewport_h);
    const float box_w = vw - 240.0f;
    const float box_h = 24.0f;
    const float bx = 120.0f;
    const float by = vh - 26.0f;
    dl->AddRectFilled({bx, by}, {bx + box_w, by + box_h},
                      IM_COL32(0, 0, 0, 200));
    dl->AddRect      ({bx, by}, {bx + box_w, by + box_h},
                      IM_COL32(255, 255, 255, 200));
    const char* prefix = s.scope == ChatScope::Team ? "Team:" : "Say:";
    char line[256];
    std::snprintf(line, sizeof(line), "%s  %s_", prefix, s.buffer.c_str());
    dl->AddText({bx + 6.0f, by + 4.0f},
                IM_COL32(230, 230, 230, 240), line);
}

}  // namespace open_siege
