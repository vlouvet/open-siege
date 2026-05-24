#ifndef DTS_VIEWER_NET_CLIENT_HPP
#define DTS_VIEWER_NET_CLIENT_HPP

// Spec 20/15 — net-client integration for dts-viewer.
//
// Wraps a background-thread loop that either:
//   (a) replays a captured ghost-stream JSON (offline path — used for the
//       v1 demo until the live handshake stabilises in spec 23), or
//   (b) drives the Groove/vanilla template-paste handshake against a
//       live Tribes server and consumes its ghost stream.
//
// In both modes the loop maintains a `net20::GhostRegistry` and the
// render thread reads a thread-safe snapshot via `snapshot_registry()`.

#include <osengine/ghost_types.hpp>
#include <osengine/movecommand.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Spec 29/02b — server_info packet shape. Sent once by the server
// right after AcceptConnect; the client surfaces the decoded fields
// via NetClient::server_info().
namespace dts_viewer {

struct ServerInfo {
    std::string   mission_short_name;
    std::uint16_t player_slot = 0;
    std::uint8_t  team_raw    = 0;     // matches dts_viewer::Team
    std::uint32_t server_tick = 0;
};

// VC packet-type word for server_info. Chosen above the standard
// VC type-word range (0..31) so it doesn't collide with kDataPacket,
// kPureAck, kPing, kAcceptConnect, etc. See reliable_acks.hpp pkt_type.
constexpr std::uint8_t kServerInfoTypeWord = 0x12;

// Wire layout (after the variable-length VC header):
//   offset 0..3  : 'S','I','N','F' magic
//   offset 4     : version (currently 0x01)
//   offset 5..6  : player_slot (u16 LE)
//   offset 7     : team_raw
//   offset 8..11 : server_tick (u32 LE)
//   offset 12    : mission_short_name length (u8, 0..64)
//   offset 13..  : mission_short_name bytes (utf-8, no terminator)
std::vector<std::uint8_t> encode_server_info(const ServerInfo& info,
                                             std::uint16_t vc_send_seq,
                                             bool vc_parity);

// Best-effort decoder. Returns nullopt if the packet doesn't look
// like a server_info (wrong magic, wrong version, length out of range).
std::optional<ServerInfo> decode_server_info(const std::uint8_t* data,
                                             std::size_t size);

int server_info_roundtrip_selftest();

}  // namespace dts_viewer

namespace dts_viewer {

class NetClient {
public:
    NetClient();
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Start the offline replay path: read `path` (the JSON shape produced
    // by net-test-client --ghost-dump or by scripts/tribes_capture_proxy.py)
    // and feed every s->c packet through `net20::parse_typed_packet`.
    // Returns false if the file cannot be opened.
    bool start_replay(const std::string& path);

    // Start the live-server path: spawn the IO thread, run the
    // template-paste handshake against (host, port) and consume ghost
    // packets. v1 wraps the same code path as net-test-client's
    // --template-paste --decode-ghosts --ack --send-ready. Returns
    // false on local-bind failure; remote errors are reported via
    // `last_error()`.
    bool start_live(const std::string& host, std::uint16_t port,
                    bool use_groove);

    // Stop the IO thread (if running) and join. Safe to call multiple
    // times; the destructor invokes this as well.
    void stop();

    // Copy the current registry snapshot. Returned by value so the
    // render path can iterate without holding the lock.
    net20::GhostRegistry snapshot_registry() const;

    // Spec 29/03 — push the local input state. The IO thread reads
    // this when assembling each ~33 Hz movecommand. yaw_delta and
    // pitch_delta are CONSUMED by the next emit_move and reset to 0;
    // calls in between accumulate into the same window so caller can
    // sample mouse motion every frame without losing pixels.
    void set_input(const net20::MoveInput& input);

    // Spec 29/02b — server-info accessor. Populated when the IO thread
    // receives a packet with type_word == kServerInfoTypeWord. Returns
    // nullopt until the server's first server_info reaches us.
    std::optional<ServerInfo> server_info() const;

    // Spec 29/08 — queue an outgoing chat line. scope: 0=Global, 1=Team.
    // v1 stub: logs to stderr. Will encode + send via the event
    // sub-stream once spec 28/10b lands the wire format.
    void send_chat(const std::string& text, std::uint8_t scope);

    // Diagnostics.
    bool   running() const { return running_.load(std::memory_order_relaxed); }
    int    packets_seen() const { return packets_seen_.load(std::memory_order_relaxed); }
    int    typed_records() const { return typed_records_.load(std::memory_order_relaxed); }
    std::string last_error() const;

private:
    void replay_thread_main(std::string path);
    void live_thread_main(std::string host, std::uint16_t port, bool use_groove);

    void apply_registry(const net20::GhostRegistry& new_reg);
    void set_last_error(const std::string& s);

    mutable std::mutex     mu_;
    net20::GhostRegistry   registry_;
    std::string            last_error_;
    net20::MoveInput       pending_input_;       // spec 29/03
    std::optional<ServerInfo> server_info_;       // spec 29/02b

    std::thread            io_thread_;
    std::atomic<bool>      running_       { false };
    std::atomic<bool>      stop_requested_ { false };
    std::atomic<int>       packets_seen_  { 0 };
    std::atomic<int>       typed_records_ { 0 };
};

}  // namespace dts_viewer

#endif  // DTS_VIEWER_NET_CLIENT_HPP
