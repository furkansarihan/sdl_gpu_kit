#pragma once

#include "../render_manager/render_manager.h"
#include "resource_manager.h"

struct ShadowVertexUniforms
{
    glm::mat4 lightViewProj; // light VP for current cascade
    glm::mat4 model;         // world transform for current draw
};

class RenderableModel : public Renderable
{
public:
    ModelData *m_model;
    RenderManager *m_manager;
    bool m_castingShadow;

    RenderableModel(ModelData *m, RenderManager *rm)
        : m_model(m),
          m_manager(rm),
          m_castingShadow(true)
    {
    }

    static void renderPrimitive(
        const PrimitiveData &prim,
        const glm::mat4 &model,
        bool blend,
        bool checkDoubleSide,
        bool doubleSide,
        RenderManager *renderManager,
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const Frustum &frustum);

    void renderModel(
        bool blend,
        bool checkDoubleSide,
        bool doubleSide,
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const Frustum &frustum);

    void renderOpaque(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const Frustum &frustum) override;
    void renderOpaqueDoubleSided(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const Frustum &frustum) override;
    void renderTransparent(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const Frustum &frustum) override;
    void renderShadow(
        SDL_GPUCommandBuffer *cmd,
        SDL_GPURenderPass *pass,
        const glm::mat4 &viewProj,
        const Frustum &frustum) override;

private:
    static void bindTextures(RenderManager *renderManager, SDL_GPURenderPass *pass, Material *mat);
};
