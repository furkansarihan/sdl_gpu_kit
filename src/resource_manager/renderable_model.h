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

    void draw(SDL_GPUCommandBuffer *cmd,
              SDL_GPURenderPass *pass,
              const glm::mat4 &view,
              const glm::mat4 &projection,
              const glm::vec3 &camPos,
              Frustum &frustum) override;
    void drawShadow(SDL_GPUCommandBuffer *cmd,
                    SDL_GPURenderPass *pass,
                    const glm::mat4 &viewProj,
                    Frustum &frustum) override;

private:
    void bindTextures(SDL_GPURenderPass *pass, Material *mat);
};
