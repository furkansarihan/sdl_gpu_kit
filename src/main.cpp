#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_scancode.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/norm.hpp>

#include <tiny_gltf.h>

#include "stb_image.h"

#include "external/imgui/imgui_impl_sdl3.h"
#include "external/imgui/imgui_impl_sdlgpu3.h"
#include "ui/root_ui.h"
#include "ui/system_monitor/system_monitor_ui.h"
#include <imgui.h>

#include "utils/utils.h"

#include "post_process/post_process.h"
#include "shadow_manager/shadow_manager.h"

#include "camera.h"
#include "resource_manager/resource_manager.h"

struct VertexUniforms
{
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 normalMatrix;
};

struct FragmentUniforms
{
    glm::vec3 lightDir;
    float padding1;
    glm::vec3 viewPos;
    float padding2;
    glm::vec3 lightColor;
    float exposure;
};

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

struct MaterialUniforms
{
    glm::vec4 albedoFactor;   // w component is unused
    glm::vec4 emissiveFactor; // w component is emissiveStrength
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    int hasAlbedoTexture;
    int hasNormalTexture;
    int hasMetallicRoughnessTexture;
    int hasOcclusionTexture;
    int hasEmissiveTexture;
    glm::vec2 uvScale;
    float padding[2];
};

Camera camera;

SDL_Window *window;
SDL_GPUDevice *device;

SDL_GPUGraphicsPipeline *graphicsPipeline;
SDL_GPUGraphicsPipeline *brdfPipeline;
SDL_GPUGraphicsPipeline *cubemapPipeline;
SDL_GPUGraphicsPipeline *irradiancePipeline;
SDL_GPUGraphicsPipeline *prefilterPipeline;
SDL_GPUGraphicsPipeline *skyboxPipeline;

int prefilterMipLevels = 5;
int prefilterSize = 128;
int irradianceSize = 64;
int cubemapSize = 1024;

SDL_GPUSampler *hdrSampler;
SDL_GPUSampler *brdfSampler;

VertexUniforms vertexUniforms{};
FragmentUniforms fragmentUniforms{};

bool keys[SDL_SCANCODE_COUNT]{};
bool mouseButtons[6]{};
float deltaTime = 0.0f;
Uint64 lastFrame = 0;

// Global resource vectors
std::vector<ModelData *> loadedModels;
SDL_GPUSampler *cubeSampler;
SDL_GPUSampler *skySampler;

// Default resources
SDL_GPUTexture *defaultTexture = nullptr;
Material defaultMaterial("default");

Texture hdrTexture;
SDL_GPUTexture *brdfTexture = nullptr;
SDL_GPUTexture *cubemapTexture = nullptr;
SDL_GPUTexture *irradianceTexture = nullptr;
SDL_GPUTexture *prefilterTexture = nullptr;

ModelData *quadModel;
ModelData *cubeModel;

RootUI *rootUI;
SystemMonitorUI *systemMonitorUI;

ResourceManager *resourceManager;
PostProcess *postProcess;
ShadowManager *shadowManager;

// Helper to create a default 1x1 texture
SDL_GPUTexture *CreateDefaultTexture(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width = 1;
    texInfo.height = 1;
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels = 1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texInfo);

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.size = 4;
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    SDL_GPUTransferBuffer *transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

    Uint8 *data = (Uint8 *)SDL_MapGPUTransferBuffer(device, transferBuffer, false);
    data[0] = r;
    data[1] = g;
    data[2] = b;
    data[3] = a;
    SDL_UnmapGPUTransferBuffer(device, transferBuffer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo tti = {0};
    SDL_GPUTextureRegion region = {0};
    tti.transfer_buffer = transferBuffer;
    region.texture = texture;
    region.w = 1;
    region.h = 1;
    region.d = 1;
    SDL_UploadToGPUTexture(copyPass, &tti, &region, 0);

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);

    // We can destroy the transfer buffer now
    SDL_ReleaseGPUTransferBuffer(device, transferBuffer);

    return texture;
}

void UpdateCamera(float dt)
{
    float velocity = camera.speed * dt;

    if (keys[SDL_SCANCODE_LSHIFT])
        velocity *= 0.2f;
    if (keys[SDL_SCANCODE_SPACE])
        velocity *= 5.f;

    glm::vec3 direction(0.f);
    if (keys[SDL_SCANCODE_W])
        direction += camera.front;
    if (keys[SDL_SCANCODE_S])
        direction -= camera.front;
    if (keys[SDL_SCANCODE_A])
        direction -= glm::normalize(glm::cross(camera.front, camera.up));
    if (keys[SDL_SCANCODE_D])
        direction += glm::normalize(glm::cross(camera.front, camera.up));

    if (glm::length2(direction) > 0.f)
        camera.position += glm::normalize(direction) * velocity;
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

void RenderSkybox(
    SDL_GPUCommandBuffer *commandBuffer,
    SDL_GPURenderPass *renderPass,
    SDL_GPUGraphicsPipeline *skyboxPipeline,
    PrimitiveData *cubePrimitive,
    const glm::mat4 &viewMatrix,
    const glm::mat4 &projectionMatrix,
    SDL_GPUTexture *environmentCubemap,
    SDL_GPUSampler *sampler)
{
    CubemapViewUBO vertUniforms;
    vertUniforms.model = glm::scale(glm::mat4(1.0), {1.f, -1.f, 1.f});
    vertUniforms.view = viewMatrix;
    vertUniforms.projection = projectionMatrix;

    // Push uniforms
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertUniforms, sizeof(vertUniforms));
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &postProcess->m_skyUBO, sizeof(postProcess->m_skyUBO));

    // Bind pipeline
    SDL_BindGPUGraphicsPipeline(renderPass, skyboxPipeline);

    // Bind vertex buffer (your cube's vertex buffer)
    SDL_GPUBufferBinding vertexBinding;
    vertexBinding.buffer = cubePrimitive->vertexBuffer;
    vertexBinding.offset = 0;
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    // Bind index buffer (your cube's index buffer)
    SDL_GPUBufferBinding indexBinding;
    indexBinding.buffer = cubePrimitive->indexBuffer;
    indexBinding.offset = 0;
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // Bind cubemap texture
    SDL_GPUTextureSamplerBinding textureSamplerBinding;
    textureSamplerBinding.texture = environmentCubemap;
    textureSamplerBinding.sampler = sampler;
    SDL_BindGPUFragmentSamplers(renderPass, 0, &textureSamplerBinding, 1);

    // Draw the cube
    SDL_DrawGPUIndexedPrimitives(renderPass, cubePrimitive->indices.size(), 1, 0, 0, 0);
}

void RenderToShadowMaps(SDL_GPUCommandBuffer *commandBuffer)
{
    SDL_GPUColorTargetInfo colorTargetInfo{};
    colorTargetInfo.texture = shadowManager->m_shadowMapTexture;
    colorTargetInfo.clear_color = {1.f, 0.f, 0.f, 0.0f};
    colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.w = static_cast<float>(shadowManager->m_shadowMapResolution);
    viewport.h = static_cast<float>(shadowManager->m_shadowMapResolution);
    viewport.min_depth = 0.0f;
    viewport.max_depth = 1.0f;

    for (int cascadeIndex = 0; cascadeIndex < NUM_CASCADES; ++cascadeIndex)
    {
        colorTargetInfo.layer_or_depth_plane = cascadeIndex;

        SDL_GPURenderPass *shadowPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, nullptr);
        if (!shadowPass)
            continue;

        SDL_BindGPUGraphicsPipeline(shadowPass, shadowManager->m_shadowPipeline);
        SDL_SetGPUViewport(shadowPass, &viewport);

        for (const auto &model : loadedModels)
        {
            for (const auto &node : model->nodes)
            {
                if (node.meshIndex < 0 || node.meshIndex >= static_cast<int>(model->meshes.size()))
                    continue;

                const MeshData &mesh = model->meshes[node.meshIndex];

                for (const auto &prim : mesh.primitives)
                {
                    // shadowVertexUniforms.lightViewProj = shadowUniforms.lightViewProj[cascadeIndex];
                    shadowManager->m_shadowVertexUniforms.lightViewProj =
                        shadowManager->m_cascades[cascadeIndex].projection * shadowManager->m_cascades[cascadeIndex].view;
                    shadowManager->m_shadowVertexUniforms.model = node.worldTransform;

                    SDL_PushGPUVertexUniformData(
                        commandBuffer,
                        0,
                        &shadowManager->m_shadowVertexUniforms,
                        sizeof(shadowManager->m_shadowVertexUniforms));

                    SDL_GPUBufferBinding vtxBinding{prim.vertexBuffer, 0};
                    SDL_BindGPUVertexBuffers(shadowPass, 0, &vtxBinding, 1);

                    if (!prim.indices.empty())
                    {
                        SDL_GPUBufferBinding idxBinding{prim.indexBuffer, 0};
                        SDL_BindGPUIndexBuffer(shadowPass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                        SDL_DrawGPUIndexedPrimitives(shadowPass, static_cast<Uint32>(prim.indices.size()), 1, 0, 0, 0);
                    }
                    else
                    {
                        SDL_DrawGPUPrimitives(shadowPass, static_cast<Uint32>(prim.vertices.size()), 0, 0, 0);
                    }
                }
            }
        }

        SDL_EndGPURenderPass(shadowPass);
    }
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
#if defined(__APPLE__)
    // macOS and iOS use Metal
    const char *deviceName = "Metal";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
#else
    // Windows, Linux, Android, etc. use SPIR-V
    const char *deviceName = "Vulkan";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
#endif

    glm::ivec2 screenSize{1280, 720};

    // create a window
    window = SDL_CreateWindow("SDL_GPU_Kit", screenSize.x, screenSize.y, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // create the device
    device = SDL_CreateGPUDevice(shaderFormat, false, deviceName);
    if (!device)
    {
        SDL_Log("Failed to create device: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_ClaimWindowForGPUDevice(device, window);

    Utils::device = device;
    Utils::window = window;

    resourceManager = new ResourceManager(device);
    postProcess = new PostProcess();
    postProcess->update(screenSize);

    shadowManager = new ShadowManager();
    shadowManager->m_camera = &camera;

    std::string exePath = Utils::getExecutablePath();

    defaultTexture = CreateDefaultTexture(255, 255, 255, 255);

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.enable_anisotropy = true;
    samplerInfo.max_anisotropy = 16.0f;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 1000.0f;
    Utils::baseSampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!Utils::baseSampler)
    {
        SDL_Log("Failed to create baseSampler: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

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
    cubeSamplerInfo.max_lod = (float)(prefilterMipLevels - 1);
    cubeSampler = SDL_CreateGPUSampler(device, &cubeSamplerInfo);
    if (!cubeSampler)
    {
        SDL_Log("Failed to create cube sampler: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    skySampler = SDL_CreateGPUSampler(device, &cubeSamplerInfo);
    if (!skySampler)
    {
        SDL_Log("Failed to create sky sampler: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // create shaders
    SDL_GPUShader *vertexShader = Utils::loadShader("src/shaders/pbr.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fragmentShader = Utils::loadShader("src/shaders/pbr.frag", 9, 3, SDL_GPU_SHADERSTAGE_FRAGMENT);

    // create the graphics pipeline
    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    // describe the vertex buffers
    SDL_GPUVertexBufferDescription vertexBufferDesctiptions[1];
    vertexBufferDesctiptions[0].slot = 0;
    vertexBufferDesctiptions[0].pitch = sizeof(Vertex);
    vertexBufferDesctiptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDesctiptions;

    SDL_GPUVertexAttribute vertexAttributes[4]{};
    vertexAttributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};                 // pos
    vertexAttributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3}; // normal
    vertexAttributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6}; // uv
    vertexAttributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 8}; // uv
    pipelineInfo.vertex_input_state.num_vertex_attributes = 4;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    // describe the color target
    SDL_GPUColorTargetDescription colorTargetDescriptions[1];
    colorTargetDescriptions[0] = {};
    colorTargetDescriptions[0].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = colorTargetDescriptions;
    pipelineInfo.target_info.has_depth_stencil_target = true;
    pipelineInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    pipelineInfo.depth_stencil_state.enable_depth_test = true;
    pipelineInfo.depth_stencil_state.enable_depth_write = true;

    // create the pipeline
    graphicsPipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);

    // we don't need to store the shaders after creating the pipeline
    SDL_ReleaseGPUShader(device, vertexShader);
    SDL_ReleaseGPUShader(device, fragmentShader);

    // load model
    const char *modelPath = "assets/models/DamagedHelmet.glb";
    // const char *modelPath = "assets/models/ABeautifulGame.glb";
    ModelData *model = resourceManager->loadModel(std::string(exePath + "/" + modelPath).c_str());
    loadedModels.push_back(model);

    SDL_GPUSamplerCreateInfo brdfSamplerInfo{};
    brdfSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    brdfSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    brdfSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;           // No mipmaps
    brdfSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; // IMPORTANT!
    brdfSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; // IMPORTANT!
    brdfSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    brdfSamplerInfo.max_anisotropy = 1.0f;
    brdfSampler = SDL_CreateGPUSampler(device, &brdfSamplerInfo);

    // setup uniform values
    fragmentUniforms.lightDir = glm::normalize(glm::vec3(-0.3f, -0.8f, -0.3f));
    fragmentUniforms.lightColor = glm::vec3(1.0f) * 6.0f;
    fragmentUniforms.exposure = 1.0f;

    quadModel = resourceManager->loadModel(std::string(exePath + "/assets/models/quad.glb").c_str());
    // loadedModels.push_back(quadModel);
    cubeModel = resourceManager->loadModel(std::string(exePath + "/assets/models/cube.glb").c_str());
    // loadedModels.push_back(cubeModel);

    // shaders
    SDL_GPUShader *quadVert = Utils::loadShader("src/shaders/quad.vert", 0, 0, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *cubeVert = Utils::loadShader("src/shaders/cube.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *hdrToCubeFrag = Utils::loadShader("src/shaders/hdr_to_cube.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *irradianceFrag = Utils::loadShader("src/shaders/irradiance.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *prefilterFrag = Utils::loadShader("src/shaders/prefilter.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *brdfFrag = Utils::loadShader("src/shaders/brdf.frag", 0, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);

    SDL_GPUSamplerCreateInfo hdrSamplerInfo{};
    hdrSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    hdrSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    hdrSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    hdrSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    hdrSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    hdrSampler = SDL_CreateGPUSampler(device, &hdrSamplerInfo);

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
        brdfTexture = SDL_CreateGPUTexture(device, &brdfInfo);

        // cubemap
        SDL_GPUTextureCreateInfo cubemapInfo = {};
        cubemapInfo.type = SDL_GPU_TEXTURETYPE_CUBE;
        cubemapInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // Good HDR format
        cubemapInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        cubemapInfo.layer_count_or_depth = 6;
        cubemapInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        cubemapInfo.width = cubemapSize;
        cubemapInfo.height = cubemapSize;
        cubemapInfo.num_levels = 5;
        cubemapTexture = SDL_CreateGPUTexture(device, &cubemapInfo);

        // irradiance
        cubemapInfo.width = irradianceSize;
        cubemapInfo.height = irradianceSize;
        irradianceTexture = SDL_CreateGPUTexture(device, &cubemapInfo);

        // prefilter
        cubemapInfo.width = prefilterSize;
        cubemapInfo.height = prefilterSize;
        cubemapInfo.num_levels = prefilterMipLevels;
        prefilterTexture = SDL_CreateGPUTexture(device, &cubemapInfo);

        const char *hdriPath = "/assets/hdris/kloofendal_43d_clear_2k.hdr";
        // const char *hdriPath = "/assets/hdris/golden_gate_hills_8k.hdr";
        // const char *hdriPath = "/assets/hdris/studio_small_03_1k.hdr";
        // const char *hdriPath = "/assets/hdris/TCom_IcelandGolfCourse_2K_hdri_sphere.hdr";

        TextureParams params;
        params.dataType = TextureDataType::Float32;
        params.sample = true;
        hdrTexture = resourceManager->loadTextureFromFile(params, std::string(exePath + hdriPath));
    }

    // pipelines
    {
        brdfPipeline = CreatePbrPipeline(device, quadVert, brdfFrag, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT);
        cubemapPipeline = CreatePbrPipeline(device, cubeVert, hdrToCubeFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        irradiancePipeline = CreatePbrPipeline(device, cubeVert, irradianceFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        prefilterPipeline = CreatePbrPipeline(device, cubeVert, prefilterFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    }

    {
        SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUColorTargetInfo colorTargetInfo{};
        colorTargetInfo.clear_color = {0.f, 0.f, 0.f, 1.0f};
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.texture = brdfTexture;

        SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, nullptr);
        if (!renderPass)
        {
            SDL_Log("Failed to begin render pass: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }

        SDL_BindGPUGraphicsPipeline(renderPass, brdfPipeline);

        const PrimitiveData &prim = quadModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vertexBinding{prim.vertexBuffer, 0};
        SDL_GPUBufferBinding indexBinding = {prim.indexBuffer, 0};

        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_DrawGPUIndexedPrimitives(renderPass, (Uint32)prim.indices.size(), 1, 0, 0, 0);

        SDL_EndGPURenderPass(renderPass);

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
        SDL_WaitForGPUFences(device, true, &initFence, 1);
        SDL_ReleaseGPUFence(device, initFence);
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
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = cubemapTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPUViewport viewport = {0, 0, (float)cubemapSize, (float)cubemapSize, 0.0f, 1.0f};

        // Bind the input HDR texture
        SDL_GPUTextureSamplerBinding hdrBinding = {hdrTexture.id, hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = cubeModel->meshes[0].primitives[0];

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
                SDL_BindGPUGraphicsPipeline(pass, cubemapPipeline);
                SDL_SetGPUViewport(pass, &viewport);

                SDL_PushGPUVertexUniformData(cmdbuf, 0, &uniforms, sizeof(uniforms));
                SDL_BindGPUFragmentSamplers(pass, 0, &hdrBinding, 1);

                SDL_BindGPUVertexBuffers(pass, 0, &vtxBinding, 1);
                SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                SDL_DrawGPUIndexedPrimitives(pass, prim.indices.size(), 1, 0, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }

        SDL_GenerateMipmapsForGPUTexture(cmdbuf, cubemapTexture);

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
        SDL_WaitForGPUFences(device, true, &initFence, 1);
        SDL_ReleaseGPUFence(device, initFence);
    }

    // irradiance
    {
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = irradianceTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPUViewport viewport = {0, 0, (float)irradianceSize, (float)irradianceSize, 0.0f, 1.0f};

        SDL_GPUTextureSamplerBinding hdrBinding = {cubemapTexture, hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = cubeModel->meshes[0].primitives[0];

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
                SDL_BindGPUGraphicsPipeline(pass, irradiancePipeline);
                SDL_SetGPUViewport(pass, &viewport);

                SDL_PushGPUVertexUniformData(cmdbuf, 0, &uniforms, sizeof(uniforms));
                SDL_BindGPUFragmentSamplers(pass, 0, &hdrBinding, 1);

                SDL_BindGPUVertexBuffers(pass, 0, &vtxBinding, 1);
                SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                SDL_DrawGPUIndexedPrimitives(pass, prim.indices.size(), 1, 0, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }

        SDL_GenerateMipmapsForGPUTexture(cmdbuf, irradianceTexture);

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
        SDL_WaitForGPUFences(device, true, &initFence, 1);
        SDL_ReleaseGPUFence(device, initFence);
    }

    // prefilter
    {
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = prefilterTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPUTextureSamplerBinding hdrBinding = {cubemapTexture, hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = cubeModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vtxBinding = {prim.vertexBuffer, 0};
        SDL_GPUBufferBinding idxBinding = {prim.indexBuffer, 0};

        CubemapViewUBO uniforms = {};
        uniforms.projection = m_captureProjection;
        uniforms.model = glm::mat4(1.0);

        PrefilterUBO fragmentUniform;
        fragmentUniform.roughness = 0.5f;
        fragmentUniform.cubemapSize = (float)cubemapSize;

        // Render to each mip level
        for (unsigned int mip = 0; mip < prefilterMipLevels; ++mip)
        {
            unsigned int mipWidth = static_cast<unsigned int>(prefilterSize * std::pow(0.5, mip));
            unsigned int mipHeight = static_cast<unsigned int>(prefilterSize * std::pow(0.5, mip));

            SDL_GPUViewport viewport = {0, 0, (float)mipWidth, (float)mipHeight, 0.0f, 1.0f};

            fragmentUniform.roughness = (float)mip / (float)(prefilterMipLevels - 1);

            colorTargetInfo.mip_level = mip;

            for (unsigned int i = 0; i < 6; ++i)
            {
                colorTargetInfo.layer_or_depth_plane = i;
                uniforms.view = m_captureViews[i];

                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, nullptr);
                {
                    SDL_BindGPUGraphicsPipeline(pass, prefilterPipeline);
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
        SDL_WaitForGPUFences(device, true, &initFence, 1);
        SDL_ReleaseGPUFence(device, initFence);
    }

    SDL_ReleaseGPUShader(device, quadVert);
    SDL_ReleaseGPUShader(device, brdfFrag);

    skyboxPipeline = CreateSkyboxPipeline(device);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    ImGui::StyleColorsDark();

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    // Setup scaling
    ImGuiStyle &style = ImGui::GetStyle();
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    style.ScaleAllSizes(main_scale); // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
    style.FontScaleDpi = main_scale; // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;                     // Only used in multi-viewports mode.
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR; // Only used in multi-viewports mode.
    init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    ImGui_ImplSDLGPU3_Init(&init_info);

    rootUI = new RootUI();
    systemMonitorUI = new SystemMonitorUI();
    rootUI->add(systemMonitorUI);
    rootUI->add(postProcess);
    rootUI->add(shadowManager);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    Uint64 currentFrame = SDL_GetTicksNS();
    deltaTime = (currentFrame - lastFrame) / 1e9f;
    lastFrame = currentFrame;

    UpdateCamera(deltaTime);

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);

    // get the swapchain texture
    SDL_GPUTexture *swapchainTexture;
    Uint32 width, height;
    SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, &width, &height);

    // end the frame early if a swapchain texture is not available
    if (swapchainTexture == NULL)
    {
        // you must always submit the command buffer
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return SDL_APP_CONTINUE;
    }

    postProcess->update({width, height});

    // update common uniform data
    vertexUniforms.view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
    vertexUniforms.projection = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, camera.near, camera.far);
    fragmentUniforms.viewPos = camera.position;

    // shadow map
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    shadowManager->updateCascades(vertexUniforms.view, -fragmentUniforms.lightDir, aspect);
    RenderToShadowMaps(commandBuffer);

    // create the color target
    SDL_GPUColorTargetInfo colorTargetInfo{};
    colorTargetInfo.texture = postProcess->m_colorTexture;
    colorTargetInfo.clear_color = {0.f, 0.f, 0.f, 1.0f};
    colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

    // create the depth target
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    depthInfo.texture = postProcess->m_depthTexture;
    depthInfo.clear_depth = 1.0f;
    depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.store_op = SDL_GPU_STOREOP_STORE;
    depthInfo.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.stencil_store_op = SDL_GPU_STOREOP_STORE;

    // begin a render pass
    SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, &depthInfo);

    glm::vec3 center{0.f, 0.f, 0.f};
    RenderSkybox(
        commandBuffer,
        renderPass,
        skyboxPipeline,
        &cubeModel->meshes[0].primitives[0],
        vertexUniforms.view,
        vertexUniforms.projection,
        cubemapTexture, // Or prefilterMap, or irradianceMap
        skySampler);

    // bind the pipeline
    SDL_BindGPUGraphicsPipeline(renderPass, graphicsPipeline);

    // Push Scene-wide uniforms (Slot 0 for VS, Slot 0 for FS)
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniforms, sizeof(vertexUniforms));       // VS binding 0
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragmentUniforms, sizeof(fragmentUniforms)); // FS binding 0
    SDL_PushGPUFragmentUniformData(commandBuffer, 2, &shadowManager->m_shadowUniforms, sizeof(shadowManager->m_shadowUniforms));

    // --- Render Models ---
    for (const auto &model : loadedModels)
        for (const auto &node : model->nodes)
        {
            if (node.meshIndex < 0 || node.meshIndex > model->meshes.size() - 1)
                continue;

            const MeshData &mesh = model->meshes[node.meshIndex];

            for (const auto &prim : mesh.primitives)
            {
                // update per-node uniform data
                vertexUniforms.model = node.worldTransform;
                vertexUniforms.normalMatrix = glm::transpose(glm::inverse(vertexUniforms.model));
                SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniforms, sizeof(vertexUniforms)); // Update VS UBO

                // Get material (fallback to default)
                Material *material = prim.material ? prim.material : &defaultMaterial;

                // Populate material uniform struct
                MaterialUniforms materialUniforms{};
                materialUniforms.albedoFactor = material->albedo;
                materialUniforms.emissiveFactor = material->emissiveColor;
                materialUniforms.metallicFactor = material->metallic;
                materialUniforms.roughnessFactor = material->roughness;
                materialUniforms.occlusionStrength = 1.0f; // TODO: from texture
                materialUniforms.uvScale = material->uvScale;
                materialUniforms.hasAlbedoTexture = (material->albedoTexture.id != nullptr);
                materialUniforms.hasNormalTexture = (material->normalTexture.id != nullptr);
                materialUniforms.hasMetallicRoughnessTexture = (material->metallicRoughnessTexture.id != nullptr);
                materialUniforms.hasOcclusionTexture = (material->occlusionTexture.id != nullptr);
                materialUniforms.hasEmissiveTexture = (material->emissiveTexture.id != nullptr);

                // Push Material uniforms (Slot 1 for FS)
                SDL_PushGPUFragmentUniformData(commandBuffer, 1, &materialUniforms, sizeof(materialUniforms)); // FS binding 1

                // --- Bind Textures (Slots 0-4 for FS) ---
                SDL_GPUTextureSamplerBinding texBindings[9];

                // Binding 0: Albedo
                texBindings[0].texture = material->albedoTexture.id ? material->albedoTexture.id : defaultTexture;
                texBindings[0].sampler = Utils::baseSampler;

                // Binding 1: Normal
                texBindings[1].texture = material->normalTexture.id ? material->normalTexture.id : defaultTexture;
                texBindings[1].sampler = Utils::baseSampler;

                // Binding 2: Metallic-Roughness
                texBindings[2].texture = material->metallicRoughnessTexture.id ? material->metallicRoughnessTexture.id : defaultTexture; // Use white (1,1,1) -> (metallic=1, rough=1)
                texBindings[2].sampler = Utils::baseSampler;

                // Binding 3: Occlusion
                texBindings[3].texture = material->occlusionTexture.id ? material->occlusionTexture.id : defaultTexture; // Use white (1.0) -> no occlusion
                texBindings[3].sampler = Utils::baseSampler;

                // Binding 4: Emissive
                texBindings[4].texture = material->emissiveTexture.id ? material->emissiveTexture.id : defaultTexture; // Use black -> no emission
                texBindings[4].sampler = Utils::baseSampler;

                // Binding 5: Irradiance
                texBindings[5].texture = irradianceTexture;
                texBindings[5].sampler = cubeSampler;

                // Binding 6: Prefilter
                texBindings[6].texture = prefilterTexture;
                texBindings[6].sampler = cubeSampler;

                // Binding 7: Lut
                texBindings[7].texture = brdfTexture;
                texBindings[7].sampler = brdfSampler;

                // Binding 8: Cascaded shadow map (2D array)
                texBindings[8].texture = shadowManager->m_shadowMapTexture;
                texBindings[8].sampler = shadowManager->m_shadowSampler;

                SDL_BindGPUFragmentSamplers(renderPass, 0, texBindings, 9);

                // bind vertex buffer
                SDL_GPUBufferBinding bufferBinding{prim.vertexBuffer, 0};
                SDL_BindGPUVertexBuffers(renderPass, 0, &bufferBinding, 1);

                // bind the index buffer
                if (!prim.indices.empty())
                {
                    SDL_GPUBufferBinding indexBinding = {prim.indexBuffer, 0};
                    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                }

                // issue a draw call
                if (!prim.indices.empty())
                    SDL_DrawGPUIndexedPrimitives(renderPass, (Uint32)prim.indices.size(), 1, 0, 0, 0);
                else
                    SDL_DrawGPUPrimitives(renderPass, (Uint32)prim.vertices.size(), 0, 0, 0);
            }
        }

    // end the render pass
    SDL_EndGPURenderPass(renderPass);

    // contruct depth
    postProcess->copyDepth(commandBuffer);

    // ssao pass
    postProcess->computeGTAO(commandBuffer, vertexUniforms.projection, vertexUniforms.view, camera.near, camera.far);

    // bloom pass
    postProcess->downsample(commandBuffer);
    postProcess->upsample(commandBuffer);

    // post process pass
    postProcess->postProcess(commandBuffer, swapchainTexture);

    // render ui
    rootUI->render(commandBuffer, swapchainTexture);

    // submit the command buffer
    SDL_SubmitGPUCommandBuffer(commandBuffer);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return SDL_APP_SUCCESS;

    if (event->type == SDL_EVENT_KEY_DOWN)
        keys[event->key.scancode] = true;
    if (event->type == SDL_EVENT_KEY_UP)
        keys[event->key.scancode] = false;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        mouseButtons[event->button.button] = true;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP)
        mouseButtons[event->button.button] = false;

    if (event->type == SDL_EVENT_MOUSE_MOTION && mouseButtons[SDL_BUTTON_RIGHT])
    {
        float xoffset = event->motion.xrel * camera.sensitivity;
        float yoffset = -event->motion.yrel * camera.sensitivity;

        camera.yaw += xoffset;
        camera.pitch += yoffset;

        if (camera.pitch > 89.0f)
            camera.pitch = 89.0f;
        if (camera.pitch < -89.0f)
            camera.pitch = -89.0f;

        glm::vec3 direction;
        direction.x = cos(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        direction.y = sin(glm::radians(camera.pitch));
        direction.z = sin(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        camera.front = glm::normalize(direction);
    }

    if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE)
    {
        bool relative = SDL_GetWindowRelativeMouseMode(window);
        SDL_SetWindowRelativeMouseMode(window, !relative);
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    delete rootUI;
    delete systemMonitorUI;

    delete postProcess;
    delete shadowManager;

    for (const auto &model : loadedModels)
        resourceManager->dispose(model);

    resourceManager->dispose(cubeModel);
    resourceManager->dispose(quadModel);
    resourceManager->dispose(hdrTexture);

    SDL_ReleaseGPUSampler(device, Utils::baseSampler);
    SDL_ReleaseGPUSampler(device, cubeSampler);

    if (graphicsPipeline)
        SDL_ReleaseGPUGraphicsPipeline(device, graphicsPipeline);
    if (device)
        SDL_DestroyGPUDevice(device);
    if (window)
        SDL_DestroyWindow(window);
}
