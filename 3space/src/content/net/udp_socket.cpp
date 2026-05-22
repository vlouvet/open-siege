#include "content/net/udp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace studio::content::net
{

namespace
{

void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool fill_sockaddr(const Endpoint& ep, sockaddr_in& out_sa)
{
    std::memset(&out_sa, 0, sizeof(out_sa));
    out_sa.sin_family = AF_INET;
    out_sa.sin_port = htons(ep.port);
    if (inet_pton(AF_INET, ep.host.c_str(), &out_sa.sin_addr) == 1) {
        return true;
    }
    // Fall back to DNS resolve.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(ep.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }
    auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out_sa.sin_addr = sa->sin_addr;
    freeaddrinfo(res);
    return true;
}

Endpoint endpoint_from_sockaddr(const sockaddr_in& sa)
{
    Endpoint ep;
    char buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf))) {
        ep.host = buf;
    }
    ep.port = ntohs(sa.sin_port);
    return ep;
}

} // anonymous namespace

UdpSocket::UdpSocket() = default;

UdpSocket::~UdpSocket()
{
    close();
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept
    : fd_(o.fd_), local_port_(o.local_port_),
      last_error_(std::move(o.last_error_))
{
    o.fd_ = -1;
    o.local_port_ = 0;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept
{
    if (this != &o) {
        close();
        fd_ = o.fd_;
        local_port_ = o.local_port_;
        last_error_ = std::move(o.last_error_);
        o.fd_ = -1;
        o.local_port_ = 0;
    }
    return *this;
}

void UdpSocket::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    local_port_ = 0;
}

bool UdpSocket::bind(std::uint16_t local_port)
{
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        last_error_ = std::strerror(errno);
        return false;
    }
    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(local_port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        last_error_ = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Read back the actual port (in case caller passed 0).
    socklen_t sl = sizeof(sa);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &sl) == 0) {
        local_port_ = ntohs(sa.sin_port);
    } else {
        local_port_ = local_port;
    }
    set_nonblocking(fd_);
    return true;
}

bool UdpSocket::send_to(const Endpoint& peer, const void* data, std::size_t size)
{
    if (fd_ < 0) {
        last_error_ = "socket not bound";
        return false;
    }
    sockaddr_in sa{};
    if (!fill_sockaddr(peer, sa)) {
        last_error_ = "address resolution failed";
        return false;
    }
    ssize_t n = ::sendto(fd_, data, size, 0,
                         reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (n < 0) {
        last_error_ = std::strerror(errno);
        return false;
    }
    last_error_.clear();
    return true;
}

bool UdpSocket::try_recv(std::vector<std::uint8_t>& out_data, Endpoint& out_peer)
{
    if (fd_ < 0) return false;
    std::uint8_t buf[2048];
    sockaddr_in sa{};
    socklen_t sl = sizeof(sa);
    ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&sa), &sl);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            last_error_.clear();
            return false;
        }
        last_error_ = std::strerror(errno);
        return false;
    }
    out_data.assign(buf, buf + n);
    out_peer = endpoint_from_sockaddr(sa);
    return true;
}

std::optional<Endpoint> resolve_endpoint(const std::string& host,
                                         std::uint16_t port)
{
    Endpoint ep;
    ep.host = host;
    ep.port = port;
    sockaddr_in probe{};
    if (!fill_sockaddr(ep, probe)) return std::nullopt;
    // Normalise to dotted-quad so future comparisons are stable.
    char buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &probe.sin_addr, buf, sizeof(buf))) {
        ep.host = buf;
    }
    return ep;
}

} // namespace studio::content::net
