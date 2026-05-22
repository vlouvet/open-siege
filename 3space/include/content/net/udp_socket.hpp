#ifndef LIB3SPACE_NET_UDP_SOCKET_HPP
#define LIB3SPACE_NET_UDP_SOCKET_HPP

// Track 20 spec 02 — BSD UDP socket wrapper for the Tribes VC layer.
//
// Minimal portable shape: bind/sendto/recvfrom with a peer address that
// can round-trip to host:port strings. Non-blocking by default so the VC
// can poll once per tick. POSIX-only for now (macOS/Linux); Windows
// (winsock2) port lives in Track 24.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace studio::content::net
{

struct Endpoint
{
    std::string host;       // dotted-quad or hostname
    std::uint16_t port = 0;

    bool operator==(const Endpoint& o) const noexcept {
        return port == o.port && host == o.host;
    }
};

class UdpSocket
{
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;

    // Bind to a local port (0 = ephemeral). Returns false on error;
    // last_error() carries strerror.
    bool bind(std::uint16_t local_port);

    // The actual bound port (useful when bind(0) chose an ephemeral one).
    std::uint16_t local_port() const { return local_port_; }

    // Send `data` (size bytes) to `peer`. Returns false on error.
    bool send_to(const Endpoint& peer, const void* data, std::size_t size);

    // Try to recv one datagram. Returns true if one was read; fills
    // `out_data` (resized to actual size) and `out_peer`. Returns false
    // (with last_error() empty) when no packet was pending.
    bool try_recv(std::vector<std::uint8_t>& out_data, Endpoint& out_peer);

    // Lifecycle
    bool is_open() const { return fd_ >= 0; }
    void close();

    const std::string& last_error() const { return last_error_; }

private:
    int fd_ = -1;
    std::uint16_t local_port_ = 0;
    std::string last_error_;
};

// Helper: resolve "host:port" or "host" + separate port to a sockaddr_in.
// Returns an Endpoint that send_to() can use.
std::optional<Endpoint> resolve_endpoint(const std::string& host, std::uint16_t port);

} // namespace studio::content::net

#endif // LIB3SPACE_NET_UDP_SOCKET_HPP
