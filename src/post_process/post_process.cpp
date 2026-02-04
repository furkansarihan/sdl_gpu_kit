#include "post_process.h"

#include <SDL3/SDL_gpu.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include "../resource_manager/dds_loader.h"
#include "../utils/utils.h"

PostProcess::PostProcess(SDL_GPUSampleCount sampleCount)
{
    m_fullscreenVert = Utils::loadShader("src/shaders/fullscreen.vert", 0, 0, SDL_GPU_SHADERSTAGE_VERTEX);
    m_postProcessFrag = Utils::loadShader("src/shaders/post.frag", 4, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_bloomDownFrag = Utils::loadShader("src/shaders/downsample.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_bloomUpFrag = Utils::loadShader("src/shaders/upsample.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);

    // Load GTAO Shaders (replaces SSAO)
    m_gtaoGenFrag = Utils::loadShader("src/shaders/gtao.frag", 2, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_gtaoBlurFrag = Utils::loadShader("src/shaders/bilateral_blur.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_depthCopyFrag = Utils::loadShader("src/shaders/depth_copy.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_depthResolveFrag = Utils::loadShader("src/shaders/depth_resolve.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);

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

    // --- SMAA Edge Pass ---
    SDL_GPUColorTargetDescription smaaEdgeDesc{};
    smaaEdgeDesc.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    SDL_GPUGraphicsPipelineCreateInfo smaaEdgeInfo{};
    smaaEdgeInfo.vertex_shader = m_fullscreenVert;
    smaaEdgeInfo.fragment_shader = Utils::loadShader(
        "src/shaders/smaa_edge.frag",
        1,
        1,
        SDL_GPU_SHADERSTAGE_FRAGMENT);
    smaaEdgeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    smaaEdgeInfo.target_info.num_color_targets = 1;
    smaaEdgeInfo.target_info.color_target_descriptions = &smaaEdgeDesc;

    m_smaaEdgePipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &smaaEdgeInfo);
    if (!m_smaaEdgePipeline)
    {
        SDL_Log("Failed to create m_smaaEdgePipeline: %s", SDL_GetError());
        return;
    }

    // --- SMAA Blend Weights Pass ---
    SDL_GPUColorTargetDescription smaaBlendDesc{};
    smaaBlendDesc.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    SDL_GPUGraphicsPipelineCreateInfo smaaBlendInfo{};
    smaaBlendInfo.vertex_shader = m_fullscreenVert;
    smaaBlendInfo.fragment_shader = Utils::loadShader(
        "src/shaders/smaa_blend.frag", 3, 1,
        SDL_GPU_SHADERSTAGE_FRAGMENT);
    smaaBlendInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    smaaBlendInfo.target_info.num_color_targets = 1;
    smaaBlendInfo.target_info.color_target_descriptions = &smaaBlendDesc;

    m_smaaBlendPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &smaaBlendInfo);
    if (!m_smaaBlendPipeline)
    {
        SDL_Log("Failed to create m_smaaBlendPipeline: %s", SDL_GetError());
        return;
    }

    // --- SMAA Neighborhood Blending Pass ---
    SDL_GPUColorTargetDescription smaaColorDesc{};
    smaaColorDesc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // same as m_colorTexture

    SDL_GPUGraphicsPipelineCreateInfo smaaNeighborInfo{};
    smaaNeighborInfo.vertex_shader = m_fullscreenVert;
    smaaNeighborInfo.fragment_shader = Utils::loadShader(
        "src/shaders/smaa_neighbor.frag",
        /*numSamplers=*/2, // scene + blend weights
        /*numUniforms=*/1,
        SDL_GPU_SHADERSTAGE_FRAGMENT);
    smaaNeighborInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    smaaNeighborInfo.target_info.num_color_targets = 1;
    smaaNeighborInfo.target_info.color_target_descriptions = &smaaColorDesc;

    m_smaaNeighborPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &smaaNeighborInfo);
    if (!m_smaaNeighborPipeline)
    {
        SDL_Log("Failed to create m_smaaNeighborPipeline: %s", SDL_GetError());
        return;
    }

    loadSmaaLuts();

    colorTargetDesc[0].format = SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
    pp.target_info.color_target_descriptions = colorTargetDesc;
    pp.fragment_shader = m_bloomDownFrag;
    m_bloomDownPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &pp);

    // upsample - blend
    colorTargetDesc[0].blend_state.enable_blend = true;
    colorTargetDesc[0].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTargetDesc[0].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTargetDesc[0].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTargetDesc[0].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTargetDesc[0].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTargetDesc[0].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;

    pp.fragment_shader = m_bloomUpFrag;
    m_bloomUpPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &pp);

    // depth copy
    SDL_GPUGraphicsPipelineCreateInfo pp1{};
    pp1.vertex_shader = m_fullscreenVert;
    pp1.fragment_shader = m_depthCopyFrag;
    pp1.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUColorTargetDescription colorOut{};
    colorOut.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;

    pp1.target_info.num_color_targets = 1;
    pp1.target_info.color_target_descriptions = &colorOut;

    m_depthCopyPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &pp1);

    // depth resolve
    SDL_GPUGraphicsPipelineCreateInfo pp2{};
    pp2.vertex_shader = m_fullscreenVert;
    pp2.fragment_shader = m_depthResolveFrag;
    pp2.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUColorTargetDescription colorOut2{};
    colorOut2.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;

    pp2.target_info.num_color_targets = 1;
    pp2.target_info.color_target_descriptions = &colorOut2;

    m_depthResolvePipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &pp2);

    // 1. GTAO Generation Pipeline (Output: RG8 or RG16F)
    SDL_GPUColorTargetDescription gtaoTargetDesc{};
    gtaoTargetDesc.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM; // R=visibility, G=packed depth

    SDL_GPUGraphicsPipelineCreateInfo gtaoPP{};
    gtaoPP.vertex_shader = m_fullscreenVert;
    gtaoPP.fragment_shader = m_gtaoGenFrag;
    gtaoPP.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gtaoPP.target_info.num_color_targets = 1;
    gtaoPP.target_info.color_target_descriptions = &gtaoTargetDesc;
    m_gtaoGenPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &gtaoPP);

    // 2. GTAO Blur Pipeline (can reuse SSAO blur shader)
    SDL_GPUColorTargetDescription blurTargetDesc{};
    blurTargetDesc.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM; // Just the visibility channel

    gtaoPP.fragment_shader = m_gtaoBlurFrag;
    gtaoPP.target_info.color_target_descriptions = &blurTargetDesc;
    m_gtaoBlurPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &gtaoPP);

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.enable_anisotropy = true;
    samplerInfo.max_anisotropy = 16.0f;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 1000.0f;
    m_clampedSampler = SDL_CreateGPUSampler(Utils::device, &samplerInfo);

    m_UBO.exposure = 1.1f;
    m_UBO.gamma = 2.2f;
    m_UBO.bloomIntensity = 0.2f;
    m_UBO.fxaaEnabled = 0;
    m_UBO.lutEnabled = 0;
    m_UBO.lutIntensity = 1.f;
    m_upsampleUBO.filterRadius = 1.;
    m_downsampleUBO.highlight = 100.0f;

    {
        m_gtaoResolutionFactor = 1.f;
        m_gtaoParams.intensity = 1.0f;
        m_gtaoParams.radius = 0.4f;
        m_gtaoParams.power = 1.0f;
        m_gtaoParams.thicknessHeuristic = 0.0f;
        m_gtaoParams.constThickness = 0.1f;
        m_gtaoParams.sliceCount = glm::vec2(4.0f, 1.0f / 4.0f); // 4 slices
        m_gtaoParams.stepsPerSlice = 4.0f;
        m_gtaoParams.maxLevel = 0; // Mip level for depth sampling
    }

    m_sampleCount = sampleCount;

    m_aaMode = AA_SMAA;
    m_smaaUniforms.edgeDetectionMode = 0;
}

PostProcess::~PostProcess()
{
    SDL_ReleaseGPUSampler(Utils::device, m_clampedSampler);
    SDL_ReleaseGPUTexture(Utils::device, m_intermediateTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_colorTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_depthTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_gtaoRawTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_gtaoBlur0Texture);
    SDL_ReleaseGPUTexture(Utils::device, m_gtaoBlur1Texture);

    for (int i = 0; i < BLOOM_MIPS; i++)
        SDL_ReleaseGPUTexture(Utils::device, m_bloomMip[i]);

    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_postProcessPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_bloomDownPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_bloomUpPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_gtaoGenPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_gtaoBlurPipeline);

    // SMAA
    SDL_ReleaseGPUTexture(Utils::device, m_smaaEdgeTex);
    SDL_ReleaseGPUTexture(Utils::device, m_smaaBlendTex);
    SDL_ReleaseGPUTexture(Utils::device, m_smaaColorTex);
    SDL_ReleaseGPUTexture(Utils::device, m_smaaAreaTex);
    SDL_ReleaseGPUTexture(Utils::device, m_smaaSearchTex);
    SDL_ReleaseGPUSampler(Utils::device, m_smaaLutSampler);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_smaaEdgePipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_smaaBlendPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_smaaNeighborPipeline);
}

void PostProcess::renderUI()
{
    if (!ImGui::CollapsingHeader("Post Process", m_uiDefaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        return;

    ImGui::Text("Screen Size: (%d, %d)", (int)m_UBO.screenSize.x, (int)m_UBO.screenSize.y);

    const char *aaItems[] = {"None", "FXAA", "SMAA"};
    int aaMode = m_aaMode;
    if (ImGui::Combo("Anti-Aliasing", &aaMode, aaItems, IM_ARRAYSIZE(aaItems)))
        setAntiAliasingMode((AntiAliasingMode)aaMode);

    {
        const char *msaaOptions[] = {"1x", "2x", "4x", "8x"};
        SDL_GPUSampleCount msaaValues[] = {
            SDL_GPU_SAMPLECOUNT_1,
            SDL_GPU_SAMPLECOUNT_2,
            SDL_GPU_SAMPLECOUNT_4,
            SDL_GPU_SAMPLECOUNT_8};

        int currentIndex = 0;
        switch (m_sampleCount)
        {
        case SDL_GPU_SAMPLECOUNT_1:
            currentIndex = 0;
            break;
        case SDL_GPU_SAMPLECOUNT_2:
            currentIndex = 1;
            break;
        case SDL_GPU_SAMPLECOUNT_4:
            currentIndex = 2;
            break;
        case SDL_GPU_SAMPLECOUNT_8:
            currentIndex = 3;
            break;
        }

        if (ImGui::BeginCombo("MSAA", msaaOptions[currentIndex]))
        {
            for (int i = 0; i < 4; i++)
            {
                bool isSupported = SDL_GPUTextureSupportsSampleCount(Utils::device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, msaaValues[i]);

                if (!isSupported)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    ImGui::Selectable(msaaOptions[i], false, ImGuiSelectableFlags_Disabled);
                    ImGui::PopStyleColor();
                }
                else
                {
                    bool isSelected = (currentIndex == i);
                    if (ImGui::Selectable(msaaOptions[i], isSelected))
                    {
                        m_sampleCount = msaaValues[i];
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    if (ImGui::TreeNode("Tone Mapping"))
    {
        ImGui::DragFloat("Exposure", &m_UBO.exposure, 0.01f, 0.f);
        ImGui::DragFloat("Gamma", &m_UBO.gamma, 0.01f, 0.f);
        ImGui::DragFloat("Lut Intensity", &m_UBO.lutIntensity, 0.01f, 0.f);

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Bloom"))
    {
        ImGui::DragFloat("Intensity", &m_UBO.bloomIntensity, 0.01f, 0.f);
        ImGui::DragFloat("Filter Radius", &m_upsampleUBO.filterRadius, 0.001f, 0.f);
        ImGui::DragFloat("Higlight", &m_downsampleUBO.highlight, 0.1f, 0.f);

        if (ImGui::TreeNode("Mip Textures"))
        {
            for (int i = 0; i < BLOOM_MIPS; i++)
            {
                if (!m_bloomMip[i])
                    continue;

                ImGui::Text("Mip %d", i);
                ImTextureID texID = (ImTextureID)(m_bloomMip[i]);
                ImGui::Image(texID, ImVec2(m_UBO.screenSize.x * 0.2f, m_UBO.screenSize.y * 0.2f));

                ImGui::Spacing();
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("GTAO"))
    // if (ImGui::TreeNodeEx("GTAO", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::DragFloat("Resolution Factor", &m_gtaoResolutionFactor, 0.01f, 0.1f, 1.0f);
        ImGui::DragFloat("Intensity", &m_gtaoParams.intensity, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat("Radius", &m_gtaoParams.radius, 0.01f, 0.f, 5.0f);
        ImGui::DragFloat("Power", &m_gtaoParams.power, 0.1f, 0.f, 10.0f);

        if (ImGui::TreeNode("Advanced"))
        {
            int slices = (int)m_gtaoParams.sliceCount.x;
            if (ImGui::SliderInt("Slices", &slices, 1, 8))
            {
                m_gtaoParams.sliceCount = glm::vec2(slices, 1.0f / slices);
            }

            ImGui::DragFloat("Steps Per Slice", &m_gtaoParams.stepsPerSlice, 0.5f, 1.0f, 16.0f);
            ImGui::DragFloat("Thickness Heuristic", &m_gtaoParams.thicknessHeuristic, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Const Thickness", &m_gtaoParams.constThickness, 0.01f, 0.0f, 1.0f);

            ImGui::TreePop();
        }

        ImVec2 size(m_UBO.screenSize.x * 0.3f, m_UBO.screenSize.y * 0.3f);

        ImGui::Text("GTAO - Raw");
        if (m_gtaoRawTexture)
            ImGui::Image((ImTextureID)(m_gtaoRawTexture), size);

        ImGui::Text("GTAO - Blur");
        if (m_gtaoBlur1Texture)
            ImGui::Image((ImTextureID)(m_gtaoBlur1Texture), size);

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("SMAA"))
    {
        const char *edgeItems[] = {"Color-based (better quality)", "Luma-based (faster)"};
        int edgeMode = m_smaaUniforms.edgeDetectionMode;

        if (ImGui::Combo("Edge Detection", &edgeMode, edgeItems, IM_ARRAYSIZE(edgeItems)))
            m_smaaUniforms.edgeDetectionMode = edgeMode;

        float scale = 0.4f;
        ImVec2 size(m_UBO.screenSize.x * scale, m_UBO.screenSize.y * scale);

        ImGui::Text("m_smaaEdgeTex");
        ImGui::Image((ImTextureID)(m_smaaEdgeTex), size);
        ImGui::Text("m_smaaBlendTex");
        ImGui::Image((ImTextureID)(m_smaaBlendTex), size);
        ImGui::Text("m_smaaColorTex");
        ImGui::Image((ImTextureID)(m_smaaColorTex), size);

        if (ImGui::TreeNode("Lut Textures"))
        {

#define AREATEX_WIDTH 160
#define AREATEX_HEIGHT 560
#define SEARCHTEX_WIDTH 64
#define SEARCHTEX_HEIGHT 16

            ImVec2 sizeArea(AREATEX_WIDTH, AREATEX_HEIGHT);
            ImVec2 sizeSearch(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT);

            ImGui::Text("m_smaaAreaTex");
            ImGui::Image((ImTextureID)(m_smaaAreaTex), sizeArea);
            ImGui::Text("m_smaaSearchTex");
            ImGui::Image((ImTextureID)(m_smaaSearchTex), sizeSearch);

            ImGui::TreePop();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Textures"))
    {
        ImGui::Text("Color");
        ImGui::Image((ImTextureID)(m_colorTexture), ImVec2(m_UBO.screenSize.x * 0.2f, m_UBO.screenSize.y * 0.2f));
        ImGui::Text("Depth");
        ImGui::Image((ImTextureID)(m_depthTexture), ImVec2(m_UBO.screenSize.x * 0.2f, m_UBO.screenSize.y * 0.2f));

        ImGui::TreePop();
    }
}

void PostProcess::setAntiAliasingMode(AntiAliasingMode mode)
{
    m_aaMode = (AntiAliasingMode)mode;
    m_UBO.fxaaEnabled = (m_aaMode == AA_FXAA) ? 1 : 0;
}

void PostProcess::update(glm::ivec2 screenSize)
{
    m_UBO.screenSize = screenSize;

    static Uint32 lastW = 0, lastH = 0;
    static SDL_GPUSampleCount lastSampleCount;
    static float lastGtaoResolution;
    if (lastW != screenSize.x ||
        lastH != screenSize.y ||
        lastSampleCount != m_sampleCount ||
        lastGtaoResolution != m_gtaoResolutionFactor)
    {
        if (m_intermediateTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_intermediateTexture);

        SDL_GPUTextureCreateInfo colorInfo{};
        colorInfo.format = SDL_GetGPUSwapchainTextureFormat(Utils::device, Utils::window);
        colorInfo.width = screenSize.x;
        colorInfo.height = screenSize.y;
        colorInfo.num_levels = 1;
        colorInfo.type = SDL_GPU_TEXTURETYPE_2D;
        colorInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        colorInfo.sample_count = SDL_GPUSampleCount::SDL_GPU_SAMPLECOUNT_1;
        m_intermediateTexture = SDL_CreateGPUTexture(Utils::device, &colorInfo);

        if (m_msaaColorTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_msaaColorTexture);
        if (m_msaaDepthTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_msaaDepthTexture);

        // MSAA Color Target
        SDL_GPUTextureCreateInfo msaaColorInfo{};
        msaaColorInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        msaaColorInfo.width = screenSize.x;
        msaaColorInfo.height = screenSize.y;
        msaaColorInfo.num_levels = 1;
        msaaColorInfo.type = SDL_GPU_TEXTURETYPE_2D;
        msaaColorInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; // Not sampled directly
        msaaColorInfo.sample_count = m_sampleCount;
        m_msaaColorTexture = SDL_CreateGPUTexture(Utils::device, &msaaColorInfo);

        // MSAA Depth Target
        SDL_GPUTextureCreateInfo msaaDepthInfo{};
        msaaDepthInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        msaaDepthInfo.width = screenSize.x;
        msaaDepthInfo.height = screenSize.y;
        msaaDepthInfo.num_levels = 1;
        msaaDepthInfo.type = SDL_GPU_TEXTURETYPE_2D;
        msaaDepthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        msaaDepthInfo.sample_count = m_sampleCount;
        m_msaaDepthTexture = SDL_CreateGPUTexture(Utils::device, &msaaDepthInfo);

        if (m_colorTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_colorTexture);

        SDL_GPUTextureCreateInfo rtInfo{};
        rtInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
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
            info.format = SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
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
        depthInfo.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        depthInfo.width = screenSize.x;
        depthInfo.height = screenSize.y;
        depthInfo.layer_count_or_depth = 1;
        depthInfo.num_levels = 1;
        depthInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        m_depthTexture = SDL_CreateGPUTexture(Utils::device, &depthInfo);

        if (m_gtaoRawTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_gtaoRawTexture);
        if (m_gtaoBlur0Texture)
            SDL_ReleaseGPUTexture(Utils::device, m_gtaoBlur0Texture);
        if (m_gtaoBlur1Texture)
            SDL_ReleaseGPUTexture(Utils::device, m_gtaoBlur1Texture);

        SDL_GPUTextureCreateInfo gtaoRawInfo{};
        gtaoRawInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM; // RG for visibility + packed depth
        m_gtaoResolutionFactor = std::max(m_gtaoResolutionFactor, 0.1f);
        gtaoRawInfo.width = screenSize.x * m_gtaoResolutionFactor;
        gtaoRawInfo.height = screenSize.y * m_gtaoResolutionFactor;
        gtaoRawInfo.num_levels = 1;
        gtaoRawInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

        m_gtaoRawTexture = SDL_CreateGPUTexture(Utils::device, &gtaoRawInfo);
        SDL_SetGPUTextureName(Utils::device, m_gtaoRawTexture, "GTAO Raw");

        SDL_GPUTextureCreateInfo gtaoBlurInfo{};
        gtaoBlurInfo.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM; // Just visibility
        gtaoBlurInfo.width = screenSize.x * m_gtaoResolutionFactor;
        gtaoBlurInfo.height = screenSize.y * m_gtaoResolutionFactor;
        gtaoBlurInfo.num_levels = 1;
        gtaoBlurInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

        m_gtaoBlur0Texture = SDL_CreateGPUTexture(Utils::device, &gtaoBlurInfo);
        m_gtaoBlur1Texture = SDL_CreateGPUTexture(Utils::device, &gtaoBlurInfo);

        if (m_gtaoMaskTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_gtaoMaskTexture);

        // 1. Create the Texture
        SDL_GPUTextureCreateInfo maskInfo{};
        maskInfo.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM; // Matches uint8_t data
        maskInfo.width = ScreenMask64::GRID_WIDTH;
        maskInfo.height = ScreenMask64::GRID_HEIGHT;
        maskInfo.layer_count_or_depth = 1;
        maskInfo.num_levels = 1;
        maskInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; // We only read it in shader

        m_gtaoMaskTexture = SDL_CreateGPUTexture(Utils::device, &maskInfo);
        SDL_SetGPUTextureName(Utils::device, m_gtaoMaskTexture, "GTAO Mask");

        // SMAA edge texture
        if (m_smaaEdgeTex)
            SDL_ReleaseGPUTexture(Utils::device, m_smaaEdgeTex);

        SDL_GPUTextureCreateInfo edgeInfo{};
        edgeInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        edgeInfo.width = screenSize.x;
        edgeInfo.height = screenSize.y;
        edgeInfo.num_levels = 1;
        edgeInfo.type = SDL_GPU_TEXTURETYPE_2D;
        edgeInfo.layer_count_or_depth = 1;
        edgeInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        m_smaaEdgeTex = SDL_CreateGPUTexture(Utils::device, &edgeInfo);

        // SMAA blend weights texture
        if (m_smaaBlendTex)
            SDL_ReleaseGPUTexture(Utils::device, m_smaaBlendTex);

        SDL_GPUTextureCreateInfo blendInfo = edgeInfo;
        m_smaaBlendTex = SDL_CreateGPUTexture(Utils::device, &blendInfo);

        // SMAA output color texture
        if (m_smaaColorTex)
            SDL_ReleaseGPUTexture(Utils::device, m_smaaColorTex);

        SDL_GPUTextureCreateInfo smaaColorInfo{};
        smaaColorInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        smaaColorInfo.width = screenSize.x;
        smaaColorInfo.height = screenSize.y;
        smaaColorInfo.num_levels = 1;
        smaaColorInfo.type = SDL_GPU_TEXTURETYPE_2D;
        smaaColorInfo.layer_count_or_depth = 1;
        smaaColorInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        m_smaaColorTex = SDL_CreateGPUTexture(Utils::device, &smaaColorInfo);

        m_smaaUniforms.rtMetrics = glm::vec4(
            1.0f / screenSize.x,
            1.0f / screenSize.y,
            (float)screenSize.x,
            (float)screenSize.y);

        lastW = screenSize.x;
        lastH = screenSize.y;
        lastSampleCount = m_sampleCount;
        lastGtaoResolution = m_gtaoResolutionFactor;
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

            m_downsampleUBO.mipLevel = i;
            SDL_PushGPUFragmentUniformData(commandBuffer, 0, &m_downsampleUBO, sizeof(m_downsampleUBO));

            SDL_GPUTextureSamplerBinding bind = {src, m_clampedSampler};
            SDL_BindGPUFragmentSamplers(rp, 0, &bind, 1);

            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(rp);

        // SDL_GenerateMipmapsForGPUTexture(commandBuffer, rt.texture);

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

        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(commandBuffer, &rt, 1, nullptr);
        {
            SDL_BindGPUGraphicsPipeline(rp, m_bloomUpPipeline);

            SDL_PushGPUFragmentUniformData(commandBuffer, 0, &m_upsampleUBO, sizeof(m_upsampleUBO));

            SDL_GPUTextureSamplerBinding bind = {m_bloomMip[i], m_clampedSampler};
            SDL_BindGPUFragmentSamplers(rp, 0, &bind, 1);

            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(rp);

        // SDL_GenerateMipmapsForGPUTexture(commandBuffer, rt.texture);
    }
}

void PostProcess::resolveDepth(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUColorTargetInfo target{};
    target.texture = m_depthTexture;
    target.clear_color = {0, 0, 0, 0};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);

    if (m_sampleCount == SDL_GPU_SAMPLECOUNT_1)
        SDL_BindGPUGraphicsPipeline(pass, m_depthCopyPipeline);
    else
        SDL_BindGPUGraphicsPipeline(pass, m_depthResolvePipeline);

    SDL_GPUTextureSamplerBinding binding{};
    binding.texture = m_msaaDepthTexture;
    binding.sampler = m_clampedSampler;

    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

    SDL_EndGPURenderPass(pass);
}

void PostProcess::computeGTAO(
    SDL_GPUCommandBuffer *commandBuffer,
    const glm::mat4 &projectionMatrix,
    const glm::mat4 &viewMatrix,
    float nearPlane,
    float farPlane)
{
    glm::mat4 invProj = glm::inverse(projectionMatrix);

    m_gtaoParams.resolution = glm::vec4(
        m_UBO.screenSize.x * m_gtaoResolutionFactor,
        m_UBO.screenSize.y * m_gtaoResolutionFactor,
        1.0f / (m_UBO.screenSize.x * m_gtaoResolutionFactor),
        1.0f / (m_UBO.screenSize.y * m_gtaoResolutionFactor));

    m_gtaoParams.positionParams = glm::vec2(
        invProj[0][0],
        invProj[1][1]);

    m_gtaoParams.invFarPlane = 1.0f / farPlane;

    // Estimate of the size in pixel units of a 1m tall/wide object viewed from 1m away (i.e. at z=-1)
    m_gtaoParams.projectionScale = std::min(
        0.5f * projectionMatrix[0].x * m_gtaoParams.resolution.x,
        0.5f * projectionMatrix[1].y * m_gtaoParams.resolution.y);

    // Calculate derived values
    m_gtaoParams.invRadiusSquared = 1.0f / (m_gtaoParams.radius * m_gtaoParams.radius);
    m_gtaoParams.projectionScaleRadius = m_gtaoParams.projectionScale * m_gtaoParams.radius;

    m_gtaoParams.nearPlane = nearPlane;
    m_gtaoParams.farPlane = farPlane;

    // update mask texture
    {
        // 1. Create a Transfer Buffer (Staging buffer)
        Uint32 dataSize = m_gtaoMask.getDataSize();

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = dataSize;

        SDL_GPUTransferBuffer *transferBuffer = SDL_CreateGPUTransferBuffer(Utils::device, &transferInfo);

        // 2. Map memory and copy CPU data
        Uint8 *mapData = (Uint8 *)SDL_MapGPUTransferBuffer(Utils::device, transferBuffer, false);
        if (mapData)
        {
            std::memcpy(mapData, m_gtaoMask.getData(), dataSize);
            SDL_UnmapGPUTransferBuffer(Utils::device, transferBuffer);
        }

        // 3. Define the copy operation
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transferBuffer;
        source.offset = 0;
        source.pixels_per_row = ScreenMask64::GRID_WIDTH;
        source.rows_per_layer = ScreenMask64::GRID_HEIGHT;

        SDL_GPUTextureRegion destination{};
        destination.texture = m_gtaoMaskTexture;
        destination.w = ScreenMask64::GRID_WIDTH;
        destination.h = ScreenMask64::GRID_HEIGHT;
        destination.d = 1;

        // 4. Record the upload command
        SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
        SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
        SDL_EndGPUCopyPass(copyPass);

        // 5. Cleanup
        // SDL3 tracks the buffer usage, so we can release the handle immediately
        // and the driver will destroy it after the command buffer finishes.
        SDL_ReleaseGPUTransferBuffer(Utils::device, transferBuffer);
    }

    // 1. GTAO Generation Pass
    SDL_GPUColorTargetInfo genTarget{};
    genTarget.texture = m_gtaoRawTexture;
    genTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    genTarget.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *genPass = SDL_BeginGPURenderPass(commandBuffer, &genTarget, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(genPass, m_gtaoGenPipeline);

        SDL_GPUTextureSamplerBinding textures[2] = {
            {m_depthTexture, m_clampedSampler},
            {m_gtaoMaskTexture, m_clampedSampler},
        };
        SDL_BindGPUFragmentSamplers(genPass, 0, textures, 2);

        // Push GTAO parameters
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &m_gtaoParams, sizeof(m_gtaoParams));

        SDL_DrawGPUPrimitives(genPass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(genPass);

    struct BlurFragUBO
    {
        glm::vec2 invResolutionDirection;
        float sharpness;
        float padding0;
    } ubo;

    // 2. GTAO Blur

    // Pass 1: Horizontal blur (raw → blur temp)
    SDL_GPUColorTargetInfo blurTarget1{};
    blurTarget1.texture = m_gtaoBlur0Texture; // temp target
    blurTarget1.load_op = SDL_GPU_LOADOP_CLEAR;
    blurTarget1.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *blurPass1 = SDL_BeginGPURenderPass(commandBuffer, &blurTarget1, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(blurPass1, m_gtaoBlurPipeline);
        SDL_GPUTextureSamplerBinding input[1] = {{m_gtaoRawTexture, m_clampedSampler}};
        SDL_BindGPUFragmentSamplers(blurPass1, 0, input, 1);

        BlurFragUBO ubo;
        ubo.sharpness = 40.f;
        ubo.invResolutionDirection = glm::vec2(1.f / m_gtaoParams.resolution.x, 0.f);
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &ubo, sizeof(ubo));
        SDL_DrawGPUPrimitives(blurPass1, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(blurPass1);

    // Pass 2: Vertical blur (blur temp → final output)
    SDL_GPUColorTargetInfo blurTarget2{};
    blurTarget2.texture = m_gtaoBlur1Texture; // your final output texture
    blurTarget2.load_op = SDL_GPU_LOADOP_CLEAR;
    blurTarget2.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *blurPass2 = SDL_BeginGPURenderPass(commandBuffer, &blurTarget2, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(blurPass2, m_gtaoBlurPipeline);
        SDL_GPUTextureSamplerBinding input[1] = {{m_gtaoBlur0Texture, m_clampedSampler}};
        SDL_BindGPUFragmentSamplers(blurPass2, 0, input, 1);

        BlurFragUBO ubo;
        ubo.sharpness = 40.f;
        ubo.invResolutionDirection = glm::vec2(0.f, 1.f / m_gtaoParams.resolution.y);
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &ubo, sizeof(ubo));
        SDL_DrawGPUPrimitives(blurPass2, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(blurPass2);
}

void PostProcess::runSMAA(SDL_GPUCommandBuffer *cmd)
{
    if (m_aaMode != AA_SMAA)
        return;

    if (!m_colorTexture || !m_smaaEdgeTex || !m_smaaBlendTex || !m_smaaColorTex)
        return;

    // Push SMAA_RT_METRICS
    SDL_PushGPUFragmentUniformData(cmd, 0, &m_smaaUniforms, sizeof(m_smaaUniforms));

    // --- 1) Edge detection ---
    SDL_GPUColorTargetInfo edgeTarget{};
    edgeTarget.texture = m_smaaEdgeTex;
    edgeTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    edgeTarget.store_op = SDL_GPU_STOREOP_STORE;
    edgeTarget.clear_color = {0, 0, 0, 0};

    SDL_GPURenderPass *edgePass = SDL_BeginGPURenderPass(cmd, &edgeTarget, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(edgePass, m_smaaEdgePipeline);

        // IMPORTANT: for best results the input read for the color/luma edge detection should *NOT* be sRGB.
        SDL_GPUTextureSamplerBinding colorBind{m_colorTexture, m_smaaLutSampler};
        SDL_BindGPUFragmentSamplers(edgePass, 0, &colorBind, 1);
        SDL_DrawGPUPrimitives(edgePass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(edgePass);

    // --- 2) Blend weight calculation ---
    SDL_GPUColorTargetInfo blendTarget{};
    blendTarget.texture = m_smaaBlendTex;
    blendTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    blendTarget.store_op = SDL_GPU_STOREOP_STORE;
    blendTarget.clear_color = {0, 0, 0, 0};

    SDL_GPURenderPass *blendPass = SDL_BeginGPURenderPass(cmd, &blendTarget, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(blendPass, m_smaaBlendPipeline);

        SDL_GPUTextureSamplerBinding samplers[3] =
            {
                {m_smaaEdgeTex, m_smaaLutSampler},   // edge texture
                {m_smaaAreaTex, m_smaaLutSampler},   // area LUT
                {m_smaaSearchTex, m_smaaLutSampler}, // search LUT
            };
        SDL_BindGPUFragmentSamplers(blendPass, 0, samplers, 3);

        SDL_DrawGPUPrimitives(blendPass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(blendPass);

    // --- 3) Neighborhood blending ---
    SDL_GPUColorTargetInfo colorTarget{};
    colorTarget.texture = m_smaaColorTex;
    colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTarget.store_op = SDL_GPU_STOREOP_STORE;
    colorTarget.clear_color = {0, 0, 0, 0};

    SDL_GPURenderPass *neighborPass = SDL_BeginGPURenderPass(cmd, &colorTarget, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(neighborPass, m_smaaNeighborPipeline);

        SDL_GPUTextureSamplerBinding samplers[2] =
            {
                {m_colorTexture, m_smaaLutSampler},
                {m_smaaBlendTex, m_smaaLutSampler},
            };
        SDL_BindGPUFragmentSamplers(neighborPass, 0, samplers, 2);

        SDL_DrawGPUPrimitives(neighborPass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(neighborPass);
}

void PostProcess::postProcess(SDL_GPUCommandBuffer *commandBuffer, SDL_GPUTexture *swapchainTexture, glm::vec2 swapchainSize)
{
    // 1: Render Post-Process effects to Intermediate Texture

    SDL_GPUColorTargetInfo intermediateTarget{};
    intermediateTarget.texture = m_intermediateTexture;
    intermediateTarget.load_op = SDL_GPU_LOADOP_DONT_CARE;
    intermediateTarget.store_op = SDL_GPU_STOREOP_STORE;
    intermediateTarget.clear_color = {0.f, 0.f, 0.f, 1.f};

    SDL_GPUTexture *color = m_colorTexture;
    if (m_aaMode == AA_SMAA)
        color = m_smaaColorTex;

    SDL_GPURenderPass *intermediatePass = SDL_BeginGPURenderPass(commandBuffer, &intermediateTarget, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(intermediatePass, m_postProcessPipeline);

        SDL_GPUTextureSamplerBinding inputs[4] =
            {
                {color, Utils::baseSampler},
                {m_bloomMip[0], Utils::baseSampler},
                {m_gtaoBlur1Texture, Utils::baseSampler},
                {m_lutTex, m_smaaLutSampler},
            };
        SDL_BindGPUFragmentSamplers(intermediatePass, 0, inputs, 4);

        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &m_UBO, sizeof(m_UBO));

        SDL_DrawGPUPrimitives(intermediatePass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(intermediatePass);

    // 2: Blit Intermediate Result to Swapchain

    SDL_GPUBlitInfo blitInfo{};
    blitInfo.source.texture = m_intermediateTexture;
    blitInfo.source.w = m_UBO.screenSize.x;
    blitInfo.source.h = m_UBO.screenSize.y;

    blitInfo.destination.texture = swapchainTexture;
    blitInfo.destination.w = swapchainSize.x;
    blitInfo.destination.h = swapchainSize.y;

    blitInfo.filter = SDL_GPU_FILTER_LINEAR;
    blitInfo.load_op = SDL_GPU_LOADOP_DONT_CARE;

    SDL_BlitGPUTexture(commandBuffer, &blitInfo);
}

void PostProcess::loadSmaaLuts()
{
    if (!m_smaaLutSampler)
    {
        SDL_GPUSamplerCreateInfo samplerInfo{};
        samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

        m_smaaLutSampler = SDL_CreateGPUSampler(Utils::device, &samplerInfo);
    }

    std::string exePath = Utils::getExecutablePath();

    // --- AREA TEXTURE (RG8) ---
    // Ensure you save "AreaTex.dds" as Uncompressed R8G8 (or A8L8 legacy)
    loadSmaaTextureFromDDS(&m_smaaAreaTex,
                           std::string(exePath + "/src/assets/textures/AreaTexDX9.dds").c_str(),
                           SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);

    SDL_SetGPUTextureName(Utils::device, m_smaaAreaTex, "SMAA Area");

    // --- SEARCH TEXTURE (R8) ---
    // Ensure you save "SearchTex.dds" as Uncompressed R8 (Luminance)
    loadSmaaTextureFromDDS(&m_smaaSearchTex,
                           std::string(exePath + "/src/assets/textures/SearchTex.dds").c_str(),
                           SDL_GPU_TEXTUREFORMAT_R8_UNORM);

    SDL_SetGPUTextureName(Utils::device, m_smaaSearchTex, "SMAA Search");
}

void PostProcess::loadSmaaTextureFromDDS(SDL_GPUTexture **textureOut,
                                         const char *filepath,
                                         SDL_GPUTextureFormat expectedFormat)
{
    DDSTextureInfo *info = DDSLoader::LoadFromFile(Utils::device, filepath);

    if (!info)
    {
        SDL_Log("Failed to load SMAA texture: %s", filepath);
        return;
    }

    // Verify it's the expected format
    if (info->format != expectedFormat)
    {
        SDL_Log("Warning: Loaded format (%d) differs from expected (%d)",
                info->format, expectedFormat);
    }

    if (*textureOut)
    {
        SDL_ReleaseGPUTexture(Utils::device, *textureOut);
    }

    *textureOut = info->texture;
    info->texture = nullptr; // Transfer ownership

    DDSLoader::Release(Utils::device, info);
}
