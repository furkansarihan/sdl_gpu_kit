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
    float alphaCutoff;

    int hasAlbedoTexture;
    int hasNormalTexture;
    int hasMetallicRoughnessTexture;
    int hasOcclusionTexture;

    int hasEmissiveTexture;
    int hasOpacityTexture;
    glm::vec2 uvScale;
};

class Renderable
{
public:
    virtual ~Renderable() = default;

    virtual void renderOpaque(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        Frustum &frustum) = 0;
    virtual void renderTransparent(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        Frustum &frustum) = 0;
    virtual void renderShadow(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &viewProj,
        Frustum &frustum) = 0;
};

class RenderManager : public BaseUI
{
public:
    RenderManager(
        SDL_GPUDevice *device,
        SDL_Window *window,
        ResourceManager *resourceManager,
        SDL_GPUSampleCount sampleCount);
    ~RenderManager();

    FragmentUniforms m_fragmentUniforms;

    SDL_GPUDevice *m_device;
    SDL_Window *m_window;
    ResourceManager *m_resourceManager;
    PbrManager *m_pbrManager;
    ShadowManager *m_shadowManager;

    SDL_GPUGraphicsPipeline *m_pbrPipeline;
    SDL_GPUSampler *m_baseSampler;
    SDL_GPUTexture *m_defaultTexture;

    SDL_GPUGraphicsPipeline *m_oitPipeline;
    SDL_GPUGraphicsPipeline *m_compositePipeline;

    SDL_GPUTexture *m_accumTexture = nullptr;
    SDL_GPUTexture *m_revealTexture = nullptr;

    glm::ivec2 m_screenSize;

    std::vector<Renderable *> m_renderables;

    SDL_GPUSampleCount m_sampleCount;

    void renderUI() override;

    void update(glm::ivec2 screenSize, SDL_GPUSampleCount sampleCount);

    // Management
    void addRenderable(Renderable *renderable);

    // Resource
    void createDefaultResources();
    void updateOitTextures(glm::ivec2 screenSize);

    // Pipeline
    void createPipeline(SDL_GPUSampleCount sampleCount);

    // Rendering
    void renderShadow(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &viewProj);
    void renderOpaque(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const glm::vec3 &camPos);
    void renderTransparent(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const glm::vec3 &camPos);
    void renderComposite(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass);
};
