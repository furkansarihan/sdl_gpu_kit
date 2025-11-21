#pragma once

#include <SDL3/SDL_gpu.h>

#include "../resource_manager/resource_manager.h"

struct CubemapViewUBO
{
    glm::mat4 projection;
    glm::mat4 view;
    glm::mat4 model;
};

struct PrefilterUBO
{
    float roughness;
    float cubemapSize;
    float padding[2];
};

struct SkyboxFragmentUBO
{
    float lod = 0.f;
    float padding[3];
};

class PbrManager
{
public:
    PbrManager(ResourceManager *resourceManager);
    ~PbrManager();

    ResourceManager *m_resourceManager;

    SkyboxFragmentUBO m_skyUBO;
    int m_prefilterMipLevels = 5;
    int m_prefilterSize = 128;
    int m_irradianceSize = 64;
    int m_cubemapSize = 1024;

    Texture m_environmentTexture;

    // pipeline
    SDL_GPUGraphicsPipeline *m_graphicsPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_brdfPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_cubemapPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_irradiancePipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_prefilterPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_skyboxPipeline = nullptr;

    // texture
    SDL_GPUTexture *m_brdfTexture = nullptr;
    SDL_GPUTexture *m_cubemapTexture = nullptr;
    SDL_GPUTexture *m_irradianceTexture = nullptr;
    SDL_GPUTexture *m_prefilterTexture = nullptr;

    // sampler
    SDL_GPUSampler *m_hdrSampler;
    SDL_GPUSampler *m_brdfSampler;
    SDL_GPUSampler *m_cubeSampler;

    // model
    ModelData *m_quadModel;
    ModelData *m_cubeModel;

    void init();
    void updateEnvironmentTexture(Texture environmentTexture);
    void renderSkybox(
        SDL_GPUCommandBuffer *commandBuffer,
        SDL_GPURenderPass *renderPass,
        const glm::mat4 &viewMatrix,
        const glm::mat4 &projectionMatrix);
};
