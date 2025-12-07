#include "default_runner.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_scancode.h>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/norm.hpp>

// Include headers for implementation details
#include "external/imgui/imgui_impl_sdl3.h"
#include "external/imgui/imgui_impl_sdlgpu3.h"
#include <imgui.h>

#include "ui/root_ui.h"
#include "ui/system_monitor/system_monitor_ui.h"
#include "utils/utils.h"

#include "post_process/post_process.h"
#include "render_manager/render_manager.h"
#include "resource_manager/resource_manager.h"
#include "shadow_manager/shadow_manager.h"
#include "update_manager/update_manager.h"

RootUI *DefaultRunner::m_rootUI = nullptr;
ResourceManager *DefaultRunner::m_resourceManager = nullptr;
RenderManager *DefaultRunner::m_renderManager = nullptr;
PostProcess *DefaultRunner::m_postProcess = nullptr;
UpdateManager *DefaultRunner::m_updateManager = nullptr;
Camera *DefaultRunner::m_camera = nullptr;

DefaultRunner::DefaultRunner(glm::ivec2 windowSize)
    : m_initWindowSize(windowSize)
{
    // Initialize input arrays
    std::memset(m_keys, 0, sizeof(m_keys));
    std::memset(m_mouseButtons, 0, sizeof(m_mouseButtons));

    m_width = windowSize.x;
    m_height = windowSize.y;
}

DefaultRunner::~DefaultRunner()
{
    // Cleanup is handled in Quit()
}

void DefaultRunner::UpdateCamera(float dt)
{
    float velocity = m_camera->speed * dt;

    if (m_keys[SDL_SCANCODE_LSHIFT])
        velocity *= 0.2f;
    if (m_keys[SDL_SCANCODE_SPACE])
        velocity *= 5.f;

    glm::vec3 direction(0.f);
    if (m_keys[SDL_SCANCODE_W])
        direction += m_camera->front;
    if (m_keys[SDL_SCANCODE_S])
        direction -= m_camera->front;
    if (m_keys[SDL_SCANCODE_A])
        direction -= glm::normalize(glm::cross(m_camera->front, m_camera->up));
    if (m_keys[SDL_SCANCODE_D])
        direction += glm::normalize(glm::cross(m_camera->front, m_camera->up));

    if (glm::length2(direction) > 0.f)
        m_camera->position += glm::normalize(direction) * velocity;
}

SDL_AppResult DefaultRunner::Init(int argc, char **argv)
{
#if defined(__APPLE__)
    const char *deviceName = "Metal";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
#else
    const char *deviceName = "Vulkan";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
#endif

    // Create window
    m_window = SDL_CreateWindow("SDL_GPU_Kit", m_initWindowSize.x, m_initWindowSize.y, SDL_WINDOW_RESIZABLE);
    if (!m_window)
    {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Create device
    m_device = SDL_CreateGPUDevice(shaderFormat, false, deviceName);
    if (!m_device)
    {
        SDL_Log("Failed to create device: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_ClaimWindowForGPUDevice(m_device, m_window);

    // Initialize Global Utils
    Utils::device = m_device;
    Utils::window = m_window;

    SDL_GPUSampleCount msaaSampleCount = Utils::getClosestSupportedMSAA(SDL_GPU_SAMPLECOUNT_2);

    // Initialize Managers
    m_resourceManager = new ResourceManager(m_device);
    m_renderManager = new RenderManager(m_device, m_window, m_resourceManager, msaaSampleCount);
    m_renderManager->updateResources(m_initWindowSize, msaaSampleCount);
    m_postProcess = new PostProcess(msaaSampleCount);
    m_postProcess->update(m_initWindowSize);
    m_updateManager = new UpdateManager();
    m_camera = new Camera();

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForSDLGPU(m_window);
    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = m_device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    ImGui_ImplSDLGPU3_Init(&init_info);

    m_rootUI = new RootUI();
    m_systemMonitorUI = new SystemMonitorUI();
    m_rootUI->add(m_systemMonitorUI);
    m_rootUI->add(m_postProcess);
    m_rootUI->add(m_renderManager);
    m_rootUI->add(m_renderManager->m_shadowManager);

    return SDL_APP_CONTINUE;
}

SDL_AppResult DefaultRunner::Iterate()
{
    Uint64 currentFrame = SDL_GetTicksNS();
    m_deltaTime = (currentFrame - m_lastFrame) / 1e9f;
    m_lastFrame = currentFrame;

    UpdateCamera(m_deltaTime);
    m_camera->view = glm::lookAt(m_camera->position, m_camera->position + m_camera->front, m_camera->up);
    m_camera->projection = glm::perspective(glm::radians(m_camera->fov), (float)m_width / (float)m_height, m_camera->near, m_camera->far);
    m_updateManager->update(m_deltaTime);

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);

    SDL_GPUTexture *swapchainTexture;
    SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, m_window, &swapchainTexture, &m_width, &m_height);

    if (swapchainTexture == NULL)
    {
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return SDL_APP_CONTINUE;
    }

    m_postProcess->update({m_width, m_height});
    m_renderManager->updateResources({m_width, m_height}, m_postProcess->m_sampleCount);

    // Camera Matrices
    const glm::mat4 &view = m_camera->view;
    const glm::mat4 &projection = m_camera->projection;
    m_renderManager->m_fragmentUniforms.viewPos = m_camera->position;

    // --- Shadow Pass ---
    float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    m_renderManager->m_shadowManager->updateCascades(m_camera, view, -m_renderManager->m_fragmentUniforms.lightDir, aspect);

    ShadowManager *shadowManager = m_renderManager->m_shadowManager;

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

        const Cascade &cascade = shadowManager->m_cascades[cascadeIndex];
        const glm::mat4 &lightView = cascade.view;
        const glm::mat4 &lightProj = cascade.projection;
        const glm::mat4 lightViewProj = lightProj * lightView;

        SDL_BindGPUGraphicsPipeline(shadowPass, shadowManager->m_shadowPipeline);
        SDL_SetGPUViewport(shadowPass, &viewport);

        const Frustum frustum = Frustum::fromMatrix(lightViewProj);

        for (Renderable *r : m_renderManager->m_renderables)
        {
            r->renderShadow(commandBuffer, shadowPass, lightViewProj, frustum);
        }

        SDL_BindGPUGraphicsPipeline(shadowPass, shadowManager->m_shadowAnimationPipeline);
        SDL_SetGPUViewport(shadowPass, &viewport);

        for (Renderable *r : m_renderManager->m_renderables)
        {
            r->renderAnimationShadow(commandBuffer, shadowPass, lightViewProj, frustum);
        }

        SDL_EndGPURenderPass(shadowPass);
    }

    // 1. Setup Color Target: Render to MSAA, Resolve to Normal
    colorTargetInfo = SDL_GPUColorTargetInfo();
    colorTargetInfo.texture = m_postProcess->m_msaaColorTexture;
    colorTargetInfo.clear_color = {0.f, 0.f, 0.f, 1.0f};
    colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargetInfo.store_op = SDL_GPU_STOREOP_RESOLVE;
    colorTargetInfo.resolve_texture = m_postProcess->m_colorTexture;

    // 2. Setup Depth Target
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    depthInfo.texture = m_postProcess->m_msaaDepthTexture;
    depthInfo.clear_depth = 1.0f;
    depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.store_op = SDL_GPU_STOREOP_STORE;
    depthInfo.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.stencil_store_op = SDL_GPU_STOREOP_STORE;

    if (m_postProcess->m_sampleCount == SDL_GPU_SAMPLECOUNT_1)
    {
        colorTargetInfo.texture = m_postProcess->m_colorTexture;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
    }

    SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, &depthInfo);
    m_renderManager->m_pbrManager->renderSkybox(commandBuffer, renderPass, view, projection);
    m_renderManager->renderOpaque(commandBuffer, renderPass, view, projection, m_camera->position);
    SDL_EndGPURenderPass(renderPass);

    m_postProcess->resolveDepth(commandBuffer);

    // OIT Pass
    SDL_GPUColorTargetInfo oitTargets[2];
    oitTargets[0] = SDL_GPUColorTargetInfo{};
    oitTargets[0].texture = m_renderManager->m_accumTexture;
    oitTargets[0].load_op = SDL_GPU_LOADOP_CLEAR;
    oitTargets[0].clear_color = {0, 0, 0, 0};
    oitTargets[1] = SDL_GPUColorTargetInfo{};
    oitTargets[1].texture = m_renderManager->m_revealTexture;
    oitTargets[1].load_op = SDL_GPU_LOADOP_CLEAR;
    oitTargets[1].clear_color = {1, 1, 1, 1};

    depthInfo.texture = m_postProcess->m_depthTexture;
    depthInfo.load_op = SDL_GPU_LOADOP_LOAD;
    depthInfo.store_op = SDL_GPU_STOREOP_STORE;
    depthInfo.stencil_load_op = SDL_GPU_LOADOP_LOAD;
    depthInfo.stencil_store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *oitPass = SDL_BeginGPURenderPass(commandBuffer, oitTargets, 2, &depthInfo);
    m_renderManager->renderTransparent(commandBuffer, oitPass, view, projection, m_camera->position);
    SDL_EndGPURenderPass(oitPass);

    // Composite pass
    colorTargetInfo.texture = m_postProcess->m_colorTexture;
    colorTargetInfo.load_op = SDL_GPU_LOADOP_LOAD;

    SDL_GPURenderPass *compPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, nullptr);
    m_renderManager->renderComposite(commandBuffer, compPass);
    SDL_EndGPURenderPass(compPass);

    // Post Processing
    m_postProcess->computeGTAO(commandBuffer, projection, view, m_camera->near, m_camera->far);
    m_postProcess->downsample(commandBuffer);
    m_postProcess->upsample(commandBuffer);
    m_postProcess->runSMAA(commandBuffer);
    m_postProcess->postProcess(commandBuffer, swapchainTexture);

    // UI
    m_rootUI->render(commandBuffer, swapchainTexture);

    SDL_SubmitGPUCommandBuffer(commandBuffer);

    return SDL_APP_CONTINUE;
}

SDL_AppResult DefaultRunner::ProcessEvent(SDL_Event *event)
{
    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return SDL_APP_SUCCESS;

    if (event->type == SDL_EVENT_KEY_DOWN)
        m_keys[event->key.scancode] = true;
    if (event->type == SDL_EVENT_KEY_UP)
        m_keys[event->key.scancode] = false;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        m_mouseButtons[event->button.button] = true;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP)
        m_mouseButtons[event->button.button] = false;

    if (event->type == SDL_EVENT_MOUSE_MOTION && m_mouseButtons[SDL_BUTTON_RIGHT])
    {
        float xoffset = event->motion.xrel * m_camera->sensitivity;
        float yoffset = -event->motion.yrel * m_camera->sensitivity;

        m_camera->yaw += xoffset;
        m_camera->pitch += yoffset;

        if (m_camera->pitch > 89.0f)
            m_camera->pitch = 89.0f;
        if (m_camera->pitch < -89.0f)
            m_camera->pitch = -89.0f;

        glm::vec3 direction;
        direction.x = cos(glm::radians(m_camera->yaw)) * cos(glm::radians(m_camera->pitch));
        direction.y = sin(glm::radians(m_camera->pitch));
        direction.z = sin(glm::radians(m_camera->yaw)) * cos(glm::radians(m_camera->pitch));
        m_camera->front = glm::normalize(direction);
    }

    if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE)
    {
        bool relative = SDL_GetWindowRelativeMouseMode(m_window);
        SDL_SetWindowRelativeMouseMode(m_window, !relative);
    }

    return SDL_APP_CONTINUE;
}

void DefaultRunner::Quit()
{
    if (m_rootUI)
        delete m_rootUI;
    if (m_systemMonitorUI)
        delete m_systemMonitorUI;
    if (m_resourceManager)
        delete m_resourceManager;
    if (m_renderManager)
        delete m_renderManager;
    if (m_postProcess)
        delete m_postProcess;
    if (m_updateManager)
        delete m_updateManager;

    if (m_device)
        SDL_DestroyGPUDevice(m_device);
    if (m_window)
        SDL_DestroyWindow(m_window);
}