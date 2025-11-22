#include "render_manager.h"
#include "../utils/utils.h"
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

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
}

void RenderManager::renderUI()
{
    if (!ImGui::CollapsingHeader("Render Manager", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (ImGui::TreeNode("Light"))
    {
        if (ImGui::DragFloat3("Light Direction", &m_fragmentUniforms.lightDir.x, 0.01f, 0.f))
            m_fragmentUniforms.lightDir = glm::normalize(m_fragmentUniforms.lightDir);
        ImGui::DragFloat3("Light Color", &m_fragmentUniforms.lightColor.x, 0.01f, 0.f);

        ImGui::TreePop();
    }
}

void RenderManager::update(SDL_GPUSampleCount sampleCount)
{
    if (sampleCount != m_sampleCount)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pbrPipeline);
        createPipeline(sampleCount);
    }
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

void RenderManager::createPipeline(SDL_GPUSampleCount sampleCount)
{
    m_sampleCount = sampleCount;

    SDL_GPUShader *vertexShader = Utils::loadShader("src/shaders/pbr.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fragmentShader = Utils::loadShader("src/shaders/pbr.frag", 9, 3, SDL_GPU_SHADERSTAGE_FRAGMENT);

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

    SDL_GPUColorTargetDescription colorTargetDesc[1];
    colorTargetDesc[0] = {};
    colorTargetDesc[0].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = colorTargetDesc;
    pipelineInfo.target_info.has_depth_stencil_target = true;
    pipelineInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    pipelineInfo.multisample_state.sample_count = sampleCount;
    pipelineInfo.multisample_state.enable_mask = false;

    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    pipelineInfo.depth_stencil_state.enable_depth_test = true;
    pipelineInfo.depth_stencil_state.enable_depth_write = true;

    m_pbrPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);

    SDL_ReleaseGPUShader(m_device, vertexShader);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
}

void RenderManager::renderShadows(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const glm::mat4 &viewProj)
{
    // Shadows don't need the complex PBR pipeline, they use the shadow pipeline managed elsewhere (ShadowManager),
    // but the objects themselves need to issue draw calls.
    // Ideally, ShadowManager sets the pipeline, then we iterate renderables.

    Frustum frustum = Frustum::fromMatrix(viewProj);

    for (Renderable *r : m_renderables)
    {
        r->drawShadow(cmd, pass, viewProj, frustum);
    }
}

void RenderManager::renderScene(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass,
                                const glm::mat4 &view, const glm::mat4 &projection,
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
    {
        r->draw(cmd, pass, view, projection, camPos, frustum);
    }
}
