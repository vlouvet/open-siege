// Track 21 spec 01 — MasterClient tests.
//
// Spins up the Python master in-process via a subprocess fork+exec,
// fires heartbeat + fetch, asserts the round-trip. Skips with WARN
// when python3 is unavailable so the suite stays green elsewhere.

#include <catch2/catch.hpp>

#include "content/net/master_client.hpp"

#if !defined(_WIN32)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

using namespace studio::content::net;

#if !defined(_WIN32)
namespace {

struct MasterServerProc
{
    pid_t pid = -1;
    std::uint16_t port = 0;

    ~MasterServerProc() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

// Pick a probably-free port in 18000..28000 range.
std::uint16_t pick_port()
{
    static std::uint16_t next = 18137;
    return next++;
}

std::unique_ptr<MasterServerProc> spawn_master(std::uint16_t port)
{
    auto out = std::make_unique<MasterServerProc>();
    out->port = port;

    const char* server_py =
        "../../master-server/server.py";
    // Try a couple of paths so the test works regardless of build cwd.
    const char* candidates[] = {
        "../../../master-server/server.py",
        "../../master-server/server.py",
        "../master-server/server.py",
        "master-server/server.py",
    };
    const char* path = nullptr;
    for (const auto* c : candidates) {
        if (::access(c, R_OK) == 0) { path = c; break; }
    }
    if (!path) {
        WARN("master-server/server.py not found relative to cwd; skipping");
        return nullptr;
    }
    char port_buf[16]; std::snprintf(port_buf, sizeof(port_buf), "%u", port);

    pid_t pid = fork();
    if (pid < 0) {
        WARN("fork failed");
        return nullptr;
    }
    if (pid == 0) {
        // Child: redirect stdout/stderr to /dev/null and exec python.
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::execlp("python3", "python3", path, "--port", port_buf,
                 "--bind", "127.0.0.1", nullptr);
        // execlp failed — bail.
        std::_Exit(1);
    }
    out->pid = pid;
    // Wait a moment for the listen socket to come up.
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Probe.
        MasterClient probe;
        std::string url = "http://127.0.0.1:" + std::to_string(port);
        auto resp = probe.fetch_server_list(url);
        if (resp.has_value()) return out;
    }
    WARN("master server did not come up");
    return nullptr;
    (void)server_py;
}

} // anonymous namespace
#endif // !_WIN32

TEST_CASE("MasterClient: heartbeat + fetch round-trip", "[net][master]")
{
#if defined(_WIN32)
    WARN("MasterClient integration test skipped on Windows (no fork/exec)");
    return;
#else
    const std::uint16_t port = pick_port();
    auto srv = spawn_master(port);
    if (!srv) {
        WARN("python3 master server unavailable; skipping integration test");
        return;
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(port);
    MasterClient mc;

    // Empty registry initially.
    auto initial = mc.fetch_server_list(url);
    REQUIRE(initial.has_value());
    REQUIRE(initial->servers.empty());

    // Heartbeat a fake server.
    ServerInfo info;
    info.address = "10.0.0.5";
    info.port = 28000;
    info.name = "Test Outpost";
    info.players = 8;
    info.max_players = 32;
    info.map = "Recess";
    info.game_type = "CTF";
    REQUIRE(mc.heartbeat(url, info));

    auto resp = mc.fetch_server_list(url);
    REQUIRE(resp.has_value());
    REQUIRE(resp->servers.size() == 1);
    REQUIRE(resp->servers[0].address == "10.0.0.5");
    REQUIRE(resp->servers[0].port == 28000);
    REQUIRE(resp->servers[0].name == "Test Outpost");
    REQUIRE(resp->servers[0].players == 8);
    REQUIRE(resp->servers[0].map == "Recess");
#endif // !_WIN32
}

TEST_CASE("MasterClient: deregister removes entry", "[net][master]")
{
#if defined(_WIN32)
    WARN("MasterClient integration test skipped on Windows (no fork/exec)");
    return;
#else
    const std::uint16_t port = pick_port();
    auto srv = spawn_master(port);
    if (!srv) return;

    const std::string url = "http://127.0.0.1:" + std::to_string(port);
    MasterClient mc;

    ServerInfo info;
    info.address = "10.0.0.5";
    info.port = 28000;
    info.name = "Doomed Server";
    REQUIRE(mc.heartbeat(url, info));
    REQUIRE(mc.fetch_server_list(url)->servers.size() == 1);

    REQUIRE(mc.deregister(url, info));
    auto resp = mc.fetch_server_list(url);
    REQUIRE(resp.has_value());
    REQUIRE(resp->servers.empty());
#endif // !_WIN32
}
