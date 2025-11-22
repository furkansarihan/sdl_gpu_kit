#pragma once

#include <glm/glm.hpp>

#include <SDL3/SDL_gpu.h>

#include "../camera.h"
#include "../ui/base_ui.h"

const int MAX_CASCADES = 4;
const int NUM_CASCADES = 4;

struct ShadowUniforms
{
    glm::mat4 depthBiasVP[MAX_CASCADES]; // per-cascade light VP
    glm::mat4 cameraView;                // camera view matrix (for cascade selection)
    glm::vec4 cascadeSplits;             // far plane distance per cascade (view-space depth)
    glm::vec4 cascadeBias;
    float shadowFar;
    float paddin[3];
};

struct Cascade
{
    glm::mat4 view;
    glm::mat4 projection;
};

class ShadowManager : public BaseUI
{
public:
    ShadowManager();
    ~ShadowManager();

    int m_shadowMapResolution = 2048;
    float m_cascadeLambda = 0.5f;

    // GPU objects
    SDL_GPUTexture *m_shadowMapTexture = nullptr;
    SDL_GPUSampler *m_shadowSampler = nullptr;
    SDL_GPUGraphicsPipeline *m_shadowPipeline = nullptr;

    // CPU-side uniform data
    ShadowUniforms m_shadowUniforms{};
    Cascade m_cascades[NUM_CASCADES];

    void renderUI() override;
    void updateCascades(
        Camera *camera,
        const glm::mat4 &view,
        const glm::vec3 &lightDir,
        float aspect);
};