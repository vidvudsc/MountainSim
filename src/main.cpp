#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#ifdef __APPLE__
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kTerrainSize = 161;
constexpr float kTerrainWorldSize = 165.0f;
constexpr float kPi = 3.14159265358979323846f;

struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{};
    glm::vec2 hydro{};
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
    float waterLevel = 0.18f;
    float waterTint = 1.35f;
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

struct MaterialTextures {
    GLuint base = 0;
    GLuint normal = 0;
    GLuint ao = 0;
    GLuint roughness = 0;

    bool ready() const { return base && normal && ao; }
};

std::string readFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Could not open file: " + path);
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

GLuint compileShader(GLenum type, const std::string& source)
{
    GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(length, '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        throw std::runtime_error("Shader compile failed:\n" + log);
    }
    return shader;
}

GLuint createProgram(const std::string& vertexPath, const std::string& fragmentPath)
{
    GLuint vertex = compileShader(GL_VERTEX_SHADER, readFile(vertexPath));
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER, readFile(fragmentPath));
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    if (!success) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(length, '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        throw std::runtime_error("Program link failed:\n" + log);
    }
    return program;
}

class Perlin2D {
public:
    explicit Perlin2D(int seed = 0) { reseed(seed); }

    void reseed(int seed)
    {
        std::iota(perm_.begin(), perm_.begin() + 256, 0);
        std::mt19937 rng(static_cast<std::uint32_t>(seed));
        std::shuffle(perm_.begin(), perm_.begin() + 256, rng);
        for (int i = 0; i < 256; ++i) {
            perm_[256 + i] = perm_[i];
        }
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
        buildIndexBuffer();
        generate(settings_);
        createGlObjects();
        upload();
    }

    ~Terrain()
    {
        glDeleteBuffers(1, &vbo_);
        glDeleteBuffers(1, &ebo_);
        glDeleteVertexArrays(1, &vao_);
    }

    void generate(const TerrainSettings& settings)
    {
        settings_ = settings;
        Perlin2D perlin(settings.seed);
        heights_.assign(kTerrainSize * kTerrainSize, 0.0f);
        water_.assign(kTerrainSize * kTerrainSize, 0.0f);
        sediment_.assign(kTerrainSize * kTerrainSize, 0.0f);
        vertices_.assign(kTerrainSize * kTerrainSize, Vertex{});

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
                float h = glm::mix(fbm * 0.42f, std::pow(fbm, 1.22f), mountainMask);
                h *= continent;

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
        std::mt19937 rng(static_cast<std::uint32_t>(settings_.seed * 1664525 + drops));
        std::uniform_real_distribution<float> dist(1.0f, static_cast<float>(kTerrainSize - 2));

        for (int i = 0; i < drops; ++i) {
            float x = dist(rng);
            float z = dist(rng);
            glm::vec2 dir(0.0f);
            float speed = 1.0f;
            float water = 1.0f;
            float sediment = 0.0f;

            for (int lifetime = 0; lifetime < 42; ++lifetime) {
                HeightSample current = sampleHeightAndGradient(x, z);
                dir = dir * settings_.inertia - current.gradient * (1.0f - settings_.inertia);
                float len = glm::length(dir);
                if (len < 0.001f) {
                    float angle = dist(rng) * 17.31f;
                    dir = glm::vec2(std::cos(angle), std::sin(angle));
                } else {
                    dir /= len;
                }

                float nextX = x + dir.x;
                float nextZ = z + dir.y;
                if (nextX < 1.0f || nextX >= kTerrainSize - 2 || nextZ < 1.0f || nextZ >= kTerrainSize - 2) {
                    break;
                }

                float nextHeight = sampleHeight(nextX, nextZ);
                float deltaHeight = nextHeight - current.height;
                float capacity = std::max(-deltaHeight * speed * water * settings_.capacity, settings_.minCapacity);

                if (sediment > capacity || deltaHeight > 0.0f) {
                    float amount = (deltaHeight > 0.0f)
                        ? std::min(deltaHeight, sediment)
                        : (sediment - capacity) * settings_.depositSpeed;
                    sediment -= amount;
                    deposit(x, z, amount);
                } else {
                    float amount = std::min((capacity - sediment) * settings_.erodeSpeed, -deltaHeight);
                    if (amount > 0.0f) {
                        erodeAt(x, z, amount, settings_.erosionRadius);
                        sediment += amount;
                    }
                }

                addHydro(x, z, water * 0.025f, sediment * 0.015f);
                speed = std::sqrt(std::max(0.0f, speed * speed + deltaHeight * settings_.gravity));
                water *= (1.0f - settings_.evaporation);
                x = nextX;
                z = nextZ;
                if (water < 0.02f) {
                    break;
                }
            }
        }

        normalizeHydro();
        smoothHeightmap(1, 0.10f);
        rebuildVertices();
        upload();
    }

    void smoothPeaks()
    {
        smoothHeightmap(1, 0.16f);
        rebuildVertices();
        upload();
    }

    void render() const
    {
        glBindVertexArray(vao_);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices_.size()), GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }

    void upload()
    {
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex)), vertices_.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices_.size() * sizeof(std::uint32_t)), indices_.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    const TerrainSettings& settings() const { return settings_; }
    TerrainSettings& settings() { return settings_; }

private:
    struct HeightSample {
        float height = 0.0f;
        glm::vec2 gradient{};
    };

    static int idx(int x, int z) { return z * kTerrainSize + x; }
    static float smoothstep01(float v)
    {
        v = glm::clamp(v, 0.0f, 1.0f);
        return v * v * (3.0f - 2.0f * v);
    }

    void createGlObjects()
    {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, hydro)));
        glBindVertexArray(0);
    }

    void buildIndexBuffer()
    {
        indices_.clear();
        indices_.reserve((kTerrainSize - 1) * (kTerrainSize - 1) * 6);
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

        HeightSample sample;
        sample.height = glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
        sample.gradient.x = (h10 - h00) * (1.0f - tz) + (h11 - h01) * tz;
        sample.gradient.y = (h01 - h00) * (1.0f - tx) + (h11 - h10) * tx;
        return sample;
    }

    void deposit(float x, float z, float amount)
    {
        int x0 = glm::clamp(static_cast<int>(std::floor(x)), 0, kTerrainSize - 2);
        int z0 = glm::clamp(static_cast<int>(std::floor(z)), 0, kTerrainSize - 2);
        float tx = x - x0;
        float tz = z - z0;
        addHeight(x0, z0, amount * (1.0f - tx) * (1.0f - tz));
        addHeight(x0 + 1, z0, amount * tx * (1.0f - tz));
        addHeight(x0, z0 + 1, amount * (1.0f - tx) * tz);
        addHeight(x0 + 1, z0 + 1, amount * tx * tz);
    }

    void erodeAt(float x, float z, float amount, float radius)
    {
        int minX = glm::clamp(static_cast<int>(std::floor(x - radius)), 0, kTerrainSize - 1);
        int maxX = glm::clamp(static_cast<int>(std::ceil(x + radius)), 0, kTerrainSize - 1);
        int minZ = glm::clamp(static_cast<int>(std::floor(z - radius)), 0, kTerrainSize - 1);
        int maxZ = glm::clamp(static_cast<int>(std::ceil(z + radius)), 0, kTerrainSize - 1);

        float totalWeight = 0.0f;
        for (int zz = minZ; zz <= maxZ; ++zz) {
            for (int xx = minX; xx <= maxX; ++xx) {
                float d = glm::length(glm::vec2(xx - x, zz - z));
                totalWeight += std::max(0.0f, radius - d);
            }
        }
        if (totalWeight <= 0.0f) return;

        for (int zz = minZ; zz <= maxZ; ++zz) {
            for (int xx = minX; xx <= maxX; ++xx) {
                float d = glm::length(glm::vec2(xx - x, zz - z));
                float weight = std::max(0.0f, radius - d) / totalWeight;
                int index = idx(xx, zz);
                heights_[index] = std::max(0.0f, heights_[index] - amount * weight);
            }
        }
    }

    void addHeight(int x, int z, float amount)
    {
        heights_[idx(x, z)] += amount;
    }

    void addHydro(float x, float z, float waterAmount, float sedimentAmount)
    {
        int ix = glm::clamp(static_cast<int>(std::round(x)), 0, kTerrainSize - 1);
        int iz = glm::clamp(static_cast<int>(std::round(z)), 0, kTerrainSize - 1);
        water_[idx(ix, iz)] += waterAmount;
        sediment_[idx(ix, iz)] += sedimentAmount;
    }

    void normalizeHydro()
    {
        float maxWater = *std::max_element(water_.begin(), water_.end());
        float maxSediment = *std::max_element(sediment_.begin(), sediment_.end());
        for (int i = 0; i < static_cast<int>(water_.size()); ++i) {
            water_[i] = std::sqrt(water_[i] / std::max(maxWater, 0.001f));
            sediment_[i] = std::sqrt(sediment_[i] / std::max(maxSediment, 0.001f));
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
                    float neighborAverage = (
                        heights_[idx(x - 1, z)] +
                        heights_[idx(x + 1, z)] +
                        heights_[idx(x, z - 1)] +
                        heights_[idx(x, z + 1)]
                    ) * 0.25f;
                    float height01 = h / std::max(settings_.heightScale, 0.001f);
                    float peakPreserve = glm::smoothstep(0.74f, 0.96f, height01);
                    float localStrength = strength * (1.0f - peakPreserve * 0.58f);
                    next[index] = glm::mix(h, neighborAverage, localStrength);
                }
            }
            heights_.swap(next);
        }
    }

    void rebuildVertices()
    {
        float step = kTerrainWorldSize / static_cast<float>(kTerrainSize - 1);
        for (int z = 0; z < kTerrainSize; ++z) {
            for (int x = 0; x < kTerrainSize; ++x) {
                int index = idx(x, z);
                float worldX = (static_cast<float>(x) / (kTerrainSize - 1) - 0.5f) * kTerrainWorldSize;
                float worldZ = (static_cast<float>(z) / (kTerrainSize - 1) - 0.5f) * kTerrainWorldSize;
                vertices_[index].position = {worldX, heights_[index], worldZ};
                vertices_[index].uv = {static_cast<float>(x) / (kTerrainSize - 1), static_cast<float>(z) / (kTerrainSize - 1)};
                vertices_[index].hydro = {water_[index], sediment_[index]};
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
                glm::vec3 normal = glm::normalize(glm::vec3(hL - hR, 2.0f * step, hD - hU));
                vertices_[idx(x, z)].normal = normal;
            }
        }
    }

    TerrainSettings settings_{};
    std::vector<float> heights_;
    std::vector<float> water_;
    std::vector<float> sediment_;
    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
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

glm::vec3 sunDirection(const TerrainSettings& settings)
{
    float azimuth = glm::radians(settings.sunAzimuth);
    float elevation = glm::radians(settings.sunElevation);
    return glm::normalize(glm::vec3(std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth)));
}

void setMat4(GLuint program, const char* name, const glm::mat4& value)
{
    glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_FALSE, glm::value_ptr(value));
}

void setVec3(GLuint program, const char* name, const glm::vec3& value)
{
    glUniform3fv(glGetUniformLocation(program, name), 1, glm::value_ptr(value));
}

void setFloat(GLuint program, const char* name, float value)
{
    glUniform1f(glGetUniformLocation(program, name), value);
}

void setInt(GLuint program, const char* name, int value)
{
    glUniform1i(glGetUniformLocation(program, name), value);
}

GLuint createSkyCube()
{
    constexpr float s = 1.0f;
    const std::array<glm::vec3, 36> positions = {
        glm::vec3{-s,  s, -s}, {-s, -s, -s}, { s, -s, -s}, { s, -s, -s}, { s,  s, -s}, {-s,  s, -s},
        {-s, -s,  s}, {-s, -s, -s}, {-s,  s, -s}, {-s,  s, -s}, {-s,  s,  s}, {-s, -s,  s},
        { s, -s, -s}, { s, -s,  s}, { s,  s,  s}, { s,  s,  s}, { s,  s, -s}, { s, -s, -s},
        {-s, -s,  s}, {-s,  s,  s}, { s,  s,  s}, { s,  s,  s}, { s, -s,  s}, {-s, -s,  s},
        {-s,  s, -s}, { s,  s, -s}, { s,  s,  s}, { s,  s,  s}, {-s,  s,  s}, {-s,  s, -s},
        {-s, -s, -s}, {-s, -s,  s}, { s, -s, -s}, { s, -s, -s}, {-s, -s,  s}, { s, -s,  s}
    };

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(positions.size() * sizeof(glm::vec3)), positions.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glBindVertexArray(0);
    return vao;
}

GLuint loadCubemap(const std::array<std::string, 6>& faces)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
    stbi_set_flip_vertically_on_load(false);

    for (std::size_t i = 0; i < faces.size(); ++i) {
        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load(faces[i].c_str(), &width, &height, &channels, 0);
        if (!pixels) {
            std::cerr << "Skybox face failed to load: " << faces[i] << "\n";
            glDeleteTextures(1, &texture);
            return 0;
        }
        GLenum format = channels == 4 ? GL_RGBA : GL_RGB;
        glTexImage2D(
            GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(i),
            0,
            static_cast<GLint>(format),
            width,
            height,
            0,
            format,
            GL_UNSIGNED_BYTE,
            pixels
        );
        stbi_image_free(pixels);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    return texture;
}

GLuint loadTexture2D(const std::string& path, bool srgb)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 0);
    if (!pixels) {
        std::cerr << "Texture failed to load: " << path << "\n";
        return 0;
    }

    GLenum format = GL_RGB;
    GLenum internalFormat = srgb ? GL_SRGB8 : GL_RGB8;
    if (channels == 1) {
        format = GL_RED;
        internalFormat = GL_R8;
    } else if (channels == 4) {
        format = GL_RGBA;
        internalFormat = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0, format, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (GLEW_EXT_texture_filter_anisotropic) {
        GLfloat maxAniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(maxAniso, 8.0f));
    }
    stbi_image_free(pixels);
    return texture;
}

MaterialTextures loadMaterial(const std::string& base, const std::string& normal, const std::string& ao, const std::string& roughness)
{
    return {
        loadTexture2D(base, true),
        loadTexture2D(normal, false),
        loadTexture2D(ao, false),
        loadTexture2D(roughness, false),
    };
}

void bindMaterial(GLuint program, const MaterialTextures& material, const char* prefix, int firstUnit)
{
    std::string baseName = std::string(prefix) + "Base";
    std::string normalName = std::string(prefix) + "Normal";
    std::string aoName = std::string(prefix) + "Ao";

    setInt(program, baseName.c_str(), firstUnit);
    setInt(program, normalName.c_str(), firstUnit + 1);
    setInt(program, aoName.c_str(), firstUnit + 2);

    glActiveTexture(GL_TEXTURE0 + firstUnit);
    glBindTexture(GL_TEXTURE_2D, material.base);
    glActiveTexture(GL_TEXTURE0 + firstUnit + 1);
    glBindTexture(GL_TEXTURE_2D, material.normal);
    glActiveTexture(GL_TEXTURE0 + firstUnit + 2);
    glBindTexture(GL_TEXTURE_2D, material.ao);
}

#ifdef __APPLE__
id installMagnifyMonitor(GLFWwindow* window, Camera& camera)
{
    NSWindow* nativeWindow = glfwGetCocoaWindow(window);
    return [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify handler:^NSEvent* (NSEvent* event) {
        if ([event window] != nativeWindow) {
            return event;
        }
        if (ImGui::GetIO().WantCaptureMouse) {
            return event;
        }
        float magnification = static_cast<float>([event magnification]);
        camera.distance *= std::exp(-magnification * 4.8f);
        camera.distance = glm::clamp(camera.distance, 16.0f, 260.0f);
        camera.lastInput = "trackpad pinch zoom";
        camera.updateFromOrbit();
        return nil;
    }];
}
#endif

void scrollCallback(GLFWwindow* window, double xOffset, double yOffset)
{
    if (ImGui::GetIO().WantCaptureMouse) {
        ImGui_ImplGlfw_ScrollCallback(window, xOffset, yOffset);
        return;
    }
    auto* camera = static_cast<Camera*>(glfwGetWindowUserPointer(window));
    if (!camera) {
        return;
    }
    glm::vec3 forward = glm::normalize(camera->target - camera->position);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 up = glm::normalize(glm::cross(right, forward));
    float scale = camera->distance * camera->panSpeed * 0.055f;
    camera->distance *= std::pow(0.86f, static_cast<float>(yOffset));
    camera->distance = glm::clamp(camera->distance, 16.0f, 260.0f);
    camera->target += (-right * static_cast<float>(xOffset)) * scale;
    camera->lastInput = "two-finger zoom";
    camera->updateFromOrbit();
}

void processKeyboardPan(GLFWwindow* window, Camera& camera, float dt)
{
    glm::vec3 forward = glm::normalize(camera.target - camera.position);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 planarForward = glm::normalize(glm::vec3(forward.x, 0.0f, forward.z));
    float velocity = camera.distance * 0.42f * dt;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) velocity *= 2.0f;
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera.target += planarForward * velocity;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera.target -= planarForward * velocity;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera.target -= right * velocity;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera.target += right * velocity;
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) camera.target.y += velocity;
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) camera.target.y -= velocity;
    camera.updateFromOrbit();
}

void drawControls(Terrain& terrain, Camera& camera, bool& wireframe)
{
    TerrainSettings editable = terrain.settings();
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
        terrain.generate(editable);
        terrain.upload();
    } else {
        terrain.settings() = editable;
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(18, 318), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 215), ImGuiCond_FirstUseEver);
    ImGui::Begin("Erosion");
    ImGui::SliderInt("Drops", &terrain.settings().erosionDrops, 1000, 45000);
    ImGui::SliderFloat("Radius", &terrain.settings().erosionRadius, 1.0f, 5.0f, "%.1f");
    ImGui::SliderFloat("Inertia", &terrain.settings().inertia, 0.0f, 0.92f, "%.2f");
    ImGui::SliderFloat("Capacity", &terrain.settings().capacity, 1.0f, 10.0f, "%.1f");
    ImGui::SliderFloat("Evaporation", &terrain.settings().evaporation, 0.005f, 0.12f, "%.3f");
    if (ImGui::Button("Run erosion")) {
        terrain.erode(terrain.settings().erosionDrops);
    }
    ImGui::SameLine();
    if (ImGui::Button("Quick pass")) {
        terrain.erode(2500);
    }
    ImGui::SameLine();
    if (ImGui::Button("Soften peaks")) {
        terrain.smoothPeaks();
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(365, 18), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 270), ImGuiCond_FirstUseEver);
    ImGui::Begin("Look");
    ImGui::SliderFloat("Snow level", &terrain.settings().snowLevel, 0.35f, 0.9f, "%.2f");
    ImGui::SliderFloat("Water line", &terrain.settings().waterLevel, 0.03f, 0.42f, "%.2f");
    ImGui::SliderFloat("Water tint", &terrain.settings().waterTint, 0.0f, 5.0f, "%.1f");
    ImGui::SliderFloat("Sediment tint", &terrain.settings().sedimentTint, 0.0f, 5.0f, "%.1f");
    ImGui::Checkbox("Show water", &terrain.settings().showWater);
    ImGui::Checkbox("Show sediment", &terrain.settings().showSediment);
    ImGui::SliderFloat("Fog density", &terrain.settings().fogDensity, 0.0f, 0.03f, "%.3f");
    ImGui::SliderFloat("Sun azimuth", &terrain.settings().sunAzimuth, -180.0f, 180.0f, "%.0f deg");
    ImGui::SliderFloat("Sun elevation", &terrain.settings().sunElevation, 2.0f, 88.0f, "%.0f deg");
    ImGui::Checkbox("Wireframe", &wireframe);
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(365, 306), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 185), ImGuiCond_FirstUseEver);
    ImGui::Begin("Camera");
    ImGui::SliderFloat("Orbit distance", &camera.distance, 16.0f, 260.0f, "%.0f");
    ImGui::SliderFloat("Pan speed", &camera.panSpeed, 0.05f, 0.55f, "%.2f");
    ImGui::Text("Last input: %s", camera.lastInput.c_str());
    if (ImGui::Button("Reset view")) {
        camera.target = glm::vec3(0.0f, 16.0f, 0.0f);
        camera.yaw = -90.0f;
        camera.pitch = -18.0f;
        camera.distance = 108.0f;
    }
    ImGui::TextWrapped("One-finger click-drag orbits. Two-finger vertical swipe zooms; horizontal swipe pans. Shift + right drag pans. WASD pans the target.");
    camera.updateFromOrbit();
    ImGui::End();
}

} // namespace

int main()
{
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(1440, 900, "Mountains - Procedural OpenGL Terrain", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW\n";
        return 1;
    }
    glGetError();

    std::cout << "GPU: " << glGetString(GL_RENDERER) << std::endl;
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << std::endl;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 410");

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);

    GLuint terrainProgram = 0;
    GLuint skyProgram = 0;
    try {
        terrainProgram = createProgram("shaders/terrain.vert", "shaders/terrain.frag");
        skyProgram = createProgram("shaders/sky.vert", "shaders/sky.frag");
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    Terrain terrain;
    Camera camera;
    camera.updateFromOrbit();
    glfwSetWindowUserPointer(window, &camera);
    glfwSetScrollCallback(window, scrollCallback);
#ifdef __APPLE__
    id magnifyMonitor = installMagnifyMonitor(window, camera);
#endif
    GLuint skyVao = createSkyCube();
    GLuint skyboxTexture = loadCubemap({
        "assets/skybox/envmap_miramar/miramar_rt.tga",
        "assets/skybox/envmap_miramar/miramar_lf.tga",
        "assets/skybox/envmap_miramar/miramar_up.tga",
        "assets/skybox/envmap_miramar/miramar_dn.tga",
        "assets/skybox/envmap_miramar/miramar_ft.tga",
        "assets/skybox/envmap_miramar/miramar_bk.tga",
    });
    bool wireframe = false;

    auto previous = std::chrono::high_resolution_clock::now();
    while (!glfwWindowShouldClose(window)) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - previous).count();
        previous = now;

        glfwPollEvents();

        bool leftMouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool rightMouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        bool middleMouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
        bool shiftDown = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        bool wantsPan = (middleMouse || (rightMouse && shiftDown)) && !ImGui::GetIO().WantCaptureMouse;
        bool wantsOrbit = (leftMouse || (rightMouse && !shiftDown)) && !ImGui::GetIO().WantCaptureMouse;

        if (wantsPan || wantsOrbit) {
            if (!camera.orbiting && !camera.panning) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                camera.firstMouse = true;
            }
            camera.orbiting = wantsOrbit;
            camera.panning = wantsPan;
        } else if (camera.orbiting || camera.panning) {
            camera.orbiting = false;
            camera.panning = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        if (camera.orbiting || camera.panning) {
            double mouseX = 0.0;
            double mouseY = 0.0;
            glfwGetCursorPos(window, &mouseX, &mouseY);
            if (camera.firstMouse) {
                camera.lastMouseX = mouseX;
                camera.lastMouseY = mouseY;
                camera.firstMouse = false;
            }
            float dx = static_cast<float>(mouseX - camera.lastMouseX);
            float dy = static_cast<float>(camera.lastMouseY - mouseY);
            camera.lastMouseX = mouseX;
            camera.lastMouseY = mouseY;
            if (camera.orbiting) {
                camera.yaw += dx * 0.10f;
                camera.pitch = glm::clamp(camera.pitch + dy * 0.10f, -86.0f, 86.0f);
                camera.lastInput = "right-drag orbit";
            } else if (camera.panning) {
                glm::vec3 forward = glm::normalize(camera.target - camera.position);
                glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                glm::vec3 up = glm::normalize(glm::cross(right, forward));
                float scale = camera.distance * camera.panSpeed * 0.004f;
                camera.target += (-right * dx + up * dy) * scale;
                camera.lastInput = "drag pan";
            }
            camera.updateFromOrbit();
        } else if (!ImGui::GetIO().WantCaptureKeyboard) {
            processKeyboardPan(window, camera, dt);
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);

        glm::mat4 projection = glm::perspective(glm::radians(58.0f), static_cast<float>(width) / static_cast<float>(std::max(height, 1)), 0.1f, 650.0f);
        glm::mat4 view = camera.view();
        glm::vec3 sunDir = sunDirection(terrain.settings());
        float sunWarmth = glm::smoothstep(0.0f, 0.55f, sunDir.y);
        glm::vec3 sunColor = glm::mix(glm::vec3(1.0f, 0.58f, 0.34f), glm::vec3(1.0f, 0.93f, 0.78f), sunWarmth);
        glm::vec3 fogColor = glm::mix(glm::vec3(0.46f, 0.50f, 0.55f), glm::vec3(0.62f, 0.72f, 0.80f), sunWarmth);

        glClearColor(fogColor.r, fogColor.g, fogColor.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glDepthFunc(GL_LEQUAL);
        glUseProgram(skyProgram);
        setMat4(skyProgram, "uView", view);
        setMat4(skyProgram, "uProjection", projection);
        setVec3(skyProgram, "uSunDir", sunDir);
        setVec3(skyProgram, "uHorizonColor", fogColor * 1.15f);
        setVec3(skyProgram, "uZenithColor", glm::vec3(0.13f, 0.27f, 0.44f));
        setVec3(skyProgram, "uSunColor", sunColor);
        setFloat(skyProgram, "uUseSkybox", skyboxTexture != 0 ? 1.0f : 0.0f);
        setInt(skyProgram, "uSkybox", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture);
        glBindVertexArray(skyVao);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        glBindVertexArray(0);
        glDepthFunc(GL_LESS);

        glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
        glUseProgram(terrainProgram);
        setMat4(terrainProgram, "uModel", glm::mat4(1.0f));
        setMat4(terrainProgram, "uView", view);
        setMat4(terrainProgram, "uProjection", projection);
        setVec3(terrainProgram, "uCameraPos", camera.position);
        setVec3(terrainProgram, "uSunDir", sunDir);
        setVec3(terrainProgram, "uSunColor", sunColor);
        setVec3(terrainProgram, "uAmbientColor", glm::vec3(0.15f, 0.18f, 0.20f));
        setVec3(terrainProgram, "uFogColor", fogColor);
        setFloat(terrainProgram, "uHeightScale", terrain.settings().heightScale);
        setFloat(terrainProgram, "uSnowLevel", terrain.settings().snowLevel);
        setFloat(terrainProgram, "uWaterLevel", terrain.settings().waterLevel);
        setFloat(terrainProgram, "uWaterTint", terrain.settings().waterTint);
        setFloat(terrainProgram, "uSedimentTint", terrain.settings().sedimentTint);
        setFloat(terrainProgram, "uShowWater", terrain.settings().showWater ? 1.0f : 0.0f);
        setFloat(terrainProgram, "uShowSediment", terrain.settings().showSediment ? 1.0f : 0.0f);
        setFloat(terrainProgram, "uFogDensity", terrain.settings().fogDensity);
        terrain.render();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawControls(terrain, camera, wireframe);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    glDeleteVertexArrays(1, &skyVao);
    glDeleteTextures(1, &skyboxTexture);
    glDeleteProgram(terrainProgram);
    glDeleteProgram(skyProgram);
#ifdef __APPLE__
    [NSEvent removeMonitor:magnifyMonitor];
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
