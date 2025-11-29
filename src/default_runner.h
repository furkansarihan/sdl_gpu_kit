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
struct UpdateManager;

#include "camera.h"

class DefaultRunner
{
public:
    DefaultRunner(glm::ivec2 windowSize);
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

    // Viewport
    glm::ivec2 m_initWindowSize;
    Uint32 m_width, m_height;

    // Sub-systems
    Camera m_camera;
    RootUI *m_rootUI = nullptr;
    SystemMonitorUI *m_systemMonitorUI = nullptr;
    ResourceManager *m_resourceManager = nullptr;
    RenderManager *m_renderManager = nullptr;
    PostProcess *m_postProcess = nullptr;
    UpdateManager *m_updateManager = nullptr;

    void UpdateCamera(float dt);

    // Core application lifecycle methods
    SDL_AppResult Init(int argc, char **argv);
    SDL_AppResult Iterate();
    SDL_AppResult ProcessEvent(SDL_Event *event);
    void Quit();
};
