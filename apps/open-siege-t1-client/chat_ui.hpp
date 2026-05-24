#ifndef OPEN_SIEGE_T1_CLIENT_CHAT_UI_HPP
#define OPEN_SIEGE_T1_CLIENT_CHAT_UI_HPP

// Spec 29/08 — chat text input box + send hook.
//
// v1 scope: full client-side capture (T = global, Y = team, Esc =
// cancel, Enter = submit), feed lines into NetClient::send_chat which
// is a stderr-log stub until spec 28/10b lands the actual wire
// encoder. The received feed is fed via HudGlueState::message_feed —
// the server side of receive lives in spec 28/10.

#include <SDL.h>

#include <cstdint>
#include <string>

namespace dts_viewer { class NetClient; }

namespace open_siege {

enum class ChatScope : std::uint8_t { None = 0, Global = 1, Team = 2 };

struct ChatUiState {
    bool        capturing  = false;
    ChatScope   scope      = ChatScope::None;
    std::string buffer;     // utf-8, hard cap 200 chars
};

// Begin capture. Returns true if accepted (was idle before).
bool begin_capture(ChatUiState& s, ChatScope scope);

enum class CaptureResult { StillCapturing, Submitted, Aborted };

// Feed one SDL event. On Submitted, the caller should drain s.buffer
// and dispatch via NetClient::send_chat, then call clear_capture().
CaptureResult feed_event(ChatUiState& s, const SDL_Event& ev);

void clear_capture(ChatUiState& s);

// Per-frame: draw the input box (when capturing). No-op otherwise.
void draw_input_box(const ChatUiState& s,
                    int viewport_w,
                    int viewport_h);

}  // namespace open_siege

#endif  // OPEN_SIEGE_T1_CLIENT_CHAT_UI_HPP
