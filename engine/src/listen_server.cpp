#include <osengine/listen_server.hpp>

#include <algorithm>
#include <chrono>

namespace dts_viewer
{

ListenServer::ListenServer(ListenServerConfig cfg)
    : cfg_(cfg), pair_(make_loopback_pair())
{}

ListenServer::~ListenServer()
{
    stop();
}

void ListenServer::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    quit_.store(false);
    thread_ = std::thread([this] { run(); });
}

void ListenServer::stop()
{
    quit_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void ListenServer::run()
{
    using namespace std::chrono;
    const auto period = milliseconds(1000 / std::max(1, cfg_.tick_hz));
    while (!quit_.load()) {
        const auto t0 = steady_clock::now();

        // Drain any inbound client packets. v1: discard. Future: feed to
        // engine net stack (reliable_acks etc).
        while (auto pkt = pair_.server.recv()) { (void)pkt; }

        const auto next = ticks_.fetch_add(1) + 1;
        if (cfg_.max_ticks && next >= cfg_.max_ticks) break;

        std::this_thread::sleep_for(period - (steady_clock::now() - t0));
    }
    running_.store(false);
}

} // namespace dts_viewer
