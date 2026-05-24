#include <osengine/chat_channel.hpp>

#include <osengine/session_table.hpp>

#include <algorithm>
#include <cstdio>

namespace dts_viewer
{

std::uint64_t ChatChannel::session_key(const Session& s)
{
    // host string is small but variable; combine port + first chars of
    // host into a single 64-bit key.
    std::uint64_t k = static_cast<std::uint64_t>(s.peer.port);
    for (char c : s.peer.host) {
        k = (k * 131u) ^ static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    }
    return k;
}

void ChatChannel::on_inbound(std::uint16_t sender_slot,
                             const std::string& text,
                             ChatScope scope,
                             const SessionTable& sessions,
                             std::uint64_t now_ms)
{
    auto& mut = const_cast<SessionTable&>(sessions);
    // Find the sender to capture their team.
    std::uint8_t team_raw = 0;
    for (auto* s : mut.active_sessions()) {
        if (s && s->player_slot == sender_slot) {
            team_raw = static_cast<std::uint8_t>(s->team);
            break;
        }
    }
    ChatLine line;
    line.sender_slot  = sender_slot;
    line.text         = text.substr(0, std::min<std::size_t>(text.size(), 200));
    line.scope        = (scope == ChatScope::System) ? ChatScope::Global : scope;
    line.sender_team  = team_raw;
    line.timestamp_ms = now_ms;
    lines_.push_back(std::move(line));
    std::fprintf(stderr, "[chat] slot %u (%s): %s\n",
        (unsigned)sender_slot,
        (scope == ChatScope::Team ? "team" : "all"),
        text.c_str());
}

void ChatChannel::emit_system(const std::string& text, std::uint64_t now_ms)
{
    ChatLine line;
    line.sender_slot  = 0xFFFFu;
    line.text         = text.substr(0, std::min<std::size_t>(text.size(), 200));
    line.scope        = ChatScope::System;
    line.sender_team  = 0;
    line.timestamp_ms = now_ms;
    lines_.push_back(std::move(line));
    std::fprintf(stderr, "[sys] %s\n", text.c_str());
}

std::vector<ChatLine> ChatChannel::drain_for(const Session& s)
{
    const std::uint64_t key = session_key(s);
    auto it = cursors_.find(key);
    std::size_t cursor = (it == cursors_.end()) ? 0 : it->second;
    std::vector<ChatLine> out;
    for (; cursor < lines_.size(); ++cursor) {
        const ChatLine& l = lines_[cursor];
        if (l.scope == ChatScope::Team) {
            if (l.sender_team != static_cast<std::uint8_t>(s.team)) continue;
        }
        // Global + System lines reach everyone.
        out.push_back(l);
    }
    cursors_[key] = lines_.size();
    return out;
}

int ChatChannel::selftest()
{
    SessionTable table(4);
    const std::uint8_t n[3] = { 1, 2, 3 };
    Session* red_a = table.allocate({"127.0.0.1", 64001}, n, 0);
    Session* red_b = table.allocate({"127.0.0.1", 64002}, n, 0);
    Session* blue  = table.allocate({"127.0.0.1", 64003}, n, 0);
    red_a->team = Team::Red;
    red_b->team = Team::Red;
    blue->team  = Team::Blue;

    ChatChannel ch;

    // Global chat from red_a: red_b AND blue both see it.
    ch.on_inbound(red_a->player_slot, "gg", ChatScope::Global, table, 100);
    auto rbA = ch.drain_for(*red_b);
    auto bA  = ch.drain_for(*blue);
    if (rbA.size() != 1 || rbA[0].text != "gg") {
        std::fputs("[chat-selftest] global: red_b missed line\n", stderr); return 1;
    }
    if (bA.size() != 1 || bA[0].text != "gg") {
        std::fputs("[chat-selftest] global: blue missed line\n", stderr); return 1;
    }

    // Team chat from red_a: red_b sees, blue does NOT.
    ch.on_inbound(red_a->player_slot, "push left", ChatScope::Team, table, 200);
    auto rbB = ch.drain_for(*red_b);
    auto bB  = ch.drain_for(*blue);
    if (rbB.size() != 1 || rbB[0].text != "push left") {
        std::fputs("[chat-selftest] team: red_b missed team line\n", stderr); return 1;
    }
    if (!bB.empty()) {
        std::fputs("[chat-selftest] team: blue should not see team-scope line\n", stderr);
        return 1;
    }

    // System line.
    ch.emit_system("Match begins!", 300);
    auto rbC = ch.drain_for(*red_b);
    auto bC  = ch.drain_for(*blue);
    if (rbC.size() != 1 || rbC[0].scope != ChatScope::System) {
        std::fputs("[chat-selftest] system: red_b missed\n", stderr); return 1;
    }
    if (bC.size() != 1 || bC[0].scope != ChatScope::System) {
        std::fputs("[chat-selftest] system: blue missed\n", stderr); return 1;
    }

    // Drain idempotence — second drain returns nothing new.
    auto rbD = ch.drain_for(*red_b);
    if (!rbD.empty()) {
        std::fputs("[chat-selftest] drain cursor not advancing\n", stderr); return 1;
    }

    // Truncate >200 chars.
    std::string huge(500, 'x');
    ch.on_inbound(red_a->player_slot, huge, ChatScope::Global, table, 400);
    auto rbE = ch.drain_for(*red_b);
    if (rbE.size() != 1 || rbE[0].text.size() != 200) {
        std::fprintf(stderr, "[chat-selftest] truncation: got %zu chars\n",
                     rbE.empty() ? std::size_t{0} : rbE[0].text.size());
        return 1;
    }

    std::fputs("[chat-selftest] OK — global, team-scope, system, drain, truncate\n",
               stderr);
    return 0;
}

} // namespace dts_viewer
