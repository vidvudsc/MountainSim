#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

constexpr int kTerrainSize = 193;
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
    // Appended for the volumetric cloud pass (existing shaders ignore these trailing
    // members; their offsets are unchanged because they come after the originals).
    glm::mat4 invViewProj{1.0f}; // world ray reconstruction from screen
    glm::vec4 volMin{};          // xyz: world position of cloud cell-center (0,0,0)
    glm::vec4 volMax{};          // xyz: world position of cell-center (nx-1,ny-1,nz-1)
    glm::vec4 cloudGrid{};       // x,y,z = grid dims; w = heightmap resolution
    glm::vec4 cloudParams{};     // x density, y steps, z sun absorption, w coverage bias
};

inline std::vector<char> readBinaryFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) throw std::runtime_error("Could not open file: " + path);
    std::vector<char> buffer(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    return buffer;
}

inline void checkVk(VkResult result, const char* message)
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
