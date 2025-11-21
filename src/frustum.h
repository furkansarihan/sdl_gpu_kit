#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>

class Frustum
{
public:
    // Planes in the form ax + by + cz + d = 0
    // Order: 0=L, 1=R, 2=B, 3=T, 4=N, 5=F
    glm::vec4 planes[6];

    static Frustum fromMatrix(const glm::mat4 &vp)
    {
        Frustum f;

        // glm is column-major, so we grab rows explicitly
        glm::vec4 row0 = glm::row(vp, 0);
        glm::vec4 row1 = glm::row(vp, 1);
        glm::vec4 row2 = glm::row(vp, 2);
        glm::vec4 row3 = glm::row(vp, 3);

        // Extract planes (OpenGL-style clip space)
        f.planes[0] = row3 + row0; // left
        f.planes[1] = row3 - row0; // right
        f.planes[2] = row3 + row1; // bottom
        f.planes[3] = row3 - row1; // top
        f.planes[4] = row3 + row2; // near
        f.planes[5] = row3 - row2; // far

        // Normalize planes
        for (int i = 0; i < 6; ++i)
        {
            glm::vec3 n = glm::vec3(f.planes[i]);
            float len = glm::length(n);
            if (len > 0.0f)
                f.planes[i] /= len;
        }

        return f;
    }

    bool containsSphere(const glm::vec3 &center, float radius)
    {
        for (int i = 0; i < 6; ++i)
        {
            const glm::vec4 &p = planes[i];
            float dist = glm::dot(glm::vec3(p), center) + p.w;
            if (dist < -radius)
                return false; // completely outside
        }

        return true; // at least partially inside
    }
};
