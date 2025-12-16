#pragma once

#include <vector>

template <int WIDTH, int HEIGHT>
class MaskTexture
{
public:
    static constexpr int GRID_WIDTH = WIDTH;
    static constexpr int GRID_HEIGHT = HEIGHT;
    // R8 Texture: 1 byte per pixel
    static constexpr int TOTAL_BYTES = WIDTH * HEIGHT;

    MaskTexture()
    {
        data_.resize(WIDTH * HEIGHT);
        clear(0);
    }

    // Pointer to raw bytes for Texture Upload
    const uint8_t *getData() const
    {
        return data_.data();
    }
    size_t getDataSize() const
    {
        return data_.size();
    }

    // --- Drawing Functions ---

    void clear(uint8_t value = 0)
    {
        std::memset(data_.data(), value, data_.size());
    }

    // Helper: Float (0.0-1.0) to Byte (0-255)
    static uint8_t toByte(float v)
    {
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
    }

    void fillRectNormalized(float nx, float ny, float nw, float nh, float value = 1.0f)
    {
        int x = static_cast<int>(nx * WIDTH);
        int y = static_cast<int>(ny * HEIGHT);
        int w = static_cast<int>(nw * WIDTH);
        int h = static_cast<int>(nh * HEIGHT);

        // Clamp to valid bounds
        if (x < 0)
        {
            w += x;
            x = 0;
        }
        if (y < 0)
        {
            h += y;
            y = 0;
        }
        if (x + w > WIDTH)
            w = WIDTH - x;
        if (y + h > HEIGHT)
            h = HEIGHT - y;

        if (w <= 0 || h <= 0)
            return;

        uint8_t byteVal = toByte(value);

        for (int dy = 0; dy < h; dy++)
        {
            std::memset(&data_[(y + dy) * WIDTH + x], byteVal, w);
        }
    }

    void fillCircle(int cx, int cy, float radius, float value = 1.0f)
    {
        int r = static_cast<int>(radius);
        uint8_t byteVal = toByte(value);
        int rSq = r * r;

        // Clamp bounds to texture
        int minY = std::max(0, cy - r);
        int maxY = std::min(HEIGHT - 1, cy + r);
        int minX = std::max(0, cx - r);
        int maxX = std::min(WIDTH - 1, cx + r);

        for (int y = minY; y <= maxY; y++)
        {
            int dy = y - cy;
            int dySq = dy * dy;
            int offset = y * WIDTH;

            for (int x = minX; x <= maxX; x++)
            {
                int dx = x - cx;
                if (dx * dx + dySq <= rSq)
                {
                    data_[offset + x] = byteVal;
                }
            }
        }
    }

    // --- 3D Drawing Functions ---

    /**
     * Projects a 3D AABB onto the 2D texture and fills the covering rectangle.
     * * @param viewProjMatrix: Pointer to 16 floats (4x4 Matrix, Column-Major)
     * @param minBounds: {x, y, z} of AABB min corner
     * @param maxBounds: {x, y, z} of AABB max corner
     * @param value: The mask value to write (0.0 - 1.0)
     */
    void fillProjectedAABB(const float *viewProjMatrix,
                           const float minBounds[3],
                           const float maxBounds[3],
                           float value = 1.0f)
    {
        // 1. Define the 8 corners of the AABB
        float corners[8][3] = {
            {minBounds[0], minBounds[1], minBounds[2]},
            {maxBounds[0], minBounds[1], minBounds[2]},
            {minBounds[0], maxBounds[1], minBounds[2]},
            {maxBounds[0], maxBounds[1], minBounds[2]},
            {minBounds[0], minBounds[1], maxBounds[2]},
            {maxBounds[0], minBounds[1], maxBounds[2]},
            {minBounds[0], maxBounds[1], maxBounds[2]},
            {maxBounds[0], maxBounds[1], maxBounds[2]}};

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();

        bool anyVisible = false;

        // 2. Project each corner
        for (int i = 0; i < 8; ++i)
        {
            float x = corners[i][0];
            float y = corners[i][1];
            float z = corners[i][2];

            // Manual Matrix Vector Multiply (Column-Major 4x4 * Vec4)
            // m[col][row] if referencing 2D, but usually flat array is:
            // 0  4  8  12
            // 1  5  9  13
            // 2  6  10 14
            // 3  7  11 15

            float clipX = x * viewProjMatrix[0] + y * viewProjMatrix[4] + z * viewProjMatrix[8] + viewProjMatrix[12];
            float clipY = x * viewProjMatrix[1] + y * viewProjMatrix[5] + z * viewProjMatrix[9] + viewProjMatrix[13];
            float clipW = x * viewProjMatrix[3] + y * viewProjMatrix[7] + z * viewProjMatrix[11] + viewProjMatrix[15];

            // 3. Check if point is behind camera (W <= 0)
            // Note: A robust implementation requires clipping against the near plane.
            // This simplified version ignores points behind the camera.
            // If the OBJECT is large and straddles the camera, this bounding box might be incorrect.
            if (clipW > 0.0001f)
            {
                anyVisible = true;

                // 4. Perspective Divide to get NDC (-1 to 1)
                float ndcX = clipX / clipW;
                float ndcY = clipY / clipW;

                // 5. Map to 0..1 UV space
                // Screen X: (-1 -> 0, 1 -> 1) => 0.5 * x + 0.5
                // Screen Y: (1 -> 0, -1 -> 1) => 0.5 - 0.5 * y (Assuming Top-Left Origin)

                float u = 0.5f * ndcX + 0.5f;
                float v = 0.5f - 0.5f * ndcY;

                if (u < minX)
                    minX = u;
                if (u > maxX)
                    maxX = u;
                if (v < minY)
                    minY = v;
                if (v > maxY)
                    maxY = v;
            }
        }

        // If we found valid points, draw the 2D bounding box
        if (anyVisible)
        {
            // Clamp to 0-1 for safety
            minX = std::max(0.0f, minX);
            minY = std::max(0.0f, minY);
            maxX = std::min(1.0f, maxX);
            maxY = std::min(1.0f, maxY);

            fillRectNormalized(minX, minY, maxX - minX, maxY - minY, value);
        }
    }

private:
    std::vector<uint8_t> data_;
};
