#include "content/net/master_client.hpp"

// gcc 15 on Linux: std::strerror / std::snprintf used inside the platform
// #ifdef helpers below need <cstring> / <cstdio> visible before the inline
// definitions. apple-clang was transitively pulling them in.
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   static void ensure_winsock() {
       static const int _ = []{ WSADATA w; return WSAStartup(MAKEWORD(2,2), &w); }();
       (void)_;
   }
   static const char* sock_strerror() {
       static char buf[32];
       std::snprintf(buf, sizeof(buf), "wsa:%d", ::WSAGetLastError());
       return buf;
   }
   static inline void sock_close_fd(int fd) { ::closesocket(static_cast<SOCKET>(fd)); }
#  ifndef EINTR
#    define EINTR WSAEINTR
#  endif
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
   static void ensure_winsock() {}
   static const char* sock_strerror() { return std::strerror(errno); }
   static inline void sock_close_fd(int fd) { ::close(fd); }
#endif

#include <sstream>

#include <nlohmann/json.hpp>

namespace studio::content::net
{

namespace
{
using json = nlohmann::json;

struct ParsedUrl
{
    std::string host;
    std::uint16_t port = 80;
    std::string path = "/";
};

bool parse_url(const std::string& url, ParsedUrl& out, std::string& err)
{
    // Accept http://host[:port]/path. No HTTPS yet.
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        err = "url must start with http://";
        return false;
    }
    std::string rest = url.substr(scheme.size());
    const auto slash = rest.find('/');
    std::string authority;
    if (slash == std::string::npos) {
        authority = rest;
        out.path = "/";
    } else {
        authority = rest.substr(0, slash);
        out.path = rest.substr(slash);
    }
    const auto colon = authority.find(':');
    if (colon == std::string::npos) {
        out.host = authority;
        out.port = 80;
    } else {
        out.host = authority.substr(0, colon);
        try {
            out.port = static_cast<std::uint16_t>(std::stoi(authority.substr(colon + 1)));
        } catch (...) {
            err = "bad port in url";
            return false;
        }
    }
    return true;
}

bool tcp_connect(const std::string& host, std::uint16_t port,
                 int& out_fd, std::string& err)
{
    ensure_winsock();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char port_s[16]; std::snprintf(port_s, sizeof(port_s), "%u", port);
    if (getaddrinfo(host.c_str(), port_s, &hints, &res) != 0 || !res) {
        err = "resolve failed";
        return false;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        err = sock_strerror();
        freeaddrinfo(res);
        return false;
    }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        err = sock_strerror();
        sock_close_fd(fd);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    out_fd = fd;
    return true;
}

bool send_all(int fd, const void* data, std::size_t n, std::string& err)
{
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
        ssize_t r = ::send(fd, p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            err = sock_strerror();
            return false;
        }
        if (r == 0) {
            err = "peer closed early";
            return false;
        }
        p += r; n -= static_cast<std::size_t>(r);
    }
    return true;
}

std::string recv_all(int fd)
{
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
}

bool http_request(const std::string& url,
                  const std::string& method,
                  const std::string& body,
                  std::string& out_response_body,
                  int& out_status,
                  std::string& err)
{
    ParsedUrl pu;
    if (!parse_url(url, pu, err)) return false;
    int fd = -1;
    if (!tcp_connect(pu.host, pu.port, fd, err)) return false;

    std::ostringstream req;
    req << method << " " << pu.path << " HTTP/1.1\r\n";
    req << "Host: " << pu.host;
    if (pu.port != 80) req << ":" << pu.port;
    req << "\r\n";
    req << "User-Agent: open-siege/1\r\n";
    req << "Connection: close\r\n";
    req << "Accept: application/json\r\n";
    if (method == "POST" || method == "PUT") {
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    if (!body.empty()) req << body;
    const std::string req_str = req.str();

    if (!send_all(fd, req_str.data(), req_str.size(), err)) {
        sock_close_fd(fd);
        return false;
    }
    const std::string resp = recv_all(fd);
    sock_close_fd(fd);

    // Parse status line + headers + body.
    const auto crlf2 = resp.find("\r\n\r\n");
    if (crlf2 == std::string::npos) {
        err = "no headers terminator in response";
        return false;
    }
    const std::string head = resp.substr(0, crlf2);
    out_response_body = resp.substr(crlf2 + 4);

    const auto first_crlf = head.find("\r\n");
    const std::string status_line = head.substr(0, first_crlf);
    // "HTTP/1.1 200 OK"
    const auto sp1 = status_line.find(' ');
    const auto sp2 = status_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        err = "bad status line";
        return false;
    }
    try {
        out_status = std::stoi(status_line.substr(sp1 + 1, sp2 - sp1 - 1));
    } catch (...) {
        err = "bad status code";
        return false;
    }
    return true;
}

} // anonymous namespace

std::optional<ServerList> MasterClient::fetch_server_list(const std::string& master_url)
{
    last_error_.clear();
    std::string url = master_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/servers";

    std::string body;
    int status = 0;
    if (!http_request(url, "GET", "", body, status, last_error_)) {
        return std::nullopt;
    }
    if (status != 200) {
        last_error_ = "HTTP " + std::to_string(status);
        return std::nullopt;
    }
    try {
        auto j = nlohmann::json::parse(body);
        ServerList out;
        if (auto it = j.find("metadata"); it != j.end()) {
            out.metadata.name = it->value("name", out.metadata.name);
            out.metadata.motd = it->value("motd", out.metadata.motd);
            out.metadata.version = it->value("version", out.metadata.version);
        }
        if (auto it = j.find("servers"); it != j.end() && it->is_array()) {
            for (const auto& s : *it) {
                ServerInfo si;
                si.address      = s.value("address", "");
                si.port         = static_cast<std::uint16_t>(s.value("port", 28000));
                si.name         = s.value("name", si.name);
                si.players      = s.value("players", 0);
                si.max_players  = s.value("max_players", 32);
                si.map          = s.value("map", "");
                si.game_type    = s.value("game_type", "");
                si.dedicated    = s.value("dedicated", true);
                si.password     = s.value("password", false);
                si.version      = s.value("version", "1.41");
                si.mission_type = s.value("mission_type", "");
                si.ping_ms      = s.value("ping_ms", 0);
                out.servers.push_back(std::move(si));
            }
        }
        return out;
    } catch (const std::exception& e) {
        last_error_ = std::string("json parse: ") + e.what();
        return std::nullopt;
    }
}

bool MasterClient::heartbeat(const std::string& master_url,
                             const ServerInfo& info)
{
    last_error_.clear();
    std::string url = master_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/heartbeat";

    nlohmann::json body;
    if (!info.address.empty()) body["address"] = info.address;
    body["port"]         = info.port;
    body["name"]         = info.name;
    body["players"]      = info.players;
    body["max_players"]  = info.max_players;
    body["map"]          = info.map;
    body["game_type"]    = info.game_type;
    body["dedicated"]    = info.dedicated;
    body["password"]     = info.password;
    body["version"]      = info.version;
    body["mission_type"] = info.mission_type;
    body["ping_ms"]      = info.ping_ms;

    std::string resp;
    int status = 0;
    if (!http_request(url, "POST", body.dump(), resp, status, last_error_)) {
        return false;
    }
    if (status != 200) {
        last_error_ = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

bool MasterClient::deregister(const std::string& master_url,
                              const ServerInfo& info)
{
    last_error_.clear();
    std::string url = master_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/deregister";

    nlohmann::json body;
    if (!info.address.empty()) body["address"] = info.address;
    body["port"] = info.port;

    std::string resp;
    int status = 0;
    if (!http_request(url, "POST", body.dump(), resp, status, last_error_)) {
        return false;
    }
    return status == 200;
}

} // namespace studio::content::net
