#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_scancode.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/norm.hpp>

#include "external/imgui/imgui_impl_sdl3.h"
#include "external/imgui/imgui_impl_sdlgpu3.h"
#include <imgui.h>

#include "ui/root_ui.h"
#include "ui/system_monitor/system_monitor_ui.h"

#include "utils/utils.h"

#include "post_process/post_process.h"
#include "render_manager/render_manager.h"
#include "resource_manager/renderable_model.h"
#include "resource_manager/resource_manager.h"
#include "shadow_manager/shadow_manager.h"

#include "camera.h"

Camera camera;
SDL_Window *window;
SDL_GPUDevice *device;

bool keys[SDL_SCANCODE_COUNT]{};
bool mouseButtons[6]{};
float deltaTime = 0.0f;
Uint64 lastFrame = 0;

// Sub-systems
RootUI *rootUI;
SystemMonitorUI *systemMonitorUI;

ResourceManager *resourceManager;
RenderManager *renderManager;
PostProcess *postProcess;

Texture hdrTexture;
std::vector<RenderableModel *> renderableModels;

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

    // Initialize Global Utils
    Utils::device = device;
    Utils::window = window;

    SDL_GPUSampleCount msaaSampleCount = Utils::getHighestSupportedMSAA();

    // Initialize Managers
    resourceManager = new ResourceManager(device);
    renderManager = new RenderManager(device, window, resourceManager, msaaSampleCount);
    postProcess = new PostProcess(msaaSampleCount);
    postProcess->update(screenSize);

    // Load Assets
    std::string exePath = Utils::getExecutablePath();
    const char *modelPath = "assets/models/DamagedHelmet.glb";
    // const char *modelPath = "assets/models/ABeautifulGame.glb";
    ModelData *model = resourceManager->loadModel(std::string(exePath + "/" + modelPath).c_str());

    // Create Renderable Instance
    if (model)
    {
        RenderableModel *renderable = new RenderableModel(model, renderManager);
        renderManager->addRenderable(renderable);
        renderableModels.push_back(renderable);
    }

    // Load HDRI
    {
        const char *hdriPath = "assets/hdris/kloofendal_43d_clear_2k.hdr";
        // const char *hdriPath = "/assets/hdris/golden_gate_hills_8k.hdr";
        // const char *hdriPath = "/assets/hdris/studio_small_03_1k.hdr";
        // const char *hdriPath = "/assets/hdris/TCom_IcelandGolfCourse_2K_hdri_sphere.hdr";

        TextureParams params;
        params.dataType = TextureDataType::Float32;
        params.sample = true;
        hdrTexture = resourceManager->loadTextureFromFile(params, std::string(exePath + "/" + hdriPath));
        renderManager->getPbrManager()->updateEnvironmentTexture(hdrTexture);
    }

    // Setup ImGui
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
    rootUI->add(renderManager);
    rootUI->add(renderManager->getShadowManager());

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    Uint64 currentFrame = SDL_GetTicksNS();
    deltaTime = (currentFrame - lastFrame) / 1e9f;
    lastFrame = currentFrame;

    UpdateCamera(deltaTime);

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);

    SDL_GPUTexture *swapchainTexture;
    Uint32 width, height;
    SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, &width, &height);

    if (swapchainTexture == NULL)
    {
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return SDL_APP_CONTINUE;
    }

    postProcess->update({width, height});
    renderManager->update(postProcess->m_sampleCount);

    // Camera Matrices
    glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
    glm::mat4 projection = glm::perspective(glm::radians(camera.fov), (float)width / (float)height, camera.near, camera.far);
    renderManager->m_fragmentUniforms.viewPos = camera.position;

    // --- Shadow Pass ---
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    renderManager->getShadowManager()->updateCascades(&camera, view, -renderManager->m_fragmentUniforms.lightDir, aspect);

    ShadowManager *shadowManager = renderManager->getShadowManager();

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
        SDL_BindGPUGraphicsPipeline(shadowPass, shadowManager->m_shadowPipeline);

        const Cascade &cascade = shadowManager->m_cascades[cascadeIndex];
        const glm::mat4 &lightView = cascade.view;
        const glm::mat4 &lightProj = cascade.projection;
        const glm::mat4 lightViewProj = lightProj * lightView;

        SDL_SetGPUViewport(shadowPass, &viewport);

        renderManager->renderShadows(commandBuffer, shadowPass, lightViewProj);

        SDL_EndGPURenderPass(shadowPass);
    }

    // 1. Setup Color Target: Render to MSAA, Resolve to Normal
    colorTargetInfo = SDL_GPUColorTargetInfo();
    colorTargetInfo.texture = postProcess->m_msaaColorTexture; // Render to MSAA
    colorTargetInfo.clear_color = {0.f, 0.f, 0.f, 1.0f};
    colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargetInfo.store_op = SDL_GPU_STOREOP_RESOLVE;            // Resolve automatically
    colorTargetInfo.resolve_texture = postProcess->m_colorTexture; // Destination

    // 2. Setup Depth Target: Render to MSAA
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    depthInfo.texture = postProcess->m_msaaDepthTexture; // Render to MSAA
    depthInfo.clear_depth = 1.0f;
    depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.store_op = SDL_GPU_STOREOP_STORE; // Store MSAA depth for manual resolve later
    depthInfo.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.stencil_store_op = SDL_GPU_STOREOP_STORE;

    // TODO: test
    // no msaa
    if (postProcess->m_sampleCount == SDL_GPU_SAMPLECOUNT_1)
    {
        colorTargetInfo.texture = postProcess->m_colorTexture;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
    }

    SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, &depthInfo);

    // Render Skybox
    renderManager->getPbrManager()->renderSkybox(commandBuffer, renderPass, view, projection);

    // Render Scene Objects
    renderManager->renderScene(commandBuffer, renderPass, view, projection, camera.position);

    SDL_EndGPURenderPass(renderPass);

    // --- Post Processing ---
    postProcess->resolveDepth(commandBuffer);
    postProcess->computeGTAO(commandBuffer, projection, view, camera.near, camera.far);
    postProcess->downsample(commandBuffer);
    postProcess->upsample(commandBuffer);
    postProcess->postProcess(commandBuffer, swapchainTexture);

    // --- UI ---
    rootUI->render(commandBuffer, swapchainTexture);

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
    delete renderManager;

    // Cleanup Instances
    for (RenderableModel *obj : renderableModels)
    {
        resourceManager->dispose(obj->model);
        delete obj;
    }
    renderableModels.clear();

    // Cleanup Resources
    resourceManager->dispose(hdrTexture);

    delete resourceManager;

    if (device)
        SDL_DestroyGPUDevice(device);
    if (window)
        SDL_DestroyWindow(window);
}
