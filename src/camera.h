#pragma once

#include <glm/glm.hpp>

class Camera
{
public:
    glm::vec3 position = glm::vec3(0.0f, 0.3f, 3.0f);
    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::vec3(1.0f, 0.0f, 0.0f);
    float near = 0.1f;
    float far = 1000.0f;
    float fov = 45.0f;
    float yaw = -90.0f;
    float pitch = 0.0f;
    glm::mat4 view;
    glm::mat4 projection;
};
