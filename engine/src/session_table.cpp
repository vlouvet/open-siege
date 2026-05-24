#include <osengine/session_table.hpp>

#include <cstring>

namespace dts_viewer
{

SessionTable::SessionTable(std::uint16_t max_players)
    : max_players_(max_players),
      slot_used_(max_players, false)
{}

std::uint16_t SessionTable::alloc_slot_locked()
{
    for (std::uint16_t i = 0; i < max_players_; ++i) {
        if (!slot_used_[i]) {
            slot_used_[i] = true;
            return i;
        }
    }
    return 0xffff;   // sentinel: none free
}

void SessionTable::free_slot_locked(std::uint16_t slot)
{
    if (slot < max_players_) slot_used_[slot] = false;
}

Session* SessionTable::allocate(const studio::content::net::Endpoint& peer,
                                const std::uint8_t nonce[3],
                                std::uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_peer_.find(peer);
    if (it != by_peer_.end()) {
        // Existing session — RequestConnect retransmit. Don't change
        // nonce or slot; just refresh last_seen.
        it->second.last_seen_ms = now_ms;
        return &it->second;
    }
    std::uint16_t slot = alloc_slot_locked();
    if (slot == 0xffff) return nullptr;     // server full
    Session s;
    s.peer = peer;
    std::memcpy(s.nonce, nonce, 3);
    s.player_slot = slot;
    s.connected_at_ms = now_ms;
    s.last_seen_ms = now_ms;
    s.next_send_seq = 1;
    s.last_acked_recv_seq = 0;
    s.ghost_burst_sent = false;
    auto [ins_it, _] = by_peer_.emplace(peer, std::move(s));
    return &ins_it->second;
}

Session* SessionTable::find(const studio::content::net::Endpoint& peer)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_peer_.find(peer);
    return it == by_peer_.end() ? nullptr : &it->second;
}

void SessionTable::drop(const studio::content::net::Endpoint& peer)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_peer_.find(peer);
    if (it == by_peer_.end()) return;
    free_slot_locked(it->second.player_slot);
    by_peer_.erase(it);
}

std::size_t SessionTable::reap(std::uint64_t now_ms,
                               std::uint64_t timeout_ms,
                               std::vector<Session>* out_dropped)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t dropped = 0;
    for (auto it = by_peer_.begin(); it != by_peer_.end(); ) {
        if (now_ms - it->second.last_seen_ms > timeout_ms) {
            if (out_dropped) out_dropped->push_back(it->second);
            free_slot_locked(it->second.player_slot);
            it = by_peer_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

void SessionTable::touch(const studio::content::net::Endpoint& peer,
                         std::uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_peer_.find(peer);
    if (it != by_peer_.end()) it->second.last_seen_ms = now_ms;
}

std::size_t SessionTable::size() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return by_peer_.size();
}

std::vector<Session*> SessionTable::active_sessions()
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Session*> out;
    out.reserve(by_peer_.size());
    for (auto& kv : by_peer_) out.push_back(&kv.second);
    return out;
}

} // namespace dts_viewer
