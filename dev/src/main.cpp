#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_main.h>

#include "default_runner.h"

#include "render_manager/render_manager.h"
#include "resource_manager/renderable_model.h"
#include "resource_manager/resource_manager.h"

#include "utils/utils.h"

std::vector<RenderableModel *> g_renderableModels;
Texture g_hdrTexture;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    DefaultRunner *runner = new DefaultRunner({1280, 720});
    *appstate = runner;
    SDL_AppResult result = runner->Init(argc, argv);

    if (result != SDL_APP_CONTINUE)
        return result;

    ResourceManager *resourceManager = runner->m_resourceManager;
    RenderManager *renderManager = runner->m_renderManager;

    std::string exePath = Utils::getExecutablePath();

    // load hdri
    const char *hdriPath = "assets/hdris/kloofendal_43d_clear_2k.hdr";
    TextureParams params;
    params.dataType = TextureDataType::Float32;
    params.sample = true;
    g_hdrTexture = resourceManager->loadTextureFromFile(params, std::string(exePath + "/" + hdriPath));
    renderManager->m_pbrManager->updateEnvironmentTexture(g_hdrTexture);

    // Load Asset
    const char *modelPath = "assets/models/DamagedHelmet.glb";
    ModelData *model = resourceManager->loadModel(std::string(exePath + "/" + modelPath).c_str());

    // Create Renderable Instance
    if (model)
    {
        RenderableModel *renderable = new RenderableModel(model, renderManager);
        renderManager->addRenderable(renderable);
        g_renderableModels.push_back(renderable);
    }

    return result;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    auto *runner = static_cast<DefaultRunner *>(appstate);
    return runner->Iterate();
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    auto *runner = static_cast<DefaultRunner *>(appstate);
    return runner->ProcessEvent(event);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    if (!appstate)
        return;

    auto *runner = static_cast<DefaultRunner *>(appstate);
    ResourceManager *resourceManager = runner->m_resourceManager;

    for (RenderableModel *obj : g_renderableModels)
    {
        if (resourceManager)
            resourceManager->dispose(obj->m_model);
        delete obj;
    }
    g_renderableModels.clear();

    resourceManager->dispose(g_hdrTexture);

    runner->Quit();
    delete runner;
}
