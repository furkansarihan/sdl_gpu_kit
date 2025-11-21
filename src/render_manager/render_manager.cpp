#include "render_manager.h"
#include "pbr_manager.h"

RenderManager::RenderManager(ResourceManager *resourceManager)
    : m_resourceManager(resourceManager)
{
    m_pbrManager = new PbrManager(m_resourceManager);
}

RenderManager::~RenderManager()
{
    delete m_pbrManager;
}