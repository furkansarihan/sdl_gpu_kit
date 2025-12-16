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
    Uint32 fxaaEnabled;
    float padding[2];
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

enum AntiAliasingMode
{
    AA_None = 0,
    AA_FXAA = 1,
    AA_SMAA = 2,
};

enum SMAAEdgeDetectionMode
{
    SMAA_EDGE_COLOR = 0,
    SMAA_EDGE_LUMA = 1
};

struct SMAAUniforms
{
    glm::vec4 rtMetrics; // (1/width, 1/height, width, height)
    int edgeDetectionMode;
    glm::vec3 padding;
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
    float m_gtaoResolutionFactor;

    SDL_GPUSampleCount m_sampleCount;

    SMAAUniforms m_smaaUniforms;
    AntiAliasingMode m_aaMode;

    SDL_GPUSampler *m_clampedSampler = nullptr;
    SDL_GPUTexture *m_msaaColorTexture = nullptr;
    SDL_GPUTexture *m_msaaDepthTexture = nullptr;
    SDL_GPUTexture *m_colorTexture = nullptr;
    SDL_GPUTexture *m_depthTexture = nullptr;
    SDL_GPUTexture *m_gtaoRawTexture = nullptr;  // Raw GTAO result (RG format)
    SDL_GPUTexture *m_gtaoBlur0Texture = nullptr;
    SDL_GPUTexture *m_gtaoBlur1Texture = nullptr; // Final blurred GTAO

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

    // SMAA textures
    SDL_GPUTexture *m_smaaEdgeTex = nullptr;  // RGBA8
    SDL_GPUTexture *m_smaaBlendTex = nullptr; // RGBA8
    SDL_GPUTexture *m_smaaColorTex = nullptr; // AA’d color (same format as m_colorTexture)

    // SMAA LUTs
    SDL_GPUTexture *m_smaaAreaTex = nullptr;
    SDL_GPUTexture *m_smaaSearchTex = nullptr;
    SDL_GPUSampler *m_smaaLutSampler = nullptr;

    // SMAA pipelines
    SDL_GPUGraphicsPipeline *m_smaaEdgePipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_smaaBlendPipeline = nullptr;
    SDL_GPUGraphicsPipeline *m_smaaNeighborPipeline = nullptr;

    void renderUI() override;

    void setAntiAliasingMode(AntiAliasingMode mode);

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
    void runSMAA(SDL_GPUCommandBuffer *cmd);
    void postProcess(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchainTexture);

    void loadSmaaLuts();
    void loadSmaaTextureFromDDS(SDL_GPUTexture **textureOut, const char *filepath, SDL_GPUTextureFormat format);
};
