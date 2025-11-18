#include "post_process.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include "../utils/utils.h"

PostProcess::PostProcess()
{
    m_fullscreenVert = Utils::LoadShader("src/shaders/fullscreen.vert", 0, 0, SDL_GPU_SHADERSTAGE_VERTEX);
    m_postProcessFrag = Utils::LoadShader("src/shaders/post.frag", 3, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_bloomDownFrag = Utils::LoadShader("src/shaders/downsample.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_bloomUpFrag = Utils::LoadShader("src/shaders/upsample.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);

    // Load GTAO Shaders (replaces SSAO)
    m_gtaoGenFrag = Utils::LoadShader("src/shaders/gtao.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_gtaoBlurFrag = Utils::LoadShader("src/shaders/ssao_blur.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);
    m_depthCopyFrag = Utils::LoadShader("src/shaders/depth_copy.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);

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
    m_skyUBO.lod = 0.f;
    m_upsampleUBO.filterRadius = 1.;
    m_downsampleUBO.highlight = 100.0f;

    {
        m_gtaoParams.intensity = 1.0f;
        m_gtaoParams.radius = 0.4f;
        m_gtaoPower = 1.0f;
        m_gtaoParams.thicknessHeuristic = 0.0f;
        m_gtaoParams.constThickness = 0.1f;
        m_gtaoParams.sliceCount = glm::vec2(4.0f, 1.0f / 4.0f); // 4 slices
        m_gtaoParams.stepsPerSlice = 4.0f;
        m_gtaoParams.maxLevel = 0; // Mip level for depth sampling
    }
}

PostProcess::~PostProcess()
{
    SDL_ReleaseGPUSampler(Utils::device, m_clampedSampler);
    SDL_ReleaseGPUTexture(Utils::device, m_colorTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_depthTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_gtaoRawTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_gtaoBlurTexture);

    for (int i = 0; i < BLOOM_MIPS; i++)
        SDL_ReleaseGPUTexture(Utils::device, m_bloomMip[i]);

    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_postProcessPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_bloomDownPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_bloomUpPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_gtaoGenPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_gtaoBlurPipeline);
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

    if (ImGui::TreeNode("Tone Mapping"))
    {
        ImGui::DragFloat("Exposure", &m_UBO.exposure, 0.01f, 0.f);
        ImGui::DragFloat("Gamma", &m_UBO.gamma, 0.01f, 0.f);

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
        ImGui::DragFloat("Intensity", &m_gtaoParams.intensity, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat("Radius", &m_gtaoParams.radius, 0.01f, 0.f, 5.0f);
        ImGui::DragFloat("Power", &m_gtaoPower, 0.1f, 0.f, 10.0f);

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

        ImGui::Text("GTAO - Raw");
        if (m_gtaoRawTexture)
            ImGui::Image((ImTextureID)(m_gtaoRawTexture), ImVec2(m_UBO.screenSize.x * 0.2f, m_UBO.screenSize.y * 0.2f));

        ImGui::Text("GTAO - Blur");
        if (m_gtaoBlurTexture)
            ImGui::Image((ImTextureID)(m_gtaoBlurTexture), ImVec2(m_UBO.screenSize.x * 0.2f, m_UBO.screenSize.y * 0.2f));

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Textures"))
    {
        ImGui::Text("Color");
        ImGui::Image((ImTextureID)(m_colorTexture), ImVec2(m_UBO.screenSize.x * 0.2f, m_UBO.screenSize.y * 0.2f));
        // ImGui::Text("Depth");
        // ImGui::Image((ImTextureID)(m_depthTexture), ImVec2(m_UBO.screenSize.x * 0.2f, m_UBO.screenSize.y * 0.2f));
        ImGui::Text("Depth - Copy");
        ImGui::Image((ImTextureID)(m_depthCopyTexture), ImVec2(m_UBO.screenSize.x * 0.2f, m_UBO.screenSize.y * 0.2f));
        ImGui::TreePop();
    }
}

void PostProcess::update(glm::ivec2 screenSize)
{
    m_UBO.screenSize = screenSize;

    static Uint32 lastW = 0, lastH = 0;
    if (lastW != screenSize.x || lastH != screenSize.y)
    {
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
        depthInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        depthInfo.width = screenSize.x;
        depthInfo.height = screenSize.y;
        depthInfo.layer_count_or_depth = 1;
        depthInfo.num_levels = 1;
        depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        m_depthTexture = SDL_CreateGPUTexture(Utils::device, &depthInfo);

        SDL_GPUTextureCreateInfo depthCopyInfo{};
        depthCopyInfo.type = SDL_GPU_TEXTURETYPE_2D;
        depthCopyInfo.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        depthCopyInfo.width = screenSize.x;
        depthCopyInfo.height = screenSize.y;
        depthCopyInfo.num_levels = 1;
        depthCopyInfo.layer_count_or_depth = 1;
        depthCopyInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

        m_depthCopyTexture = SDL_CreateGPUTexture(Utils::device, &depthCopyInfo);

        if (m_gtaoRawTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_gtaoRawTexture);
        if (m_gtaoBlurTexture)
            SDL_ReleaseGPUTexture(Utils::device, m_gtaoBlurTexture);

        SDL_GPUTextureCreateInfo gtaoRawInfo{};
        gtaoRawInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM; // RG for visibility + packed depth
        gtaoRawInfo.width = screenSize.x;
        gtaoRawInfo.height = screenSize.y;
        gtaoRawInfo.num_levels = 1;
        gtaoRawInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

        m_gtaoRawTexture = SDL_CreateGPUTexture(Utils::device, &gtaoRawInfo);
        SDL_SetGPUTextureName(Utils::device, m_gtaoRawTexture, "GTAO Raw");

        SDL_GPUTextureCreateInfo gtaoBlurInfo{};
        gtaoBlurInfo.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM; // Just visibility
        gtaoBlurInfo.width = screenSize.x;
        gtaoBlurInfo.height = screenSize.y;
        gtaoBlurInfo.num_levels = 1;
        gtaoBlurInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

        m_gtaoBlurTexture = SDL_CreateGPUTexture(Utils::device, &gtaoBlurInfo);
        SDL_SetGPUTextureName(Utils::device, m_gtaoBlurTexture, "GTAO Blur");

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

void PostProcess::copyDepth(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUColorTargetInfo target{};
    target.texture = m_depthCopyTexture;
    target.clear_color = {0, 0, 0, 0};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);

    SDL_BindGPUGraphicsPipeline(pass, m_depthCopyPipeline);

    SDL_GPUTextureSamplerBinding binding{};
    binding.texture = m_depthTexture;
    binding.sampler = m_clampedSampler;

    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

    SDL_EndGPURenderPass(pass);
}

void PostProcess::computeGTAO(
    SDL_GPUCommandBuffer *commandBuffer,
    const glm::mat4 &projectionMatrix,
    const glm::mat4 &viewMatrix,
    float farPlane)
{
    glm::mat4 invProj = glm::inverse(projectionMatrix);

    m_gtaoParams.resolution = glm::vec4(
        m_UBO.screenSize.x,
        m_UBO.screenSize.y,
        1.0f / m_UBO.screenSize.x,
        1.0f / m_UBO.screenSize.y);

    m_gtaoParams.positionParams = glm::vec2(
        invProj[0][0] * 2.0f,
        invProj[1][1] * 2.0f);

    m_gtaoParams.invFarPlane = 1.0f / farPlane;

    // Calculate projection scale for screen-space radius
    float fov = 2.0f * atan(1.0f / projectionMatrix[1][1]);
    m_gtaoParams.projectionScale = m_UBO.screenSize.y / (2.0f * tan(fov * 0.5f));

    // Calculate derived values
    m_gtaoParams.invRadiusSquared = 1.0f / (m_gtaoParams.radius * m_gtaoParams.radius);
    m_gtaoParams.projectionScaleRadius = m_gtaoParams.projectionScale * m_gtaoParams.radius;

    // Always square AO result, as it looks much better - they said
    m_gtaoParams.power = m_gtaoPower * 2.0f;

    // 1. GTAO Generation Pass
    SDL_GPUColorTargetInfo genTarget{};
    genTarget.texture = m_gtaoRawTexture;
    genTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    genTarget.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *genPass = SDL_BeginGPURenderPass(commandBuffer, &genTarget, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(genPass, m_gtaoGenPipeline);

        // Bind Depth texture
        SDL_GPUTextureSamplerBinding depthBind = {m_depthCopyTexture, m_clampedSampler};
        SDL_BindGPUFragmentSamplers(genPass, 0, &depthBind, 1);

        // Push GTAO parameters
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &m_gtaoParams, sizeof(m_gtaoParams));

        SDL_DrawGPUPrimitives(genPass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(genPass);

    // 2. GTAO Blur Pass
    SDL_GPUColorTargetInfo blurTarget{};
    blurTarget.texture = m_gtaoBlurTexture;
    blurTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    blurTarget.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *blurPass = SDL_BeginGPURenderPass(commandBuffer, &blurTarget, 1, nullptr);
    {
        SDL_BindGPUGraphicsPipeline(blurPass, m_gtaoBlurPipeline);

        // Bind Raw GTAO (only need R channel for blur)
        SDL_GPUTextureSamplerBinding input = {m_gtaoRawTexture, m_clampedSampler};
        SDL_BindGPUFragmentSamplers(blurPass, 0, &input, 1);

        SDL_DrawGPUPrimitives(blurPass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(blurPass);
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

        SDL_GPUTextureSamplerBinding inputs[3] =
            {
                {m_colorTexture, Utils::baseSampler},
                {m_bloomMip[0], Utils::baseSampler},
                {m_gtaoBlurTexture, Utils::baseSampler},
            };
        SDL_BindGPUFragmentSamplers(finalPass, 0, inputs, 3);

        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &m_UBO, sizeof(m_UBO));

        SDL_DrawGPUPrimitives(finalPass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(finalPass);
}
