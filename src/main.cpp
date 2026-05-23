#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kTerrainSize = 161;
constexpr float kTerrainWorldSize = 165.0f;
constexpr int kMaxFramesInFlight = 2;

struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{};
    glm::vec2 hydro{};
};

struct SceneUniforms {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec4 cameraPos{};
    glm::vec4 sunDir{};
    glm::vec4 sunColor{};
    glm::vec4 fogColor{};
    glm::vec4 terrain{};
    glm::vec4 effects{};
};

struct TerrainSettings {
    int seed = 1047;
    int octaves = 6;
    float frequency = 1.72f;
    float lacunarity = 2.02f;
    float gain = 0.46f;
    float ridgeMix = 0.46f;
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
    float erosionRadius = 2.4f;
    float inertia = 0.18f;
    float capacity = 3.6f;
    float minCapacity = 0.02f;
    float depositSpeed = 0.22f;
    float erodeSpeed = 0.16f;
    float evaporation = 0.035f;
    float gravity = 5.5f;
};

struct Camera {
    glm::vec3 position{0.0f, 38.0f, 105.0f};
    glm::vec3 target{0.0f, 16.0f, 0.0f};
    float yaw = -90.0f;
    float pitch = -18.0f;
    float distance = 108.0f;
    float panSpeed = 0.18f;
    bool orbiting = false;
    bool panning = false;
    bool firstMouse = true;
    std::string lastInput = "none";
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;

    glm::vec3 forward() const
    {
        float cy = std::cos(glm::radians(yaw));
        float sy = std::sin(glm::radians(yaw));
        float cp = std::cos(glm::radians(pitch));
        float sp = std::sin(glm::radians(pitch));
        return glm::normalize(glm::vec3(cy * cp, sp, sy * cp));
    }

    void updateFromOrbit()
    {
        position = target - forward() * distance;
    }

    glm::mat4 view() const
    {
        return glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

class Perlin2D {
public:
    explicit Perlin2D(int seed = 0) { reseed(seed); }

    void reseed(int seed)
    {
        std::iota(perm_.begin(), perm_.begin() + 256, 0);
        std::mt19937 rng(static_cast<std::uint32_t>(seed));
        std::shuffle(perm_.begin(), perm_.begin() + 256, rng);
        for (int i = 0; i < 256; ++i) perm_[256 + i] = perm_[i];
    }

    float noise(float x, float y) const
    {
        int xi = static_cast<int>(std::floor(x)) & 255;
        int yi = static_cast<int>(std::floor(y)) & 255;
        float xf = x - std::floor(x);
        float yf = y - std::floor(y);
        float u = fade(xf);
        float v = fade(yf);
        int aa = perm_[perm_[xi] + yi];
        int ab = perm_[perm_[xi] + yi + 1];
        int ba = perm_[perm_[xi + 1] + yi];
        int bb = perm_[perm_[xi + 1] + yi + 1];
        float x1 = glm::mix(grad(aa, xf, yf), grad(ba, xf - 1.0f, yf), u);
        float x2 = glm::mix(grad(ab, xf, yf - 1.0f), grad(bb, xf - 1.0f, yf - 1.0f), u);
        return glm::mix(x1, x2, v);
    }

private:
    static float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
    static float grad(int hash, float x, float y)
    {
        switch (hash & 7) {
            case 0: return x + y;
            case 1: return -x + y;
            case 2: return x - y;
            case 3: return -x - y;
            case 4: return x;
            case 5: return -x;
            case 6: return y;
            default: return -y;
        }
    }
    std::array<int, 512> perm_{};
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
                float px = centered.x * settings.frequency;
                float py = centered.y * settings.frequency;
                float amp = 1.0f;
                float freq = 1.0f;
                float sum = 0.0f;
                float norm = 0.0f;
                for (int o = 0; o < settings.octaves; ++o) {
                    float n = perlin.noise(px * freq, py * freq);
                    sum += n * amp;
                    norm += amp;
                    amp *= settings.gain;
                    freq *= settings.lacunarity;
                }
                float fbm = sum / std::max(norm, 0.001f) * 0.5f + 0.5f;
                float largeForms = perlin.noise(centered.x * 0.72f + 11.4f, centered.y * 0.72f - 3.1f) * 0.5f + 0.5f;
                float continent = smoothstep01(1.10f - glm::length(centered) * 0.46f);
                float mountainMask = smoothstep01((largeForms - 0.18f) / 0.74f);
                float h = glm::mix(fbm * 0.42f, std::pow(fbm, 1.22f), mountainMask) * continent;
                heights_[idx(x, z)] = h;
                minHeight = std::min(minHeight, h);
                maxHeight = std::max(maxHeight, h);
            }
        }
        for (float& h : heights_) {
            h = (h - minHeight) / std::max(maxHeight - minHeight, 0.001f);
            h = std::pow(h, 1.32f) * settings.heightScale;
        }
        smoothHeightmap(2, 0.18f);
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
        smoothHeightmap(2, 0.075f);
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

std::vector<char> readBinaryFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) throw std::runtime_error("Could not open file: " + path);
    std::vector<char> buffer(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    return buffer;
}

void checkVk(VkResult result, const char* message)
{
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(message) + " (" + std::to_string(result) + ")");
}

struct QueueFamilyIndices {
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;
    bool complete() const { return graphics.has_value() && present.has_value(); }
};

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR caps{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanApp {
public:
    void run()
    {
        initWindow();
        initVulkan();
        initImGui();
        mainLoop();
        cleanup();
    }

private:
    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    std::uint32_t graphicsFamily_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline skyPipeline_ = VK_NULL_HANDLE;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkImage skyboxImage_ = VK_NULL_HANDLE;
    VkDeviceMemory skyboxMemory_ = VK_NULL_HANDLE;
    VkImageView skyboxView_ = VK_NULL_HANDLE;
    VkSampler skyboxSampler_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory_ = VK_NULL_HANDLE;
    std::array<VkBuffer, kMaxFramesInFlight> uniformBuffers_{};
    std::array<VkDeviceMemory, kMaxFramesInFlight> uniformMemories_{};
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxFramesInFlight> descriptorSets_{};
    VkDescriptorPool imguiPool_ = VK_NULL_HANDLE;
    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable_{};
    std::array<VkSemaphore, kMaxFramesInFlight> renderFinished_{};
    std::array<VkFence, kMaxFramesInFlight> inFlight_{};
    std::size_t currentFrame_ = 0;
    Terrain terrain_;
    Camera camera_;
    bool terrainDirty_ = true;
    bool wireframe_ = false;
    bool showFlowParticles_ = true;
    bool erosionAnimating_ = false;
    float erosionSpeed_ = 0.35f;
    bool framebufferResized_ = false;

    void initWindow()
    {
        if (!glfwInit()) throw std::runtime_error("Failed to initialize GLFW");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window_ = glfwCreateWindow(1440, 900, "Mountains - Vulkan Terrain", nullptr, nullptr);
        if (!window_) throw std::runtime_error("Failed to create window");
        camera_.updateFromOrbit();
        glfwSetWindowUserPointer(window_, this);
        glfwSetScrollCallback(window_, [](GLFWwindow* window, double xOffset, double yOffset) {
            auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
            if (app) app->onScroll(xOffset, yOffset);
        });
        glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* window, int, int) {
            auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
            if (app) app->framebufferResized_ = true;
        });
    }

    void initVulkan()
    {
        createInstance();
        checkVk(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "Failed to create surface");
        pickPhysicalDevice();
        createDevice();
        createSwapchain();
        createRenderPass();
        createDescriptorSetLayout();
        createPipeline();
        createDepthResources();
        createFramebuffers();
        createCommandPool();
        createSkyboxResources();
        createTerrainBuffers();
        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();
        createCommandBuffers();
        createSyncObjects();
    }

    void createInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Mountains";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "Mountains";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        std::uint32_t glfwCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwCount);
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwCount);
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "Failed to create Vulkan instance");
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device)
    {
        QueueFamilyIndices indices;
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
        for (std::uint32_t i = 0; i < count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphics = i;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present);
            if (present) indices.present = i;
            if (indices.complete()) break;
        }
        return indices;
    }

    SwapchainSupport querySwapchain(VkPhysicalDevice device)
    {
        SwapchainSupport support;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &support.caps);
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &count, nullptr);
        support.formats.resize(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &count, support.formats.data());
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &count, nullptr);
        support.presentModes.resize(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &count, support.presentModes.data());
        return support;
    }

    void pickPhysicalDevice()
    {
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) throw std::runtime_error("No Vulkan physical devices found");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        for (VkPhysicalDevice device : devices) {
            QueueFamilyIndices indices = findQueueFamilies(device);
            SwapchainSupport swap = querySwapchain(device);
            if (indices.complete() && !swap.formats.empty() && !swap.presentModes.empty()) {
                physicalDevice_ = device;
                graphicsFamily_ = *indices.graphics;
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(device, &props);
                std::cout << "Vulkan GPU: " << props.deviceName << "\n";
                return;
            }
        }
        throw std::runtime_error("No suitable Vulkan device found");
    }

    void createDevice()
    {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
        std::set<std::uint32_t> uniqueFamilies{*indices.graphics, *indices.present};
        float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queues;
        for (std::uint32_t family : uniqueFamilies) {
            VkDeviceQueueCreateInfo queue{};
            queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            queues.push_back(queue);
        }
        const std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset"};
        VkPhysicalDeviceFeatures features{};
        features.fillModeNonSolid = VK_TRUE;
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
        createInfo.pQueueCreateInfos = queues.data();
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.pEnabledFeatures = &features;
        checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "Failed to create Vulkan device");
        vkGetDeviceQueue(device_, *indices.graphics, 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, *indices.present, 0, &presentQueue_);
    }

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
    {
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return format;
        }
        return formats.front();
    }

    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes)
    {
        for (VkPresentModeKHR mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps)
    {
        if (caps.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) return caps.currentExtent;
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        return {
            std::clamp(static_cast<std::uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(static_cast<std::uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height)
        };
    }

    void createSwapchain()
    {
        SwapchainSupport support = querySwapchain(physicalDevice_);
        VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
        VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
        swapchainExtent_ = chooseExtent(support.caps);
        std::uint32_t imageCount = support.caps.minImageCount + 1;
        if (support.caps.maxImageCount > 0) imageCount = std::min(imageCount, support.caps.maxImageCount);

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = swapchainExtent_;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
        std::array<std::uint32_t, 2> families{*indices.graphics, *indices.present};
        if (indices.graphics != indices.present) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = families.data();
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        createInfo.preTransform = support.caps.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        checkVk(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "Failed to create swapchain");
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        swapchainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
        swapchainFormat_ = surfaceFormat.format;
        swapchainImageViews_.resize(swapchainImages_.size());
        for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
            swapchainImageViews_[i] = createImageView(swapchainImages_[i], swapchainFormat_, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect)
    {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = image;
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = format;
        createInfo.subresourceRange.aspectMask = aspect;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        checkVk(vkCreateImageView(device_, &createInfo, nullptr, &view), "Failed to create image view");
        return view;
    }

    void createRenderPass()
    {
        VkAttachmentDescription color{};
        color.format = swapchainFormat_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentDescription depth{};
        depth.format = VK_FORMAT_D32_SFLOAT;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        std::array<VkAttachmentDescription, 2> attachments{color, depth};
        VkRenderPassCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        createInfo.pAttachments = attachments.data();
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 1;
        createInfo.pDependencies = &dependency;
        checkVk(vkCreateRenderPass(device_, &createInfo, nullptr, &renderPass_), "Failed to create render pass");
    }

    void createDescriptorSetLayout()
    {
        VkDescriptorSetLayoutBinding ubo{};
        ubo.binding = 0;
        ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo.descriptorCount = 1;
        ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding skybox{};
        skybox.binding = 1;
        skybox.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        skybox.descriptorCount = 1;
        skybox.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{ubo, skybox};
        VkDescriptorSetLayoutCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        createInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        createInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device_, &createInfo, nullptr, &descriptorSetLayout_), "Failed to create descriptor set layout");
    }

    VkShaderModule createShaderModule(const std::vector<char>& code)
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        checkVk(vkCreateShaderModule(device_, &createInfo, nullptr, &module), "Failed to create shader module");
        return module;
    }

    void createPipeline()
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout_;
        checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_), "Failed to create pipeline layout");
        createSkyPipeline();
        createTerrainPipeline();
    }

    void createTerrainPipeline()
    {
        VkShaderModule vert = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/terrain.vert.spv"));
        VkShaderModule frag = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/terrain.frag.spv"));
        VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vert;
        vertStage.pName = "main";
        VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = frag;
        fragStage.pName = "main";
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{vertStage, fragStage};

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription, 4> attributes{{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, hydro)}
        }};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;
        checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_), "Failed to create graphics pipeline");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
    }

    void createSkyPipeline()
    {
        VkShaderModule vert = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/sky.vert.spv"));
        VkShaderModule frag = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/sky.frag.spv"));
        VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vert;
        vertStage.pName = "main";
        VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = frag;
        fragStage.pName = "main";
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{vertStage, fragStage};

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;
        checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skyPipeline_), "Failed to create sky pipeline");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
    }

    std::uint32_t findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
        for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
        }
        throw std::runtime_error("No suitable memory type found");
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        createInfo.size = size;
        createInfo.usage = usage;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateBuffer(device_, &createInfo, nullptr, &buffer), "Failed to create buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buffer, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, properties);
        checkVk(vkAllocateMemory(device_, &alloc, nullptr, &memory), "Failed to allocate buffer memory");
        vkBindBufferMemory(device_, buffer, memory, 0);
    }

    VkCommandBuffer beginSingleTimeCommands()
    {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandPool = commandPool_;
        alloc.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        checkVk(vkAllocateCommandBuffers(device_, &alloc, &commandBuffer), "Failed to allocate one-shot command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(commandBuffer, &begin), "Failed to begin one-shot command buffer");
        return commandBuffer;
    }

    void endSingleTimeCommands(VkCommandBuffer commandBuffer)
    {
        checkVk(vkEndCommandBuffer(commandBuffer), "Failed to end one-shot command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        checkVk(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "Failed to submit one-shot command buffer");
        vkQueueWaitIdle(graphicsQueue_);
        vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    }

    void copyBufferToImage(VkBuffer buffer, VkImage image, std::uint32_t width, std::uint32_t height, std::uint32_t layers)
    {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(layers);
        VkDeviceSize layerSize = static_cast<VkDeviceSize>(width) * height * 4;
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            VkBufferImageCopy region{};
            region.bufferOffset = layerSize * layer;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {width, height, 1};
            regions.push_back(region);
        }
        vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());
        endSingleTimeCommands(commandBuffer);
    }

    void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, std::uint32_t layers)
    {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = layers;
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        endSingleTimeCommands(commandBuffer);
    }

    void createImage(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory)
    {
        VkImageCreateInfo createInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.extent = {width, height, 1};
        createInfo.mipLevels = 1;
        createInfo.arrayLayers = 1;
        createInfo.format = format;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        createInfo.usage = usage;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateImage(device_, &createInfo, nullptr, &image), "Failed to create image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vkAllocateMemory(device_, &alloc, nullptr, &memory), "Failed to allocate image memory");
        vkBindImageMemory(device_, image, memory, 0);
    }

    void createSkyboxResources()
    {
        const std::array<std::string, 6> faces{
            "assets/skybox/envmap_miramar/miramar_rt.tga",
            "assets/skybox/envmap_miramar/miramar_lf.tga",
            "assets/skybox/envmap_miramar/miramar_up.tga",
            "assets/skybox/envmap_miramar/miramar_dn.tga",
            "assets/skybox/envmap_miramar/miramar_ft.tga",
            "assets/skybox/envmap_miramar/miramar_bk.tga",
        };
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<unsigned char> pixels;
        for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            const std::string& face = faces[faceIndex];
            int w = 0;
            int h = 0;
            int c = 0;
            stbi_uc* data = stbi_load(face.c_str(), &w, &h, &c, STBI_rgb_alpha);
            if (!data) throw std::runtime_error("Failed to load skybox face: " + face);
            if (pixels.empty()) {
                width = w;
                height = h;
                channels = 4;
                pixels.resize(static_cast<std::size_t>(width) * height * channels * faces.size());
            }
            if (w != width || h != height) {
                stbi_image_free(data);
                throw std::runtime_error("Skybox faces must have matching sizes");
            }
            std::memcpy(pixels.data() + static_cast<std::size_t>(width) * height * channels * faceIndex, data, static_cast<std::size_t>(width) * height * channels);
            stbi_image_free(data);
        }

        VkDeviceSize imageSize = static_cast<VkDeviceSize>(pixels.size());
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory);
        void* mapped = nullptr;
        vkMapMemory(device_, stagingMemory, 0, imageSize, 0, &mapped);
        std::memcpy(mapped, pixels.data(), pixels.size());
        vkUnmapMemory(device_, stagingMemory);

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 6;
        imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateImage(device_, &imageInfo, nullptr, &skyboxImage_), "Failed to create skybox image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, skyboxImage_, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vkAllocateMemory(device_, &alloc, nullptr, &skyboxMemory_), "Failed to allocate skybox memory");
        vkBindImageMemory(device_, skyboxImage_, skyboxMemory_, 0);

        transitionImageLayout(skyboxImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6);
        copyBufferToImage(stagingBuffer, skyboxImage_, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 6);
        transitionImageLayout(skyboxImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6);
        vkDestroyBuffer(device_, stagingBuffer, nullptr);
        vkFreeMemory(device_, stagingMemory, nullptr);

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = skyboxImage_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 6;
        checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &skyboxView_), "Failed to create skybox image view");

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 1.0f;
        checkVk(vkCreateSampler(device_, &sampler, nullptr, &skyboxSampler_), "Failed to create skybox sampler");
    }

    void createDepthResources()
    {
        createImage(swapchainExtent_.width, swapchainExtent_.height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depthImage_, depthMemory_);
        depthView_ = createImageView(depthImage_, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void createFramebuffers()
    {
        framebuffers_.resize(swapchainImageViews_.size());
        for (std::size_t i = 0; i < swapchainImageViews_.size(); ++i) {
            std::array<VkImageView, 2> attachments{swapchainImageViews_[i], depthView_};
            VkFramebufferCreateInfo createInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            createInfo.renderPass = renderPass_;
            createInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            createInfo.pAttachments = attachments.data();
            createInfo.width = swapchainExtent_.width;
            createInfo.height = swapchainExtent_.height;
            createInfo.layers = 1;
            checkVk(vkCreateFramebuffer(device_, &createInfo, nullptr, &framebuffers_[i]), "Failed to create framebuffer");
        }
    }

    void cleanupSwapchain()
    {
        for (VkFramebuffer framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffers_.clear();
        if (depthView_) vkDestroyImageView(device_, depthView_, nullptr);
        if (depthImage_) vkDestroyImage(device_, depthImage_, nullptr);
        if (depthMemory_) vkFreeMemory(device_, depthMemory_, nullptr);
        depthView_ = VK_NULL_HANDLE;
        depthImage_ = VK_NULL_HANDLE;
        depthMemory_ = VK_NULL_HANDLE;
        for (VkImageView view : swapchainImageViews_) vkDestroyImageView(device_, view, nullptr);
        swapchainImageViews_.clear();
        swapchainImages_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    void recreateSwapchain()
    {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        while (width == 0 || height == 0) {
            glfwWaitEvents();
            glfwGetFramebufferSize(window_, &width, &height);
        }
        vkDeviceWaitIdle(device_);
        cleanupSwapchain();
        createSwapchain();
        createDepthResources();
        createFramebuffers();
        ImGui_ImplVulkan_SetMinImageCount(2);
        framebufferResized_ = false;
    }

    void createCommandPool()
    {
        VkCommandPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        createInfo.queueFamilyIndex = graphicsFamily_;
        checkVk(vkCreateCommandPool(device_, &createInfo, nullptr, &commandPool_), "Failed to create command pool");
    }

    void createTerrainBuffers()
    {
        createBuffer(sizeof(Vertex) * terrain_.vertices().size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, vertexBuffer_, vertexMemory_);
        createBuffer(sizeof(std::uint32_t) * terrain_.indices().size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, indexBuffer_, indexMemory_);
        uploadTerrain();
    }

    void uploadTerrain()
    {
        void* data = nullptr;
        vkMapMemory(device_, vertexMemory_, 0, VK_WHOLE_SIZE, 0, &data);
        std::memcpy(data, terrain_.vertices().data(), sizeof(Vertex) * terrain_.vertices().size());
        vkUnmapMemory(device_, vertexMemory_);
        vkMapMemory(device_, indexMemory_, 0, VK_WHOLE_SIZE, 0, &data);
        std::memcpy(data, terrain_.indices().data(), sizeof(std::uint32_t) * terrain_.indices().size());
        vkUnmapMemory(device_, indexMemory_);
        terrainDirty_ = false;
    }

    void createUniformBuffers()
    {
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uniformBuffers_[i], uniformMemories_[i]);
        }
    }

    void createDescriptorPool()
    {
        std::array<VkDescriptorPoolSize, 2> poolSizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxFramesInFlight},
        }};
        VkDescriptorPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        createInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        createInfo.pPoolSizes = poolSizes.data();
        createInfo.maxSets = kMaxFramesInFlight;
        checkVk(vkCreateDescriptorPool(device_, &createInfo, nullptr, &descriptorPool_), "Failed to create descriptor pool");
    }

    void createDescriptorSets()
    {
        std::array<VkDescriptorSetLayout, kMaxFramesInFlight> layouts{};
        layouts.fill(descriptorSetLayout_);
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool_;
        alloc.descriptorSetCount = kMaxFramesInFlight;
        alloc.pSetLayouts = layouts.data();
        checkVk(vkAllocateDescriptorSets(device_, &alloc, descriptorSets_.data()), "Failed to allocate descriptor sets");
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            VkDescriptorBufferInfo bufferInfo{uniformBuffers_[i], 0, sizeof(SceneUniforms)};
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = skyboxView_;
            imageInfo.sampler = skyboxSampler_;
            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = descriptorSets_[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &bufferInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = descriptorSets_[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    void createCommandBuffers()
    {
        commandBuffers_.resize(kMaxFramesInFlight);
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = commandPool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size());
        checkVk(vkAllocateCommandBuffers(device_, &alloc, commandBuffers_.data()), "Failed to allocate command buffers");
    }

    void createSyncObjects()
    {
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            checkVk(vkCreateSemaphore(device_, &semaphore, nullptr, &imageAvailable_[i]), "Failed to create semaphore");
            checkVk(vkCreateSemaphore(device_, &semaphore, nullptr, &renderFinished_[i]), "Failed to create semaphore");
            checkVk(vkCreateFence(device_, &fence, nullptr, &inFlight_[i]), "Failed to create fence");
        }
    }

    void initImGui()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 5.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.PopupRounding = 5.0f;
        style.WindowBorderSize = 1.0f;
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.070f, 0.88f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.045f, 0.050f, 0.056f, 0.82f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.060f, 0.068f, 0.96f);
        colors[ImGuiCol_Border] = ImVec4(0.22f, 0.26f, 0.28f, 0.42f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.040f, 0.048f, 0.055f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.100f, 0.110f, 1.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.095f, 0.110f, 0.120f, 0.92f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.135f, 0.160f, 0.170f, 0.95f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.165f, 0.195f, 0.205f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.110f, 0.135f, 0.145f, 0.95f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.165f, 0.205f, 0.215f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.200f, 0.255f, 0.265f, 1.00f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.36f, 0.50f, 0.48f, 1.00f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.48f, 0.65f, 0.60f, 1.00f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.50f, 0.72f, 0.66f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.130f, 0.160f, 0.170f, 0.88f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.180f, 0.225f, 0.235f, 0.95f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.220f, 0.280f, 0.285f, 1.00f);
        ImGui_ImplGlfw_InitForVulkan(window_, true);
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 64;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &imguiPool_), "Failed to create ImGui descriptor pool");
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_2;
        info.Instance = instance_;
        info.PhysicalDevice = physicalDevice_;
        info.Device = device_;
        info.QueueFamily = graphicsFamily_;
        info.Queue = graphicsQueue_;
        info.DescriptorPool = imguiPool_;
        info.MinImageCount = 2;
        info.ImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
        info.PipelineInfoMain.RenderPass = renderPass_;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_Init(&info);
    }

    void onScroll(double xOffset, double yOffset)
    {
        if (ImGui::GetIO().WantCaptureMouse && !camera_.orbiting && !camera_.panning) return;
        glm::vec3 forward = glm::normalize(camera_.target - camera_.position);
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        float scale = camera_.distance * camera_.panSpeed * 0.055f;
        camera_.distance *= std::pow(0.86f, static_cast<float>(yOffset));
        camera_.distance = glm::clamp(camera_.distance, 16.0f, 260.0f);
        camera_.target += (-right * static_cast<float>(xOffset)) * scale;
        camera_.lastInput = "two-finger zoom";
        camera_.updateFromOrbit();
    }

    void processInput(float dt)
    {
        bool leftMouse = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool rightMouse = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        bool middleMouse = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
        bool shiftDown = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        bool canStartCameraDrag = !ImGui::GetIO().WantCaptureMouse || camera_.orbiting || camera_.panning;
        bool wantsPan = (middleMouse || (rightMouse && shiftDown)) && canStartCameraDrag;
        bool wantsOrbit = (leftMouse || (rightMouse && !shiftDown)) && canStartCameraDrag;
        if (wantsPan || wantsOrbit) {
            if (!camera_.orbiting && !camera_.panning) {
                glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                camera_.firstMouse = true;
            }
            camera_.orbiting = wantsOrbit;
            camera_.panning = wantsPan;
        } else if (camera_.orbiting || camera_.panning) {
            camera_.orbiting = false;
            camera_.panning = false;
            glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        if (camera_.orbiting || camera_.panning) {
            double mouseX = 0.0;
            double mouseY = 0.0;
            glfwGetCursorPos(window_, &mouseX, &mouseY);
            if (camera_.firstMouse) {
                camera_.lastMouseX = mouseX;
                camera_.lastMouseY = mouseY;
                camera_.firstMouse = false;
            }
            float dx = static_cast<float>(mouseX - camera_.lastMouseX);
            float dy = static_cast<float>(camera_.lastMouseY - mouseY);
            camera_.lastMouseX = mouseX;
            camera_.lastMouseY = mouseY;
            if (camera_.orbiting) {
                camera_.yaw += dx * 0.10f;
                camera_.pitch = glm::clamp(camera_.pitch + dy * 0.10f, -86.0f, 86.0f);
                camera_.lastInput = "drag orbit";
            } else {
                glm::vec3 forward = glm::normalize(camera_.target - camera_.position);
                glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                glm::vec3 up = glm::normalize(glm::cross(right, forward));
                float scale = camera_.distance * camera_.panSpeed * 0.004f;
                camera_.target += (-right * dx + up * dy) * scale;
                camera_.lastInput = "drag pan";
            }
            camera_.updateFromOrbit();
        }
        if (!ImGui::GetIO().WantTextInput) {
            glm::vec3 forward = glm::normalize(camera_.target - camera_.position);
            glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
            glm::vec3 planarForward = glm::normalize(glm::vec3(forward.x, 0.0f, forward.z));
            float velocity = camera_.distance * 0.42f * dt;
            if (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) velocity *= 2.0f;
            if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) camera_.target += planarForward * velocity;
            if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) camera_.target -= planarForward * velocity;
            if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) camera_.target -= right * velocity;
            if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) camera_.target += right * velocity;
            if (glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) camera_.target.y += velocity;
            if (glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) camera_.target.y -= velocity;
            camera_.updateFromOrbit();
        }
    }

    glm::vec3 sunDirection() const
    {
        float azimuth = glm::radians(terrain_.settings().sunAzimuth);
        float elevation = glm::radians(terrain_.settings().sunElevation);
        return glm::normalize(glm::vec3(std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth)));
    }

    void updateUniformBuffer(std::uint32_t frame)
    {
        TerrainSettings& s = terrain_.settings();
        glm::vec3 sunDir = sunDirection();
        float sunWarmth = glm::smoothstep(0.0f, 0.55f, sunDir.y);
        glm::vec3 sunColor = glm::mix(glm::vec3(1.0f, 0.58f, 0.34f), glm::vec3(1.0f, 0.93f, 0.78f), sunWarmth);
        glm::vec3 fogColor = glm::mix(glm::vec3(0.30f, 0.36f, 0.42f), glm::vec3(0.42f, 0.56f, 0.68f), sunWarmth);
        SceneUniforms ubo{};
        ubo.view = camera_.view();
        ubo.proj = glm::perspective(glm::radians(58.0f), static_cast<float>(swapchainExtent_.width) / static_cast<float>(swapchainExtent_.height), 0.1f, 650.0f);
        ubo.proj[1][1] *= -1.0f;
        ubo.cameraPos = glm::vec4(camera_.position, 1.0f);
        ubo.sunDir = glm::vec4(sunDir, 0.0f);
        ubo.sunColor = glm::vec4(sunColor, 1.0f);
        ubo.fogColor = glm::vec4(fogColor, 1.0f);
        ubo.terrain = glm::vec4(s.heightScale, s.snowLevel, s.waterLevel, s.fogDensity);
        ubo.effects = glm::vec4(s.waterTint, s.sedimentTint, s.showWater ? 1.0f : 0.0f, s.showSediment ? 1.0f : 0.0f);
        void* data = nullptr;
        vkMapMemory(device_, uniformMemories_[frame], 0, sizeof(ubo), 0, &data);
        std::memcpy(data, &ubo, sizeof(ubo));
        vkUnmapMemory(device_, uniformMemories_[frame]);
    }

    void updateErosionAnimation()
    {
        if (!erosionAnimating_) return;
        terrain_.stepLiveDroplets(erosionSpeed_);
        terrainDirty_ = true;
        if (!terrain_.hasActiveDroplets()) {
            erosionAnimating_ = false;
            terrain_.finishLiveErosion();
        }
    }

    void drawControls()
    {
        TerrainSettings editable = terrain_.settings();
        ImGui::SetNextWindowPos(ImVec2(18, 18), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 285), ImGuiCond_FirstUseEver);
        ImGui::Begin("Terrain");
        bool regenerate = false;
        regenerate |= ImGui::InputInt("Seed", &editable.seed);
        regenerate |= ImGui::SliderInt("Octaves", &editable.octaves, 1, 10);
        regenerate |= ImGui::SliderFloat("Noise frequency", &editable.frequency, 0.5f, 7.0f, "%.2f");
        regenerate |= ImGui::SliderFloat("Lacunarity", &editable.lacunarity, 1.4f, 3.2f, "%.2f");
        regenerate |= ImGui::SliderFloat("Gain", &editable.gain, 0.25f, 0.72f, "%.2f");
        regenerate |= ImGui::SliderFloat("Ridge mix", &editable.ridgeMix, 0.0f, 1.0f, "%.2f");
        regenerate |= ImGui::SliderFloat("Height scale", &editable.heightScale, 12.0f, 78.0f, "%.1f");
        if (regenerate || ImGui::Button("Regenerate heightmap")) {
            erosionAnimating_ = false;
            terrain_.generate(editable);
            terrainDirty_ = true;
        } else {
            terrain_.settings() = editable;
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(18, 318), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 292), ImGuiCond_FirstUseEver);
        ImGui::Begin("Erosion");
        ImGui::SliderInt("Drops", &terrain_.settings().erosionDrops, 1000, 45000);
        ImGui::SliderFloat("Radius", &terrain_.settings().erosionRadius, 1.0f, 5.0f, "%.1f");
        ImGui::SliderFloat("Inertia", &terrain_.settings().inertia, 0.0f, 0.92f, "%.2f");
        ImGui::SliderFloat("Capacity", &terrain_.settings().capacity, 1.0f, 10.0f, "%.1f");
        ImGui::SliderFloat("Evaporation", &terrain_.settings().evaporation, 0.005f, 0.12f, "%.3f");
        ImGui::Checkbox("Show particles", &showFlowParticles_);
        ImGui::SliderFloat("Simulation speed", &erosionSpeed_, 0.06f, 1.0f, "%.2f");
        if (erosionAnimating_) ImGui::BeginDisabled();
        if (ImGui::Button(erosionAnimating_ ? "Simulating" : "Start simulation")) {
            int drops = terrain_.settings().erosionDrops;
            terrain_.beginLiveErosion(drops);
            terrain_.spawnLiveDroplets(drops, drops);
            erosionAnimating_ = true;
            terrainDirty_ = true;
        }
        if (erosionAnimating_) ImGui::EndDisabled();
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(365, 18), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 270), ImGuiCond_FirstUseEver);
        ImGui::Begin("Look");
        ImGui::SliderFloat("Snow level", &terrain_.settings().snowLevel, 0.35f, 0.9f, "%.2f");
        ImGui::SliderFloat("Water line", &terrain_.settings().waterLevel, 0.03f, 0.42f, "%.2f");
        ImGui::SliderFloat("Water tint", &terrain_.settings().waterTint, 0.0f, 5.0f, "%.1f");
        ImGui::SliderFloat("Sediment tint", &terrain_.settings().sedimentTint, 0.0f, 5.0f, "%.1f");
        ImGui::Checkbox("Show water", &terrain_.settings().showWater);
        ImGui::Checkbox("Show sediment", &terrain_.settings().showSediment);
        ImGui::SliderFloat("Fog density", &terrain_.settings().fogDensity, 0.0f, 0.03f, "%.3f");
        ImGui::SliderFloat("Sun azimuth", &terrain_.settings().sunAzimuth, -180.0f, 180.0f, "%.0f deg");
        ImGui::SliderFloat("Sun elevation", &terrain_.settings().sunElevation, 2.0f, 88.0f, "%.0f deg");
        ImGui::Checkbox("Wireframe", &wireframe_);
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(365, 306), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 185), ImGuiCond_FirstUseEver);
        ImGui::Begin("Camera");
        ImGui::SliderFloat("Orbit distance", &camera_.distance, 16.0f, 260.0f, "%.0f");
        ImGui::SliderFloat("Pan speed", &camera_.panSpeed, 0.05f, 0.55f, "%.2f");
        ImGui::Text("Last input: %s", camera_.lastInput.c_str());
        if (ImGui::Button("Reset view")) {
            camera_.target = glm::vec3(0.0f, 16.0f, 0.0f);
            camera_.yaw = -90.0f;
            camera_.pitch = -18.0f;
            camera_.distance = 108.0f;
        }
        ImGui::TextWrapped("One-finger click-drag orbits. Two-finger vertical swipe zooms; horizontal swipe pans. Shift + right drag pans. WASD pans the target.");
        camera_.updateFromOrbit();
        ImGui::End();
    }

    bool projectToScreen(const glm::vec3& world, const glm::mat4& viewProj, ImVec2& screen) const
    {
        glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (clip.w <= 0.05f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.12f || ndc.x > 1.12f || ndc.y < -1.12f || ndc.y > 1.12f || ndc.z < -1.0f || ndc.z > 1.0f) return false;
        if (terrain_.occludesSegment(camera_.position, world)) return false;
        ImVec2 size = ImGui::GetIO().DisplaySize;
        screen.x = (ndc.x * 0.5f + 0.5f) * size.x;
        screen.y = (ndc.y * 0.5f + 0.5f) * size.y;
        return true;
    }

    void drawFlowOverlay()
    {
        if (!showFlowParticles_) return;
        const auto& particles = terrain_.particlePositions();
        if (particles.empty()) return;
        float aspect = static_cast<float>(std::max(1u, swapchainExtent_.width)) / static_cast<float>(std::max(1u, swapchainExtent_.height));
        glm::mat4 proj = glm::perspective(glm::radians(58.0f), aspect, 0.1f, 650.0f);
        proj[1][1] *= -1.0f;
        glm::mat4 viewProj = proj * camera_.view();
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        for (const glm::vec3& world : particles) {
            ImVec2 screen{};
            if (projectToScreen(world, viewProj, screen)) {
                drawList->AddRectFilled(ImVec2(screen.x - 1.05f, screen.y - 1.05f), ImVec2(screen.x + 1.05f, screen.y + 1.05f), IM_COL32(168, 226, 244, 172));
            }
        }
    }

    void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        checkVk(vkBeginCommandBuffer(commandBuffer, &begin), "Failed to begin command buffer");
        std::array<VkClearValue, 2> clears{};
        clears[0].color = {{0.30f, 0.42f, 0.52f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo renderPass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderPass.renderPass = renderPass_;
        renderPass.framebuffer = framebuffers_[imageIndex];
        renderPass.renderArea.offset = {0, 0};
        renderPass.renderArea.extent = swapchainExtent_;
        renderPass.clearValueCount = static_cast<std::uint32_t>(clears.size());
        renderPass.pClearValues = clears.data();
        vkCmdBeginRenderPass(commandBuffer, &renderPass, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(swapchainExtent_.width), static_cast<float>(swapchainExtent_.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, swapchainExtent_};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSets_[currentFrame_], 0, nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer_, &offset);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSets_[currentFrame_], 0, nullptr);
        vkCmdDrawIndexed(commandBuffer, static_cast<std::uint32_t>(terrain_.indices().size()), 1, 0, 0, 0);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
        vkCmdEndRenderPass(commandBuffer);
        checkVk(vkEndCommandBuffer(commandBuffer), "Failed to record command buffer");
    }

    void drawFrame()
    {
        vkWaitForFences(device_, 1, &inFlight_[currentFrame_], VK_TRUE, UINT64_MAX);
        std::uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            checkVk(acquire, "Failed to acquire swapchain image");
        }
        vkResetFences(device_, 1, &inFlight_[currentFrame_]);
        if (terrainDirty_) uploadTerrain();
        updateUniformBuffer(static_cast<std::uint32_t>(currentFrame_));
        vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
        recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex);
        VkSemaphore waitSemaphores[] = {imageAvailable_[currentFrame_]};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphore signalSemaphores[] = {renderFinished_[currentFrame_]};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = waitSemaphores;
        submit.pWaitDstStageMask = waitStages;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffers_[currentFrame_];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = signalSemaphores;
        checkVk(vkQueueSubmit(graphicsQueue_, 1, &submit, inFlight_[currentFrame_]), "Failed to submit frame");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = signalSemaphores;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &imageIndex;
        VkResult presentResult = vkQueuePresentKHR(presentQueue_, &present);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || framebufferResized_) {
            recreateSwapchain();
        } else if (presentResult != VK_SUCCESS) {
            checkVk(presentResult, "Failed to present swapchain image");
        }
        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
    }

    void mainLoop()
    {
        auto previous = std::chrono::high_resolution_clock::now();
        while (!glfwWindowShouldClose(window_)) {
            auto now = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float>(now - previous).count();
            previous = now;
            glfwPollEvents();
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            processInput(dt);
            updateErosionAnimation();
            drawControls();
            drawFlowOverlay();
            ImGui::Render();
            drawFrame();
        }
        vkDeviceWaitIdle(device_);
    }

    void cleanup()
    {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
            vkDestroySemaphore(device_, renderFinished_[i], nullptr);
            vkDestroyFence(device_, inFlight_[i], nullptr);
            vkDestroyBuffer(device_, uniformBuffers_[i], nullptr);
            vkFreeMemory(device_, uniformMemories_[i], nullptr);
        }
        vkDestroyDescriptorPool(device_, imguiPool_, nullptr);
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        vkDestroyBuffer(device_, indexBuffer_, nullptr);
        vkFreeMemory(device_, indexMemory_, nullptr);
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexMemory_, nullptr);
        vkDestroySampler(device_, skyboxSampler_, nullptr);
        vkDestroyImageView(device_, skyboxView_, nullptr);
        vkDestroyImage(device_, skyboxImage_, nullptr);
        vkFreeMemory(device_, skyboxMemory_, nullptr);
        cleanupSwapchain();
        vkDestroyPipeline(device_, skyPipeline_, nullptr);
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
};

} // namespace

int main()
{
    try {
        VulkanApp app;
        app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
