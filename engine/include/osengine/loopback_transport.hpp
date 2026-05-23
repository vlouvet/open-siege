#ifndef OSENGINE_LOOPBACK_TRANSPORT_HPP
#define OSENGINE_LOOPBACK_TRANSPORT_HPP

// In-process message queue pair for the listen-server (spec 26/07).
//
// Two LoopbackEndpoints share a pair of std::deque<std::vector<uint8_t>>
// — one direction each — so a single-process SP session can run an
// authoritative server thread and a render-thread client without ever
// touching UDP. Threads communicate only through queue push/pop +
// std::mutex.

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace dts_viewer
{

struct LoopbackChannel
{
    std::mutex                            mu;
    std::deque<std::vector<std::uint8_t>> queue;
};

class LoopbackEndpoint
{
public:
    LoopbackEndpoint(std::shared_ptr<LoopbackChannel> tx,
                     std::shared_ptr<LoopbackChannel> rx)
        : tx_(std::move(tx)), rx_(std::move(rx)) {}

    void send(std::vector<std::uint8_t> bytes)
    {
        std::lock_guard<std::mutex> lk(tx_->mu);
        tx_->queue.emplace_back(std::move(bytes));
    }

    std::optional<std::vector<std::uint8_t>> recv()
    {
        std::lock_guard<std::mutex> lk(rx_->mu);
        if (rx_->queue.empty()) return std::nullopt;
        auto out = std::move(rx_->queue.front());
        rx_->queue.pop_front();
        return out;
    }

    std::size_t pending() const
    {
        std::lock_guard<std::mutex> lk(rx_->mu);
        return rx_->queue.size();
    }

private:
    std::shared_ptr<LoopbackChannel> tx_;
    std::shared_ptr<LoopbackChannel> rx_;
};

// Create a paired (server_endpoint, client_endpoint). Each endpoint
// writes into one channel and reads from the other.
struct LoopbackPair
{
    LoopbackEndpoint server;
    LoopbackEndpoint client;
};

inline LoopbackPair make_loopback_pair()
{
    auto s_to_c = std::make_shared<LoopbackChannel>();
    auto c_to_s = std::make_shared<LoopbackChannel>();
    return LoopbackPair{
        LoopbackEndpoint{s_to_c, c_to_s},   // server: tx=s->c, rx=c->s
        LoopbackEndpoint{c_to_s, s_to_c},   // client: tx=c->s, rx=s->c
    };
}

} // namespace dts_viewer

#endif // OSENGINE_LOOPBACK_TRANSPORT_HPP
