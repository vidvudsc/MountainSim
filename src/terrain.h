#pragma once

#include "common.h"
#include "perlin.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
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
    float sunAzimuth = 42.0f;
    float sunElevation = 34.0f;
    bool showWater = true;
    bool showSediment = true;
    int erosionDrops = 18000;
    float erosionRadius = 1.9f;
    float inertia = 0.18f;
    float capacity = 3.6f;
    float minCapacity = 0.02f;
    float depositSpeed = 0.22f;
    float erodeSpeed = 0.16f;
    float evaporation = 0.035f;
    float gravity = 5.5f;
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
                float micro = fbm(centered + glm::vec2(19.1f, 13.3f), 3, settings.frequency * 11.0f, settings.persistence * 0.55f);
                float massif = fbm(centered + glm::vec2(31.0f, -17.0f), 3, 0.78f, 0.58f);
                float islandMask = smoothstep01(1.13f - glm::length(centered * glm::vec2(0.92f, 1.05f)) * 0.52f);
                float mountainMask = smoothstep01((massif - 0.30f) / 0.48f);
                float h = base * 0.72f + detail * 0.22f + micro * 0.06f;
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
        smoothHeightmap(1, 0.08f);
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

    void stepLiveDroplets(float speedScale)
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
        rebuildVertices();
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
        smoothHeightmap(1, 0.028f);
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
        droplet.water *= std::pow(1.0f - settings_.evaporation, stepScale);
        droplet.x = nextX;
        droplet.z = nextZ;
        droplet.age += stepScale;
        if (droplet.water < 0.02f || droplet.age >= 46.0f) droplet.alive = false;
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
        updateHydroDisplay();
        vertices_.assign(kTerrainSize * kTerrainSize, Vertex{});
        indices_.clear();
        indices_.reserve((kTerrainSize - 1) * (kTerrainSize - 1) * 12);
        for (int z = 0; z < kTerrainSize; ++z) {
            for (int x = 0; x < kTerrainSize; ++x) {
                int index = idx(x, z);
                vertices_[index].position = gridToWorld(static_cast<float>(x), static_cast<float>(z), heights_[index]);
                vertices_[index].uv = {static_cast<float>(x) / (kTerrainSize - 1), static_cast<float>(z) / (kTerrainSize - 1)};
                vertices_[index].hydro = {displayWater_[index], displaySediment_[index]};
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
    std::vector<Droplet> liveDroplets_;
    std::vector<glm::vec3> particlePositions_;
    bool hasErosionFlow_ = false;
    std::mt19937 erosionRng_{static_cast<std::uint32_t>(settings_.seed)};
    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
};
