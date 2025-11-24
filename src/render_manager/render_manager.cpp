#include "render_manager.h"

#include <SDL3/SDL_gpu.h>

#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

#include "../utils/utils.h"

RenderManager::RenderManager(
    SDL_GPUDevice *device,
    SDL_Window *window,
    ResourceManager *resourceManager,
    SDL_GPUSampleCount sampleCount)
    : m_device(device),
      m_window(window),
      m_resourceManager(resourceManager),
      m_sampleCount(sampleCount),
      m_pbrPipeline(nullptr),
      m_baseSampler(nullptr),
      m_defaultTexture(nullptr)
{
    m_pbrManager = new PbrManager(m_resourceManager);
    m_shadowManager = new ShadowManager();

    createDefaultResources();
    createPipeline(SDL_GPU_SAMPLECOUNT_4);

    m_fragmentUniforms.lightDir = glm::normalize(glm::vec3(-0.3f, -0.8f, -0.3f));
    m_fragmentUniforms.lightColor = glm::vec3(1.0f) * 6.0f;
}

RenderManager::~RenderManager()
{
    delete m_pbrManager;
    delete m_shadowManager;

    if (m_pbrPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pbrPipeline);
    if (m_baseSampler)
        SDL_ReleaseGPUSampler(m_device, m_baseSampler);
    if (m_defaultTexture)
        SDL_ReleaseGPUTexture(m_device, m_defaultTexture);

    if (m_oitPipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_oitPipeline);
    if (m_compositePipeline)
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_compositePipeline);

    if (m_accumTexture)
        SDL_ReleaseGPUTexture(m_device, m_accumTexture);
    if (m_revealTexture)
        SDL_ReleaseGPUTexture(m_device, m_revealTexture);
}

void RenderManager::renderUI()
{
    if (!ImGui::CollapsingHeader("Render Manager", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::PushID(this);

    if (ImGui::TreeNode("Light"))
    {
        if (ImGui::DragFloat3("Light Direction", &m_fragmentUniforms.lightDir.x, 0.01f, 0.f))
            m_fragmentUniforms.lightDir = glm::normalize(m_fragmentUniforms.lightDir);
        ImGui::DragFloat3("Light Color", &m_fragmentUniforms.lightColor.x, 0.01f, 0.f);

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Transparency Textures"))
    {
        ImGui::Text("Accumulate");
        ImGui::Image((ImTextureID)(m_accumTexture), ImVec2(m_screenSize.x * 0.4f, m_screenSize.y * 0.4f));
        ImGui::Text("Reveal");
        ImGui::Image((ImTextureID)(m_revealTexture), ImVec2(m_screenSize.x * 0.4f, m_screenSize.y * 0.4f));

        ImGui::TreePop();
    }

    ImGui::PopID();
}

void RenderManager::update(glm::ivec2 screenSize, SDL_GPUSampleCount sampleCount)
{
    if (sampleCount != m_sampleCount)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pbrPipeline);
        createPipeline(sampleCount);
    }

    updateOitTextures(screenSize);
}

void RenderManager::addRenderable(Renderable *renderable)
{
    m_renderables.push_back(renderable);
}

void RenderManager::createDefaultResources()
{
    // 1. Create Sampler
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
    Utils::baseSampler = SDL_CreateGPUSampler(m_device, &samplerInfo); // Keep Utils updated for now
    m_baseSampler = Utils::baseSampler;

    // 2. Create Default White Texture
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width = 1;
    texInfo.height = 1;
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels = 1;
    m_defaultTexture = SDL_CreateGPUTexture(m_device, &texInfo);

    // Upload white pixel
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.size = 4;
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    SDL_GPUTransferBuffer *transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    Uint8 *data = (Uint8 *)SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
    data[0] = 255;
    data[1] = 255;
    data[2] = 255;
    data[3] = 255;
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo tti = {0};
    tti.transfer_buffer = transferBuffer;
    SDL_GPUTextureRegion region = {0};
    region.texture = m_defaultTexture;
    region.w = 1;
    region.h = 1;
    region.d = 1;
    SDL_UploadToGPUTexture(copyPass, &tti, &region, 0);
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
}

void RenderManager::updateOitTextures(glm::ivec2 screenSize)
{
    if (m_screenSize == screenSize)
        return;

    m_screenSize = screenSize;

    if (m_accumTexture)
        SDL_ReleaseGPUTexture(m_device, m_accumTexture);

    if (m_revealTexture)
        SDL_ReleaseGPUTexture(m_device, m_revealTexture);

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = m_screenSize.x;
    info.height = m_screenSize.y;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    // Accumulation
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    m_accumTexture = SDL_CreateGPUTexture(m_device, &info);

    // Revealage
    info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    m_revealTexture = SDL_CreateGPUTexture(m_device, &info);
}

void RenderManager::createPipeline(SDL_GPUSampleCount sampleCount)
{
    m_sampleCount = sampleCount;

    SDL_GPUShader *vertexShader = Utils::loadShader("src/shaders/pbr.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fragmentShader = Utils::loadShader("src/shaders/pbr.frag", 10, 3, SDL_GPU_SHADERSTAGE_FRAGMENT);

    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUVertexBufferDescription vertexBufferDesc[1];
    vertexBufferDesc[0].slot = 0;
    vertexBufferDesc[0].pitch = sizeof(Vertex);
    vertexBufferDesc[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDesc;

    SDL_GPUVertexAttribute vertexAttributes[4]{};
    vertexAttributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};
    vertexAttributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3};
    vertexAttributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6};
    vertexAttributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 8};
    pipelineInfo.vertex_input_state.num_vertex_attributes = 4;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    pipelineInfo.multisample_state.sample_count = sampleCount;
    pipelineInfo.target_info.has_depth_stencil_target = true;
    pipelineInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    pipelineInfo.target_info.num_color_targets = 1;

    // --- 1. Opaque Pipeline ---
    SDL_GPUColorTargetDescription opaqueTargetDesc{};
    opaqueTargetDesc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    opaqueTargetDesc.blend_state.enable_blend = false; // No blending for opaque

    pipelineInfo.target_info.color_target_descriptions = &opaqueTargetDesc;

    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    pipelineInfo.depth_stencil_state.enable_depth_test = true;
    pipelineInfo.depth_stencil_state.enable_depth_write = true; // Write Depth

    m_pbrPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    if (m_pbrPipeline == nullptr)
    {
        SDL_Log("Failed to create m_pbrPipeline: %s", SDL_GetError());
        return;
    }

    SDL_ReleaseGPUShader(m_device, fragmentShader);

    // --- 2. OIT Geometry Pipeline ---
    SDL_GPUShader *oitShader = Utils::loadShader("src/shaders/pbr_oit.frag", 10, 3, SDL_GPU_SHADERSTAGE_FRAGMENT);

    pipelineInfo = SDL_GPUGraphicsPipelineCreateInfo{};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = oitShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDesc;

    pipelineInfo.vertex_input_state.num_vertex_attributes = 4;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    SDL_GPUColorTargetDescription targetDescs[2];

    // Target 0: Accumulation (Float16 or Float32)
    targetDescs[0].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    targetDescs[0].blend_state.enable_blend = true;
    targetDescs[0].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targetDescs[0].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targetDescs[0].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    targetDescs[0].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targetDescs[0].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targetDescs[0].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    // Target 1: Revealage (R8 or R16 Float)
    targetDescs[1].format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    targetDescs[1].blend_state.enable_blend = true;
    targetDescs[1].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    targetDescs[1].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    targetDescs[1].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    targetDescs[1].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    targetDescs[1].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    targetDescs[1].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    pipelineInfo.target_info.num_color_targets = 2;
    pipelineInfo.target_info.color_target_descriptions = targetDescs;

    pipelineInfo.target_info.has_depth_stencil_target = true;
    pipelineInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    // Depth: Read-Only!
    pipelineInfo.depth_stencil_state.enable_depth_write = false;
    pipelineInfo.depth_stencil_state.enable_depth_test = true;
    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

    m_oitPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    if (m_oitPipeline == nullptr)
    {
        SDL_Log("Failed to create m_oitPipeline: %s", SDL_GetError());
        return;
    }

    SDL_ReleaseGPUShader(m_device, vertexShader);
    SDL_ReleaseGPUShader(m_device, oitShader);

    // --- 3. OIT Composite Pipeline ---
    SDL_GPUShader *fullscreenVertexShader = Utils::loadShader("src/shaders/fullscreen.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *oitCompositeShader = Utils::loadShader("src/shaders/oit_composite.frag", 2, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);

    pipelineInfo = SDL_GPUGraphicsPipelineCreateInfo{};
    pipelineInfo.vertex_shader = fullscreenVertexShader;
    pipelineInfo.fragment_shader = oitCompositeShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUColorTargetDescription compDesc{};
    compDesc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // Main color format
    compDesc.blend_state.enable_blend = true;
    compDesc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    compDesc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    compDesc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    compDesc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    compDesc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = &compDesc;
    pipelineInfo.depth_stencil_state.enable_depth_test = false; // No depth test for fullscreen
    pipelineInfo.depth_stencil_state.enable_depth_write = false;

    // ?
    pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;

    m_compositePipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    if (m_compositePipeline == nullptr)
    {
        SDL_Log("Failed to create m_compositePipeline: %s", SDL_GetError());
        return;
    }

    SDL_ReleaseGPUShader(m_device, fullscreenVertexShader);
    SDL_ReleaseGPUShader(m_device, oitCompositeShader);
}

void RenderManager::renderShadow(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const glm::mat4 &viewProj)
{
    Frustum frustum = Frustum::fromMatrix(viewProj);

    for (Renderable *r : m_renderables)
    {
        r->renderShadow(cmd, pass, viewProj, frustum);
    }
}

void RenderManager::renderOpaque(
    SDL_GPUCommandBuffer *cmd,
    SDL_GPURenderPass *pass,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const glm::vec3 &camPos)
{
    // 1. Bind Pipeline
    SDL_BindGPUGraphicsPipeline(pass, m_pbrPipeline);

    // 2. Push Global Uniforms
    VertexUniforms vUniforms{};
    vUniforms.view = view;
    vUniforms.projection = projection;

    // VS Binding 0: View/Proj
    SDL_PushGPUVertexUniformData(cmd, 0, &vUniforms, sizeof(vUniforms));
    // FS Binding 0: Light info, ViewPos
    SDL_PushGPUFragmentUniformData(cmd, 0, &m_fragmentUniforms, sizeof(FragmentUniforms));
    // FS Binding 2: Shadow uniforms
    SDL_PushGPUFragmentUniformData(cmd, 2, &m_shadowManager->m_shadowUniforms, sizeof(ShadowUniforms));

    // 3. Render Objects
    Frustum frustum = Frustum::fromMatrix(projection * view);

    for (Renderable *r : m_renderables)
        r->renderOpaque(cmd, pass, view, projection, frustum);
}

void RenderManager::renderTransparent(
    SDL_GPUCommandBuffer *cmd,
    SDL_GPURenderPass *pass,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const glm::vec3 &camPos)
{
    Frustum frustum = Frustum::fromMatrix(projection * view);

    // --- PASS 3: TRANSPARENT ---
    SDL_BindGPUGraphicsPipeline(pass, m_oitPipeline);

    // TODO: ?
    SDL_PushGPUFragmentUniformData(cmd, 0, &m_fragmentUniforms, sizeof(FragmentUniforms));
    SDL_PushGPUFragmentUniformData(cmd, 2, &m_shadowManager->m_shadowUniforms, sizeof(ShadowUniforms));

    for (Renderable *r : m_renderables)
        r->renderTransparent(cmd, pass, view, projection, frustum);
}

void RenderManager::renderComposite(
    SDL_GPUCommandBuffer *cmd,
    SDL_GPURenderPass *pass)
{
    SDL_BindGPUGraphicsPipeline(pass, m_compositePipeline);

    // TODO: clamped sampler?
    SDL_GPUTextureSamplerBinding bindings[2];
    bindings[0] = {m_accumTexture, m_baseSampler};  // Accum
    bindings[1] = {m_revealTexture, m_baseSampler}; // Reveal

    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0); // Draw fullscreen triangle
}
