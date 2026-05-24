#ifndef OSENGINE_CHAT_CHANNEL_HPP
#define OSENGINE_CHAT_CHANNEL_HPP

// Spec 28/10 — text chat + system event feed.
//
// ChatChannel is a per-server (NOT per-session) ring of recent lines.
// Each Session keeps a "next undelivered index" cursor; drain_for(s)
// returns every line newer than the cursor that the session is
// permitted to see (global lines, team-scope lines for their team,
// system-emitted lines).
//
// Encoding on the wire is deferred to a follow-on encoder spec; for
// v1 the lines are exchanged via the same OSGB ghost stream by
// attaching a "system message" record kind. The split here keeps the
// orchestration testable without the encoder.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dts_viewer
{

struct Session;
class  SessionTable;

enum class ChatScope : std::uint8_t {
    Global = 0,
    Team   = 1,
    System = 2,         // server-system message (kill feed, capture, ...)
};

struct ChatLine
{
    std::uint16_t sender_slot = 0xFFFFu;   // 0xFFFF == server-system
    std::string   text;
    ChatScope     scope        = ChatScope::Global;
    std::uint8_t  sender_team  = 0;        // raw Team value, 0 == Spectator
    std::uint64_t timestamp_ms = 0;
};

class ChatChannel
{
public:
    // Push an inbound chat line from a client. Truncates to 200 chars.
    void on_inbound(std::uint16_t sender_slot,
                    const std::string& text,
                    ChatScope scope,
                    const SessionTable& sessions,
                    std::uint64_t now_ms);

    // System / server-side messages (kill feed, capture, match end).
    void emit_system(const std::string& text, std::uint64_t now_ms);

    // Drain lines this session hasn't seen yet AND is allowed to see.
    std::vector<ChatLine> drain_for(const Session& s);

    // Diagnostic only — total line count ever written.
    std::size_t total_lines() const noexcept { return lines_.size(); }

    static int selftest();

private:
    std::vector<ChatLine> lines_;
    // session-key -> next-undelivered index. Key is endpoint port; v1
    // single-binding (host == 127.0.0.1 most of the time) is fine.
    std::unordered_map<std::uint64_t, std::size_t> cursors_;

    static std::uint64_t session_key(const Session& s);
};

} // namespace dts_viewer

#endif // OSENGINE_CHAT_CHANNEL_HPP
