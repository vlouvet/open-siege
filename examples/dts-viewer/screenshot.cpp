#include "screenshot.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;

namespace dts_viewer
{

namespace
{

fs::path default_screenshot_dir()
{
    if (const char* env = std::getenv("TRIBES_SCREENSHOT_DIR")) {
        return fs::path(env);
    }
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / "Pictures" / "tribes-viewer";
    }
    return fs::path("./screenshots");
}

std::string timestamp_now()
{
    auto now    = std::chrono::system_clock::now();
    auto epoch  = std::chrono::system_clock::to_time_t(now);
    auto subsec = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &epoch);
#else
    localtime_r(&epoch, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d%02d%02d_%03lld",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(subsec));
    return buf;
}

} // anonymous namespace

bool write_ppm(
    const fs::path& path,
    int width,
    int height,
    const std::uint8_t* rgba)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << width << " " << height << "\n255\n";
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = rgba + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        f.write(reinterpret_cast<const char*>(row.data()),
                static_cast<std::streamsize>(row.size()));
    }
    return f.good();
}

fs::path capture_screenshot(
    int viewport_width,
    int viewport_height,
    const fs::path& out_dir_arg,
    const std::string& mission_name)
{
    if (viewport_width <= 0 || viewport_height <= 0) return {};

    fs::path out_dir = out_dir_arg.empty() ? default_screenshot_dir() : out_dir_arg;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr,
            "screenshot: cannot create %s: %s\n",
            out_dir.string().c_str(), ec.message().c_str());
        return {};
    }

    const std::string stamp = timestamp_now();
    const std::string clean_mission = mission_name.empty() ? "viewer" : mission_name;
    fs::path path = out_dir / (stamp + "_" + clean_mission + ".ppm");

    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(viewport_width) * viewport_height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, viewport_width, viewport_height,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // GL is bottom-up; PPM/file conventions are top-down — flip.
    std::vector<std::uint8_t> flipped(rgba.size());
    const std::size_t row_bytes =
        static_cast<std::size_t>(viewport_width) * 4;
    for (int y = 0; y < viewport_height; ++y) {
        std::memcpy(
            &flipped[(viewport_height - 1 - y) * row_bytes],
            &rgba[static_cast<std::size_t>(y) * row_bytes],
            row_bytes);
    }

    if (!write_ppm(path, viewport_width, viewport_height, flipped.data())) {
        std::fprintf(stderr, "screenshot: write failed: %s\n",
            path.string().c_str());
        return {};
    }

    std::fprintf(stderr, "screenshot: %s\n", path.string().c_str());
    return fs::absolute(path);
}

} // namespace dts_viewer
