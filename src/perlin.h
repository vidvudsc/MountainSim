#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>

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
