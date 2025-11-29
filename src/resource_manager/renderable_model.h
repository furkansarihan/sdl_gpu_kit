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
    ModelData *model;
    RenderManager *manager;

    RenderableModel(ModelData *m, RenderManager *rm)
        : model(m),
          manager(rm)
    {
    }

    void renderModel(
        bool blend,
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
    void bindTextures(SDL_GPURenderPass *pass, Material *mat);
};
