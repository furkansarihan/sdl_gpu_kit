#pragma once

#include <SDL3/SDL_gpu.h>

#include "../resource_manager/resource_manager.h"

#include "pbr_manager.h"

class RenderManager
{
public:
    RenderManager(ResourceManager *resourceManager);
    ~RenderManager();

    ResourceManager *m_resourceManager;

    PbrManager *m_pbrManager;
};
