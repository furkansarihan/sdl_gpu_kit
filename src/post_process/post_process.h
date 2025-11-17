#pragma once

#include <glm/glm.hpp>

#include <SDL3/SDL_gpu.h>

#include "../ui/base_ui.h"

struct SkyboxFragmentUBO
{
    float lod;
    float padding[3];
};

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

class PostProcess : public BaseUI
{
public:
    PostProcess();
    ~PostProcess();

    PostProcessFragmentUBO m_UBO;
    SkyboxFragmentUBO m_skyUBO;
    BloomDownsampleUBO m_downsampleUBO;
    BloomUpsampleUBO m_upsampleUBO;

    SDL_GPUSampler *m_clampedSampler = nullptr;
    SDL_GPUTexture *m_colorTexture = nullptr;
    SDL_GPUTexture *m_depthTexture = nullptr;

    static const int BLOOM_MIPS = 5;
    SDL_GPUTexture *m_bloomMip[BLOOM_MIPS] = {};

    SDL_GPUGraphicsPipeline *m_postProcessPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_bloomDownPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_bloomUpPipeline = nullptr;

    SDL_GPUShader *m_fullscreenVert = nullptr;
    SDL_GPUShader *m_postProcessFrag = nullptr;
    SDL_GPUShader *m_bloomDownFrag = nullptr;
    SDL_GPUShader *m_bloomUpFrag = nullptr;

    void renderUI() override;

    void update(glm::ivec2 screenSize);
    void downsample(SDL_GPUCommandBuffer *commandBuffer);
    void upsample(SDL_GPUCommandBuffer *commandBuffer);
    void postProcess(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchainTexture);
};
