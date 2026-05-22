#ifndef LIB3SPACE_NET_MASTER_CLIENT_HPP
#define LIB3SPACE_NET_MASTER_CLIENT_HPP

// Track 21 spec 01 — HTTP master server client.
//
// Talks to the modern HTTP master at `master-server/server.py`. Two
// operations:
//
//   * fetch_server_list("http://master:8080")  -> ServerInfo[]
//   * heartbeat("http://master:8080", info)    -> bool
//
// HTTP/1.1 over plain TCP, no TLS in v1 (private deployments only;
// public deployments belong behind a reverse proxy that adds TLS).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace studio::content::net
{

struct ServerInfo
{
    std::string address;             // IPv4 dotted quad
    std::uint16_t port = 0;
    std::string name = "Tribes Server";
    int players = 0;
    int max_players = 32;
    std::string map;
    std::string game_type;
    bool dedicated = true;
    bool password = false;
    std::string version = "1.41";
    std::string mission_type;
    int ping_ms = 0;
};

struct MasterMetadata
{
    std::string name = "open-siege master";
    std::string motd;
    int version = 1;
};

struct ServerList
{
    MasterMetadata metadata;
    std::vector<ServerInfo> servers;
};

class MasterClient
{
public:
    // Returns nullopt on transport / parse error; sets last_error().
    std::optional<ServerList> fetch_server_list(const std::string& master_url);

    // Sends a heartbeat. `info.address` may be empty to let the master
    // record the source IP automatically.
    bool heartbeat(const std::string& master_url, const ServerInfo& info);

    // Deregister explicitly (call on clean server shutdown).
    bool deregister(const std::string& master_url,
                    const ServerInfo& info);

    const std::string& last_error() const { return last_error_; }

private:
    std::string last_error_;
};

} // namespace studio::content::net

#endif // LIB3SPACE_NET_MASTER_CLIENT_HPP
