#pragma once

#include <vector>

#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include "../frustum.h"
#include "../resource_manager/resource_manager.h"
#include "../shadow_manager/shadow_manager.h"

#include "pbr_manager.h"

struct VertexUniforms
{
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 normalMatrix;
};

struct FragmentUniforms
{
    glm::vec3 lightDir;
    float padding1;
    glm::vec3 viewPos;
    float padding2;
    glm::vec3 lightColor;
    float padding3;
};

struct MaterialUniforms
{
    glm::vec4 albedoFactor;
    glm::vec4 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    int hasAlbedoTexture;
    int hasNormalTexture;
    int hasMetallicRoughnessTexture;
    int hasOcclusionTexture;
    int hasEmissiveTexture;
    glm::vec2 uvScale;
    float padding[2];
};

class Renderable
{
public:
    virtual ~Renderable() = default;

    // Called during the main color pass
    virtual void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const glm::mat4 &view, const glm::mat4 &projection, const glm::vec3 &camPos, Frustum &frustum) = 0;

    // Called during the shadow pass
    virtual void drawShadow(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const glm::mat4 &viewProj, Frustum &frustum) = 0;
};

class RenderManager : public BaseUI
{
public:
    RenderManager(SDL_GPUDevice *device, SDL_Window *window, ResourceManager *resourceManager);
    ~RenderManager();

    FragmentUniforms m_fragmentUniforms;

    void renderUI() override;

    // Management
    void addRenderable(Renderable *renderable);

    // Rendering
    void renderShadows(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, const glm::mat4 &viewProj);
    void renderScene(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass,
                     const glm::mat4 &view, const glm::mat4 &projection,
                     const glm::vec3 &camPos);

    SDL_GPUGraphicsPipeline *getPbrPipeline() const
    {
        return m_pbrPipeline;
    }
    SDL_GPUSampler *getBaseSampler() const
    {
        return m_baseSampler;
    }
    SDL_GPUTexture *getDefaultTexture() const
    {
        return m_defaultTexture;
    }
    PbrManager *getPbrManager()
    {
        return m_pbrManager;
    }
    ShadowManager *getShadowManager()
    {
        return m_shadowManager;
    }

private:
    SDL_GPUDevice *m_device;
    SDL_Window *m_window;
    ResourceManager *m_resourceManager;
    PbrManager *m_pbrManager;
    ShadowManager *m_shadowManager;

    SDL_GPUGraphicsPipeline *m_pbrPipeline;
    SDL_GPUSampler *m_baseSampler;
    SDL_GPUTexture *m_defaultTexture;

    std::vector<Renderable *> m_renderables;

    void createPipeline();
    void createDefaultResources();
};
