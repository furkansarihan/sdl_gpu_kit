#include "pbr_manager.h"

#include <SDL3/SDL_gpu.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "../utils/utils.h"

PbrManager::PbrManager(ResourceManager *resourceManager)
    : m_resourceManager(resourceManager)
{
    init();
}

PbrManager::~PbrManager()
{
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_graphicsPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_brdfPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_cubemapPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_irradiancePipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_prefilterPipeline);
    SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_skyboxPipeline);

    SDL_ReleaseGPUTexture(Utils::device, m_brdfTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_cubemapTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_irradianceTexture);
    SDL_ReleaseGPUTexture(Utils::device, m_prefilterTexture);

    SDL_ReleaseGPUSampler(Utils::device, m_hdrSampler);
    SDL_ReleaseGPUSampler(Utils::device, m_brdfSampler);
    SDL_ReleaseGPUSampler(Utils::device, m_cubeSampler);

    m_resourceManager->dispose(m_cubeModel);
    m_resourceManager->dispose(m_quadModel);
}

SDL_GPUGraphicsPipeline *CreatePbrPipeline(
    SDL_GPUDevice *device,
    SDL_GPUShader *vertexShader,
    SDL_GPUShader *fragmentShader,
    SDL_GPUTextureFormat targetFormat,
    SDL_GPUSampleCount sampleCount = SDL_GPU_SAMPLECOUNT_1)
{
    SDL_GPUVertexAttribute vertexAttributes[3]{};
    vertexAttributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};                 // pos
    vertexAttributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3}; // normal
    vertexAttributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6}; // uv

    // --- Rasterizer State ---
    SDL_GPURasterizerState rasterizerState = {};
    rasterizerState.cull_mode = SDL_GPU_CULLMODE_NONE;
    rasterizerState.fill_mode = SDL_GPU_FILLMODE_FILL;
    rasterizerState.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    // --- No Depth/Stencil ---
    SDL_GPUDepthStencilState depthStencilState = {};
    depthStencilState.enable_depth_test = false;
    depthStencilState.enable_depth_write = false;

    // --- Color Target ---
    SDL_GPUColorTargetDescription colorTargetDesc = {};
    colorTargetDesc.format = targetFormat;
    colorTargetDesc.blend_state.enable_blend = false;

    SDL_GPUGraphicsPipelineTargetInfo targetInfo = {};
    targetInfo.color_target_descriptions = &colorTargetDesc;
    targetInfo.num_color_targets = 1;
    targetInfo.has_depth_stencil_target = false;

    SDL_GPUVertexBufferDescription vertexBufferDesctiptions[1];
    vertexBufferDesctiptions[0].slot = 0;
    vertexBufferDesctiptions[0].pitch = sizeof(Vertex);
    vertexBufferDesctiptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    // --- Pipeline Create Info ---
    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDesctiptions;
    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 3;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipelineInfo.rasterizer_state = rasterizerState;
    pipelineInfo.multisample_state.sample_count = sampleCount;
    pipelineInfo.depth_stencil_state = depthStencilState;
    pipelineInfo.target_info = targetInfo;

    return SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
}

SDL_GPUGraphicsPipeline *CreateSkyboxPipeline(SDL_GPUDevice *device)
{
    // Load shaders
    SDL_GPUShader *skyboxVertShader = Utils::loadShader("src/shaders/cube.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *skyboxFragShader = Utils::loadShader("src/shaders/skybox.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);

    // Vertex input - reuse your existing Vertex structure
    SDL_GPUVertexAttribute vertexAttributes[4] = {};

    // Position (location = 0)
    vertexAttributes[0].location = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[0].offset = offsetof(Vertex, position);
    vertexAttributes[0].buffer_slot = 0;

    // Normal (location = 1) - not used but must be defined
    vertexAttributes[1].location = 1;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[1].offset = offsetof(Vertex, normal);
    vertexAttributes[1].buffer_slot = 0;

    // UV (location = 2) - not used but must be defined
    vertexAttributes[2].location = 2;
    vertexAttributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[2].offset = offsetof(Vertex, uv);
    vertexAttributes[2].buffer_slot = 0;

    // Tangent (location = 3) - not used but must be defined
    vertexAttributes[3].location = 3;
    vertexAttributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    vertexAttributes[3].offset = offsetof(Vertex, tangent);
    vertexAttributes[3].buffer_slot = 0;

    SDL_GPUVertexBufferDescription vertexBufferDesc = {};
    vertexBufferDesc.slot = 0;
    vertexBufferDesc.pitch = sizeof(Vertex);
    vertexBufferDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexInputState vertexInputState = {};
    vertexInputState.vertex_buffer_descriptions = &vertexBufferDesc;
    vertexInputState.num_vertex_buffers = 1;
    vertexInputState.vertex_attributes = vertexAttributes;
    vertexInputState.num_vertex_attributes = 4;

    // Rasterizer state - disable culling or use front face culling
    SDL_GPURasterizerState rasterizerState = {};
    rasterizerState.fill_mode = SDL_GPU_FILLMODE_FILL;
    rasterizerState.cull_mode = SDL_GPU_CULLMODE_NONE;
    rasterizerState.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    // Depth stencil state
    SDL_GPUDepthStencilState depthStencilState = {};
    depthStencilState.enable_depth_test = true;
    depthStencilState.enable_depth_write = false; // Don't write to depth
    depthStencilState.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;

    // Multisample state
    SDL_GPUMultisampleState multisampleState = {};
    multisampleState.sample_count = SDL_GPU_SAMPLECOUNT_1;

    // Color target
    SDL_GPUColorTargetDescription colorTarget = {};
    colorTarget.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    SDL_GPUGraphicsPipelineTargetInfo targetInfo = {};
    targetInfo.color_target_descriptions = &colorTarget;
    targetInfo.num_color_targets = 1;
    targetInfo.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; // Or your depth format
    targetInfo.has_depth_stencil_target = true;

    // Create pipeline
    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.vertex_shader = skyboxVertShader;
    pipelineInfo.fragment_shader = skyboxFragShader;
    pipelineInfo.vertex_input_state = vertexInputState;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipelineInfo.rasterizer_state = rasterizerState;
    pipelineInfo.depth_stencil_state = depthStencilState;
    pipelineInfo.multisample_state = multisampleState;
    pipelineInfo.target_info = targetInfo;

    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);

    // Release shaders
    SDL_ReleaseGPUShader(device, skyboxVertShader);
    SDL_ReleaseGPUShader(device, skyboxFragShader);

    return pipeline;
}

void PbrManager::init()
{
    // shaders
    SDL_GPUShader *quadVert = Utils::loadShader("src/shaders/quad.vert", 0, 0, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *cubeVert = Utils::loadShader("src/shaders/cube.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *hdrToCubeFrag = Utils::loadShader("src/shaders/hdr_to_cube.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *irradianceFrag = Utils::loadShader("src/shaders/irradiance.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *prefilterFrag = Utils::loadShader("src/shaders/prefilter.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *brdfFrag = Utils::loadShader("src/shaders/brdf.frag", 0, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);

    // pipelines
    {
        m_brdfPipeline = CreatePbrPipeline(Utils::device, quadVert, brdfFrag, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT);
        m_cubemapPipeline = CreatePbrPipeline(Utils::device, cubeVert, hdrToCubeFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        m_irradiancePipeline = CreatePbrPipeline(Utils::device, cubeVert, irradianceFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        m_prefilterPipeline = CreatePbrPipeline(Utils::device, cubeVert, prefilterFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    }

    // textures
    {
        // brdf
        SDL_GPUTextureCreateInfo brdfInfo{};
        brdfInfo.type = SDL_GPU_TEXTURETYPE_2D;
        brdfInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
        brdfInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        brdfInfo.width = 512;
        brdfInfo.height = 512;
        brdfInfo.layer_count_or_depth = 1;
        brdfInfo.num_levels = 1;
        brdfInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        m_brdfTexture = SDL_CreateGPUTexture(Utils::device, &brdfInfo);

        // cubemap
        SDL_GPUTextureCreateInfo cubemapInfo = {};
        cubemapInfo.type = SDL_GPU_TEXTURETYPE_CUBE;
        cubemapInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // Good HDR format
        cubemapInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        cubemapInfo.layer_count_or_depth = 6;
        cubemapInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        cubemapInfo.width = m_cubemapSize;
        cubemapInfo.height = m_cubemapSize;
        cubemapInfo.num_levels = 5;
        m_cubemapTexture = SDL_CreateGPUTexture(Utils::device, &cubemapInfo);

        // irradiance
        cubemapInfo.width = m_irradianceSize;
        cubemapInfo.height = m_irradianceSize;
        m_irradianceTexture = SDL_CreateGPUTexture(Utils::device, &cubemapInfo);

        // prefilter
        cubemapInfo.width = m_prefilterSize;
        cubemapInfo.height = m_prefilterSize;
        cubemapInfo.num_levels = m_prefilterMipLevels;
        m_prefilterTexture = SDL_CreateGPUTexture(Utils::device, &cubemapInfo);
    }

    // samplers
    {
        SDL_GPUSamplerCreateInfo hdrSamplerInfo{};
        hdrSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        hdrSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        hdrSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        hdrSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        hdrSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_hdrSampler = SDL_CreateGPUSampler(Utils::device, &hdrSamplerInfo);

        SDL_GPUSamplerCreateInfo brdfSamplerInfo{};
        brdfSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        brdfSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        brdfSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;           // No mipmaps
        brdfSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; // IMPORTANT!
        brdfSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; // IMPORTANT!
        brdfSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        brdfSamplerInfo.max_anisotropy = 1.0f;
        m_brdfSampler = SDL_CreateGPUSampler(Utils::device, &brdfSamplerInfo);

        SDL_GPUSamplerCreateInfo cubeSamplerInfo{};
        cubeSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        cubeSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        cubeSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        cubeSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        cubeSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        cubeSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        cubeSamplerInfo.enable_anisotropy = true;
        cubeSamplerInfo.max_anisotropy = 16.0f;
        cubeSamplerInfo.min_lod = 0.0f;
        cubeSamplerInfo.max_lod = (float)(m_prefilterMipLevels - 1);
        m_cubeSampler = SDL_CreateGPUSampler(Utils::device, &cubeSamplerInfo);
    }

    // models
    {
        // TODO:
        std::string exePath = Utils::getExecutablePath();

        m_quadModel = m_resourceManager->loadModel(std::string(exePath + "/assets/models/quad.glb").c_str());
        m_cubeModel = m_resourceManager->loadModel(std::string(exePath + "/assets/models/cube.glb").c_str());
    }

    SDL_ReleaseGPUShader(Utils::device, quadVert);
    SDL_ReleaseGPUShader(Utils::device, cubeVert);
    SDL_ReleaseGPUShader(Utils::device, hdrToCubeFrag);
    SDL_ReleaseGPUShader(Utils::device, irradianceFrag);
    SDL_ReleaseGPUShader(Utils::device, prefilterFrag);
    SDL_ReleaseGPUShader(Utils::device, brdfFrag);
}

void PbrManager::updateEnvironmentTexture(Texture environmentTexture)
{
    m_environmentTexture = environmentTexture;

    {
        SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(Utils::device);

        SDL_GPUColorTargetInfo colorTargetInfo{};
        colorTargetInfo.clear_color = {0.f, 0.f, 0.f, 1.0f};
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.texture = m_brdfTexture;

        SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, nullptr);
        if (!renderPass)
        {
            SDL_Log("Failed to begin render pass: %s", SDL_GetError());
            return;
        }

        SDL_BindGPUGraphicsPipeline(renderPass, m_brdfPipeline);

        const PrimitiveData &prim = m_quadModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vertexBinding{prim.vertexBuffer, 0};
        SDL_GPUBufferBinding indexBinding = {prim.indexBuffer, 0};

        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_DrawGPUIndexedPrimitives(renderPass, (Uint32)prim.indices.size(), 1, 0, 0, 0);

        SDL_EndGPURenderPass(renderPass);

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
        SDL_WaitForGPUFences(Utils::device, true, &initFence, 1);
        SDL_ReleaseGPUFence(Utils::device, initFence);
    }

    glm::mat4 m_captureViews[6] = {
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
    };
    glm::mat4 m_captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

    // cubemap
    {
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(Utils::device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = m_cubemapTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPUViewport viewport = {0, 0, (float)m_cubemapSize, (float)m_cubemapSize, 0.0f, 1.0f};

        // Bind the input HDR texture
        SDL_GPUTextureSamplerBinding hdrBinding = {environmentTexture.id, m_hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = m_cubeModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vtxBinding = {prim.vertexBuffer, 0};
        SDL_GPUBufferBinding idxBinding = {prim.indexBuffer, 0};

        CubemapViewUBO uniforms = {};
        uniforms.projection = m_captureProjection;
        uniforms.model = glm::scale(glm::mat4(1.0), glm::vec3(1.f, -1.f, 1.f));

        for (unsigned int i = 0; i < 6; ++i)
        {
            colorTargetInfo.layer_or_depth_plane = i;
            uniforms.view = m_captureViews[i];

            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, nullptr);
            {
                SDL_BindGPUGraphicsPipeline(pass, m_cubemapPipeline);
                SDL_SetGPUViewport(pass, &viewport);

                SDL_PushGPUVertexUniformData(cmdbuf, 0, &uniforms, sizeof(uniforms));
                SDL_BindGPUFragmentSamplers(pass, 0, &hdrBinding, 1);

                SDL_BindGPUVertexBuffers(pass, 0, &vtxBinding, 1);
                SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                SDL_DrawGPUIndexedPrimitives(pass, prim.indices.size(), 1, 0, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }

        SDL_GenerateMipmapsForGPUTexture(cmdbuf, m_cubemapTexture);

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
        SDL_WaitForGPUFences(Utils::device, true, &initFence, 1);
        SDL_ReleaseGPUFence(Utils::device, initFence);
    }

    // irradiance
    {
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(Utils::device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = m_irradianceTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPUViewport viewport = {0, 0, (float)m_irradianceSize, (float)m_irradianceSize, 0.0f, 1.0f};

        SDL_GPUTextureSamplerBinding hdrBinding = {m_cubemapTexture, m_hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = m_cubeModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vtxBinding = {prim.vertexBuffer, 0};
        SDL_GPUBufferBinding idxBinding = {prim.indexBuffer, 0};

        CubemapViewUBO uniforms = {};
        uniforms.projection = m_captureProjection;
        uniforms.model = glm::mat4(1.0);

        for (unsigned int i = 0; i < 6; ++i)
        {
            colorTargetInfo.layer_or_depth_plane = i;
            uniforms.view = m_captureViews[i];

            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, nullptr);
            {
                SDL_BindGPUGraphicsPipeline(pass, m_irradiancePipeline);
                SDL_SetGPUViewport(pass, &viewport);

                SDL_PushGPUVertexUniformData(cmdbuf, 0, &uniforms, sizeof(uniforms));
                SDL_BindGPUFragmentSamplers(pass, 0, &hdrBinding, 1);

                SDL_BindGPUVertexBuffers(pass, 0, &vtxBinding, 1);
                SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                SDL_DrawGPUIndexedPrimitives(pass, prim.indices.size(), 1, 0, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }

        SDL_GenerateMipmapsForGPUTexture(cmdbuf, m_irradianceTexture);

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
        SDL_WaitForGPUFences(Utils::device, true, &initFence, 1);
        SDL_ReleaseGPUFence(Utils::device, initFence);
    }

    // prefilter
    {
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(Utils::device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = m_prefilterTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPUTextureSamplerBinding hdrBinding = {m_cubemapTexture, m_hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = m_cubeModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vtxBinding = {prim.vertexBuffer, 0};
        SDL_GPUBufferBinding idxBinding = {prim.indexBuffer, 0};

        CubemapViewUBO uniforms = {};
        uniforms.projection = m_captureProjection;
        uniforms.model = glm::mat4(1.0);

        PrefilterUBO fragmentUniform;
        fragmentUniform.roughness = 0.5f;
        fragmentUniform.cubemapSize = (float)m_cubemapSize;

        // Render to each mip level
        for (unsigned int mip = 0; mip < m_prefilterMipLevels; ++mip)
        {
            unsigned int mipWidth = static_cast<unsigned int>(m_prefilterSize * std::pow(0.5, mip));
            unsigned int mipHeight = static_cast<unsigned int>(m_prefilterSize * std::pow(0.5, mip));

            SDL_GPUViewport viewport = {0, 0, (float)mipWidth, (float)mipHeight, 0.0f, 1.0f};

            fragmentUniform.roughness = (float)mip / (float)(m_prefilterMipLevels - 1);

            colorTargetInfo.mip_level = mip;

            for (unsigned int i = 0; i < 6; ++i)
            {
                colorTargetInfo.layer_or_depth_plane = i;
                uniforms.view = m_captureViews[i];

                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, nullptr);
                {
                    SDL_BindGPUGraphicsPipeline(pass, m_prefilterPipeline);
                    SDL_SetGPUViewport(pass, &viewport);

                    // Push uniforms to both vertex and fragment stages
                    SDL_PushGPUVertexUniformData(cmdbuf, 0, &uniforms, sizeof(uniforms));
                    SDL_PushGPUFragmentUniformData(cmdbuf, 0, &fragmentUniform, sizeof(fragmentUniform));

                    SDL_BindGPUFragmentSamplers(pass, 0, &hdrBinding, 1);

                    SDL_BindGPUVertexBuffers(pass, 0, &vtxBinding, 1);
                    SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                    SDL_DrawGPUIndexedPrimitives(pass, prim.indices.size(), 1, 0, 0, 0);
                }
                SDL_EndGPURenderPass(pass);
            }
        }

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
        SDL_WaitForGPUFences(Utils::device, true, &initFence, 1);
        SDL_ReleaseGPUFence(Utils::device, initFence);
    }

    m_skyboxPipeline = CreateSkyboxPipeline(Utils::device);
}

void PbrManager::renderSkybox(
    SDL_GPUCommandBuffer *commandBuffer,
    SDL_GPURenderPass *renderPass,
    const glm::mat4 &viewMatrix,
    const glm::mat4 &projectionMatrix)
{
    PrimitiveData &cubePrimitive = m_cubeModel->meshes[0].primitives[0];

    CubemapViewUBO vertUniforms;
    vertUniforms.model = glm::scale(glm::mat4(1.0), {1.f, -1.f, 1.f});
    vertUniforms.view = viewMatrix;
    vertUniforms.projection = projectionMatrix;

    // Push uniforms
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertUniforms, sizeof(vertUniforms));
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &m_skyUBO, sizeof(m_skyUBO));

    // Bind pipeline
    SDL_BindGPUGraphicsPipeline(renderPass, m_skyboxPipeline);

    // Bind vertex buffer
    SDL_GPUBufferBinding vertexBinding{cubePrimitive.vertexBuffer, 0};
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    // Bind index buffer
    SDL_GPUBufferBinding indexBinding{cubePrimitive.indexBuffer, 0};
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // Bind cubemap texture
    SDL_GPUTextureSamplerBinding textureSamplerBinding{m_cubemapTexture, m_cubeSampler};
    SDL_BindGPUFragmentSamplers(renderPass, 0, &textureSamplerBinding, 1);

    // Draw the cube
    SDL_DrawGPUIndexedPrimitives(renderPass, cubePrimitive.indices.size(), 1, 0, 0, 0);
}
