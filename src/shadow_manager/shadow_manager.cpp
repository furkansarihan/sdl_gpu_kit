#include "shadow_manager.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <imgui.h>

#include "../resource_manager/resource_manager.h"
#include "../utils/utils.h"

// --- Cascaded shadow map texture (2D array) ---
ShadowManager::ShadowManager()
{
    updateTexture();

    SDL_GPUSamplerCreateInfo shadowSamplerInfo{};
    shadowSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    shadowSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    shadowSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    shadowSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowSamplerInfo.enable_anisotropy = false;

    m_shadowSampler = SDL_CreateGPUSampler(Utils::device, &shadowSamplerInfo);
    if (!m_shadowSampler)
    {
        SDL_Log("Failed to create shadow sampler: %s", SDL_GetError());
    }

    // --- Shadow map graphics pipeline ---
    {
        SDL_GPUShader *shadowVert = Utils::loadShader("src/shaders/shadow_csm.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
        SDL_GPUShader *shadowFrag = Utils::loadShader("src/shaders/shadow_csm.frag", 0, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);

        SDL_GPUGraphicsPipelineCreateInfo shadowInfo{};
        shadowInfo.vertex_shader = shadowVert;
        shadowInfo.fragment_shader = shadowFrag;
        shadowInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        // Vertex layout: we only really need position, but we can keep it simple
        SDL_GPUVertexBufferDescription vbDesc{};
        vbDesc.slot = 0;
        vbDesc.pitch = sizeof(Vertex);
        vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute vAttribs[1]{};
        vAttribs[0].location = 0;
        vAttribs[0].buffer_slot = 0;
        vAttribs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        vAttribs[0].offset = offsetof(Vertex, position);

        shadowInfo.vertex_input_state.vertex_buffer_descriptions = &vbDesc;
        shadowInfo.vertex_input_state.num_vertex_buffers = 1;
        shadowInfo.vertex_input_state.vertex_attributes = vAttribs;
        shadowInfo.vertex_input_state.num_vertex_attributes = 1;

        SDL_GPURasterizerState rs{};
        rs.cull_mode = SDL_GPU_CULLMODE_BACK;
        rs.fill_mode = SDL_GPU_FILLMODE_FILL;
        rs.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        shadowInfo.rasterizer_state = rs;

        SDL_GPUDepthStencilState ds{};
        ds.enable_depth_test = false;
        ds.enable_depth_write = false;
        shadowInfo.depth_stencil_state = ds;

        SDL_GPUMultisampleState ms{};
        ms.sample_count = SDL_GPU_SAMPLECOUNT_1;
        shadowInfo.multisample_state = ms;

        SDL_GPUColorTargetDescription colorTarget = {};
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R16_UNORM;

        SDL_GPUGraphicsPipelineTargetInfo targetInfo{};
        targetInfo.color_target_descriptions = &colorTarget;
        targetInfo.num_color_targets = 1;
        targetInfo.has_depth_stencil_target = false;

        shadowInfo.target_info = targetInfo;

        m_shadowPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &shadowInfo);
        if (!m_shadowPipeline)
        {
            SDL_Log("Failed to create shadow pipeline: %s", SDL_GetError());
        }

        shadowInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        m_shadowDoubleSidedPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &shadowInfo);
        if (!m_shadowDoubleSidedPipeline)
        {
            SDL_Log("Failed to create m_shadowDoubleSidedPipeline: %s", SDL_GetError());
        }

        SDL_GPUShader *shadowAnimationVert = Utils::loadShader("src/shaders/shadow_csm_skinned.vert", 0, 2, SDL_GPU_SHADERSTAGE_VERTEX);
        shadowInfo.vertex_shader = shadowAnimationVert;

        SDL_GPUVertexAttribute vertexAnimAttributes[6]{};
        vertexAnimAttributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, position)};
        vertexAnimAttributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, normal)};
        vertexAnimAttributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, uv)};
        vertexAnimAttributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, tangent)};
        vertexAnimAttributes[4] = {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, offsetof(Vertex, joints)};
        vertexAnimAttributes[5] = {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, weights)};
        shadowInfo.vertex_input_state.num_vertex_attributes = 6;
        shadowInfo.vertex_input_state.vertex_attributes = vertexAnimAttributes;

        m_shadowAnimationPipeline = SDL_CreateGPUGraphicsPipeline(Utils::device, &shadowInfo);
        if (!m_shadowAnimationPipeline)
        {
            SDL_Log("Failed to create m_shadowAnimationPipeline: %s", SDL_GetError());
        }

        SDL_ReleaseGPUShader(Utils::device, shadowVert);
        SDL_ReleaseGPUShader(Utils::device, shadowAnimationVert);
        SDL_ReleaseGPUShader(Utils::device, shadowFrag);
    }

    m_shadowUniforms.cascadeBias = {0.0005f, 0.0005f, 0.0005f, 0.0005f};
    m_shadowUniforms.shadowFar = 60.f;
    m_shadowUniforms.strength = 0.8f;
    m_cascadeLambda = 0.8f;
}

ShadowManager::~ShadowManager()
{
    if (m_shadowPipeline)
        SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_shadowPipeline);
    if (m_shadowDoubleSidedPipeline)
        SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_shadowDoubleSidedPipeline);
    if (m_shadowAnimationPipeline)
        SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_shadowAnimationPipeline);
    if (m_shadowMapTexture)
        SDL_ReleaseGPUTexture(Utils::device, m_shadowMapTexture);
    if (m_shadowSampler)
        SDL_ReleaseGPUSampler(Utils::device, m_shadowSampler);
}

void ShadowManager::renderUI()
{
    // if (!ImGui::CollapsingHeader("Shadow Manager", ImGuiTreeNodeFlags_DefaultOpen))
    if (!ImGui::CollapsingHeader("Shadow Manager"))
        return;

    ImGui::PushID(this);

    if (ImGui::DragInt("Shadowmap Size", &m_shadowMapResolution, 16, 16, 4096))
        updateTexture();
    ImGui::DragFloat("Cascade Lambda", &m_cascadeLambda, 0.01f, 0.f, 1.f);
    ImGui::DragFloat("Shadow Far", &m_shadowUniforms.shadowFar, 0.2f, 0.f, 1000.f);
    ImGui::DragFloat("Shadow Strength", &m_shadowUniforms.strength, 0.01f, 0.f);
    ImGui::DragFloat4("Bias", &m_shadowUniforms.cascadeBias.x, 0.00001f, 0.f, 1.f, "%.5f");

    // TODO:
    /* if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::DragFloat("Near", &m_camera->near, 0.1f, 0.f, 180.f);
        ImGui::DragFloat("Far", &m_camera->far, 1.f, 0.f, 10000.f);
        ImGui::DragFloat("Field Of View", &m_camera->fov, 0.1f, 0.f, 180.f);

        ImGui::TreePop();
    } */

    if (ImGui::TreeNode("Textures"))
    {
        // TODO: layers
        ImGui::Text("Shadowmap");
        ImGui::Image((ImTextureID)(m_shadowMapTexture), ImVec2(256, 256));

        ImGui::TreePop();
    }

    ImGui::PopID();
}

void ShadowManager::updateTexture()
{
    if (m_shadowMapTexture)
    {
        SDL_ReleaseGPUTexture(Utils::device, m_shadowMapTexture);
        m_shadowMapTexture = nullptr;
    }

    SDL_GPUTextureCreateInfo shadowInfo{};
    shadowInfo.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    shadowInfo.format = SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    shadowInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    shadowInfo.width = m_shadowMapResolution;
    shadowInfo.height = m_shadowMapResolution;
    shadowInfo.layer_count_or_depth = NUM_CASCADES;
    shadowInfo.num_levels = 1;
    shadowInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

    m_shadowMapTexture = SDL_CreateGPUTexture(Utils::device, &shadowInfo);

    if (!m_shadowMapTexture)
    {
        SDL_Log("Failed to create shadow map texture: %s", SDL_GetError());
    }
}

void ShadowManager::updateCascades(
    Camera *camera,
    const glm::mat4 &view,
    const glm::vec3 &lightDir,
    float aspect)
{
    float nearClip = camera->near;
    float farClip = m_shadowUniforms.shadowFar;
    float clipRange = farClip - nearClip;

    float minZ = nearClip;
    float maxZ = nearClip + clipRange;
    float range = maxZ - minZ;
    float ratio = maxZ / minZ;

    float cascadeSplitsScalar[NUM_CASCADES];

    // 1. Calculate Split Distances
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        float p = (i + 1) / static_cast<float>(NUM_CASCADES);
        float logSplit = minZ * std::pow(ratio, p);
        float uniSplit = minZ + range * p;
        float d = m_cascadeLambda * (logSplit - uniSplit) + uniSplit;
        cascadeSplitsScalar[i] = (d - nearClip) / clipRange;
    }

    glm::mat4 invView = glm::inverse(view);

    float fovY = glm::radians(camera->fov);
    float tanHalfFovY = std::tan(fovY * 0.5f);
    float tanHalfFovX = tanHalfFovY * aspect;

    glm::vec4 cascadeFarPlanes(0.0f);

    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        float prevSplitDist = (i == 0) ? 0.0f : cascadeSplitsScalar[i - 1];
        float splitDist = cascadeSplitsScalar[i];

        float nearDist = nearClip + prevSplitDist * clipRange;
        float farDist = nearClip + splitDist * clipRange;

        // 2. Reconstruct Frustum Corners (View Space)
        float xn = nearDist * tanHalfFovX;
        float yn = nearDist * tanHalfFovY;
        float xf = farDist * tanHalfFovX;
        float yf = farDist * tanHalfFovY;

        glm::vec3 frustumCornersVS[8] = {
            glm::vec3(-xn, yn, -nearDist),
            glm::vec3(xn, yn, -nearDist),
            glm::vec3(xn, -yn, -nearDist),
            glm::vec3(-xn, -yn, -nearDist),
            glm::vec3(-xf, yf, -farDist),
            glm::vec3(xf, yf, -farDist),
            glm::vec3(xf, -yf, -farDist),
            glm::vec3(-xf, -yf, -farDist)};

        // 3. Transform to World Space & Calculate Center
        glm::vec3 frustumCenter(0.0f);
        for (int j = 0; j < 8; ++j)
        {
            glm::vec4 vW = invView * glm::vec4(frustumCornersVS[j], 1.0f);
            frustumCornersVS[j] = glm::vec3(vW);
            frustumCenter += frustumCornersVS[j];
        }
        frustumCenter /= 8.0f;

        // 4. Calculate Bounding Sphere Radius
        float radius = 0.0f;
        for (int j = 0; j < 8; ++j)
        {
            float dist = glm::length(frustumCornersVS[j] - frustumCenter);
            radius = glm::max(radius, dist);
        }

        // Round radius to 1/16th unit to prevent jitter from floating point errors
        radius = std::ceil(radius * 16.0f) / 16.0f;

        // 5. Build Light Matrix
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(up, lightDir)) > 0.99f)
            up = glm::vec3(0.0f, 0.0f, 1.0f);

        // Initial View Matrix (at center)
        glm::mat4 lightView = glm::lookAt(glm::vec3(0), -lightDir, up);

        // 6. Texel Snapping
        // Transform center to Light Space
        glm::vec4 centerLS = lightView * glm::vec4(frustumCenter, 1.0f);

        // Calculate texel size
        float shadowMapSize = static_cast<float>(m_shadowMapResolution);
        float texelsPerUnit = shadowMapSize / (radius * 2.0f);

        // Snap to grid
        centerLS.x = glm::floor(centerLS.x * texelsPerUnit) / texelsPerUnit;
        centerLS.y = glm::floor(centerLS.y * texelsPerUnit) / texelsPerUnit;

        // Transform snapped center back to World Space
        glm::vec3 centerSnapped = glm::vec3(glm::inverse(lightView) * centerLS);

        // 7. Final View Matrix
        float zMargin = radius * 2.f;
        glm::vec3 eye = centerSnapped - (lightDir * zMargin);

        lightView = glm::lookAt(eye, centerSnapped, up);

        // 8. Final Projection Matrix (Zero-to-One)
        float zFarDist = zMargin + radius * 2.0f;

        glm::mat4 lightProj = glm::orthoRH_ZO(
            -radius, radius,
            -radius, radius,
            zFarDist,
            0.f);

        static const glm::mat4 m_biasMatrix = glm::mat4(
            0.5, 0.0, 0.0, 0.0,
            0.0, -0.5, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.5, 0.5, 0.0, 1.0);

        m_shadowUniforms.depthBiasVP[i] = m_biasMatrix * lightProj * lightView;
        m_cascades[i].view = lightView;
        m_cascades[i].projection = lightProj;

        (&cascadeFarPlanes.x)[i] = nearClip + splitDist * clipRange;
    }

    m_shadowUniforms.cameraView = view;
    m_shadowUniforms.cascadeSplits = cascadeFarPlanes;
}
