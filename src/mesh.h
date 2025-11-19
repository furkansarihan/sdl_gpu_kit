#pragma once

#include <glm/glm.hpp>

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent; // xyz = tangent direction, w = handedness (+1 or -1)
};
