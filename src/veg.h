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
    static constexpr int kVariants = 3;
    VegMeshRange conifer[kVariants], broadleaf[kVariants], coniferMid, broadleafMid;
    VegMeshRange boulder, billboard, grass, fern, mossRock, flower;
    static constexpr float kRNear = 7.0f;            // full tree meshes within ~420 m

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

        // Widened water mask: no trees or boulders in pools or along channels.
        std::vector<float> wet = terrain.waterMap();
        if (wet.size() == static_cast<std::size_t>(N * N)) {
            std::vector<float> tmp(wet.size());
            for (int pass = 0; pass < 2; ++pass) {
                for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
                    float m = 0.0f;
                    for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
                        int sx = std::clamp(x + dx, 0, N - 1), sz = std::clamp(z + dz, 0, N - 1);
                        m = std::max(m, wet[sz * N + sx]);
                    }
                    tmp[z * N + x] = m;
                }
                wet.swap(tmp);
            }
        }
        const std::vector<float>& flowM = terrain.flowMap();
        auto isWet = [&](int i) {
            float w = wet.empty() ? 0.0f : wet[i];
            float fl = flowM.empty() ? 0.0f : flowM[i];
            return w > 0.10f || fl > 0.68f;
        };
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
                if (isWet(i)) continue;
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
    struct DrawGroups {
        std::uint32_t conifer[kVariants][2], broadleaf[kVariants][2], coniferMid[2], broadleafMid[2];
        std::uint32_t boulder[2], billboard[2], grass[2], fern[2], mossRock[2], flower[2];
    };

    DrawGroups gather(const Terrain& terrain, const glm::vec3& camPos, const glm::vec3& camFwd, VegInstanceGpu* out, std::uint32_t cap) const
    {
        DrawGroups g{};
        std::uint32_t count = 0;
        float subWorld = kSub * kTerrainWorldSize / static_cast<float>(kTerrainSize - 1);
        auto push = [&](const Instance& in, float type, float scaleMul, float fade) {
            if (count >= cap) return;
            out[count++] = {glm::vec4(in.x, in.y, in.z, in.scale * scaleMul), glm::vec4(in.rot, type, fade, static_cast<float>(in.type))};
        };
        // Passes: near full meshes per type/variant, mid LOD per type, boulders, billboards.
        auto forChunks = [&](float rMin, float rMax, auto&& fn) {
            for (int gz = 0; gz < gridN; ++gz) {
                for (int gx = 0; gx < gridN; ++gx) {
                    float cx = -kTerrainWorldSize * 0.5f + (gx + 0.5f) * subWorld;
                    float cz = -kTerrainWorldSize * 0.5f + (gz + 0.5f) * subWorld;
                    float dist = std::sqrt((cx - camPos.x) * (cx - camPos.x) + (cz - camPos.z) * (cz - camPos.z)) - subWorld * 0.71f;
                    if (dist > rMax || dist + subWorld * 1.42f < rMin) continue;
                    fn(gz * gridN + gx, dist);
                }
            }
        };
        auto treeDist = [&](const Instance& in) {
            float dx = in.x - camPos.x, dz = in.z - camPos.z;
            return std::sqrt(dx * dx + dz * dz);
        };
        for (int type = 0; type < 2; ++type) {
            for (int v = 0; v < kVariants; ++v) {
                std::uint32_t begin = count;
                forChunks(0.0f, kRNear, [&](int key, float) {
                    const Range& r = treeRanges[key];
                    for (int i = 0; i < r.count; ++i) {
                        const Instance& in = trees[r.start + i];
                        if (in.type != type || (r.start + i) % kVariants != v) continue;
                        if (treeDist(in) > kRNear) continue;
                        push(in, static_cast<float>(type), 1.0f, 1.0f);
                    }
                });
                std::uint32_t* dst = (type == 0) ? g.conifer[v] : g.broadleaf[v];
                dst[0] = begin; dst[1] = count - begin;
            }
            std::uint32_t begin = count;
            forChunks(kRNear, kRMesh, [&](int key, float) {
                const Range& r = treeRanges[key];
                for (int i = 0; i < r.count; ++i) {
                    const Instance& in = trees[r.start + i];
                    if (in.type != type) continue;
                    float d = treeDist(in);
                    if (d <= kRNear || d > kRMesh) continue;
                    push(in, static_cast<float>(type), 1.0f, 1.0f);
                }
            });
            std::uint32_t* dst = (type == 0) ? g.coniferMid : g.broadleafMid;
            dst[0] = begin; dst[1] = count - begin;
        }
        {
            std::uint32_t begin = count;
            forChunks(0.0f, kRMesh * 1.5f, [&](int key, float) {
                const Range& r = rockRanges[key];
                for (int i = 0; i < r.count; ++i) push(rocks[r.start + i], 2.0f, 1.0f, 1.0f);
            });
            g.boulder[0] = begin; g.boulder[1] = count - begin;
        }
        {
            std::uint32_t begin = count;
            forChunks(kRMesh, 1e9f, [&](int key, float dist) {
                int thin = (dist > 110.0f) ? 6 : (dist > 55.0f) ? 3 : (dist > 35.0f) ? 2 : 1;
                const Range& r = treeRanges[key];
                for (int i = 0; i < r.count; i += thin) {
                    const Instance& in = trees[r.start + i];
                    if (treeDist(in) <= kRMesh) continue;
                    push(in, 3.0f, 1.2f, 1.0f);
                }
            });
            g.billboard[0] = begin; g.billboard[1] = count - begin;
        }
        // grass then ferns: hashed per cell around the camera, nothing stored
        for (int fernPassI = 0; fernPassI < 3; ++fernPassI) {
            bool fernPass = fernPassI == 1;
            bool flowerPass = fernPassI == 2;
            std::uint32_t begin = count;
            const int N = kTerrainSize;
            float cell = kTerrainWorldSize / static_cast<float>(N - 1);
            float cellMeters = cell * kMetersPerUnit;
            const float radius = 3.2f;   // ~190 m
            int perCell = static_cast<int>(cellMeters * cellMeters / 1.6f);   // one tuft per 1.3x1.3 m
            const std::vector<float>& H = terrain.heights();
            const std::vector<float>& F = terrain.forestMap();
            const std::vector<float>& W = terrain.waterMap();
            const std::vector<float>& FL = terrain.flowMap();
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
                    if (!W.empty() && W[ci] > 0.08f) continue;
                    if (!FL.empty() && FL[ci] > 0.66f) continue;
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
                        bool fern = forestHere > 0.25f && (k % 6 == 0);
                        bool flower = !fern && forestHere < 0.45f && (k % 9 == 4);
                        if (flowerPass) { if (!flower) continue; }
                        else { if (flower || fern != fernPass) continue; }
                        float h = flower ? (0.45f + fr * 0.3f) : fern ? (0.9f + fr * 0.8f) : (0.55f + fr * 0.6f);
                        float type = flower ? 7.0f : fern ? 5.0f : 4.0f;
                        Instance in{wx, wy, wz, h * fade, fr * 6.2831853f, static_cast<std::uint8_t>(type)};
                        push(in, type, 1.0f, fade);
                    }
                }
            }
            if (flowerPass) { g.flower[0] = begin; g.flower[1] = count - begin; }
            else if (!fernPass) { g.grass[0] = begin; g.grass[1] = count - begin; }
            else { g.fern[0] = begin; g.fern[1] = count - begin; }
        }
        // Small mossy rocks scattered on the forest floor and meadows near the camera.
        {
            std::uint32_t begin = count;
            const int N = kTerrainSize;
            const float radius = 2.6f;
            const std::vector<float>& F = terrain.forestMap();
            const std::vector<float>& W = terrain.waterMap();
            int cx0 = static_cast<int>(std::floor(((camPos.x - radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cx1 = static_cast<int>(std::ceil(((camPos.x + radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cz0 = static_cast<int>(std::floor(((camPos.z - radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cz1 = static_cast<int>(std::ceil(((camPos.z + radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            for (int cz = std::max(cz0, 0); cz <= std::min(cz1, N - 2); ++cz) {
                for (int cx = std::max(cx0, 0); cx <= std::min(cx1, N - 2); ++cx) {
                    int ci = cz * N + cx;
                    float slope = terrain.slopeAt(cx, cz);
                    if (slope > 0.30f) continue;
                    if (!W.empty() && W[ci] > 0.08f) continue;
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


    // Tapered, gently curving branch made of ring segments. Returns the end point and direction.
    void branchTube(glm::vec3 base, glm::vec3 dir, float len, float r0, float r1, int segs, int sides,
                    float wobble, float upBias, const glm::vec3& col, glm::vec3& endP, glm::vec3& endD)
    {
        const float s = 1.0f / kMetersPerUnit;
        glm::vec3 p = base;
        glm::vec3 d = glm::normalize(dir);
        int prevRing = -1;
        for (int seg = 0; seg <= segs; ++seg) {
            float t = seg / static_cast<float>(segs);
            float r = glm::mix(r0, r1, t);
            glm::vec3 axisA = glm::normalize(glm::cross(d, std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
            glm::vec3 axisB = glm::cross(d, axisA);
            int ring = static_cast<int>(vertices.size());
            for (int i = 0; i < sides; ++i) {
                float a = i / static_cast<float>(sides) * 6.2831853f;
                glm::vec3 n = axisA * std::cos(a) + axisB * std::sin(a);
                addVertex((p + n * r) * s, n, col * (0.94f + rand01() * 0.12f));
            }
            if (prevRing >= 0) {
                for (int i = 0; i < sides; ++i) {
                    int a0 = prevRing + i, a1 = prevRing + (i + 1) % sides;
                    int b0 = ring + i, b1 = ring + (i + 1) % sides;
                    tri(a0, b0, a1); tri(a1, b0, b1);
                }
            }
            prevRing = ring;
            if (seg < segs) {
                glm::vec3 jitter{rand01() - 0.5f, rand01() - 0.5f, rand01() - 0.5f};
                d = glm::normalize(d + jitter * wobble + glm::vec3(0, upBias, 0));
                p += d * (len / segs);
            }
        }
        endP = p; endD = d;
    }

    void leafCluster(glm::vec3 c, float radius, int count, float leafSize, const glm::vec3& col)
    {
        const float s = 1.0f / kMetersPerUnit;
        for (int k = 0; k < count; ++k) {
            glm::vec3 o{rand01() - 0.5f, (rand01() - 0.5f) * 0.6f, rand01() - 0.5f};
            glm::vec3 p = c + o * (2.0f * radius);
            float a = rand01() * 6.2831853f, tilt = (rand01() - 0.5f) * 1.2f;
            glm::vec3 u{std::cos(a), tilt, std::sin(a)};
            u = glm::normalize(u);
            glm::vec3 w = glm::normalize(glm::cross(u, glm::vec3(0.3f, 1.0f, 0.2f)));
            glm::vec3 n = glm::normalize(glm::cross(u, w));
            if (n.y < 0.0f) n = -n;
            float L = leafSize * (0.8f + rand01() * 0.5f), W = L * 0.55f;
            glm::vec3 shade = col * (0.75f + rand01() * 0.5f);
            int i0 = addVertex(p * s, n, shade);
            int i1 = addVertex((p + u * (L * 0.5f) + w * (W * 0.5f)) * s, n, shade);
            int i2 = addVertex((p + u * L) * s, n, shade * 1.1f);
            int i3 = addVertex((p + u * (L * 0.5f) - w * (W * 0.5f)) * s, n, shade);
            tri(i0, i1, i2); tri(i0, i2, i3);
        }
    }

    // Broadleaf: curved trunk, 2 levels of branches, leaf lozenges at the branch tips.
    void buildBroadleafTree(bool midLod)
    {
        glm::vec3 bark{0.22f, 0.17f, 0.12f};
        glm::vec3 leaf{0.18f, 0.34f, 0.13f};
        glm::vec3 endP, endD;
        branchTube({0, 0, 0}, {0.02f * (rand01() - 0.5f), 1, 0.02f * (rand01() - 0.5f)}, 0.40f, 0.030f, 0.020f,
                   midLod ? 3 : 5, midLod ? 5 : 7, 0.025f, 0.01f, bark, endP, endD);
        if (midLod) {
            leafCluster({0, 0.74f, 0}, 0.30f, 34, 0.24f, leaf);
            return;
        }
        int nMain = 4 + static_cast<int>(rand01() * 2.0f);
        for (int b = 0; b < nMain; ++b) {
            float a = b / static_cast<float>(nMain) * 6.2831853f + rand01() * 0.6f;
            float spread = 0.55f + rand01() * 0.3f;
            glm::vec3 d = glm::normalize(glm::vec3(std::cos(a) * spread, 1.0f + rand01() * 0.4f, std::sin(a) * spread));
            glm::vec3 p2, d2;
            branchTube(endP, d, 0.26f + rand01() * 0.12f, 0.016f, 0.008f, 3, 5, 0.10f, 0.02f, bark, p2, d2);
            leafCluster(glm::mix(endP, p2, 0.7f), 0.10f, 8, 0.11f, leaf);
            int nSub = 3 + static_cast<int>(rand01() * 2.0f);
            for (int c = 0; c < nSub; ++c) {
                float ca = rand01() * 6.2831853f;
                glm::vec3 sd = glm::normalize(d2 * 0.6f + glm::vec3(std::cos(ca), 0.25f + rand01() * 0.5f, std::sin(ca)) * 0.7f);
                glm::vec3 p3, d3;
                branchTube(p2, sd, 0.12f + rand01() * 0.10f, 0.007f, 0.003f, 2, 4, 0.15f, 0.0f, bark, p3, d3);
                leafCluster(p3, 0.12f, 14, 0.11f, leaf);
                leafCluster(glm::mix(p2, p3, 0.5f), 0.08f, 7, 0.10f, leaf);
            }
            leafCluster(p2, 0.11f, 10, 0.11f, leaf);
        }
    }

    // Conifer: straight tapered trunk with whorls of drooping branch fans, needle lozenges dark.
    void buildConiferTree(bool midLod)
    {
        const float s = 1.0f / kMetersPerUnit;
        glm::vec3 bark{0.20f, 0.14f, 0.10f};
        glm::vec3 needle{0.09f, 0.18f, 0.09f};
        glm::vec3 endP, endD;
        branchTube({0, 0, 0}, {0, 1, 0}, 0.96f, 0.024f, 0.004f, midLod ? 3 : 6, midLod ? 5 : 6, 0.01f, 0.0f, bark, endP, endD);
        int whorls = midLod ? 6 : 11;
        for (int w = 0; w < whorls; ++w) {
            float t = (w + 1) / static_cast<float>(whorls + 1);
            float y = 0.20f + t * 0.76f;
            float reach = (1.0f - t) * 0.30f + 0.04f;
            int nb = midLod ? 5 : 6 + (w % 2);
            for (int b = 0; b < nb; ++b) {
                float a = b / static_cast<float>(nb) * 6.2831853f + w * 0.6f + rand01() * 0.3f;
                glm::vec3 d{std::cos(a), -0.28f + rand01() * 0.15f, std::sin(a)};
                d = glm::normalize(d);
                glm::vec3 side = glm::normalize(glm::cross(d, glm::vec3(0, 1, 0)));
                glm::vec3 base{0, y, 0};
                glm::vec3 tip = base + d * reach;
                // branch fan: a flat lozenge with a slight droop, plus a couple of side tufts
                glm::vec3 n = glm::normalize(glm::cross(side, d));
                if (n.y < 0.0f) n = -n;
                glm::vec3 shade = needle * (0.8f + rand01() * 0.4f);
                float hw = reach * 0.22f;
                int i0 = addVertex(base * s, n, shade * 0.8f);
                int i1 = addVertex((base + d * (reach * 0.5f) + side * hw - glm::vec3(0, 0.02f, 0)) * s, n, shade);
                int i2 = addVertex(tip * s, n, shade * 1.15f);
                int i3 = addVertex((base + d * (reach * 0.5f) - side * hw - glm::vec3(0, 0.02f, 0)) * s, n, shade);
                tri(i0, i1, i2); tri(i0, i2, i3);
                if (!midLod) {
                    for (int k = 0; k < 2; ++k) {
                        glm::vec3 c = base + d * (reach * (0.35f + 0.35f * k));
                        glm::vec3 sd = glm::normalize(d * 0.5f + side * (k == 0 ? 1.0f : -1.0f) * 0.8f + glm::vec3(0, -0.2f, 0));
                        glm::vec3 tp = c + sd * (reach * 0.35f);
                        int j0 = addVertex(c * s, n, shade * 0.9f);
                        int j1 = addVertex((c + sd * (reach * 0.17f) + d * (hw * 0.5f)) * s, n, shade);
                        int j2 = addVertex(tp * s, n, shade * 1.1f);
                        int j3 = addVertex((c + sd * (reach * 0.17f) - d * (hw * 0.5f)) * s, n, shade);
                        tri(j0, j1, j2); tri(j0, j2, j3);
                    }
                }
            }
        }
        // crown spike
        glm::vec3 shade = needle;
        int c0 = addVertex(glm::vec3(0, 0.90f, 0) * s, {0, 1, 0}, shade);
        int c1 = addVertex(glm::vec3(0.05f, 0.92f, 0) * s, {0, 1, 0}, shade);
        int c2 = addVertex(glm::vec3(0, 1.0f, 0) * s, {0, 1, 0}, shade * 1.1f);
        int c3 = addVertex(glm::vec3(-0.05f, 0.92f, 0) * s, {0, 1, 0}, shade);
        tri(c0, c1, c2); tri(c0, c2, c3);
    }

    void buildMeshes()
    {
        const float s = 1.0f / kMetersPerUnit;
        rng_ = 777u;
        // Unit trees are 1 m tall; the instance scale is the tree height in metres.
        std::uint32_t f0 = 0;
        for (int v = 0; v < kVariants; ++v) {
            rng_ = 900u + v * 7919u;
            std::uint32_t f = static_cast<std::uint32_t>(indices.size());
            buildBroadleafTree(false);
            broadleaf[v] = finishRange(f);
            rng_ = 1300u + v * 104729u;
            f = static_cast<std::uint32_t>(indices.size());
            buildConiferTree(false);
            conifer[v] = finishRange(f);
        }
        rng_ = 4242u;
        f0 = static_cast<std::uint32_t>(indices.size());
        buildBroadleafTree(true);
        broadleafMid = finishRange(f0);
        f0 = static_cast<std::uint32_t>(indices.size());
        buildConiferTree(true);
        coniferMid = finishRange(f0);

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
        for (int k = 0; k < 26; ++k) {
            float a = k / 26.0f * 6.2831853f + rand01() * 0.5f;
            float lean = 0.25f + rand01() * 0.55f;
            float hgt = 0.55f + rand01() * 0.65f;
            float w = 0.012f + rand01() * 0.014f;
            glm::vec3 d{std::cos(a), 0, std::sin(a)};
            glm::vec3 side{-std::sin(a) * w, 0, std::cos(a) * w};
            glm::vec3 root{d.x * 0.10f * rand01(), 0, d.z * 0.10f * rand01()};
            glm::vec3 base{0.07f, 0.15f, 0.05f}, tip{0.15f, 0.29f, 0.09f};
            glm::vec3 n{d.x, 0.6f, d.z};
            // two segments so the blade curves over
            glm::vec3 mid{(root.x + d.x * lean * 0.35f), hgt * 0.55f, (root.z + d.z * lean * 0.35f)};
            glm::vec3 top{(root.x + d.x * lean), hgt, (root.z + d.z * lean)};
            int i0 = addVertex((root - side) * s, n, base);
            int i1 = addVertex((root + side) * s, n, base);
            int i2 = addVertex((mid - side * 0.6f) * s, n, glm::mix(base, tip, 0.5f));
            int i3 = addVertex((mid + side * 0.6f) * s, n, glm::mix(base, tip, 0.5f));
            int i4 = addVertex(top * s, n, tip);
            tri(i0, i1, i3); tri(i0, i3, i2); tri(i2, i3, i4);
        }
        grass = finishRange(f0);

        // Fern: pinnate fronds. A thin stem strip carries pairs of leaflets that shrink toward the tip.
        f0 = static_cast<std::uint32_t>(indices.size());
        for (int k = 0; k < 9; ++k) {
            float a = k / 9.0f * 6.2831853f + rand01() * 0.6f;
            glm::vec3 d{std::cos(a), 0, std::sin(a)};
            glm::vec3 side{-std::sin(a), 0, std::cos(a)};
            glm::vec3 stemCol{0.16f, 0.26f, 0.10f};
            glm::vec3 leafCol{0.13f, 0.30f, 0.11f};
            const int segs = 9;
            int prevL = -1, prevR = -1;
            for (int seg = 0; seg <= segs; ++seg) {
                float t = seg / static_cast<float>(segs);
                float out = t * 0.95f;
                float up = 0.12f + 0.95f * std::sin(t * 2.2f) - t * t * 0.5f;
                glm::vec3 c = glm::vec3(d.x * out, up, d.z * out);
                glm::vec3 n = glm::normalize(glm::vec3(d.x * 0.3f, 1.0f, d.z * 0.3f));
                float sw = 0.012f * (1.0f - t * 0.7f);
                int L = addVertex((c - side * sw) * s, n, stemCol);
                int R = addVertex((c + side * sw) * s, n, stemCol);
                if (prevL >= 0) { tri(prevL, prevR, L); tri(prevR, R, L); }
                prevL = L; prevR = R;
                if (seg > 0 && seg < segs) {
                    float len = (0.10f + 0.16f * std::sin(t * 3.14159f));
                    float lw = 0.035f;
                    for (int sideSign = -1; sideSign <= 1; sideSign += 2) {
                        glm::vec3 lo = side * (float)sideSign;
                        glm::vec3 tipP = c + lo * len + d * 0.03f + glm::vec3(0, -0.02f, 0);
                        glm::vec3 shade = leafCol * (0.85f + 0.3f * t);
                        int b0 = addVertex((c + d * lw) * s, n, shade);
                        int b1 = addVertex((c - d * lw) * s, n, shade);
                        int b2 = addVertex(tipP * s, n, shade * 1.1f);
                        int bm = addVertex((c + lo * len * 0.5f + d * lw * 1.6f) * s, n, shade);
                        int bn = addVertex((c + lo * len * 0.5f - d * lw * 1.6f) * s, n, shade);
                        tri(b0, bm, b2); tri(b0, b2, b1); tri(b1, b2, bn);
                    }
                }
            }
        }
        fern = finishRange(f0);

        // Flower: thin stems with a five-petal white head, three per instance.
        f0 = static_cast<std::uint32_t>(indices.size());
        for (int k = 0; k < 3; ++k) {
            float a = rand01() * 6.2831853f;
            float r = rand01() * 0.12f;
            glm::vec3 base{std::cos(a) * r, 0, std::sin(a) * r};
            float h = 0.7f + rand01() * 0.3f;
            glm::vec3 stemCol{0.18f, 0.30f, 0.10f};
            glm::vec3 n{0, 1, 0};
            int s0 = addVertex((base + glm::vec3(-0.008f, 0, 0)) * s, n, stemCol);
            int s1 = addVertex((base + glm::vec3(0.008f, 0, 0)) * s, n, stemCol);
            int s2 = addVertex((base + glm::vec3(0, h, 0)) * s, n, stemCol);
            tri(s0, s1, s2);
            glm::vec3 head = base + glm::vec3(0, h, 0);
            for (int pth = 0; pth < 5; ++pth) {
                float pa = pth / 5.0f * 6.2831853f;
                glm::vec3 pd{std::cos(pa), 0, std::sin(pa)};
                glm::vec3 ps{-std::sin(pa), 0, std::cos(pa)};
                glm::vec3 white{0.92f, 0.92f, 0.85f};
                int c0 = addVertex(head * s, n, {0.85f, 0.75f, 0.30f});
                int c1 = addVertex((head + pd * 0.05f + ps * 0.025f) * s, n, white);
                int c2 = addVertex((head + pd * 0.09f) * s, n, white);
                int c3 = addVertex((head + pd * 0.05f - ps * 0.025f) * s, n, white);
                tri(c0, c1, c2); tri(c0, c2, c3);
            }
        }
        flower = finishRange(f0);

        // Moss rock: small lumpy boulder, greenish grey; instance fade carries forest cover for a mossier tint.
        f0 = static_cast<std::uint32_t>(indices.size());
        blob({0, 0.35f * s, 0}, 0.5f, 0.75f, 4, 7, {0.34f, 0.36f, 0.30f}, 0.5f);
        mossRock = finishRange(f0);
    }
};
