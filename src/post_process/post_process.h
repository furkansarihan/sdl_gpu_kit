#pragma once

#include <glm/glm.hpp>

#include <SDL3/SDL_gpu.h>

#include "../ui/base_ui.h"

struct PostProcessFragmentUBO
{
    glm::vec2 screenSize;
    float exposure;
    float gamma;
    float bloomIntensity;
    float padding[3];
};

struct BloomDownsampleUBO
{
    int mipLevel;
    float highlight;
    float padding[2];
};

struct BloomUpsampleUBO
{
    float filterRadius;
    float padding[3];
};

struct GTAOParamsUBO
{
    glm::vec4 resolution;     // xy = size, zw = 1/size
    glm::vec2 positionParams; // invProj[0][0] * 2, invProj[1][1] * 2
    float padding1[2];

    float invFarPlane;
    int maxLevel;
    float projectionScale;
    float intensity;

    glm::vec2 sliceCount; // (sliceCount, 1/sliceCount)
    float stepsPerSlice;
    float radius;

    float invRadiusSquared;
    float projectionScaleRadius;
    float power;
    float thicknessHeuristic;

    float constThickness;
    float nearPlane;
    float farPlane;
    float padding2;
};

class PostProcess : public BaseUI
{
public:
    PostProcess(SDL_GPUSampleCount sampleCount);
    ~PostProcess();

    PostProcessFragmentUBO m_UBO;
    BloomDownsampleUBO m_downsampleUBO;
    BloomUpsampleUBO m_upsampleUBO;
    GTAOParamsUBO m_gtaoParams;
    float m_gtaoPower;

    SDL_GPUSampleCount m_sampleCount;

    SDL_GPUSampler *m_clampedSampler = nullptr;
    SDL_GPUTexture *m_msaaColorTexture = nullptr;
    SDL_GPUTexture *m_msaaDepthTexture = nullptr;
    SDL_GPUTexture *m_colorTexture = nullptr;
    SDL_GPUTexture *m_depthTexture = nullptr;
    SDL_GPUTexture *m_gtaoRawTexture = nullptr;  // Raw GTAO result (RG format)
    SDL_GPUTexture *m_gtaoBlurTexture = nullptr; // Final blurred GTAO

    static const int BLOOM_MIPS = 5;
    SDL_GPUTexture *m_bloomMip[BLOOM_MIPS] = {};

    SDL_GPUGraphicsPipeline *m_postProcessPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_bloomDownPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_bloomUpPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_depthCopyPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_depthResolvePipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_gtaoGenPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_gtaoBlurPipeline = nullptr;

    SDL_GPUShader *m_fullscreenVert = nullptr;
    SDL_GPUShader *m_postProcessFrag = nullptr;
    SDL_GPUShader *m_bloomDownFrag = nullptr;
    SDL_GPUShader *m_bloomUpFrag = nullptr;
    SDL_GPUShader *m_depthCopyFrag = nullptr;
    SDL_GPUShader *m_depthResolveFrag = nullptr;
    SDL_GPUShader *m_gtaoGenFrag = nullptr;
    SDL_GPUShader *m_gtaoBlurFrag = nullptr;

    void renderUI() override;
    void update(glm::ivec2 screenSize);
    void downsample(SDL_GPUCommandBuffer *commandBuffer);
    void upsample(SDL_GPUCommandBuffer *commandBuffer);
    void resolveDepth(SDL_GPUCommandBuffer *commandBuffer);
    void computeGTAO(
        SDL_GPUCommandBuffer *commandBuffer,
        const glm::mat4 &projectionMatrix,
        const glm::mat4 &viewMatrix,
        float nearPlane,
        float farPlane);
    void postProcess(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchainTexture);
};
