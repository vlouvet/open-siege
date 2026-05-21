#ifndef DTS_VIEWER_HEIGHT_SAMPLER_HPP
#define DTS_VIEWER_HEIGHT_SAMPLER_HPP

// Bilinear terrain heightmap sampler.
//
// Wraps a contiguous (size+1) * (size+1) float array (the same buffer
// build_terrain_mesh consumes) and produces a smoothly varying height
// at any world (x, z).  Out-of-range queries are clamped to the edge.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace dts_viewer
{

struct HeightSampler
{
    const float* heights      = nullptr;  // (size_plus_one * size_plus_one) floats
    int          size_plus_one = 0;       // typically 257
    float        metres_per_quad = 8.0f;
    // World-space origin of the terrain block. Mission mode passes
    // -half_size so the rendered tile is centered at origin; the
    // sampler subtracts this before computing the quad index.
    float        world_origin_x = 0.0f;
    float        world_origin_z = 0.0f;

    bool valid() const { return heights != nullptr && size_plus_one > 1; }

    float sample(float world_x, float world_z) const
    {
        if (!valid()) return 0.0f;

        const float qx_f = (world_x - world_origin_x) / metres_per_quad;
        const float qz_f = (world_z - world_origin_z) / metres_per_quad;

        const int max_q = size_plus_one - 1;     // last valid vertex index
        int qx = static_cast<int>(std::floor(qx_f));
        int qz = static_cast<int>(std::floor(qz_f));

        qx = std::clamp(qx, 0, max_q - 1);
        qz = std::clamp(qz, 0, max_q - 1);

        float fx = std::clamp(qx_f - static_cast<float>(qx), 0.0f, 1.0f);
        float fz = std::clamp(qz_f - static_cast<float>(qz), 0.0f, 1.0f);

        auto H = [&](int x, int z) -> float {
            return heights[static_cast<std::size_t>(z) * size_plus_one + x];
        };

        const float h00 = H(qx,     qz    );
        const float h10 = H(qx + 1, qz    );
        const float h01 = H(qx,     qz + 1);
        const float h11 = H(qx + 1, qz + 1);

        const float h0 = h00 + (h10 - h00) * fx;
        const float h1 = h01 + (h11 - h01) * fx;
        return h0 + (h1 - h0) * fz;
    }

    // Returns the surface normal (Y-up) at world (x, z) by central
    // differences of neighbouring samples.  Default (0,1,0) when invalid.
    inline void sample_normal(float world_x, float world_z, float out[3]) const
    {
        if (!valid()) { out[0] = 0; out[1] = 1; out[2] = 0; return; }
        const float h = metres_per_quad;
        const float hl = sample(world_x - h, world_z);
        const float hr = sample(world_x + h, world_z);
        const float hd = sample(world_x, world_z - h);
        const float hu = sample(world_x, world_z + h);
        // n = normalize(cross(d/dz tangent, d/dx tangent))
        //   = normalize(-dh/dx, 2h, -dh/dz)
        float nx = -(hr - hl);
        float ny =  2.0f * h;
        float nz = -(hu - hd);
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-6f) { out[0] = 0; out[1] = 1; out[2] = 0; return; }
        out[0] = nx / len;
        out[1] = ny / len;
        out[2] = nz / len;
    }
};

} // namespace dts_viewer

#endif // DTS_VIEWER_HEIGHT_SAMPLER_HPP
