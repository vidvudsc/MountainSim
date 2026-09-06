#pragma once

// Vegetation: trees, boulders, undergrowth placed from the terrain's ecology maps and drawn
// as GPU instances with distance LOD. Foliage uses procedurally painted alpha-cut textures
// (leaf clusters, grass, fern fronds, needle sprays, litter) so one card carries a whole
// cluster. World unit = kMetersPerUnit metres on all axes.

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
    glm::vec4 uvLayer{0.0f, 0.0f, -1.0f, 0.0f};   // xy uv, z foliage atlas layer (-1 = plain colour), w unused
};

struct VegInstanceGpu {
    glm::vec4 posScale{};   // xyz world position, w = scale in metres
    glm::vec4 rotType{};    // x rotation, y type, z fade, w subtype
};
// Instance types: 0 conifer, 1 broadleaf, 2 boulder, 3 billboard, 4 grass, 5 fern, 6 moss rock,
// 7 flower, 8 mushroom, 9 litter, 10 dead tree.

struct VegMeshRange {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::int32_t vertexOffset = 0;
};

// Foliage atlas layers.
enum FoliageLayer : int { FOL_LEAF = 0, FOL_GRASS = 1, FOL_FERN = 2, FOL_LITTER = 3, FOL_BIRCH = 4, FOL_NEEDLE = 5, FOL_COUNT = 6 };
constexpr int kFoliageTexSize = 512;

class Vegetation {
public:
    static constexpr int kSub = 32;                  // terrain cells per sub-chunk
    static constexpr float kRMesh = 18.0f;           // mid meshes out to here (world units)
    static constexpr float kRNear = 5.0f;            // full meshes within ~300 m
    static constexpr std::uint32_t kMaxInstances = 520000;
    static constexpr int kVariants = 3;

    struct Instance { float x, y, z, scale, rot; std::uint8_t type; };
    struct Range { int start = 0, count = 0; };

    std::vector<VegVertex> vertices;
    std::vector<std::uint32_t> indices;
    VegMeshRange conifer[kVariants], broadleaf[kVariants], coniferMid, broadleafMid, deadTree;
    VegMeshRange boulder, billboard, grass, fern, mossRock, flower, mushroom, litter;

    std::vector<Instance> trees, rocks;
    std::vector<Range> treeRanges, rockRanges;
    int gridN = 0;

    Vegetation() { buildMeshes(); }

    // ---- procedurally painted foliage atlas ------------------------------------------------
    // RGBA8, kFoliageTexSize square, FOL_COUNT layers. Alpha is the cutout.
    static std::vector<std::uint8_t> paintFoliageAtlas()
    {
        const int S = kFoliageTexSize;
        std::vector<std::uint8_t> px(static_cast<std::size_t>(S) * S * 4 * FOL_COUNT, 0);
        std::uint32_t rng = 1234567u;
        auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0f; };
        auto put = [&](int layer, int x, int y, glm::vec3 c, float a) {
            if (x < 0 || y < 0 || x >= S || y >= S) return;
            std::uint8_t* p = &px[((static_cast<std::size_t>(layer) * S + y) * S + x) * 4];
            float oldA = p[3] / 255.0f;
            float na = std::max(oldA, a);
            // painter's order: later shapes cover earlier ones
            p[0] = static_cast<std::uint8_t>(glm::clamp(c.r * 255.0f, 0.0f, 255.0f));
            p[1] = static_cast<std::uint8_t>(glm::clamp(c.g * 255.0f, 0.0f, 255.0f));
            p[2] = static_cast<std::uint8_t>(glm::clamp(c.b * 255.0f, 0.0f, 255.0f));
            p[3] = static_cast<std::uint8_t>(na * 255.0f);
        };
        // Rotated ellipse with a darker mid-vein and lighter rim.
        auto leaf = [&](int layer, float cx, float cy, float len, float wid, float ang, glm::vec3 col, bool pointed) {
            float ca = std::cos(ang), sa = std::sin(ang);
            int r = static_cast<int>(std::max(len, wid)) + 2;
            for (int y = static_cast<int>(cy) - r; y <= static_cast<int>(cy) + r; ++y) {
                for (int x = static_cast<int>(cx) - r; x <= static_cast<int>(cx) + r; ++x) {
                    float dx = x - cx, dy = y - cy;
                    float u = (dx * ca + dy * sa) / len;       // along
                    float v = (-dx * sa + dy * ca) / wid;      // across
                    float w = pointed ? (1.0f - std::abs(u)) * 1.15f : std::sqrt(std::max(0.0f, 1.0f - u * u));
                    if (std::abs(u) > 1.0f || std::abs(v) > w) continue;
                    float edge = 1.0f - std::abs(v) / std::max(w, 0.01f);
                    float vein = std::exp(-std::abs(v) * 40.0f) * 0.35f;
                    glm::vec3 c = col * (0.85f + 0.3f * edge) * (1.0f - vein) * (0.9f + 0.2f * (0.5f + 0.5f * u));
                    put(layer, x, y, c, 1.0f);
                }
            }
        };
        // Tapered curved blade from (x0,y0) upward.
        auto blade = [&](int layer, float x0, float y0, float h, float w, float curve, glm::vec3 base, glm::vec3 tip) {
            int steps = static_cast<int>(h);
            for (int i = 0; i <= steps; ++i) {
                float t = i / static_cast<float>(steps);
                float x = x0 + curve * t * t * 60.0f;
                float y = y0 - t * h;
                float hw = w * (1.0f - t) * 0.5f + 0.4f;
                glm::vec3 c = glm::mix(base, tip, t);
                for (int k = static_cast<int>(x - hw); k <= static_cast<int>(x + hw); ++k) put(layer, k, static_cast<int>(y), c, 1.0f);
            }
        };

        // FOL_LEAF: a cluster of broad leaves, more toward the centre.
        for (int i = 0; i < 44; ++i) {
            float a = rnd() * 6.2831853f, r = std::pow(rnd(), 0.6f) * S * 0.42f;
            glm::vec3 col{0.19f + rnd() * 0.07f, 0.30f + rnd() * 0.09f, 0.12f + rnd() * 0.05f};
            leaf(FOL_LEAF, S * 0.5f + std::cos(a) * r, S * 0.5f + std::sin(a) * r, S * (0.07f + rnd() * 0.05f), S * (0.035f + rnd() * 0.02f), rnd() * 6.28f, col, true);
        }
        // FOL_BIRCH: small rounder lighter leaves, sparser.
        for (int i = 0; i < 60; ++i) {
            float a = rnd() * 6.2831853f, r = std::pow(rnd(), 0.6f) * S * 0.44f;
            glm::vec3 col{0.27f + rnd() * 0.08f, 0.40f + rnd() * 0.10f, 0.15f + rnd() * 0.05f};
            leaf(FOL_BIRCH, S * 0.5f + std::cos(a) * r, S * 0.5f + std::sin(a) * r, S * (0.035f + rnd() * 0.02f), S * (0.028f + rnd() * 0.012f), rnd() * 6.28f, col, false);
        }
        // FOL_GRASS: many blades rooted along the bottom edge.
        for (int i = 0; i < 95; ++i) {
            float x0 = S * (0.08f + rnd() * 0.84f);
            float h = S * (0.30f + rnd() * 0.52f);   // keep the card top clear so tufts do not read as boxes
            glm::vec3 base{0.09f, 0.17f, 0.06f}, tip{0.22f + rnd() * 0.08f, 0.34f + rnd() * 0.08f, 0.13f};
            blade(FOL_GRASS, x0, S - 1.0f, h, S * 0.018f, (rnd() - 0.5f) * 1.6f, base, tip);
        }
        // FOL_FERN: a frond, stem up the middle with paired leaflets shrinking to the tip; each
        // leaflet made of small serrated sub-leaflets.
        {
            glm::vec3 stem{0.20f, 0.30f, 0.10f};
            for (int y = S - 4; y > S * 0.06f; --y) for (int k = -2; k <= 2; ++k) put(FOL_FERN, S / 2 + k, y, stem, 1.0f);
            int pairs = 15;
            for (int p = 0; p < pairs; ++p) {
                float t = p / static_cast<float>(pairs - 1);
                float y = S * 0.94f - t * S * 0.86f;
                float len = S * 0.44f * std::sin(t * 2.6f + 0.35f) * (1.0f - t * 0.55f) + S * 0.03f;
                for (int side = -1; side <= 1; side += 2) {
                    float ang = side * (0.35f + 0.25f * t) - 0.05f;   // leaflets angle up toward the tip
                    int subs = 7;
                    for (int q = 0; q <= subs; ++q) {
                        float u = q / static_cast<float>(subs);
                        float lx = S * 0.5f + side * u * len * std::cos(ang);
                        float ly = y - u * len * std::sin(std::abs(ang)) * 0.55f;
                        float subLen = S * 0.028f * (1.0f - u * 0.6f) + 3.0f;
                        glm::vec3 col{0.14f + rnd() * 0.05f, 0.32f + rnd() * 0.1f, 0.11f};
                        leaf(FOL_FERN, lx, ly - subLen * 0.4f, subLen, subLen * 0.45f, 1.5708f + side * 0.6f, col, true);
                        leaf(FOL_FERN, lx, ly + subLen * 0.4f, subLen, subLen * 0.45f, 1.5708f - side * 0.6f, col, true);
                    }
                    // leaflet midrib
                    int segs = static_cast<int>(len);
                    for (int i = 0; i < segs; ++i) {
                        float u = i / static_cast<float>(std::max(segs, 1));
                        put(FOL_FERN, static_cast<int>(S * 0.5f + side * u * len * std::cos(ang)), static_cast<int>(y - u * len * std::sin(std::abs(ang)) * 0.55f), stem, 1.0f);
                    }
                }
            }
        }
        // FOL_LITTER: fallen leaves in browns and ochres, sparse.
        for (int i = 0; i < 38; ++i) {
            float x = rnd() * S, y = rnd() * S;
            float h = rnd();
            glm::vec3 col = glm::mix(glm::vec3(0.45f, 0.30f, 0.14f), glm::vec3(0.62f, 0.46f, 0.20f), h);
            if (rnd() < 0.25f) col = glm::vec3(0.30f, 0.36f, 0.14f);
            leaf(FOL_LITTER, x, y, S * (0.05f + rnd() * 0.04f), S * (0.03f + rnd() * 0.015f), rnd() * 6.28f, col, true);
        }
        // FOL_NEEDLE: a spruce spray, central twig with dense short needles either side, tapering.
        {
            glm::vec3 twig{0.22f, 0.16f, 0.10f};
            for (int y = S - 3; y > S * 0.05f; --y) put(FOL_NEEDLE, S / 2, y, twig, 1.0f), put(FOL_NEEDLE, S / 2 + 1, y, twig, 1.0f);
            for (int i = 0; i < 260; ++i) {
                float t = rnd();
                float y = S * 0.96f - t * S * 0.9f;
                float len = S * (0.09f + 0.16f * (1.0f - t)) * (0.7f + rnd() * 0.3f);
                float side = rnd() < 0.5f ? -1.0f : 1.0f;
                float ang = side * (0.9f + rnd() * 0.5f);
                glm::vec3 col{0.07f + rnd() * 0.05f, 0.20f + rnd() * 0.10f, 0.09f + rnd() * 0.04f};
                leaf(FOL_NEEDLE, S * 0.5f + std::cos(ang) * len * 0.5f, y - std::sin(std::abs(ang)) * len * 0.15f, len * 0.5f, 3.4f, ang, col, true);
            }
            // side twigs with their own needles
            for (int b = 0; b < 8; ++b) {
                float t = 0.15f + b * 0.1f;
                float y0 = S * 0.94f - t * S * 0.86f;
                float side = (b % 2) ? 1.0f : -1.0f;
                float len = S * 0.3f * (1.0f - t * 0.5f);
                for (int i = 0; i < 60; ++i) {
                    float u = rnd();
                    float x = S * 0.5f + side * u * len, y = y0 - u * len * 0.35f;
                    float nl = S * 0.06f * (0.6f + rnd() * 0.4f);
                    float ang = (rnd() < 0.5f ? -1.0f : 1.0f) * (1.1f + rnd() * 0.4f);
                    glm::vec3 col{0.08f + rnd() * 0.04f, 0.21f + rnd() * 0.08f, 0.10f};
                    leaf(FOL_NEEDLE, x + std::cos(ang) * nl * 0.5f, y - std::sin(std::abs(ang)) * nl * 0.3f, nl * 0.5f, 3.0f, ang, col, true);
                }
            }
        }
        return px;
    }

    // ---- placement ---------------------------------------------------------------------------
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
        float maxTreesPerCell = std::max(0.5f, (cellMeters * cellMeters) / (8.5f * 8.5f));

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
        kt.reserve(800000);
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
                        float height = (type == 0) ? 18.0f + rand01() * 18.0f : 13.0f + rand01() * rand01() * 20.0f;
                        height *= 0.75f + 0.25f * density;
                        if (rand01() < 0.035f) { type = 10; height *= 0.7f; }   // dead snag
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
                        float wy = terrain.surfaceHeightAtWorld(wx, wz) - 0.35f / kMetersPerUnit;
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

    struct DrawGroups {
        std::uint32_t conifer[kVariants][2], broadleaf[kVariants][2], coniferMid[2], broadleafMid[2], dead[2];
        std::uint32_t boulder[2], billboard[2], grass[2], fern[2], mossRock[2], flower[2], mushroom[2], litter[2];
    };

    DrawGroups gather(const Terrain& terrain, const glm::vec3& camPos, const glm::vec3& camFwd, VegInstanceGpu* out, std::uint32_t cap) const
    {
        DrawGroups g{};
        std::uint32_t count = 0;
        float subWorld = kSub * kTerrainWorldSize / static_cast<float>(kTerrainSize - 1);
        auto push = [&](const Instance& in, float type, float scaleMul, float fade, float sub = 0.0f) {
            if (count >= cap) return;
            out[count++] = {glm::vec4(in.x, in.y, in.z, in.scale * scaleMul), glm::vec4(in.rot, type, fade, sub)};
        };
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
        // Near full meshes per type and variant; dead snags in their own group.
        for (int type = 0; type < 2; ++type) {
            for (int v = 0; v < kVariants; ++v) {
                std::uint32_t begin = count;
                forChunks(0.0f, kRNear, [&](int key, float) {
                    const Range& r = treeRanges[key];
                    for (int i = 0; i < r.count; ++i) {
                        const Instance& in = trees[r.start + i];
                        int slot = (r.start + i) % 5;                 // birch (variant 2) on one slot in five
                        int variant = (slot == 4) ? 2 : (slot % 2);
                        if (in.type != type || variant != v) continue;
                        if (treeDist(in) > kRNear) continue;
                        push(in, static_cast<float>(type), 1.0f, 1.0f, static_cast<float>(v));
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
                    push(in, static_cast<float>(type), 1.0f, 1.0f, static_cast<float>((r.start + i) % kVariants));
                }
            });
            std::uint32_t* dst = (type == 0) ? g.coniferMid : g.broadleafMid;
            dst[0] = begin; dst[1] = count - begin;
        }
        {
            std::uint32_t begin = count;
            forChunks(0.0f, kRMesh, [&](int key, float) {
                const Range& r = treeRanges[key];
                for (int i = 0; i < r.count; ++i) {
                    const Instance& in = trees[r.start + i];
                    if (in.type != 10 || treeDist(in) > kRMesh) continue;
                    push(in, 10.0f, 1.0f, 1.0f);
                }
            });
            g.dead[0] = begin; g.dead[1] = count - begin;
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
                    push(in, 3.0f, 1.2f, 1.0f, in.type == 10 ? 1.0f : static_cast<float>(in.type));
                }
            });
            g.billboard[0] = begin; g.billboard[1] = count - begin;
        }

        // ---- undergrowth around the camera, hashed per cell, nothing stored -----------------
        const int N = kTerrainSize;
        float cell = kTerrainWorldSize / static_cast<float>(N - 1);
        float cellMeters = cell * kMetersPerUnit;
        const std::vector<float>& H = terrain.heights();
        const std::vector<float>& F = terrain.forestMap();
        const std::vector<float>& W = terrain.waterMap();
        const std::vector<float>& FL = terrain.flowMap();
        float minH = 1e9f, maxH = -1e9f;
        for (float h : H) { minH = std::min(minH, h); maxH = std::max(maxH, h); }
        float span = std::max(maxH - minH, 0.001f);
        auto cellLoop = [&](float radius, auto&& fn) {
            int cx0 = static_cast<int>(std::floor(((camPos.x - radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cx1 = static_cast<int>(std::ceil(((camPos.x + radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cz0 = static_cast<int>(std::floor(((camPos.z - radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            int cz1 = static_cast<int>(std::ceil(((camPos.z + radius) / kTerrainWorldSize + 0.5f) * (N - 1)));
            for (int cz = std::max(cz0, 0); cz <= std::min(cz1, N - 2); ++cz)
                for (int cx = std::max(cx0, 0); cx <= std::min(cx1, N - 2); ++cx) {
                    int ci = cz * N + cx;
                    if (!W.empty() && W[ci] > 0.08f) continue;
                    if (!FL.empty() && FL[ci] > 0.66f) continue;
                    fn(cx, cz, ci);
                }
        };
        // grass (pass 0), ferns (1), flowers (2), mushrooms (3), litter (4)
        const float radius = 2.7f;   // ~160 m
        for (int pass = 0; pass < 5; ++pass) {
            std::uint32_t begin = count;
            cellLoop(radius, [&](int cx, int cz, int ci) {
                float slope = terrain.slopeAt(cx, cz);
                if (slope > 0.26f) return;
                float hN = (H[ci] - minH) / span;
                if (hN > 0.72f) return;
                float forestHere = F.empty() ? 0.0f : F[ci];
                int perCell;
                float density;
                if (pass == 0) { perCell = static_cast<int>(cellMeters * cellMeters / 1.3f); density = (1.0f - glm::clamp((slope - 0.14f) / 0.10f, 0.0f, 1.0f)) * (1.0f - 0.25f * forestHere); }
                else if (pass == 1) { perCell = static_cast<int>(cellMeters * cellMeters / 9.0f); density = glm::clamp((forestHere - 0.2f) / 0.5f, 0.0f, 1.0f); }
                else if (pass == 2) { perCell = static_cast<int>(cellMeters * cellMeters / 14.0f); density = (1.0f - glm::clamp((forestHere - 0.1f) / 0.4f, 0.0f, 1.0f)) * (1.0f - glm::clamp((slope - 0.12f) / 0.1f, 0.0f, 1.0f)); }
                else if (pass == 3) { perCell = static_cast<int>(cellMeters * cellMeters / 40.0f); density = glm::clamp((forestHere - 0.3f) / 0.5f, 0.0f, 1.0f); }
                else { perCell = static_cast<int>(cellMeters * cellMeters / 12.0f); density = glm::clamp((forestHere - 0.15f) / 0.5f, 0.0f, 1.0f); }
                int n = static_cast<int>(perCell * density);
                std::uint32_t r = static_cast<std::uint32_t>(cx * 73856093) ^ static_cast<std::uint32_t>(cz * 19349663) ^ (0x9E3779B9u + pass * 0x632BE5ABu);
                for (int k = 0; k < n && count < cap; ++k) {
                    r = r * 1664525u + 1013904223u; float fx = (r >> 8) / 16777216.0f;
                    r = r * 1664525u + 1013904223u; float fz = (r >> 8) / 16777216.0f;
                    r = r * 1664525u + 1013904223u; float fr = (r >> 8) / 16777216.0f;
                    float wx = ((cx + fx) / (N - 1) - 0.5f) * kTerrainWorldSize;
                    float wz = ((cz + fz) / (N - 1) - 0.5f) * kTerrainWorldSize;
                    float ddx = wx - camPos.x, ddz = wz - camPos.z;
                    float d2 = ddx * ddx + ddz * ddz;
                    if (d2 > radius * radius) continue;
                    if (ddx * camFwd.x + ddz * camFwd.z < -0.2f) continue;
                    float dn = std::sqrt(d2) / radius;
                    float fade = 1.0f - glm::clamp((dn - 0.6f) / 0.4f, 0.0f, 1.0f);
                    if (fade <= 0.02f) continue;
                    if (dn > 0.45f && (k % 2) == 1) continue;           // thin out with distance
                    if (dn > 0.75f && (k % 4) != 0) continue;
                    float wy = terrain.surfaceHeightAtWorld(wx, wz) - 0.02f / kMetersPerUnit;
                    float sc, type;
                    if (pass == 0) { sc = (0.8f + fr * 0.7f) * fade; type = 4.0f; }
                    else if (pass == 1) { sc = (0.9f + fr * 0.7f) * fade; type = 5.0f; }
                    else if (pass == 2) { sc = (0.45f + fr * 0.3f) * fade; type = 7.0f; }
                    else if (pass == 3) { sc = 0.10f + fr * 0.14f; type = 8.0f; }
                    else { sc = 0.9f + fr * 0.8f; type = 9.0f; }
                    Instance in{wx, wy, wz, sc, fr * 6.2831853f, static_cast<std::uint8_t>(type)};
                    push(in, type, 1.0f, fade, forestHere);
                }
            });
            std::uint32_t* dst = (pass == 0) ? g.grass : (pass == 1) ? g.fern : (pass == 2) ? g.flower : (pass == 3) ? g.mushroom : g.litter;
            dst[0] = begin; dst[1] = count - begin;
        }
        // Small mossy rocks on the forest floor and meadows near the camera.
        {
            std::uint32_t begin = count;
            cellLoop(2.8f, [&](int cx, int cz, int ci) {
                float slope = terrain.slopeAt(cx, cz);
                if (slope > 0.30f) return;
                float forestHere = F.empty() ? 0.0f : F[ci];
                std::uint32_t r = static_cast<std::uint32_t>(cx * 2654435761u) ^ static_cast<std::uint32_t>(cz * 40503u) ^ 0x51ED27u;
                r = r * 1664525u + 1013904223u;
                int n = ((r >> 8) % 100) < static_cast<unsigned>(28 + 45 * forestHere) ? 1 : 0;
                for (int k = 0; k < n && count < cap; ++k) {
                    r = r * 1664525u + 1013904223u; float fx = (r >> 8) / 16777216.0f;
                    r = r * 1664525u + 1013904223u; float fz = (r >> 8) / 16777216.0f;
                    r = r * 1664525u + 1013904223u; float fr = (r >> 8) / 16777216.0f;
                    float wx = ((cx + fx) / (N - 1) - 0.5f) * kTerrainWorldSize;
                    float wz = ((cz + fz) / (N - 1) - 0.5f) * kTerrainWorldSize;
                    float wy = terrain.surfaceHeightAtWorld(wx, wz) - 0.25f / kMetersPerUnit;
                    Instance in{wx, wy, wz, 0.6f + fr * fr * 2.4f, fr * 6.2831853f, 6};
                    push(in, 6.0f, 1.0f, forestHere);
                }
            });
            g.mossRock[0] = begin; g.mossRock[1] = count - begin;
        }
        return g;
    }

private:
    std::uint32_t rng_ = 12345u;
    float rand01() { rng_ = rng_ * 1664525u + 1013904223u; return (rng_ >> 8) / 16777216.0f; }
    static constexpr float kS = 1.0f / kMetersPerUnit;   // metres -> world units

    int addVertex(const glm::vec3& p, const glm::vec3& n, const glm::vec3& c, glm::vec2 uv = {0, 0}, float layer = -1.0f)
    {
        vertices.push_back({p, n, c, glm::vec4(uv, layer, 0.0f)});
        return static_cast<int>(vertices.size()) - 1;
    }
    void tri(int a, int b, int c) { indices.push_back(a); indices.push_back(b); indices.push_back(c); }
    glm::vec3 shade(const glm::vec3& c, float lo, float hi) { return c * (lo + rand01() * (hi - lo)); }

    // Textured card: centre c, right axis r (half width), up axis u (half height), both in metres.
    void card(glm::vec3 c, glm::vec3 r, glm::vec3 u, int layer, glm::vec3 tint, bool bottomAnchored = false)
    {
        glm::vec3 n = glm::normalize(glm::cross(r, u));
        glm::vec3 base = bottomAnchored ? c : c - u;
        int i0 = addVertex((base - r) * kS, n, tint, {0, 1}, static_cast<float>(layer));
        int i1 = addVertex((base + r) * kS, n, tint, {1, 1}, static_cast<float>(layer));
        int i2 = addVertex((base + r + u * 2.0f) * kS, n, tint, {1, 0}, static_cast<float>(layer));
        int i3 = addVertex((base - r + u * 2.0f) * kS, n, tint, {0, 0}, static_cast<float>(layer));
        tri(i0, i1, i2); tri(i0, i2, i3);
    }

    void blob(glm::vec3 center, float radius, float squashY, int rings, int segs, glm::vec3 col, float jitter)
    {
        int first = static_cast<int>(vertices.size());
        for (int r = 0; r <= rings; ++r) {
            float phi = r / static_cast<float>(rings) * 3.14159265f;
            for (int q = 0; q <= segs; ++q) {
                float th = q / static_cast<float>(segs) * 6.2831853f;
                glm::vec3 d{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
                float jr = (q == segs || r == 0 || r == rings) ? 1.0f : 1.0f + (rand01() - 0.5f) * jitter;
                glm::vec3 p = center + glm::vec3(d.x * radius * jr, d.y * radius * squashY * jr, d.z * radius * jr);
                addVertex(p * kS, d, shade(col, 0.8f, 1.2f));
            }
        }
        for (int r = 0; r < rings; ++r) for (int q = 0; q < segs; ++q) {
            int a = first + r * (segs + 1) + q;
            int b = a + segs + 1;
            tri(a, a + 1, b); tri(a + 1, b + 1, b);
        }
    }

    // Tapered, gently curving branch of ring segments (metres). Returns end point and direction.
    void branchTube(glm::vec3 base, glm::vec3 dir, float len, float r0, float r1, int segs, int sides,
                    float wobble, float upBias, const glm::vec3& col, glm::vec3& endP, glm::vec3& endD)
    {
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
                addVertex((p + n * r) * kS, n, col);
            }
            if (prevRing >= 0) for (int i = 0; i < sides; ++i) {
                int a0 = prevRing + i, a1 = prevRing + (i + 1) % sides;
                int b0 = ring + i, b1 = ring + (i + 1) % sides;
                tri(a0, b0, a1); tri(a1, b0, b1);
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

    // Leaf cards around a point: each card is a textured cluster ~cardSize metres across.
    void leafCards(glm::vec3 c, float radius, int count, float cardSize, int layer, glm::vec3 tint)
    {
        for (int k = 0; k < count; ++k) {
            glm::vec3 o{rand01() - 0.5f, (rand01() - 0.5f) * 0.7f, rand01() - 0.5f};
            glm::vec3 p = c + o * (2.0f * radius);
            float a = rand01() * 6.2831853f, tilt = (rand01() - 0.5f) * 1.6f;
            glm::vec3 r = glm::normalize(glm::vec3(std::cos(a), 0.0f, std::sin(a))) * (cardSize * 0.5f);
            glm::vec3 u = glm::normalize(glm::vec3(-std::sin(a) * std::sin(tilt), std::cos(tilt), std::cos(a) * std::sin(tilt))) * (cardSize * 0.5f);
            card(p, r, u, layer, tint * (0.85f + rand01() * 0.3f));
        }
    }

    // Broadleaf: a unit-height tree (1 m tall in the mesh; instance scale = height in metres).
    // Cards are sized in real metres assuming a ~20 m tree, so leaves stay leaf-sized.
    void buildBroadleafTree(bool midLod, bool birch)
    {
        const float H = 20.0f;   // reference height used to size cards
        glm::vec3 bark = birch ? glm::vec3(0.80f, 0.78f, 0.72f) : glm::vec3(0.24f, 0.19f, 0.14f);
        glm::vec3 leafTint = birch ? glm::vec3(1.05f, 1.05f, 0.95f) : glm::vec3(1.0f);
        int layer = birch ? FOL_BIRCH : FOL_LEAF;
        glm::vec3 endP, endD;
        float trunkH = birch ? 0.58f : 0.52f;
        branchTube({0, 0, 0}, {0.02f * (rand01() - 0.5f), 1, 0.02f * (rand01() - 0.5f)}, trunkH, birch ? 0.013f : 0.018f, birch ? 0.007f : 0.010f,
                   midLod ? 3 : 6, midLod ? 5 : 8, 0.03f, 0.01f, bark, endP, endD);
        float cardM = (birch ? 1.6f : 2.2f) / H;   // card size in unit-tree metres
        if (midLod) {
            leafCards({0, 0.76f, 0}, 0.26f, birch ? 14 : 18, cardM * 2.2f, layer, leafTint);
            return;
        }
        int nMain = 4 + static_cast<int>(rand01() * 3.0f);
        for (int b = 0; b < nMain; ++b) {
            float a = b / static_cast<float>(nMain) * 6.2831853f + rand01() * 0.6f;
            float spread = 0.5f + rand01() * 0.35f;
            glm::vec3 d = glm::normalize(glm::vec3(std::cos(a) * spread, 1.0f + rand01() * 0.5f, std::sin(a) * spread));
            glm::vec3 p2, d2;
            float len = 0.24f + rand01() * 0.14f;
            branchTube(endP, d, len, birch ? 0.006f : 0.009f, 0.0035f, 3, 5, 0.12f, 0.02f, bark, p2, d2);
            int nSub = 3 + static_cast<int>(rand01() * 3.0f);
            for (int c = 0; c < nSub; ++c) {
                float ca = rand01() * 6.2831853f;
                glm::vec3 sd = glm::normalize(d2 * 0.6f + glm::vec3(std::cos(ca), 0.2f + rand01() * 0.5f, std::sin(ca)) * 0.7f);
                glm::vec3 p3, d3;
                branchTube(p2, sd, 0.10f + rand01() * 0.10f, 0.003f, 0.0012f, 2, 4, 0.15f, 0.0f, bark, p3, d3);
                leafCards(p3, 0.10f, 9, cardM, layer, leafTint);
                leafCards(glm::mix(p2, p3, 0.5f), 0.07f, 4, cardM, layer, leafTint);
            }
            leafCards(p2, 0.09f, 5, cardM, layer, leafTint);
        }
    }

    // Conifer: straight trunk, whorls of branch tubes each carrying a needle-spray card.
    void buildConiferTree(bool midLod)
    {
        const float H = 26.0f;
        glm::vec3 bark{0.22f, 0.15f, 0.10f};
        glm::vec3 endP, endD;
        branchTube({0, 0, 0}, {0, 1, 0}, 0.96f, 0.020f, 0.003f, midLod ? 3 : 6, midLod ? 5 : 6, 0.01f, 0.0f, bark, endP, endD);
        int whorls = midLod ? 7 : 12;
        float sprayM = 3.2f / H;
        for (int w = 0; w < whorls; ++w) {
            float t = (w + 0.5f) / static_cast<float>(whorls);
            float y = 0.18f + t * 0.78f;
            float reach = (1.0f - t) * 0.26f + 0.03f;
            int nb = midLod ? 4 : 6;
            for (int b = 0; b < nb; ++b) {
                float a = b / static_cast<float>(nb) * 6.2831853f + w * 0.7f + rand01() * 0.4f;
                glm::vec3 d = glm::normalize(glm::vec3(std::cos(a), -0.25f + rand01() * 0.12f, std::sin(a)));
                glm::vec3 base{0, y, 0};
                if (!midLod) {
                    glm::vec3 pe, de;
                    branchTube(base, d, reach * 0.55f, 0.004f, 0.0015f, 1, 4, 0.0f, 0.0f, bark, pe, de);
                }
                // spray card lying along the branch, slightly drooping, plus a crossed one
                glm::vec3 mid = base + d * (reach * 0.55f);
                glm::vec3 up = glm::normalize(glm::cross(glm::cross(d, glm::vec3(0, 1, 0)), d));
                float half = reach * 0.5f;
                glm::vec3 side = glm::normalize(glm::cross(d, up));
                float sw = std::min(half * 0.9f, sprayM * 0.6f);
                // card(): r = half width axis, u = half height axis, texture "up" = along the branch
                card(mid, side * sw, d * half, FOL_NEEDLE, glm::vec3(1.0f));
                if (!midLod) card(mid, up * sw * 0.8f, d * half, FOL_NEEDLE, glm::vec3(0.95f));
            }
        }
        // crown spike
        card({0, 0.955f, 0}, glm::vec3(0.035f, 0, 0), glm::vec3(0, 0.045f, 0), FOL_NEEDLE, glm::vec3(1.0f));
        card({0, 0.955f, 0}, glm::vec3(0, 0, 0.035f), glm::vec3(0, 0.045f, 0), FOL_NEEDLE, glm::vec3(1.0f));
    }

    // Dead snag: grey trunk, a few bare broken limbs, no foliage.
    void buildDeadTree()
    {
        glm::vec3 bark{0.42f, 0.40f, 0.36f};
        glm::vec3 endP, endD;
        branchTube({0, 0, 0}, {0.04f * (rand01() - 0.5f), 1, 0.04f * (rand01() - 0.5f)}, 0.7f, 0.026f, 0.010f, 5, 7, 0.05f, 0.0f, bark, endP, endD);
        int n = 3 + static_cast<int>(rand01() * 3.0f);
        for (int b = 0; b < n; ++b) {
            float y = 0.35f + rand01() * 0.4f;
            float a = rand01() * 6.2831853f;
            glm::vec3 d = glm::normalize(glm::vec3(std::cos(a), 0.1f + rand01() * 0.6f, std::sin(a)));
            glm::vec3 pe, de;
            branchTube({0, y, 0}, d, 0.10f + rand01() * 0.18f, 0.010f, 0.002f, 2, 4, 0.2f, -0.05f, bark, pe, de);
        }
        // broken top
        card({0, 0.72f, 0}, glm::vec3(0.02f, 0, 0), glm::vec3(0, 0.03f, 0), -1, bark * 0.7f);
    }

    VegMeshRange finishRange(std::uint32_t firstIndex)
    {
        VegMeshRange r;
        r.firstIndex = firstIndex;
        r.indexCount = static_cast<std::uint32_t>(indices.size()) - firstIndex;
        return r;
    }

    void buildMeshes()
    {
        std::uint32_t f0 = 0;
        for (int v = 0; v < kVariants; ++v) {
            rng_ = 900u + v * 7919u;
            f0 = static_cast<std::uint32_t>(indices.size());
            buildBroadleafTree(false, v == 2);   // third variant is birch
            broadleaf[v] = finishRange(f0);
            rng_ = 1300u + v * 104729u;
            f0 = static_cast<std::uint32_t>(indices.size());
            buildConiferTree(false);
            conifer[v] = finishRange(f0);
        }
        rng_ = 4242u;
        f0 = static_cast<std::uint32_t>(indices.size()); buildBroadleafTree(true, false); broadleafMid = finishRange(f0);
        f0 = static_cast<std::uint32_t>(indices.size()); buildConiferTree(true); coniferMid = finishRange(f0);
        rng_ = 5150u;
        f0 = static_cast<std::uint32_t>(indices.size()); buildDeadTree(); deadTree = finishRange(f0);

        // Boulder (textured in the shader).
        rng_ = 777u;
        f0 = static_cast<std::uint32_t>(indices.size());
        blob({0, 0.30f, 0}, 0.5f, 0.7f, 4, 7, {0.30f, 0.29f, 0.27f}, 0.45f);
        boulder = finishRange(f0);

        // Billboard: two crossed cards with the leaf texture for far trees; coloured per subtype in the shader.
        f0 = static_cast<std::uint32_t>(indices.size());
        card({0, 0.5f, 0}, glm::vec3(0.30f, 0, 0), glm::vec3(0, 0.5f, 0), FOL_LEAF, glm::vec3(1.0f));
        card({0, 0.5f, 0}, glm::vec3(0, 0, 0.30f), glm::vec3(0, 0.5f, 0), FOL_LEAF, glm::vec3(1.0f));
        billboard = finishRange(f0);

        // Grass: three crossed cards, unit height 1 m.
        f0 = static_cast<std::uint32_t>(indices.size());
        for (int k = 0; k < 3; ++k) {
            float a = k / 3.0f * 3.14159265f + rand01() * 0.3f;
            glm::vec3 r{std::cos(a) * 0.55f, 0, std::sin(a) * 0.55f};
            card({0, -0.22f, 0}, r, glm::vec3(0, 0.61f, 0), FOL_GRASS, glm::vec3(1.0f), true);
        }
        grass = finishRange(f0);

        // Fern: rosette of frond cards leaning outward.
        f0 = static_cast<std::uint32_t>(indices.size());
        for (int k = 0; k < 7; ++k) {
            float a = k / 7.0f * 6.2831853f + rand01() * 0.4f;
            glm::vec3 d{std::cos(a), 0, std::sin(a)};
            glm::vec3 side{-std::sin(a), 0, std::cos(a)};
            float lean = 0.85f + rand01() * 0.4f;        // radians from vertical
            glm::vec3 up = glm::normalize(glm::vec3(0, std::cos(lean), 0) + d * std::sin(lean));
            float len = 0.9f + rand01() * 0.4f;
            card(glm::vec3(0, -0.12f, 0), side * (len * 0.28f), up * (len * 0.56f), FOL_FERN, glm::vec3(0.95f + rand01() * 0.1f), true);
        }
        fern = finishRange(f0);

        // Moss rock (textured in shader).
        f0 = static_cast<std::uint32_t>(indices.size());
        blob({0, 0.35f, 0}, 0.5f, 0.75f, 4, 7, {0.34f, 0.36f, 0.30f}, 0.5f);
        mossRock = finishRange(f0);

        // Flower: thin stems with a five-petal white head, three per instance.
        f0 = static_cast<std::uint32_t>(indices.size());
        for (int k = 0; k < 3; ++k) {
            float a = rand01() * 6.2831853f, r = rand01() * 0.14f;
            glm::vec3 base{std::cos(a) * r, 0, std::sin(a) * r};
            float h = 0.7f + rand01() * 0.3f;
            glm::vec3 stemCol{0.18f, 0.30f, 0.10f}, n{0, 1, 0};
            int s0 = addVertex((base + glm::vec3(-0.006f, 0, 0)) * kS, n, stemCol);
            int s1 = addVertex((base + glm::vec3(0.006f, 0, 0)) * kS, n, stemCol);
            int s2 = addVertex((base + glm::vec3(0, h, 0)) * kS, n, stemCol);
            tri(s0, s1, s2);
            glm::vec3 head = base + glm::vec3(0, h, 0);
            for (int pth = 0; pth < 5; ++pth) {
                float pa = pth / 5.0f * 6.2831853f;
                glm::vec3 pd{std::cos(pa), 0, std::sin(pa)}, ps{-std::sin(pa), 0, std::cos(pa)};
                glm::vec3 white{0.92f, 0.92f, 0.85f};
                int c0 = addVertex(head * kS, n, {0.85f, 0.75f, 0.30f});
                int c1 = addVertex((head + pd * 0.05f + ps * 0.025f) * kS, n, white);
                int c2 = addVertex((head + pd * 0.09f) * kS, n, white);
                int c3 = addVertex((head + pd * 0.05f - ps * 0.025f) * kS, n, white);
                tri(c0, c1, c2); tri(c0, c2, c3);
            }
        }
        flower = finishRange(f0);

        // Mushroom: pale stem, domed cap. Unit height 1 m (instances are 10-25 cm).
        f0 = static_cast<std::uint32_t>(indices.size());
        {
            glm::vec3 endP, endD;
            branchTube({0, 0, 0}, {0, 1, 0}, 0.62f, 0.10f, 0.08f, 1, 6, 0.0f, 0.0f, {0.80f, 0.74f, 0.62f}, endP, endD);
            blob({0, 0.66f, 0}, 0.36f, 0.55f, 3, 8, {0.50f, 0.26f, 0.12f}, 0.15f);
        }
        mushroom = finishRange(f0);

        // Litter: one flat card on the ground with fallen leaves.
        f0 = static_cast<std::uint32_t>(indices.size());
        {
            glm::vec3 n{0, 1, 0};
            int i0 = addVertex(glm::vec3(-0.5f, 0.01f, -0.5f) * kS, n, glm::vec3(1.0f), {0, 1}, FOL_LITTER);
            int i1 = addVertex(glm::vec3(0.5f, 0.01f, -0.5f) * kS, n, glm::vec3(1.0f), {1, 1}, FOL_LITTER);
            int i2 = addVertex(glm::vec3(0.5f, 0.01f, 0.5f) * kS, n, glm::vec3(1.0f), {1, 0}, FOL_LITTER);
            int i3 = addVertex(glm::vec3(-0.5f, 0.01f, 0.5f) * kS, n, glm::vec3(1.0f), {0, 0}, FOL_LITTER);
            tri(i0, i1, i2); tri(i0, i2, i3);
        }
        litter = finishRange(f0);
    }
};
