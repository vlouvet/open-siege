#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <variant>

#include "resources/darkstar_volume.hpp"
#include "content/renderable_shape.hpp"
#include "content/dts/renderable_shape_factory.hpp"

namespace fs = std::filesystem;
namespace dv = studio::resources::vol::darkstar;
namespace sr = studio::resources;
namespace sc = studio::content;

struct counting_renderer : sc::shape_renderer
{
    std::size_t nodes = 0, objects = 0, faces = 0, vertices = 0, tex_vertices = 0;
    void update_node(std::optional<std::string_view>, std::string_view) override { ++nodes; }
    void update_object(std::optional<std::string_view>, std::string_view) override { ++objects; }
    void new_face(std::size_t) override { ++faces; }
    void end_face() override {}
    void emit_vertex(const sc::vector3f&) override { ++vertices; }
    void emit_texture_vertex(const sc::texture_vertex&) override { ++tex_vertices; }
};

struct dts_summary
{
    bool parsed = false;
    std::string error;
    std::size_t details = 0, materials = 0, sequences = 0;
    std::size_t nodes = 0, objects = 0, faces = 0, vertices = 0;
};

static dts_summary inspect_dts(std::istream& stream)
{
    dts_summary out;
    try
    {
        auto shape = sc::dts::make_shape(stream);
        if (!shape) { out.error = "make_shape returned null"; return out; }

        auto details_v = shape->get_detail_levels();
        auto materials_v = shape->get_materials();
        out.details = details_v.size();
        out.materials = materials_v.size();

        std::vector<std::size_t> detail_indexes;
        for (std::size_t i = 0; i < details_v.size(); ++i) detail_indexes.push_back(i);

        auto sequences = shape->get_sequences(detail_indexes);
        out.sequences = sequences.size();

        counting_renderer geom;
        shape->render_shape(geom, detail_indexes, sequences);
        out.nodes = geom.nodes;
        out.objects = geom.objects;
        out.faces = geom.faces;
        out.vertices = geom.vertices;
        out.parsed = true;
    }
    catch (const std::exception& e) { out.error = e.what(); }
    catch (...) { out.error = "unknown exception"; }
    return out;
}

static void cmd_list(const fs::path& vol_path)
{
    std::ifstream in(vol_path, std::ios::binary);
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) {
        std::fprintf(stderr, "not a darkstar VOL: %s\n", vol_path.c_str());
        std::exit(3);
    }
    in.clear(); in.seekg(0);
    auto all = sr::get_all_content(vol_path, in, plugin);
    std::size_t n_files = 0;
    for (auto& entry : all) {
        if (auto* f = std::get_if<sr::file_info>(&entry)) {
            ++n_files;
            std::cout << "[FILE] " << f->filename.string()
                      << "  (" << f->size << " bytes)\n";
        }
    }
    std::cout << "\n--- " << n_files << " files ---\n";
}

static void cmd_dts(const fs::path& vol_path)
{
    std::ifstream in(vol_path, std::ios::binary);
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) {
        std::fprintf(stderr, "not a darkstar VOL: %s\n", vol_path.c_str());
        std::exit(3);
    }
    in.clear(); in.seekg(0);
    auto all = sr::get_all_content(vol_path, in, plugin);

    std::size_t total_dts = 0, ok = 0, fail = 0;
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        auto name = f->filename.string();
        auto lower = name;
        for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
        if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".dts") continue;
        ++total_dts;

        std::stringstream buf;
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, buf);

        auto s = inspect_dts(buf);
        if (s.parsed) {
            ++ok;
            std::printf("  OK  %-40s details=%zu  materials=%zu  seq=%zu  nodes=%zu  objs=%zu  faces=%zu  verts=%zu\n",
                name.c_str(), s.details, s.materials, s.sequences,
                s.nodes, s.objects, s.faces, s.vertices);
        } else {
            ++fail;
            std::printf("  FAIL %-40s %s\n", name.c_str(), s.error.c_str());
        }
    }
    std::cout << "\n--- DTS: " << ok << "/" << total_dts << " parsed cleanly, " << fail << " failed ---\n";
}

static void cmd_extract(const fs::path& vol_path, const std::string& name_substr, const fs::path& out_dir)
{
    std::ifstream in(vol_path, std::ios::binary);
    dv::vol_file_archive plugin;
    if (!plugin.stream_is_supported(in)) {
        std::fprintf(stderr, "not a darkstar VOL: %s\n", vol_path.c_str());
        std::exit(3);
    }
    fs::create_directories(out_dir);
    in.clear(); in.seekg(0);
    auto all = sr::get_all_content(vol_path, in, plugin);
    std::size_t count = 0;
    for (auto& entry : all) {
        auto* f = std::get_if<sr::file_info>(&entry);
        if (!f) continue;
        auto name = f->filename.string();
        auto lower = name;
        for (auto& c : lower) c = std::tolower((unsigned char)c);
        if (!name_substr.empty() && lower.find(name_substr) == std::string::npos) continue;

        fs::path dst = out_dir / f->filename.filename();
        std::ofstream o(dst, std::ios::binary);
        in.clear(); in.seekg(0);
        plugin.extract_file_contents(in, *f, o);
        ++count;
        std::printf("  %s -> %s (%zu bytes)\n", name.c_str(), dst.c_str(), f->size);
    }
    std::printf("--- extracted %zu files ---\n", count);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage:\n"
            "  %s <vol>                                  # list files\n"
            "  %s --dts <vol>                            # parse every .dts inside\n"
            "  %s --extract <vol> <substr> <out-dir>     # extract files whose name contains substr\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    if (std::string(argv[1]) == "--dts") {
        if (argc < 3) { std::fprintf(stderr, "missing vol path\n"); return 1; }
        cmd_dts(argv[2]);
    } else if (std::string(argv[1]) == "--extract") {
        if (argc < 5) { std::fprintf(stderr, "usage: --extract <vol> <substr> <out-dir>\n"); return 1; }
        std::string s = argv[3];
        for (auto& c : s) c = std::tolower((unsigned char)c);
        cmd_extract(argv[2], s, argv[4]);
    } else {
        cmd_list(argv[1]);
    }
    return 0;
}
