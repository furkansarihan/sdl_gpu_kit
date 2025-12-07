#pragma once

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

class RootUI;
class SystemMonitorUI;
class ResourceManager;
class RenderManager;
class PostProcess;
struct UpdateManager;
struct InputManager;

#include "camera.h"

class DefaultRunner
{
public:
    DefaultRunner(glm::ivec2 windowSize);
    ~DefaultRunner();

    // Core SDL resources
    SDL_Window *m_window = nullptr;
    SDL_GPUDevice *m_device = nullptr;

    // Time State
    float m_deltaTime = 0.0f;
    Uint64 m_lastFrame = 0;

    // Viewport
    glm::ivec2 m_initWindowSize;
    Uint32 m_width, m_height;

    // Sub-systems
    SystemMonitorUI *m_systemMonitorUI = nullptr;
    static RootUI *m_rootUI;
    static ResourceManager *m_resourceManager;
    static RenderManager *m_renderManager;
    static PostProcess *m_postProcess;
    static UpdateManager *m_updateManager;
    static Camera *m_camera;

    // Core application lifecycle methods
    SDL_AppResult Init(int argc, char **argv);
    SDL_AppResult Iterate();
    SDL_AppResult ProcessEvent(SDL_Event *event);
    void Quit();
};
