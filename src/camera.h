#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>

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
