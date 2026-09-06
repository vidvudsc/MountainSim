#pragma once

#include "common.h"
#include "perlin.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <queue>
#include <random>
#include <utility>
#include <vector>

struct TerrainSettings {
    int seed = 1047;
    int octaves = 6;
    float frequency = 1.72f;
    float lacunarity = 2.02f;
    float persistence = 0.48f;
    float peakSharpness = 1.45f;
    float heightScale = 42.0f;
    float snowLevel = 0.67f;
    float waterLevel = 0.10f;
    float waterTint = 1.10f;
    float sedimentTint = 0.85f;
    float fogDensity = 0.0f;
    float surfaceTempC = 8.0f;
    float sunAzimuth = 42.0f;
    float sunElevation = 34.0f;
    bool showWater = true;
    bool showSediment = true;
    int erosionDrops = 120000;
    float erosionRadius = 1.9f * kGridScale;
    float inertia = 0.18f;
    float capacity = 3.6f;
    float minCapacity = 0.02f;
    float depositSpeed = 0.22f;
    float erodeSpeed = 0.16f;
    float evaporation = 0.035f;
    float gravity = 5.5f;
};

enum class TerrainMaterial : int {
    Grass = 0,
    Rock = 1,
    Sediment = 2,
    Water = 3,
};

class Terrain {
public:
    Terrain()
    {
        generate(settings_);
    }

    void generate(const TerrainSettings& settings)
    {
        settings_ = settings;
        Perlin2D perlin(settings.seed);
        heights_.assign(kTerrainSize * kTerrainSize, 0.0f);
        water_.assign(kTerrainSize * kTerrainSize, 0.0f);
        sediment_.assign(kTerrainSize * kTerrainSize, 0.0f);
        displayWater_.assign(kTerrainSize * kTerrainSize, 0.0f);
        displaySediment_.assign(kTerrainSize * kTerrainSize, 0.0f);
        material_.assign(kTerrainSize * kTerrainSize, TerrainMaterial::Grass);
        surfaceTempC_.assign(kTerrainSize * kTerrainSize, settings.surfaceTempC);
        wetness_.assign(kTerrainSize * kTerrainSize, 0.0f);
        snowMass_.assign(kTerrainSize * kTerrainSize, 0.0f);
        iceMass_.assign(kTerrainSize * kTerrainSize, 0.0f);
        hasErosionFlow_ = false;
        liveDroplets_.clear();
        particlePositions_.clear();

        float minHeight = 1e9f;
        float maxHeight = -1e9f;
        for (int z = 0; z < kTerrainSize; ++z) {
            for (int x = 0; x < kTerrainSize; ++x) {
                glm::vec2 uv(static_cast<float>(x) / (kTerrainSize - 1), static_cast<float>(z) / (kTerrainSize - 1));
                glm::vec2 centered = uv * 2.0f - 1.0f;
                auto fbm = [&](glm::vec2 q, int octaves, float baseFrequency, float persistence) {
                    float amp = 1.0f;
                    float freq = baseFrequency;
                    float sum = 0.0f;
                    float norm = 0.0f;
                    for (int o = 0; o < octaves; ++o) {
                        sum += perlin.noise(q.x * freq, q.y * freq) * amp;
                        norm += amp;
                        amp *= persistence;
                        freq *= settings.lacunarity;
                    }
                    return sum / std::max(norm, 0.001f) * 0.5f + 0.5f;
                };

                float base = fbm(centered + glm::vec2(4.2f, -8.7f), settings.octaves, settings.frequency, settings.persistence);
                float detail = fbm(centered + glm::vec2(-11.4f, 5.9f), std::max(3, settings.octaves - 2), settings.frequency * 3.4f, settings.persistence * 0.82f);
                float micro = fbm(centered + glm::vec2(19.1f, 13.3f), 5, settings.frequency * 11.0f, settings.persistence * 0.60f);
                float massif = fbm(centered + glm::vec2(31.0f, -17.0f), 3, 0.78f, 0.58f);
                float islandMask = smoothstep01(1.13f - glm::length(centered * glm::vec2(0.92f, 1.05f)) * 0.52f);
                float mountainMask = smoothstep01((massif - 0.30f) / 0.48f);
                float h = base * 0.72f + detail * 0.22f + micro * 0.075f;
                h = std::pow(glm::clamp(h, 0.0f, 1.0f), settings.peakSharpness);
                h *= glm::mix(0.42f, 1.18f, mountainMask) * islandMask;
                heights_[idx(x, z)] = h;
                minHeight = std::min(minHeight, h);
                maxHeight = std::max(maxHeight, h);
            }
        }
        for (float& h : heights_) {
            h = (h - minHeight) / std::max(maxHeight - minHeight, 0.001f);
            h = h * settings.heightScale;
        }
        smoothHeightmap(static_cast<int>(std::round(kGridScale)), 0.08f);
        rebuildSurfaceState();
        rebuildVertices();
    }

    void erode(int drops)
    {
        beginErosionPass(drops);
        erodeDrops(drops);
        finishErosionPass();
    }

    void beginLiveErosion(int plannedDrops)
    {
        beginErosionPass(plannedDrops);
        rebuildVertices();
    }

    int spawnLiveDroplets(int requested, int maxActive)
    {
        if (requested <= 0 || maxActive <= 0) return 0;
        std::uniform_real_distribution<float> dist(1.0f, static_cast<float>(kTerrainSize - 2));
        int available = std::max(0, maxActive - static_cast<int>(liveDroplets_.size()));
        int spawned = std::min(requested, available);
        liveDroplets_.reserve(static_cast<std::size_t>(maxActive));
        for (int i = 0; i < spawned; ++i) {
            Droplet droplet{};
            droplet.x = dist(erosionRng_);
            droplet.z = dist(erosionRng_);
            droplet.speed = 1.0f;
            droplet.water = 1.0f;
            droplet.alive = true;
            liveDroplets_.push_back(droplet);
        }
        return spawned;
    }

    void stepLiveDroplets(float speedScale, bool rebuild = true)
    {
        if (liveDroplets_.empty()) {
            particlePositions_.clear();
            return;
        }
        speedScale = glm::clamp(speedScale, 0.06f, 1.0f);
        for (Droplet& droplet : liveDroplets_) {
            if (droplet.alive) advanceDroplet(droplet, speedScale);
        }
        liveDroplets_.erase(
            std::remove_if(liveDroplets_.begin(), liveDroplets_.end(), [](const Droplet& droplet) { return !droplet.alive; }),
            liveDroplets_.end()
        );
        particlePositions_.clear();
        particlePositions_.reserve(liveDroplets_.size());
        for (const Droplet& droplet : liveDroplets_) particlePositions_.push_back(terrainPoint(droplet.x, droplet.z));
        hasErosionFlow_ = hasErosionFlow_ || !particlePositions_.empty();
        if (rebuild) rebuildVertices();
    }

    void finishLiveErosion()
    {
        particlePositions_.clear();
        liveDroplets_.clear();
        finishErosionPass();
    }

    void beginErosionPass(int salt)
    {
        auto now = static_cast<std::uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        std::random_device entropy;
        erosionRng_.seed(static_cast<std::uint32_t>(settings_.seed * 1664525u + static_cast<std::uint32_t>(salt) * 1013904223u + now + entropy()));
        std::fill(water_.begin(), water_.end(), 0.0f);
        std::fill(sediment_.begin(), sediment_.end(), 0.0f);
        hasErosionFlow_ = false;
        liveDroplets_.clear();
        particlePositions_.clear();
    }

    void finishErosionPass()
    {
        updateHydroDisplay();
        fillSmallPits(4);
        smoothHeightmap(1, 0.028f);
        rebuildSurfaceState();
        rebuildVertices();
        hasErosionFlow_ = true;
    }

    void erodeDrops(int drops)
    {
        std::uniform_real_distribution<float> dist(1.0f, static_cast<float>(kTerrainSize - 2));
        for (int i = 0; i < drops; ++i) {
            Droplet droplet{};
            droplet.x = dist(erosionRng_);
            droplet.z = dist(erosionRng_);
            droplet.speed = 1.0f;
            droplet.water = 1.0f;
            droplet.alive = true;
            while (droplet.alive) advanceDroplet(droplet, 1.0f);
        }
    }

    void smoothPeaks()
    {
        smoothHeightmap(1, 0.16f);
        rebuildSurfaceState();
        rebuildVertices();
        particlePositions_.clear();
    }

    const std::vector<Vertex>& vertices() const { return vertices_; }
    const std::vector<std::uint32_t>& indices() const { return indices_; }
    const std::vector<glm::vec3>& particlePositions() const { return particlePositions_; }
    bool hasActiveDroplets() const { return !liveDroplets_.empty(); }
    TerrainSettings& settings() { return settings_; }
    const TerrainSettings& settings() const { return settings_; }

    float surfaceHeightAtWorld(float worldX, float worldZ) const
    {
        float x = (worldX / kTerrainWorldSize + 0.5f) * static_cast<float>(kTerrainSize - 1);
        float z = (worldZ / kTerrainWorldSize + 0.5f) * static_cast<float>(kTerrainSize - 1);
        x = glm::clamp(x, 0.0f, static_cast<float>(kTerrainSize - 1));
        z = glm::clamp(z, 0.0f, static_cast<float>(kTerrainSize - 1));
        return sampleHeight(x, z);
    }

    float minSurfaceHeight() const
    {
        float m = 1e9f;
        for (float h : heights_) m = std::min(m, h);
        return (m > 1e8f) ? 0.0f : m;
    }

    float maxSurfaceHeight() const
    {
        float m = -1e9f;
        for (float h : heights_) m = std::max(m, h);
        return (m < -1e8f) ? settings_.heightScale : m;
    }

    // Drive the existing droplet erosion using a precipitation weight grid (column-major,
    // wnx by wnz over the same world extent). Lets real weather carve the terrain on demand.
    void erodeWeighted(const std::vector<float>& weights, int wnx, int wnz, int drops)
    {
        if (weights.empty() || wnx <= 0 || wnz <= 0 || drops <= 0) return;
        std::vector<float> cdf(weights.size());
        float total = 0.0f;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            total += std::max(0.0f, weights[i]);
            cdf[i] = total;
        }
        if (total <= 1e-6f) return;
        beginErosionPass(drops * 7 + 13);
        std::uniform_real_distribution<float> pick(0.0f, total);
        std::uniform_real_distribution<float> jitter(-0.5f, 0.5f);
        for (int d = 0; d < drops; ++d) {
            float r = pick(erosionRng_);
            std::size_t lo = 0, hi = cdf.size() - 1;
            while (lo < hi) {
                std::size_t mid = (lo + hi) / 2;
                if (cdf[mid] < r) lo = mid + 1; else hi = mid;
            }
            int wi = static_cast<int>(lo % static_cast<std::size_t>(wnx));
            int wk = static_cast<int>(lo / static_cast<std::size_t>(wnx));
            float gx = (static_cast<float>(wi) + 0.5f + jitter(erosionRng_)) / static_cast<float>(wnx) * static_cast<float>(kTerrainSize - 1);
            float gz = (static_cast<float>(wk) + 0.5f + jitter(erosionRng_)) / static_cast<float>(wnz) * static_cast<float>(kTerrainSize - 1);
            Droplet droplet{};
            droplet.x = glm::clamp(gx, 1.0f, static_cast<float>(kTerrainSize - 2));
            droplet.z = glm::clamp(gz, 1.0f, static_cast<float>(kTerrainSize - 2));
            droplet.speed = 1.0f;
            droplet.water = 1.0f;
            droplet.alive = true;
            while (droplet.alive) advanceDroplet(droplet, 1.0f);
        }
        finishErosionPass();
    }

    bool occludesSegment(const glm::vec3& from, const glm::vec3& to, float clearance = 0.52f) const
    {
        glm::vec3 delta = to - from;
        float length = glm::length(delta);
        if (length < 0.001f) return false;
        int samples = glm::clamp(static_cast<int>(length / 2.2f), 14, 88);
        for (int i = 3; i < samples - 2; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(samples - 1);
            glm::vec3 p = from + delta * t;
            glm::vec2 grid{};
            if (!worldToGrid(p, grid)) continue;
            if (sampleHeight(grid.x, grid.y) > p.y + clearance) return true;
        }
        return false;
    }

private:
    struct HeightSample {
        float height = 0.0f;
        glm::vec2 gradient{};
    };

    struct Droplet {
        float x = 0.0f;
        float z = 0.0f;
        glm::vec2 dir{0.0f};
        float speed = 1.0f;
        float water = 1.0f;
        float sediment = 0.0f;
        float age = 0.0f;
        bool alive = false;
    };

    static int idx(int x, int z) { return z * kTerrainSize + x; }
    static float smoothstep01(float v)
    {
        v = glm::clamp(v, 0.0f, 1.0f);
        return v * v * (3.0f - 2.0f * v);
    }

    static glm::vec3 gridToWorld(float x, float z, float y)
    {
        float worldX = (x / static_cast<float>(kTerrainSize - 1) - 0.5f) * kTerrainWorldSize;
        float worldZ = (z / static_cast<float>(kTerrainSize - 1) - 0.5f) * kTerrainWorldSize;
        return {worldX, y, worldZ};
    }

    static bool worldToGrid(const glm::vec3& world, glm::vec2& grid)
    {
        float x = (world.x / kTerrainWorldSize + 0.5f) * static_cast<float>(kTerrainSize - 1);
        float z = (world.z / kTerrainWorldSize + 0.5f) * static_cast<float>(kTerrainSize - 1);
        if (x < 0.0f || x > static_cast<float>(kTerrainSize - 1) || z < 0.0f || z > static_cast<float>(kTerrainSize - 1)) return false;
        grid = {x, z};
        return true;
    }

    void rebuildSurfaceState()
    {
        if (material_.size() != heights_.size()) {
            material_.assign(heights_.size(), TerrainMaterial::Grass);
            surfaceTempC_.assign(heights_.size(), settings_.surfaceTempC);
            wetness_.assign(heights_.size(), 0.0f);
            snowMass_.assign(heights_.size(), 0.0f);
            iceMass_.assign(heights_.size(), 0.0f);
        }

        computeHydrology();
        float maxH = std::max(maxSurfaceHeight(), 0.001f);
        for (int z = 0; z < kTerrainSize; ++z) {
            for (int x = 0; x < kTerrainSize; ++x) {
                int index = idx(x, z);
                int xl = std::max(0, x - 1);
                int xr = std::min(kTerrainSize - 1, x + 1);
                int zd = std::max(0, z - 1);
                int zu = std::min(kTerrainSize - 1, z + 1);
                float sx = heights_[idx(xr, z)] - heights_[idx(xl, z)];
                float sz = heights_[idx(x, zu)] - heights_[idx(x, zd)];
                // Rise per world unit so thresholds do not depend on grid resolution;
                // 0.77 reproduces the original 193-grid tuning.
                float stepWorld = kTerrainWorldSize / static_cast<float>(kTerrainSize - 1);
                float slope = glm::clamp(glm::length(glm::vec2(sx, sz)) / (2.0f * stepWorld) * 0.77f, 0.0f, 1.0f);
                float height01 = glm::clamp(heights_[index] / maxH, 0.0f, 1.0f);
                float sediment = (index < static_cast<int>(displaySediment_.size())) ? displaySediment_[index] : 0.0f;
                float water = (index < static_cast<int>(displayWater_.size())) ? displayWater_[index] : 0.0f;

                TerrainMaterial mat = TerrainMaterial::Grass;
                if (water > 0.70f) mat = TerrainMaterial::Water;
                else if (sediment > 0.42f) mat = TerrainMaterial::Sediment;
                else if (slope > 0.46f || height01 > 0.54f) mat = TerrainMaterial::Rock;
                material_[index] = mat;

                wetness_[index] = glm::clamp(water * 0.85f + sediment * 0.25f, 0.0f, 1.0f);

                // Temporary initial condition: altitude seeds snow mass so the existing
                // mountain starts plausibly cold. Rendering no longer invents snow from
                // altitude; future weather/energy steps should mutate this mass directly.
                float coldAltitude = smoothstep01((height01 - settings_.snowLevel) / 0.10f);
                float snow = coldAltitude * (1.0f - smoothstep01((slope - 0.58f) / 0.22f));
                snowMass_[index] = glm::clamp(snow, 0.0f, 1.0f);
                iceMass_[index] = glm::clamp(wetness_[index] * smoothstep01((height01 - settings_.snowLevel + 0.08f) / 0.12f) * 0.35f, 0.0f, 1.0f);

                float materialOffset = 0.0f;
                if (mat == TerrainMaterial::Rock) materialOffset = 1.5f;
                else if (mat == TerrainMaterial::Water) materialOffset = -2.0f;
                else if (mat == TerrainMaterial::Sediment) materialOffset = 0.5f;
                surfaceTempC_[index] = settings_.surfaceTempC - height01 * 16.0f - snowMass_[index] * 7.0f - iceMass_[index] * 4.0f + materialOffset;
            }
        }
        rebuildEcology();
    }

    // ---- ecology maps: drainage, curvature, forest density, sky view -------------------
    // Feed the material shader and vegetation placement so colour and objects agree.
    void rebuildEcology()
    {
        const int N = kTerrainSize;
        const int count = N * N;
        if (flowAccum_.size() != static_cast<std::size_t>(count)) computeHydrology();
        curvature_.assign(count, 0.5f);
        forest_.assign(count, 0.0f);
        skyView_.assign(count, 1.0f);
        float maxH = std::max(maxSurfaceHeight(), 0.001f);
        float stepWorld = kTerrainWorldSize / static_cast<float>(N - 1);

        // Curvature: Laplacian of a blurred height field, robustly normalised.
        {
            std::vector<float> sm = heights_;
            blurField(sm, 5);
            double sum2 = 0.0;
            for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
                int xl = std::max(0, x - 1), xr = std::min(N - 1, x + 1);
                int zd = std::max(0, z - 1), zu = std::min(N - 1, z + 1);
                float lap = sm[z * N + xl] + sm[z * N + xr] + sm[zd * N + x] + sm[zu * N + x] - 4.0f * sm[z * N + x];
                curvature_[z * N + x] = lap;
                sum2 += double(lap) * lap;
            }
            float sd = static_cast<float>(std::sqrt(sum2 / count)) + 1e-6f;
            for (float& c : curvature_) c = glm::clamp(0.5f + 0.5f * (c / (2.5f * sd)), 0.0f, 1.0f);
        }
        // Sky view factor on a coarse grid, upsampled.
        {
            const int ds = 4, dirs = 16, maxSteps = 40;
            int cw = N / ds;
            std::vector<float> coarse(static_cast<std::size_t>(cw) * cw);
            for (int cz = 0; cz < cw; ++cz) for (int cx = 0; cx < cw; ++cx) {
                float fx = cx * float(ds), fz = cz * float(ds);
                float h0 = sampleHeight(fx, fz);
                float sinSum = 0.0f;
                for (int d = 0; d < dirs; ++d) {
                    float ang = d / float(dirs) * 6.2831853f;
                    float dx = std::cos(ang), dz = std::sin(ang);
                    float maxTan = 0.0f;
                    for (int st = 1; st <= maxSteps; ++st) {
                        float sx = fx + dx * st * ds, sz = fz + dz * st * ds;
                        if (sx < 0 || sz < 0 || sx > N - 1 || sz > N - 1) break;
                        float t = (sampleHeight(sx, sz) - h0) / (st * ds * stepWorld);
                        maxTan = std::max(maxTan, t);
                    }
                    sinSum += std::sin(std::atan(maxTan));
                }
                coarse[cz * cw + cx] = glm::clamp(1.0f - sinSum / dirs, 0.0f, 1.0f);
            }
            for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
                float gx = std::min(x / float(ds), cw - 1.001f), gz = std::min(z / float(ds), cw - 1.001f);
                int x0 = int(gx), z0 = int(gz);
                float tx = gx - x0, tz = gz - z0;
                float a = coarse[z0 * cw + x0], b = coarse[z0 * cw + x0 + 1];
                float c = coarse[(z0 + 1) * cw + x0], d = coarse[(z0 + 1) * cw + x0 + 1];
                skyView_[z * N + x] = glm::mix(glm::mix(a, b, tx), glm::mix(c, d, tx), tz);
            }
        }
        // Forest density: below a treeline, gentler ground, clumped, following drainage.
        {
            std::vector<float> wideWater = displayWater_;
            if (!wideWater.empty()) blurField(wideWater, 2);
            Perlin2D perlin(settings_.seed + 4001);
            auto fbm2 = [&](float x, float z, int oct, float freq) {
                float amp = 1.0f, sum = 0.0f, norm = 0.0f;
                for (int o = 0; o < oct; ++o) { sum += perlin.noise(x * freq, z * freq) * amp; norm += amp; amp *= 0.5f; freq *= 2.03f; }
                return sum / norm * 0.5f + 0.5f;
            };
            for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
                int i = z * N + x;
                float wx = (x / float(N - 1) - 0.5f) * kTerrainWorldSize;
                float wz = (z / float(N - 1) - 0.5f) * kTerrainWorldSize;
                float hN = heights_[i] / maxH;
                int xl = std::max(0, x - 1), xr = std::min(N - 1, x + 1);
                int zd = std::max(0, z - 1), zu = std::min(N - 1, z + 1);
                float gx = (heights_[z * N + xr] - heights_[z * N + xl]) / (2.0f * stepWorld);
                float gz = (heights_[zu * N + x] - heights_[zd * N + x]) / (2.0f * stepWorld);
                float ny = 1.0f / std::sqrt(gx * gx + gz * gz + 1.0f);
                float slope = 1.0f - ny;
                float macro2 = fbm2(wx * 0.07f - 31.0f, wz * 0.07f + 17.0f, 4, 1.0f);
                float clump = fbm2(wx * 0.17f - 4.0f, wz * 0.17f + 27.0f, 3, 1.0f);
                float drainage = smoothstep01((flowAccum_[i] - 0.35f) / 0.40f);
                float convex = glm::clamp((0.5f - curvature_[i]) * 2.0f, 0.0f, 1.0f);
                float rock = smoothstep01((slope + convex * 0.08f - 0.16f) / 0.16f);
                float treeLine = 0.48f + (macro2 - 0.5f) * 0.12f;
                float f = 1.0f - smoothstep01((hN - (treeLine - 0.10f)) / 0.13f);
                f *= 1.0f - smoothstep01((slope - 0.16f) / 0.14f);
                f *= smoothstep01((clump * 0.70f + drainage * 0.45f + 0.28f - 0.30f) / 0.30f);
                f *= (1.0f - rock);
                // No forest in water or on the wash tracks; the clearing is widened by a blur so
                // the strip along a channel is a few cells wide, not a one-cell line.
                float water = wideWater.empty() ? 0.0f : wideWater[i];
                f *= 1.0f - smoothstep01((water - 0.10f) / 0.22f);
                f *= 1.0f - smoothstep01((flowAccum_[i] - 0.60f) / 0.18f) * 0.9f;
                forest_[i] = glm::clamp(f, 0.0f, 1.0f);
            }
            blurField(forest_, 1);
        }
        ecoPixels_.resize(static_cast<std::size_t>(count) * 4);
        for (int i = 0; i < count; ++i) {
            ecoPixels_[i * 4 + 0] = static_cast<std::uint8_t>(std::lround(flowAccum_[i] * 255.0f));
            ecoPixels_[i * 4 + 1] = static_cast<std::uint8_t>(std::lround(curvature_[i] * 255.0f));
            ecoPixels_[i * 4 + 2] = static_cast<std::uint8_t>(std::lround(forest_[i] * 255.0f));
            ecoPixels_[i * 4 + 3] = static_cast<std::uint8_t>(std::lround(skyView_[i] * 255.0f));
        }
    }

    // Remove 1-cell pits left by droplets: raise any cell below all eight neighbours to the
    // lowest neighbour. Big depressions survive and become lakes; the speckle does not.
    void fillSmallPits(int passes)
    {
        const int N = kTerrainSize;
        for (int p = 0; p < passes; ++p) {
            for (int z = 1; z < N - 1; ++z) {
                for (int x = 1; x < N - 1; ++x) {
                    float h = heights_[idx(x, z)];
                    float lowest = 1e9f;
                    for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dz) continue;
                        lowest = std::min(lowest, heights_[idx(x + dx, z + dz)]);
                    }
                    if (h < lowest) heights_[idx(x, z)] = lowest + 0.0005f;
                }
            }
        }
    }

    // Drainage and standing water from the height field itself:
    //  - flow accumulation by multiple-flow-direction routing (rivers),
    //  - depression filling (priority flood) for lake levels,
    // then displayWater_ becomes lakes + channels instead of wherever droplets happened to die.
    void computeHydrology()
    {
        const int N = kTerrainSize;
        const int count = N * N;
        flowAccum_.assign(count, 0.0f);
        lakeDepth_.assign(count, 0.0f);

        // Depression fill: flood from the border with a min-heap; water level = max(h, spill).
        {
            std::vector<float> filled(count, 1e9f);
            std::vector<char> done(count, 0);
            using Item = std::pair<float, int>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
            for (int x = 0; x < N; ++x) {
                for (int z : {0, N - 1}) { int i = z * N + x; filled[i] = heights_[i]; done[i] = 1; pq.push({filled[i], i}); }
            }
            for (int z = 1; z < N - 1; ++z) {
                for (int x : {0, N - 1}) { int i = z * N + x; filled[i] = heights_[i]; done[i] = 1; pq.push({filled[i], i}); }
            }
            while (!pq.empty()) {
                auto [level, i] = pq.top(); pq.pop();
                int x = i % N, z = i / N;
                for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dz) continue;
                    int sx = x + dx, sz = z + dz;
                    if (sx < 0 || sz < 0 || sx >= N || sz >= N) continue;
                    int j = sz * N + sx;
                    if (done[j]) continue;
                    done[j] = 1;
                    filled[j] = std::max(heights_[j], level);
                    pq.push({filled[j], j});
                }
            }
            for (int i = 0; i < count; ++i) lakeDepth_[i] = std::max(0.0f, filled[i] - heights_[i]);
            // Route flow over the filled surface so rivers cross lakes instead of dying in them.
            std::vector<int> order(count);
            for (int i = 0; i < count; ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](int a, int b) { return filled[a] > filled[b]; });
            std::vector<float> acc(count, 1.0f);
            for (int i : order) {
                int x = i % N, z = i / N;
                float wsum = 0.0f; float w[8]; int tgt[8]; int n = 0;
                for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dz) continue;
                    int sx = x + dx, sz = z + dz;
                    if (sx < 0 || sz < 0 || sx >= N || sz >= N) continue;
                    int j = sz * N + sx;
                    float drop = filled[i] - filled[j];
                    if (drop <= 0.0f) {
                        // flat lake surface: let water spread evenly toward lower-or-equal cells
                        if (drop < 0.0f || lakeDepth_[i] <= 0.0f) continue;
                        drop = 1e-4f;
                    }
                    float d = (dx && dz) ? 1.41421356f : 1.0f;
                    float wgt = std::pow(drop / d, 1.1f);
                    w[n] = wgt; tgt[n] = j; wsum += wgt; ++n;
                }
                if (wsum <= 0.0f) continue;
                for (int t = 0; t < n; ++t) acc[tgt[t]] += acc[i] * (w[t] / wsum);
            }
            float maxAcc = 1.0f;
            for (float a : acc) maxAcc = std::max(maxAcc, a);
            float logMax = std::log(1.0f + maxAcc);
            for (int i = 0; i < count; ++i) flowAccum_[i] = glm::clamp(std::log(1.0f + acc[i]) / logMax, 0.0f, 1.0f);
            blurField(flowAccum_, 1);
        }

        // Water display stays the droplet-based wash (the look the user prefers); channels from
        // flow accumulation only reinforce it a little so rivers read as continuous lines.
        if (displayWater_.size() != static_cast<std::size_t>(count)) displayWater_.assign(count, 0.0f);
        for (int i = 0; i < count; ++i) {
            float stream = smoothstep01((flowAccum_[i] - 0.70f) / 0.22f);
            displayWater_[i] = glm::clamp(std::max(displayWater_[i], stream * 0.55f), 0.0f, 1.0f);
        }
    }

    const std::vector<float>& lakeDepth() const { return lakeDepth_; }

    void blurField(std::vector<float>& f, int passes)
    {
        const int N = kTerrainSize;
        std::vector<float> tmp(f.size());
        for (int p = 0; p < passes; ++p) {
            for (int z = 0; z < N; ++z) for (int x = 0; x < N; ++x) {
                float sum = 0.0f; int n = 0;
                for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
                    int sx = x + dx, sz = z + dz;
                    if (sx < 0 || sz < 0 || sx >= N || sz >= N) continue;
                    sum += f[sz * N + sx]; ++n;
                }
                tmp[z * N + x] = sum / n;
            }
            f.swap(tmp);
        }
    }

public:
    const std::vector<std::uint8_t>& ecoPixels() const { return ecoPixels_; }
    const std::vector<float>& waterMap() const { return displayWater_; }
    const std::vector<float>& flowMap() const { return flowAccum_; }
    const std::vector<float>& forestMap() const { return forest_; }
    const std::vector<float>& heights() const { return heights_; }
    float slopeAt(int x, int z) const
    {
        const int N = kTerrainSize;
        float stepWorld = kTerrainWorldSize / static_cast<float>(N - 1);
        int xl = std::max(0, x - 1), xr = std::min(N - 1, x + 1);
        int zd = std::max(0, z - 1), zu = std::min(N - 1, z + 1);
        float gx = (heights_[z * N + xr] - heights_[z * N + xl]) / (2.0f * stepWorld);
        float gz = (heights_[zu * N + x] - heights_[zd * N + x]) / (2.0f * stepWorld);
        return 1.0f - 1.0f / std::sqrt(gx * gx + gz * gz + 1.0f);
    }
private:

    void appendSurfaceIndices()
    {
        for (int z = 0; z < kTerrainSize - 1; ++z) {
            for (int x = 0; x < kTerrainSize - 1; ++x) {
                std::uint32_t a = static_cast<std::uint32_t>(idx(x, z));
                std::uint32_t b = static_cast<std::uint32_t>(idx(x + 1, z));
                std::uint32_t c = static_cast<std::uint32_t>(idx(x, z + 1));
                std::uint32_t d = static_cast<std::uint32_t>(idx(x + 1, z + 1));
                indices_.insert(indices_.end(), {a, c, b, b, c, d});
            }
        }
    }

    float sampleHeight(float x, float z) const
    {
        int x0 = glm::clamp(static_cast<int>(std::floor(x)), 0, kTerrainSize - 2);
        int z0 = glm::clamp(static_cast<int>(std::floor(z)), 0, kTerrainSize - 2);
        float tx = x - x0;
        float tz = z - z0;
        float h00 = heights_[idx(x0, z0)];
        float h10 = heights_[idx(x0 + 1, z0)];
        float h01 = heights_[idx(x0, z0 + 1)];
        float h11 = heights_[idx(x0 + 1, z0 + 1)];
        return glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
    }

    HeightSample sampleHeightAndGradient(float x, float z) const
    {
        int x0 = glm::clamp(static_cast<int>(std::floor(x)), 0, kTerrainSize - 2);
        int z0 = glm::clamp(static_cast<int>(std::floor(z)), 0, kTerrainSize - 2);
        float tx = x - x0;
        float tz = z - z0;
        float h00 = heights_[idx(x0, z0)];
        float h10 = heights_[idx(x0 + 1, z0)];
        float h01 = heights_[idx(x0, z0 + 1)];
        float h11 = heights_[idx(x0 + 1, z0 + 1)];
        return {
            glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz),
            glm::vec2((h10 - h00) * (1.0f - tz) + (h11 - h01) * tz, (h01 - h00) * (1.0f - tx) + (h11 - h10) * tx)
        };
    }

    void deposit(float x, float z, float amount)
    {
        int x0 = glm::clamp(static_cast<int>(std::floor(x)), 0, kTerrainSize - 2);
        int z0 = glm::clamp(static_cast<int>(std::floor(z)), 0, kTerrainSize - 2);
        float tx = x - x0;
        float tz = z - z0;
        heights_[idx(x0, z0)] += amount * (1.0f - tx) * (1.0f - tz);
        heights_[idx(x0 + 1, z0)] += amount * tx * (1.0f - tz);
        heights_[idx(x0, z0 + 1)] += amount * (1.0f - tx) * tz;
        heights_[idx(x0 + 1, z0 + 1)] += amount * tx * tz;
    }

    void erodeAt(float x, float z, float amount, float radius)
    {
        int minX = glm::clamp(static_cast<int>(std::floor(x - radius)), 0, kTerrainSize - 1);
        int maxX = glm::clamp(static_cast<int>(std::ceil(x + radius)), 0, kTerrainSize - 1);
        int minZ = glm::clamp(static_cast<int>(std::floor(z - radius)), 0, kTerrainSize - 1);
        int maxZ = glm::clamp(static_cast<int>(std::ceil(z + radius)), 0, kTerrainSize - 1);
        float totalWeight = 0.0f;
        for (int zz = minZ; zz <= maxZ; ++zz) {
            for (int xx = minX; xx <= maxX; ++xx) totalWeight += std::max(0.0f, radius - glm::length(glm::vec2(xx - x, zz - z)));
        }
        if (totalWeight <= 0.0f) return;
        for (int zz = minZ; zz <= maxZ; ++zz) {
            for (int xx = minX; xx <= maxX; ++xx) {
                float weight = std::max(0.0f, radius - glm::length(glm::vec2(xx - x, zz - z))) / totalWeight;
                heights_[idx(xx, zz)] = std::max(0.0f, heights_[idx(xx, zz)] - amount * weight);
            }
        }
    }

    void addHydro(float x, float z, float waterAmount, float sedimentAmount)
    {
        int ix = glm::clamp(static_cast<int>(std::round(x)), 0, kTerrainSize - 1);
        int iz = glm::clamp(static_cast<int>(std::round(z)), 0, kTerrainSize - 1);
        water_[idx(ix, iz)] += waterAmount;
        sediment_[idx(ix, iz)] += sedimentAmount;
    }

    void advanceDroplet(Droplet& droplet, float stepScale)
    {
        if (!droplet.alive) return;
        stepScale = glm::clamp(stepScale, 0.06f, 1.0f);
        HeightSample current = sampleHeightAndGradient(droplet.x, droplet.z);
        droplet.dir = droplet.dir * settings_.inertia - current.gradient * (1.0f - settings_.inertia);
        float len = glm::length(droplet.dir);
        if (len < 0.001f) {
            std::uniform_real_distribution<float> angleDist(0.0f, 6.28318f);
            float angle = angleDist(erosionRng_);
            droplet.dir = glm::vec2(std::cos(angle), std::sin(angle));
        } else {
            droplet.dir /= len;
        }

        float nextX = droplet.x + droplet.dir.x * stepScale;
        float nextZ = droplet.z + droplet.dir.y * stepScale;
        if (nextX < 1.0f || nextX >= kTerrainSize - 2 || nextZ < 1.0f || nextZ >= kTerrainSize - 2) {
            droplet.alive = false;
            return;
        }

        float nextHeight = sampleHeight(nextX, nextZ);
        float deltaHeight = nextHeight - current.height;
        float capacity = std::max(-deltaHeight * droplet.speed * droplet.water * settings_.capacity, settings_.minCapacity);
        if (droplet.sediment > capacity || deltaHeight > 0.0f) {
            float amount = (deltaHeight > 0.0f ? std::min(deltaHeight, droplet.sediment) : (droplet.sediment - capacity) * settings_.depositSpeed) * stepScale;
            droplet.sediment -= amount;
            deposit(droplet.x, droplet.z, amount);
        } else {
            float amount = std::min((capacity - droplet.sediment) * settings_.erodeSpeed, -deltaHeight) * stepScale;
            if (amount > 0.0f) {
                erodeAt(droplet.x, droplet.z, amount, settings_.erosionRadius);
                droplet.sediment += amount;
            }
        }

        addHydro(droplet.x, droplet.z, droplet.water * 0.028f * stepScale, droplet.sediment * 0.017f * stepScale);
        droplet.speed = std::sqrt(std::max(0.0f, droplet.speed * droplet.speed + deltaHeight * settings_.gravity * stepScale));
        droplet.water *= std::pow(1.0f - settings_.evaporation, stepScale / kGridScale);
        droplet.x = nextX;
        droplet.z = nextZ;
        droplet.age += stepScale;
        if (droplet.water < 0.02f || droplet.age >= 46.0f * kGridScale) droplet.alive = false;
    }

    float hydroPercentile(const std::vector<float>& values, float p) const
    {
        std::vector<float> sorted;
        sorted.reserve(values.size());
        for (float v : values) {
            if (v > 0.0f) sorted.push_back(v);
        }
        if (sorted.empty()) return 1.0f;
        std::sort(sorted.begin(), sorted.end());
        std::size_t index = static_cast<std::size_t>(glm::clamp(p, 0.0f, 1.0f) * static_cast<float>(sorted.size() - 1));
        return std::max(sorted[index], 0.001f);
    }

    void updateHydroDisplay()
    {
        displayWater_.resize(water_.size());
        displaySediment_.resize(sediment_.size());
        float waterScale = hydroPercentile(water_, 0.990f);
        float sedimentScale = hydroPercentile(sediment_, 0.985f);
        for (int i = 0; i < static_cast<int>(water_.size()); ++i) {
            float water = glm::clamp(water_[i] / waterScale, 0.0f, 1.0f);
            float sediment = glm::clamp(sediment_[i] / sedimentScale, 0.0f, 1.0f);
            displayWater_[i] = std::pow(smoothstep01((water - 0.16f) / 0.84f), 1.08f);
            displaySediment_[i] = std::pow(smoothstep01((sediment - 0.06f) / 0.94f), 0.76f);
        }
    }

    void smoothHeightmap(int iterations, float strength)
    {
        std::vector<float> next = heights_;
        for (int pass = 0; pass < iterations; ++pass) {
            for (int z = 1; z < kTerrainSize - 1; ++z) {
                for (int x = 1; x < kTerrainSize - 1; ++x) {
                    int index = idx(x, z);
                    float h = heights_[index];
                    float neighborAverage = (heights_[idx(x - 1, z)] + heights_[idx(x + 1, z)] + heights_[idx(x, z - 1)] + heights_[idx(x, z + 1)]) * 0.25f;
                    float height01 = h / std::max(settings_.heightScale, 0.001f);
                    float peakPreserve = glm::smoothstep(0.74f, 0.96f, height01);
                    next[index] = glm::mix(h, neighborAverage, strength * (1.0f - peakPreserve * 0.58f));
                }
            }
            heights_.swap(next);
        }
    }

    void rebuildVertices()
    {
        float step = kTerrainWorldSize / static_cast<float>(kTerrainSize - 1);
        if (!liveDroplets_.empty() || displayWater_.size() != heights_.size()) updateHydroDisplay();
        vertices_.assign(kTerrainSize * kTerrainSize, Vertex{});
        indices_.clear();
        indices_.reserve((kTerrainSize - 1) * (kTerrainSize - 1) * 12);
        for (int z = 0; z < kTerrainSize; ++z) {
            for (int x = 0; x < kTerrainSize; ++x) {
                int index = idx(x, z);
                vertices_[index].position = gridToWorld(static_cast<float>(x), static_cast<float>(z), heights_[index]);
                vertices_[index].uv = {static_cast<float>(x) / (kTerrainSize - 1), static_cast<float>(z) / (kTerrainSize - 1)};
                vertices_[index].hydro = {displayWater_[index], displaySediment_[index]};
                vertices_[index].surface = {
                    static_cast<float>(material_[index] == TerrainMaterial::Grass ? 0 :
                                       material_[index] == TerrainMaterial::Rock ? 1 :
                                       material_[index] == TerrainMaterial::Sediment ? 2 : 3),
                    snowMass_[index],
                    iceMass_[index],
                    surfaceTempC_[index]
                };
            }
        }
        for (int z = 0; z < kTerrainSize; ++z) {
            for (int x = 0; x < kTerrainSize; ++x) {
                int xl = std::max(0, x - 1);
                int xr = std::min(kTerrainSize - 1, x + 1);
                int zd = std::max(0, z - 1);
                int zu = std::min(kTerrainSize - 1, z + 1);
                float hL = heights_[idx(xl, z)];
                float hR = heights_[idx(xr, z)];
                float hD = heights_[idx(x, zd)];
                float hU = heights_[idx(x, zu)];
                vertices_[idx(x, z)].normal = glm::normalize(glm::vec3(hL - hR, 2.0f * step, hD - hU));
            }
        }
        appendSurfaceIndices();
        appendTerrainBlockSides();
    }

    glm::vec3 terrainPoint(float x, float z) const
    {
        return gridToWorld(x, z, sampleHeight(x, z) + 0.42f);
    }

    void appendWallQuad(const glm::vec3& topA, const glm::vec3& topB, const glm::vec3& bottomA, const glm::vec3& bottomB, const glm::vec3& normal)
    {
        std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
        auto makeVertex = [&](const glm::vec3& position) {
            Vertex vertex{};
            vertex.position = position;
            vertex.normal = normal;
            vertex.uv = {position.x / kTerrainWorldSize + 0.5f, position.z / kTerrainWorldSize + 0.5f};
            vertex.hydro = {-1.0f, 0.0f};
            vertex.surface = {1.0f, 0.0f, 0.0f, settings_.surfaceTempC};
            return vertex;
        };
        vertices_.push_back(makeVertex(topA));
        vertices_.push_back(makeVertex(bottomA));
        vertices_.push_back(makeVertex(topB));
        vertices_.push_back(makeVertex(bottomB));
        indices_.insert(indices_.end(), {base, base + 1, base + 2, base + 2, base + 1, base + 3});
    }

    void appendTerrainBlockSides()
    {
        float baseY = -settings_.heightScale * 0.08f - 2.0f;
        for (int x = 0; x < kTerrainSize - 1; ++x) {
            glm::vec3 northA = vertices_[idx(x, 0)].position;
            glm::vec3 northB = vertices_[idx(x + 1, 0)].position;
            appendWallQuad(northA, northB, {northA.x, baseY, northA.z}, {northB.x, baseY, northB.z}, {0.0f, 0.0f, -1.0f});

            glm::vec3 southA = vertices_[idx(x, kTerrainSize - 1)].position;
            glm::vec3 southB = vertices_[idx(x + 1, kTerrainSize - 1)].position;
            appendWallQuad(southB, southA, {southB.x, baseY, southB.z}, {southA.x, baseY, southA.z}, {0.0f, 0.0f, 1.0f});
        }
        for (int z = 0; z < kTerrainSize - 1; ++z) {
            glm::vec3 westA = vertices_[idx(0, z + 1)].position;
            glm::vec3 westB = vertices_[idx(0, z)].position;
            appendWallQuad(westA, westB, {westA.x, baseY, westA.z}, {westB.x, baseY, westB.z}, {-1.0f, 0.0f, 0.0f});

            glm::vec3 eastA = vertices_[idx(kTerrainSize - 1, z)].position;
            glm::vec3 eastB = vertices_[idx(kTerrainSize - 1, z + 1)].position;
            appendWallQuad(eastA, eastB, {eastA.x, baseY, eastA.z}, {eastB.x, baseY, eastB.z}, {1.0f, 0.0f, 0.0f});
        }

        std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
        auto bottomVertex = [](const glm::vec3& position) {
            Vertex vertex{};
            vertex.position = position;
            vertex.normal = {0.0f, -1.0f, 0.0f};
            vertex.uv = {position.x / kTerrainWorldSize + 0.5f, position.z / kTerrainWorldSize + 0.5f};
            vertex.hydro = {-2.0f, 0.0f};
            vertex.surface = {1.0f, 0.0f, 0.0f, 8.0f};
            return vertex;
        };
        float half = kTerrainWorldSize * 0.5f;
        vertices_.push_back(bottomVertex({-half, baseY, -half}));
        vertices_.push_back(bottomVertex({ half, baseY, -half}));
        vertices_.push_back(bottomVertex({-half, baseY,  half}));
        vertices_.push_back(bottomVertex({ half, baseY,  half}));
        indices_.insert(indices_.end(), {base, base + 2, base + 1, base + 1, base + 2, base + 3});
    }

    TerrainSettings settings_{};
    std::vector<float> heights_;
    std::vector<float> water_;
    std::vector<float> sediment_;
    std::vector<float> displayWater_;
    std::vector<float> displaySediment_;
    std::vector<TerrainMaterial> material_;
    std::vector<float> surfaceTempC_;
    std::vector<float> wetness_;
    std::vector<float> snowMass_;
    std::vector<float> iceMass_;
    std::vector<float> flowAccum_;
    std::vector<float> lakeDepth_;
    std::vector<float> curvature_;
    std::vector<float> forest_;
    std::vector<float> skyView_;
    std::vector<std::uint8_t> ecoPixels_;
    std::vector<Droplet> liveDroplets_;
    std::vector<glm::vec3> particlePositions_;
    bool hasErosionFlow_ = false;
    std::mt19937 erosionRng_{static_cast<std::uint32_t>(settings_.seed)};
    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
};
