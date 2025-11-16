#include "post_process.h"

#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

#include "../utils/utils.h"

PostProcess::PostProcess()
{
    m_fullscreenVert = Utils::LoadShader("src/shaders/fullscreen.vert", 0, 0, SDL_GPU_SHADERSTAGE_VERTEX);
    m_postProcessFrag = Utils::LoadShader("src/shaders/post.frag", 2, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_bloomDownFrag = Utils::LoadShader("src/shaders/downsample.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_bloomUpFrag = Utils::LoadShader("src/shaders/upsample.frag", 2, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);

    SDL_GPUColorTargetDescription colorTargetDesc[1];
    colorTargetDesc[0] = {};
    colorTargetDesc[0].format = SDL_GetGPUSwapchainTextureFormat(Utils::device, Utils::window);

    SDL_GPUGraphicsPipelineCreateInfo pp{};
    pp.vertex_shader = m_fullscreenVert;
    pp.fragment_shader = m_postProcessFrag;
    pp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pp.target_info.num_color_targets = 1;
    pp.target_info.color_target_descriptions = colorTargetDesc;
    m_postProcessPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &pp);

    colorTargetDesc[0].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    pp.target_info.color_target_descriptions = colorTargetDesc;
    pp.fragment_shader = m_bloomDownFrag;
    m_bloomDownPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &pp);

    pp.fragment_shader = m_bloomUpFrag;
    m_bloomUpPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &pp);

    m_UBO.exposure = 1.1f;
    m_UBO.gamma = 2.2f;
    m_UBO.bloomIntensity = 0.1f;
    m_skyUBO.lod = 0.f;
}

PostProcess::~PostProcess()
{
    SDL_ReleaseGPUTexture(Utils::device, m_colorTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_depthTexture);

    for (int i = 0; i < BLOOM_MIPS; i++)
        SDL_ReleaseGPUTexture(Utils::device, m_bloomMip[i]);

    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_postProcessPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_bloomDownPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_bloomUpPipeline);
}

void PostProcess::renderUI()
{
    if (!ImGui::CollapsingHeader("Post Process", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (ImGui::TreeNode("Skybox"))
    {
        ImGui::DragFloat("LOD", &m_skyUBO.lod, 0.1f, 0.f, 10.f);

        ImGui::TreePop();
    }

    ImGui::DragFloat("Exposure", &m_UBO.exposure, 0.01f, 0.f);
    ImGui::DragFloat("Gamma", &m_UBO.gamma, 0.01f, 0.f);
    ImGui::DragFloat("Bloom Intensity", &m_UBO.bloomIntensity, 0.01f, 0.f);
}

void PostProcess::update(glm::ivec2 screenSize)
{
    m_UBO.screenSize = screenSize;

    static Uint32 lastW = 0, lastH = 0;
    if (!m_colorTexture || lastW != screenSize.x || lastH != screenSize.y)
    {
        if (m_colorTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_colorTexture);

        SDL_GPUTextureCreateInfo rtInfo{};
        rtInfo.format = SDL_GetGPUSwapchainTextureFormat(Utils::device, Utils::window);
        rtInfo.width = screenSize.x;
        rtInfo.height = screenSize.y;
        rtInfo.num_levels = 1;
        rtInfo.type = SDL_GPU_TEXTURETYPE_2D;
        rtInfo.layer_count_or_depth = 1;
        rtInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

        m_colorTexture = SDL_CreateGPUTexture(Utils::device, &rtInfo);

        for (int i = 0; i < BLOOM_MIPS; i++)
        {
            if (m_bloomMip[i])
                SDL_ReleaseGPUTexture(Utils::device, m_bloomMip[i]);

            SDL_GPUTextureCreateInfo info{};
            info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            info.width = screenSize.x >> i;
            info.height = screenSize.y >> i;
            info.num_levels = 1;
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.layer_count_or_depth = 1;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

            m_bloomMip[i] = SDL_CreateGPUTexture(Utils::device, &info);
        }

        if (m_depthTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_depthTexture);

        SDL_GPUTextureCreateInfo depthInfo{};
        depthInfo.type = SDL_GPU_TEXTURETYPE_2D;
        depthInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        depthInfo.width = screenSize.x;
        depthInfo.height = screenSize.y;
        depthInfo.layer_count_or_depth = 1;
        depthInfo.num_levels = 1;
        depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        m_depthTexture = SDL_CreateGPUTexture(Utils::device, &depthInfo);

        lastW = screenSize.x;
        lastH = screenSize.y;
    }
}

void PostProcess::downsample(SDL_GPUCommandBuffer *commandBuffer)
{
    // Downsample chain
    SDL_GPUTexture *src = m_colorTexture;

    for (int i = 0; i < BLOOM_MIPS; i++)
    {
        SDL_GPUColorTargetInfo rt{};
        rt.texture = m_bloomMip[i];
        rt.load_op = SDL_GPU_LOADOP_CLEAR;
        rt.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(commandBuffer, &rt, 1, nullptr);
        {
            SDL_BindGPUGraphicsPipeline(rp, m_bloomDownPipeline);

            SDL_GPUTextureSamplerBinding bind = {src, Utils::baseSampler};
            SDL_BindGPUFragmentSamplers(rp, 0, &bind, 1);

            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(rp);

        src = m_bloomMip[i]; // next mip uses this one
    }
}

void PostProcess::upsample(SDL_GPUCommandBuffer *commandBuffer)
{
    for (int i = BLOOM_MIPS - 1; i > 0; i--)
    {
        SDL_GPUColorTargetInfo rt{};
        rt.texture = m_bloomMip[i - 1];   // write to next bigger mip
        rt.load_op = SDL_GPU_LOADOP_LOAD; // keep existing bloom
        rt.store_op = SDL_GPU_STOREOP_STORE;
        rt.clear_color = {1.f, 0.f, 1.f, 1.f};

        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(commandBuffer, &rt, 1, nullptr);
        {
            SDL_BindGPUGraphicsPipeline(rp, m_bloomUpPipeline);

            SDL_GPUTextureSamplerBinding binds[2] =
                {
                    {m_bloomMip[i], Utils::baseSampler},    // smaller mip
                    {m_bloomMip[i - 1], Utils::baseSampler} // current mip
                };
            SDL_BindGPUFragmentSamplers(rp, 0, binds, 2);

            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(rp);
    }
}

void PostProcess::postProcess(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchainTexture)
{
    SDL_GPUColorTargetInfo finalTarget{};
    finalTarget.texture = swapchainTexture;
    finalTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    finalTarget.store_op = SDL_GPU_STOREOP_STORE;
    finalTarget.clear_color = {0.f, 0.f, 0.f, 1.f};

    SDL_GPURenderPass *finalPass = SDL_BeginGPURenderPass(commandBuffer, &finalTarget, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(finalPass, m_postProcessPipeline);

        SDL_GPUTextureSamplerBinding inputs[2] =
            {
                {m_colorTexture, Utils::baseSampler},
                {m_bloomMip[0], Utils::baseSampler},
            };
        SDL_BindGPUFragmentSamplers(finalPass, 0, inputs, 2);

        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &m_UBO, sizeof(m_UBO));

        SDL_DrawGPUPrimitives(finalPass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(finalPass);
}
