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

struct SunFragmentUBO
{
    glm::vec3 sunPosition;
    float turbidity;

    glm::vec3 cameraPos;
    float rayleigh;

    float mieCoefficient;
    float mieDirectionalG;
    float sunIntensityFactor;
    float padding0;

    float sunIntensityFalloffSteepness;
    float sunAngularDiameterDegrees;
    float rayleighZenithLength;
    float mieZenithLength;

    float mieV;
    float numMolecules;
    float refractiveIndex;
    float depolarizationFactor;

    glm::vec3 primaries;
    float padding1;

    glm::vec3 mieKCoefficient;
    float padding2;

    float time;
    float cloudScale;
    float cloudCoverage;
    float padding3;
};

class PbrManager
{
public:
    PbrManager(ResourceManager *resourceManager);
    ~PbrManager();

    ResourceManager *m_resourceManager;

    SkyboxFragmentUBO m_skyUBO;
    SunFragmentUBO m_sunUBO;
    bool m_proceduralSkyEnabled = false;
    int m_prefilterMipLevels = 5;
    int m_prefilterSize = 128;
    int m_irradianceSize = 64;
    int m_cubemapSize = 1024;

    SDL_GPUTexture *m_environmentTexture;

    // pipeline
    SDL_GPUGraphicsPipeline *m_brdfPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_cubemapPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_irradiancePipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_prefilterPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_skyboxPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_proceduralSkyboxPipeline = nullptr;

    // texture
    SDL_GPUTexture *m_brdfTexture = nullptr;
    SDL_GPUTexture *m_cubemapTexture = nullptr;
    SDL_GPUTexture *m_irradianceTexture = nullptr;
    SDL_GPUTexture *m_prefilterTexture = nullptr;
    Texture m_cloudNoiseTexture;

    // sampler
    SDL_GPUSampler *m_hdrSampler;
    SDL_GPUSampler *m_brdfSampler;
    SDL_GPUSampler *m_cubeSampler;

    // model
    ModelData *m_quadModel;
    ModelData *m_cubeModel;

    glm::mat4 m_captureViews[6] = {
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
    };
    glm::mat4 m_captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

    void createBRDFTexture();
    void createSkyboxPipelines(SDL_GPUDevice *device);
    void init();
    void updateEnvironmentTexture(SDL_GPUTexture *environmentTexture);
    void updateIBL(SDL_GPUTexture *cubemapTexture);
    void renderSkybox(
        SDL_GPUCommandBuffer *commandBuffer,
        SDL_GPURenderPass *renderPass,
        const glm::mat4 &viewMatrix,
        const glm::mat4 &projectionMatrix);
};
