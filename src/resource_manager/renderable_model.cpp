#include "renderable_model.h"

// Helper for Culling
float ExtractMaxScale(const glm::mat4 &m)
{
    float sx = glm::length(glm::vec3(m[0]));
    float sy = glm::length(glm::vec3(m[1]));
    float sz = glm::length(glm::vec3(m[2]));
    return std::max(sx, std::max(sy, sz));
}

bool PrimitiveInFrustum(const PrimitiveData &prim, const glm::mat4 &worldTransform, const Frustum &frustum)
{
    glm::vec3 worldCenter = glm::vec3(worldTransform * glm::vec4(prim.sphereCenter, 1.0f));
    float maxScale = ExtractMaxScale(worldTransform);
    float worldRadius = prim.sphereRadius * maxScale;
    return frustum.intersectsSphere(worldCenter, worldRadius);
}

void RenderableModel::renderModel(
    bool blend,
    SDL_GPUCommandBuffer *cmd,
    SDL_GPURenderPass *pass,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const Frustum &frustum)
{
    // Material fallback
    Material defaultMaterial("default");

    VertexUniforms vUniforms{};
    vUniforms.view = view;
    vUniforms.projection = projection;

    for (const auto &node : model->nodes)
    {
        if (node.meshIndex < 0 || node.meshIndex >= model->meshes.size())
            continue;

        const MeshData &mesh = model->meshes[node.meshIndex];
        const glm::mat4 &world = node.worldTransform;

        for (const auto &prim : mesh.primitives)
        {
            Material *mat = prim.material ? prim.material : &defaultMaterial;

            if (blend && mat->alphaMode != AlphaMode::Blend)
                continue;

            if (!blend && mat->alphaMode == AlphaMode::Blend)
                continue;

            if (!PrimitiveInFrustum(prim, world, frustum))
                continue;

            // Update Model Matrix
            vUniforms.model = world;
            vUniforms.normalMatrix = glm::transpose(glm::inverse(world));
            SDL_PushGPUVertexUniformData(cmd, 0, &vUniforms, sizeof(vUniforms));

            // Material Setup
            MaterialUniforms matUniforms{};
            matUniforms.albedoFactor = mat->albedo;
            matUniforms.emissiveFactor = mat->emissiveColor;
            matUniforms.metallicFactor = mat->metallic;
            matUniforms.roughnessFactor = mat->roughness;
            matUniforms.occlusionStrength = 1.0f;
            matUniforms.alphaCutoff = mat->alphaCutoff;
            matUniforms.uvScale = mat->uvScale;
            matUniforms.hasAlbedoTexture = (mat->albedoTexture.id != nullptr);
            matUniforms.hasNormalTexture = (mat->normalTexture.id != nullptr);
            matUniforms.hasMetallicRoughnessTexture = (mat->metallicRoughnessTexture.id != nullptr);
            matUniforms.hasOcclusionTexture = (mat->occlusionTexture.id != nullptr);
            matUniforms.hasEmissiveTexture = (mat->emissiveTexture.id != nullptr);
            matUniforms.hasOpacityTexture = (mat->opacityTexture.id != nullptr);

            SDL_PushGPUFragmentUniformData(cmd, 1, &matUniforms, sizeof(matUniforms));

            // Bind Textures
            bindTextures(pass, mat);

            // Bind Buffers & Draw
            SDL_GPUBufferBinding vb{prim.vertexBuffer, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);

            if (!prim.indices.empty())
            {
                SDL_GPUBufferBinding ib{prim.indexBuffer, 0};
                SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_DrawGPUIndexedPrimitives(pass, (Uint32)prim.indices.size(), 1, 0, 0, 0);
            }
            else
            {
                SDL_DrawGPUPrimitives(pass, (Uint32)prim.vertices.size(), 0, 0, 0);
            }
        }
    }
}

void RenderableModel::renderOpaque(
    SDL_GPUCommandBuffer *cmd,
    SDL_GPURenderPass *pass,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const Frustum &frustum)
{
    renderModel(false, cmd, pass, view, projection, frustum);
}

void RenderableModel::renderTransparent(
    SDL_GPUCommandBuffer *cmd,
    SDL_GPURenderPass *pass,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const Frustum &frustum)
{
    renderModel(true, cmd, pass, view, projection, frustum);
}

void RenderableModel::renderShadow(
    SDL_GPUCommandBuffer *cmd,
    SDL_GPURenderPass *pass,
    const glm::mat4 &viewProj,
    const Frustum &frustum)
{
    ShadowVertexUniforms shadowUniforms{};
    shadowUniforms.lightViewProj = viewProj;

    for (const auto &node : model->nodes)
    {
        if (node.meshIndex < 0)
            continue;
        const MeshData &mesh = model->meshes[node.meshIndex];
        const glm::mat4 &world = node.worldTransform;

        for (const auto &prim : mesh.primitives)
        {
            if (!PrimitiveInFrustum(prim, world, frustum))
                continue;

            shadowUniforms.model = world;
            SDL_PushGPUVertexUniformData(cmd, 0, &shadowUniforms, sizeof(shadowUniforms));

            SDL_GPUBufferBinding vb{prim.vertexBuffer, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);

            if (!prim.indices.empty())
            {
                SDL_GPUBufferBinding ib{prim.indexBuffer, 0};
                SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_DrawGPUIndexedPrimitives(pass, (Uint32)prim.indices.size(), 1, 0, 0, 0);
            }
            else
            {
                SDL_DrawGPUPrimitives(pass, (Uint32)prim.vertices.size(), 0, 0, 0);
            }
        }
    }
}

void RenderableModel::bindTextures(SDL_GPURenderPass *pass, Material *mat)
{
    SDL_GPUTextureSamplerBinding bindings[10];
    SDL_GPUTexture *def = manager->m_defaultTexture;
    SDL_GPUSampler *samp = manager->m_baseSampler;

    bindings[0] = {mat->albedoTexture.id ? mat->albedoTexture.id : def, samp};
    bindings[1] = {mat->normalTexture.id ? mat->normalTexture.id : def, samp};
    bindings[2] = {mat->metallicRoughnessTexture.id ? mat->metallicRoughnessTexture.id : def, samp};
    bindings[3] = {mat->occlusionTexture.id ? mat->occlusionTexture.id : def, samp};
    bindings[4] = {mat->emissiveTexture.id ? mat->emissiveTexture.id : def, samp};
    bindings[5] = {mat->opacityTexture.id ? mat->opacityTexture.id : def, samp};

    // PBR Global textures (Irradiance, etc) retrieved from Manager
    PbrManager *pbr = manager->m_pbrManager;
    bindings[6] = {pbr->m_irradianceTexture, pbr->m_cubeSampler};
    bindings[7] = {pbr->m_prefilterTexture, pbr->m_cubeSampler};
    bindings[8] = {pbr->m_brdfTexture, pbr->m_brdfSampler};

    // Shadowmap
    bindings[9] = {manager->m_shadowManager->m_shadowMapTexture, manager->m_shadowManager->m_shadowSampler};

    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 10);
}
