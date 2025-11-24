#pragma once

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

class RootUI;
class SystemMonitorUI;
class ResourceManager;
class RenderManager;
class PostProcess;
struct RenderableModel;
struct Texture;

#include "camera.h"

class DefaultRunner
{
public:
    DefaultRunner();
    ~DefaultRunner();

    // Core SDL resources
    SDL_Window *m_window = nullptr;
    SDL_GPUDevice *m_device = nullptr;

    // Input State
    bool m_keys[SDL_SCANCODE_COUNT]{};
    bool m_mouseButtons[6]{};

    // Time State
    float m_deltaTime = 0.0f;
    Uint64 m_lastFrame = 0;

    // Sub-systems
    Camera m_camera;
    RootUI *m_rootUI = nullptr;
    SystemMonitorUI *m_systemMonitorUI = nullptr;
    ResourceManager *m_resourceManager = nullptr;
    RenderManager *m_renderManager = nullptr;
    PostProcess *m_postProcess = nullptr;

    void UpdateCamera(float dt);

    // Core application lifecycle methods
    SDL_AppResult Init(int argc, char **argv);
    SDL_AppResult Iterate();
    SDL_AppResult ProcessEvent(SDL_Event *event);
    void Quit();
};
