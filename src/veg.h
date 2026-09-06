#pragma once

// Vegetation: trees and boulders placed from the terrain's ecology maps and drawn as GPU
// instances with distance LOD, plus grass tufts generated around the camera each frame.
// World unit = kMetersPerUnit metres on all axes.

#include "common.h"
#include "terrain.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct VegVertex {
    glm::vec3 position{};
    glm::vec3 normal{};
    glm::vec3 color{};
};

struct VegInstanceGpu {
    glm::vec4 posScale{};   // xyz world position, w = height in metres
    glm::vec4 rotType{};    // x rotation, y type (0 conifer 1 broadleaf 2 boulder 3 billboard 4 grass), z fade, w unused
};

struct VegMeshRange {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::int32_t vertexOffset = 0;
};

class Vegetation {
public:
    static constexpr int kSub = 32;                  // terrain cells per sub-chunk
    static constexpr float kRMesh = 22.0f;           // full meshes within this (world units)
    static constexpr std::uint32_t kMaxInstances = 420000;

    struct Instance { float x, y, z, scale, rot; std::uint8_t type; };
    struct Range { int start = 0, count = 0; };

    // ---- meshes (built once) -------------------------------------------------------------
    std::vector<VegVertex> vertices;
    std::vector<std::uint32_t> indices;
    VegMeshRange conifer, broadleaf, boulder, billboard, grass, fern, mossRock;

    // ---- placement -------------------------------------------------------------------------
    std::vector<Instance> trees, rocks;
    std::vector<Range> treeRanges, rockRanges;
    int gridN = 0;

    Vegetation() { buildMeshes(); }

    void place(const Terrain& terrain, int seed)
    {
        rng_ = static_cast<std::uint32_t>(seed) * 2654435761u + 97u;
        const int N = kTerrainSize;
        gridN = N / kSub;
        int cells = gridN * gridN;
        const std::vector<float>& H = terrain.heights();
        const std::vector<float>& F = terrain.forestMap();
        float minH = 1e9f, maxH = -1e9f;
        for (float h : H) { minH = std::min(minH, h); maxH = std::max(maxH, h); }
        float span = std::max(maxH - minH, 0.001f);
        float cell = kTerrainWorldSize / static_cast<float>(N - 1);
        float cellMeters = cell * kMetersPerUnit;
        // ~10 m spacing in dense forest
        float maxTreesPerCell = std::max(0.5f, (cellMeters * cellMeters) / (10.0f * 10.0f));

        struct Keyed { int key; Instance inst; };
        std::vector<Keyed> kt, kr;
        kt.reserve(600000);
        kr.reserve(80000);
        for (int z = 0; z < N; ++z) {
            for (int x = 0; x < N; ++x) {
                int i = z * N + x;
                float density = F.empty() ? 0.0f : F[i];
                float hN = (H[i] - minH) / span;
                float slope = terrain.slopeAt(x, z);
                int key = (z / kSub) * gridN + (x / kSub);
                if (density > 0.12f && kt.size() < 1500000) {
                    float expected = density * maxTreesPerCell;
                    int n = static_cast<int>(expected);
                    if (rand01() < expected - n) ++n;
                    for (int k = 0; k < n; ++k) {
                        float fx = x + rand01(), fz = z + rand01();
                        float wx = (fx / (N - 1) - 0.5f) * kTerrainWorldSize;
                        float wz = (fz / (N - 1) - 0.5f) * kTerrainWorldSize;
                        float wy = terrain.surfaceHeightAtWorld(wx, wz);
                        float coniferP = glm::clamp((hN - 0.20f) / 0.20f, 0.0f, 1.0f);
                        std::uint8_t type = (rand01() < coniferP) ? 0 : 1;
                        float height = (type == 0) ? 18.0f + rand01() * 14.0f : 12.0f + rand01() * 9.0f;
                        height *= 0.75f + 0.25f * density;
                        kt.push_back({key, {wx, wy, wz, height, rand01() * 6.2831853f, type}});
                    }
                }
                float scree = glm::clamp((slope - 0.08f) / 0.12f, 0.0f, 1.0f) * glm::clamp((hN - 0.30f) / 0.25f, 0.0f, 1.0f);
                scree *= 1.0f - glm::clamp((slope - 0.30f) / 0.15f, 0.0f, 1.0f);
                if (scree > 0.05f && kr.size() < 300000) {
                    float expected = scree * 0.18f;
                    int n = static_cast<int>(expected);
                    if (rand01() < expected - n) ++n;
                    for (int k = 0; k < n; ++k) {
                        float fx = x + rand01(), fz = z + rand01();
                        float wx = (fx / (N - 1) - 0.5f) * kTerrainWorldSize;
                        float wz = (fz / (N - 1) - 0.5f) * kTerrainWorldSize;
                        float wy = terrain.surfaceHeightAtWorld(wx, wz) - 0.3f / kMetersPerUnit;
                        float size = 1.5f + rand01() * rand01() * 7.0f;
                        kr.push_back({key, {wx, wy, wz, size, rand01() * 6.2831853f, 2}});
                    }
                }
            }
        }
        auto sortInto = [&](std::vector<Keyed>& keyed, std::vector<Instance>& out, std::vector<Range>& ranges) {
            std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
            out.resize(keyed.size());
            ranges.assign(cells, Range{});
            for (std::size_t i = 0; i < keyed.size(); ++i) {
                out[i] = keyed[i].inst;
                Range& r = ranges[keyed[i].key];
                if (r.count == 0) r.start = static_cast<int>(i);
                ++r.count;
            }
        };
        sortInto(kt, trees, treeRanges);
        sortInto(kr, rocks, rockRanges);
    }

    // Gather this frame's instances. Output is grouped: [conifer][broadleaf][boulder][billboard][grass].
    struct DrawGroups { std::uint32_t conifer[2], broadleaf[2], boulder[2], billboard[2], grass[2], fern[2], mossRock[2]; };

    DrawGroups gather(const Terrain& terrain, const glm::vec3& camPos, const glm::vec3& camFwd, VegInstanceGpu* out, std::uint32_t cap) const
    {
        DrawGroups g{};
        std::uint32_t count = 0;
        float subWorld = kSub * kTerrainWorldSize / static_cast<float>(kTerrainSize - 1);
        auto push = [&](const Instance& in, float type, float scaleMul, float fade) {
            if (count >= cap) return;
            out[count++] = {glm::vec4(in.x, in.y, in.z, in.scale * scaleMul), glm::vec4(in.rot, type, fade, static_cast<float>(in.type))};
        };
        // pass 0 conifer, 1 broadleaf, 2 boulder, 3 billboard
        for (int pass = 0; pass < 4; ++pass) {
            std::uint32_t begin = count;
            for (int gz = 0; gz < gridN; ++gz) {
                for (int gx = 0; gx < gridN; ++gx) {
                    float cx = -kTerrainWorldSize * 0.5f + (gx + 0.5f) * subWorld;
                    float cz = -kTerrainWorldSize * 0.5f + (gz + 0.5f) * subWorld;
                    float dist = std::sqrt((cx - camPos.x) * (cx - camPos.x) + (cz - camPos.z) * (cz - camPos.z)) - subWorld * 0.71f;
                    int key = gz * gridN + gx;
                    if (pass < 2) {
                        if (dist > kRMesh) continue;
                        const Range& r = treeRanges[key];
                        for (int i = 0; i < r.count; ++i) {
                            const Instance& in = trees[r.start + i];
                            if (in.type != pass) continue;
                            push(in, static_cast<float>(pass), 1.0f, 1.0f);
                        }
                    } else if (pass == 2) {
                        if (dist > kRMesh * 1.5f) continue;
                        const Range& r = rockRanges[key];
                        for (int i = 0; i < r.count; ++i) push(rocks[r.start + i], 2.0f, 1.0f, 1.0f);
                    } else {
                        if (dist <= kRMesh) continue;
                        int thin = (dist > 110.0f) ? 6 : (dist > 55.0f) ? 3 : (dist > 35.0f) ? 2 : 1;
                        const Range& r = treeRanges[key];
                        for (int i = 0; i < r.count; i += thin) push(trees[r.start + i], 3.0f, 1.2f, 1.0f);
                    }
                }
            }
            std::uint32_t* dst = (pass == 0) ? g.conifer : (pass == 1) ? g.broadleaf : (pass == 2) ? g.boulder : g.billboard;
            dst[0] = begin; dst[1] = count - begin;
        }
        // grass then ferns: hashed per cell around the camera, nothing stored
        for (int fernPassI = 0; fernPassI < 2; ++fernPassI) {
            bool fernPass = fernPassI == 1;
            std::uint32_t begin = count;
            const int N = kTerrainSize;
            float cell = kTerrainWorldSize / static_cast<float>(N - 1);
            float cellMeters = cell * kMetersPerUnit;
            const float radius = 3.2f;   // ~190 m
            int perCell = static_cast<int>(cellMeters * cellMeters / 1.6f);   // one tuft per 1.3x1.3 m
            const std::vector<float>& H = terrain.heights();
            const std::vector<float>& F = terrain.forestMap();
            float minH = 1e9f, maxH = -1e9f;
            for (float h : H) { minH = std::min(minH, h); maxH = std::max(maxH, h); }
            float span = std::max(maxH - minH, 0.001f);
            int cx0 = static_cast<int>(std::floor(((camPos.x - radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cx1 = static_cast<int>(std::ceil(((camPos.x + radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cz0 = static_cast<int>(std::floor(((camPos.z - radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cz1 = static_cast<int>(std::ceil(((camPos.z + radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            for (int cz = std::max(cz0, 0); cz <= std::min(cz1, N - 2); ++cz) {
                for (int cx = std::max(cx0, 0); cx <= std::min(cx1, N - 2); ++cx) {
                    int ci = cz * N + cx;
                    float slope = terrain.slopeAt(cx, cz);
                    if (slope > 0.22f) continue;
                    float hN = (H[ci] - minH) / span;
                    if (hN > 0.72f) continue;
                    float forestHere = F.empty() ? 0.0f : F[ci];
                    // Meadow: grass. Forest floor: fewer grass tufts but ferns take over.
                    float density = (1.0f - glm::clamp((slope - 0.12f) / 0.10f, 0.0f, 1.0f)) * (1.0f - 0.35f * forestHere);
                    int n = static_cast<int>(perCell * density);
                    std::uint32_t r = static_cast<std::uint32_t>(cx * 73856093) ^ static_cast<std::uint32_t>(cz * 19349663) ^ 0x9E3779B9u;
                    for (int k = 0; k < n && count < cap; ++k) {
                        r = r * 1664525u + 1013904223u; float fx = (r >> 8) / 16777216.0f;
                        r = r * 1664525u + 1013904223u; float fz = (r >> 8) / 16777216.0f;
                        r = r * 1664525u + 1013904223u; float fr = (r >> 8) / 16777216.0f;
                        float wx = ((cx + fx) / (N - 1) - 0.5f) * kTerrainWorldSize;
                        float wz = ((cz + fz) / (N - 1) - 0.5f) * kTerrainWorldSize;
                        float ddx = wx - camPos.x, ddz = wz - camPos.z;
                        float d2 = ddx * ddx + ddz * ddz;
                        if (d2 > radius * radius) continue;
                        if (ddx * camFwd.x + ddz * camFwd.z < -0.15f) continue;
                        float fade = 1.0f - glm::clamp((std::sqrt(d2) - radius * 0.55f) / (radius * 0.45f), 0.0f, 1.0f);
                        if (fade <= 0.02f) continue;
                        float wy = terrain.surfaceHeightAtWorld(wx, wz) - 0.02f / kMetersPerUnit;
                        // Every 7th tuft in forest becomes a fern (bigger, darker, drooping).
                        bool fern = forestHere > 0.25f && (k % 7 == 0);
                        if (fern != fernPass) continue;
                        float h = fern ? (0.9f + fr * 0.8f) : (0.45f + fr * 0.55f);
                        Instance in{wx, wy, wz, h * fade, fr * 6.2831853f, static_cast<std::uint8_t>(fern ? 5 : 4)};
                        push(in, fern ? 5.0f : 4.0f, 1.0f, fade);
                    }
                }
            }
            if (!fernPass) { g.grass[0] = begin; g.grass[1] = count - begin; }
            else { g.fern[0] = begin; g.fern[1] = count - begin; }
        }
        // Small mossy rocks scattered on the forest floor and meadows near the camera.
        {
            std::uint32_t begin = count;
            const int N = kTerrainSize;
            const float radius = 2.6f;
            const std::vector<float>& F = terrain.forestMap();
            int cx0 = static_cast<int>(std::floor(((camPos.x - radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cx1 = static_cast<int>(std::ceil(((camPos.x + radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cz0 = static_cast<int>(std::floor(((camPos.z - radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cz1 = static_cast<int>(std::ceil(((camPos.z + radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            for (int cz = std::max(cz0, 0); cz <= std::min(cz1, N - 2); ++cz) {
                for (int cx = std::max(cx0, 0); cx <= std::min(cx1, N - 2); ++cx) {
                    int ci = cz * N + cx;
                    float slope = terrain.slopeAt(cx, cz);
                    if (slope > 0.30f) continue;
                    float forestHere = F.empty() ? 0.0f : F[ci];
                    std::uint32_t r = static_cast<std::uint32_t>(cx * 2654435761u) ^ static_cast<std::uint32_t>(cz * 40503u) ^ 0x51ED27u;
                    r = r * 1664525u + 1013904223u;
                    int n = ((r >> 8) % 100) < static_cast<unsigned>(25 + 45 * forestHere) ? 1 : 0;
                    for (int k = 0; k < n && count < cap; ++k) {
                        r = r * 1664525u + 1013904223u; float fx = (r >> 8) / 16777216.0f;
                        r = r * 1664525u + 1013904223u; float fz = (r >> 8) / 16777216.0f;
                        r = r * 1664525u + 1013904223u; float fr = (r >> 8) / 16777216.0f;
                        float wx = ((cx + fx) / (N - 1) - 0.5f) * kTerrainWorldSize;
                        float wz = ((cz + fz) / (N - 1) - 0.5f) * kTerrainWorldSize;
                        float ddx = wx - camPos.x, ddz = wz - camPos.z;
                        if (ddx * ddx + ddz * ddz > radius * radius) continue;
                        float wy = terrain.surfaceHeightAtWorld(wx, wz) - 0.25f / kMetersPerUnit;
                        Instance in{wx, wy, wz, 0.6f + fr * fr * 2.2f, fr * 6.2831853f, 6};
                        push(in, 6.0f, 1.0f, forestHere);
                    }
                }
            }
            g.mossRock[0] = begin; g.mossRock[1] = count - begin;
        }
        return g;
    }

private:
    std::uint32_t rng_ = 12345u;
    float rand01() { rng_ = rng_ * 1664525u + 1013904223u; return (rng_ >> 8) / 16777216.0f; }

    // ---- mesh builder: unit objects, 1 m = 1/kMetersPerUnit world units ------------------
    int addVertex(const glm::vec3& p, const glm::vec3& n, const glm::vec3& c)
    {
        vertices.push_back({p, n, c});
        return static_cast<int>(vertices.size()) - 1;
    }
    void tri(int a, int b, int c) { indices.push_back(a); indices.push_back(b); indices.push_back(c); }
    glm::vec3 shade(const glm::vec3& c, float lo, float hi) { return c * (lo + rand01() * (hi - lo)); }

    void cone(glm::vec3 base, float radius, float height, int segs, glm::vec3 col, float jitter)
    {
        const float s = 1.0f / kMetersPerUnit;
        int apex = addVertex(base + glm::vec3(0, height * s, 0), {0, 1, 0}, col);
        int first = static_cast<int>(vertices.size());
        for (int i = 0; i < segs; ++i) {
            float a = i / static_cast<float>(segs) * 6.2831853f;
            float r = radius * (1.0f + (rand01() - 0.5f) * jitter);
            glm::vec3 p = base + glm::vec3(std::cos(a) * r * s, (rand01() - 0.5f) * jitter * height * 0.15f * s, std::sin(a) * r * s);
            glm::vec3 n = glm::normalize(glm::vec3(std::cos(a) * height, radius, std::sin(a) * height));
            addVertex(p, n, shade(col, 0.85f, 1.15f));
        }
        for (int i = 0; i < segs; ++i) tri(apex, first + (i + 1) % segs, first + i);
    }
    void cylinder(glm::vec3 base, float r0, float r1, float height, int segs, glm::vec3 col)
    {
        const float s = 1.0f / kMetersPerUnit;
        int first = static_cast<int>(vertices.size());
        for (int i = 0; i < segs; ++i) {
            float a = i / static_cast<float>(segs) * 6.2831853f;
            glm::vec3 n{std::cos(a), 0, std::sin(a)};
            addVertex(base + glm::vec3(n.x * r0 * s, 0, n.z * r0 * s), n, col);
            addVertex(base + glm::vec3(n.x * r1 * s, height * s, n.z * r1 * s), n, col);
        }
        for (int i = 0; i < segs; ++i) {
            int a0 = first + i * 2, a1 = a0 + 1;
            int b0 = first + ((i + 1) % segs) * 2, b1 = b0 + 1;
            tri(a0, b0, a1); tri(a1, b0, b1);
        }
    }
    void blob(glm::vec3 center, float radius, float squashY, int rings, int segs, glm::vec3 col, float jitter)
    {
        const float s = 1.0f / kMetersPerUnit;
        int first = static_cast<int>(vertices.size());
        for (int r = 0; r <= rings; ++r) {
            float phi = r / static_cast<float>(rings) * 3.14159265f;
            for (int q = 0; q <= segs; ++q) {
                float th = q / static_cast<float>(segs) * 6.2831853f;
                glm::vec3 d{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
                float jr = (q == segs || r == 0 || r == rings) ? 1.0f : 1.0f + (rand01() - 0.5f) * jitter;
                glm::vec3 p = center + glm::vec3(d.x * radius * jr * s, d.y * radius * squashY * jr * s, d.z * radius * jr * s);
                addVertex(p, d, shade(col, 0.8f, 1.2f));
            }
        }
        for (int r = 0; r < rings; ++r) {
            for (int q = 0; q < segs; ++q) {
                int a = first + r * (segs + 1) + q;
                int b = a + segs + 1;
                tri(a, a + 1, b); tri(a + 1, b + 1, b);
            }
        }
    }
    VegMeshRange finishRange(std::uint32_t firstIndex)
    {
        VegMeshRange r;
        r.firstIndex = firstIndex;
        r.indexCount = static_cast<std::uint32_t>(indices.size()) - firstIndex;
        r.vertexOffset = 0;
        return r;
    }

    void buildMeshes()
    {
        const float s = 1.0f / kMetersPerUnit;
        rng_ = 777u;
        // Unit trees are 1 m tall; the instance scale is the tree height in metres.
        std::uint32_t f0 = static_cast<std::uint32_t>(indices.size());
        cylinder({0, 0, 0}, 0.022f, 0.012f, 0.35f, 6, {0.31f, 0.23f, 0.16f});
        cone({0, 0.16f * s, 0}, 0.17f, 0.42f, 9, {0.10f, 0.17f, 0.09f}, 0.25f);
        cone({0, 0.42f * s, 0}, 0.13f, 0.36f, 9, {0.10f, 0.17f, 0.09f}, 0.25f);
        cone({0, 0.66f * s, 0}, 0.085f, 0.34f, 8, {0.10f, 0.17f, 0.09f}, 0.25f);
        conifer = finishRange(f0);

        f0 = static_cast<std::uint32_t>(indices.size());
        cylinder({0, 0, 0}, 0.025f, 0.016f, 0.50f, 6, {0.36f, 0.29f, 0.22f});
        blob({0, 0.70f * s, 0}, 0.24f, 0.95f, 5, 8, {0.14f, 0.23f, 0.11f}, 0.40f);
        blob({0.10f * s, 0.58f * s, 0.07f * s}, 0.17f, 0.9f, 4, 7, {0.14f, 0.23f, 0.11f}, 0.40f);
        blob({-0.09f * s, 0.62f * s, -0.06f * s}, 0.15f, 0.9f, 4, 7, {0.14f, 0.23f, 0.11f}, 0.40f);
        broadleaf = finishRange(f0);

        f0 = static_cast<std::uint32_t>(indices.size());
        blob({0, 0.30f * s, 0}, 0.5f, 0.7f, 4, 7, {0.30f, 0.29f, 0.27f}, 0.45f);
        boulder = finishRange(f0);

        f0 = static_cast<std::uint32_t>(indices.size());
        for (int k = 0; k < 2; ++k) {
            float a = k * 3.14159265f * 0.5f;
            glm::vec3 d{std::cos(a), 0, std::sin(a)};
            glm::vec3 n{-std::sin(a), 0, std::cos(a)};
            glm::vec3 col{1.0f, 1.0f, 1.0f};   // coloured per subtype in the vertex shader
            int i0 = addVertex({-d.x * 0.22f * s, 0, -d.z * 0.22f * s}, n, col);
            int i1 = addVertex({d.x * 0.22f * s, 0, d.z * 0.22f * s}, n, col);
            int i2 = addVertex({0, 1.0f * s, 0}, n, col);
            tri(i0, i1, i2);
        }
        billboard = finishRange(f0);

        f0 = static_cast<std::uint32_t>(indices.size());
        for (int k = 0; k < 18; ++k) {
            float a = k / 18.0f * 6.2831853f + rand01() * 0.4f;
            float lean = 0.30f + rand01() * 0.45f;
            float hgt = 0.6f + rand01() * 0.6f;
            float w = 0.035f + rand01() * 0.03f;
            glm::vec3 d{std::cos(a), 0, std::sin(a)};
            glm::vec3 side{-std::sin(a) * w, 0, std::cos(a) * w};
            glm::vec3 root{d.x * 0.10f, 0, d.z * 0.10f};
            glm::vec3 base{0.20f, 0.34f, 0.10f}, tip{0.42f, 0.55f, 0.20f};
            glm::vec3 n{d.x, 0.6f, d.z};
            int i0 = addVertex((root - side) * s, n, base);
            int i1 = addVertex((root + side) * s, n, base);
            int i2 = addVertex(glm::vec3((root.x + d.x * lean) * s, hgt * s, (root.z + d.z * lean) * s), n, tip);
            tri(i0, i1, i2);
        }
        grass = finishRange(f0);

        // Fern: 8 long drooping fronds, each a strip of 4 quads, darker green.
        f0 = static_cast<std::uint32_t>(indices.size());
        for (int k = 0; k < 11; ++k) {
            float a = k / 11.0f * 6.2831853f + rand01() * 0.5f;
            glm::vec3 d{std::cos(a), 0, std::sin(a)};
            glm::vec3 side{-std::sin(a), 0, std::cos(a)};
            glm::vec3 col{0.11f, 0.24f, 0.10f};
            int prevL = -1, prevR = -1;
            for (int seg = 0; seg <= 6; ++seg) {
                float t = seg / 6.0f;
                float serr = (seg % 2 == 0) ? 1.0f : 0.55f;
                float w = (0.03f + 0.09f * std::sin(t * 3.14159f)) * serr * (seg == 6 ? 0.2f : 1.0f);
                float out = t * 0.9f;
                float up = 0.15f + 0.85f * std::sin(t * 2.4f) - t * t * 0.45f;
                glm::vec3 c = glm::vec3(d.x * out, up, d.z * out) * s;
                glm::vec3 n = glm::normalize(glm::vec3(d.x * 0.3f, 1.0f, d.z * 0.3f));
                glm::vec3 shade = col * (0.85f + 0.3f * t);
                int L = addVertex(c - side * (w * s), n, shade);
                int R = addVertex(c + side * (w * s), n, shade);
                if (prevL >= 0) { tri(prevL, prevR, L); tri(prevR, R, L); }
                prevL = L; prevR = R;
            }
        }
        fern = finishRange(f0);

        // Moss rock: small lumpy boulder, greenish grey; instance fade carries forest cover for a mossier tint.
        f0 = static_cast<std::uint32_t>(indices.size());
        blob({0, 0.35f * s, 0}, 0.5f, 0.75f, 4, 7, {0.34f, 0.36f, 0.30f}, 0.5f);
        mossRock = finishRange(f0);
    }
};
