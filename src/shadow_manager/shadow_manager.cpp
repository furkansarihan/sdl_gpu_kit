#include "shadow_manager.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <imgui.h>

#include "../resource_manager/resource_manager.h"
#include "../utils/utils.h"

// --- Cascaded shadow map texture (2D array) ---
ShadowManager::ShadowManager()
{
    SDL_GPUTextureCreateInfo shadowInfo{};
    shadowInfo.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    shadowInfo.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    shadowInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    shadowInfo.width = m_shadowMapResolution;
    shadowInfo.height = m_shadowMapResolution;
    shadowInfo.layer_count_or_depth = NUM_CASCADES; // array layers = cascades
    // shadowInfo.layer_count_or_depth = 1; // array layers = cascades
    shadowInfo.num_levels = 1;
    shadowInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

    m_shadowMapTexture = SDL_CreateGPUTexture(Utils::device, &shadowInfo);
    if (!m_shadowMapTexture)
    {
        SDL_Log("Failed to create shadow map texture: %s", SDL_GetError());
    }

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
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;

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
    m_cascadeLambda = 0.8f;
}

ShadowManager::~ShadowManager()
{
    if (m_shadowPipeline)
        SDL_ReleaseGPUGraphicsPipeline(Utils::device, m_shadowPipeline);
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

    ImGui::DragFloat("Cascade Lambda", &m_cascadeLambda, 0.01f, 0.f, 1.f);
    ImGui::DragFloat("Shadow Far", &m_shadowUniforms.shadowFar, 0.2f, 0.f, 1000.f);
    // ImGui::DragFloat4("Bias", &shadowUniforms.cascadeBias.x, 0.00001f, 0.f, 1.f, "%.5f");

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

    // blend between uniform and logarithmic splits
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        float p = (i + 1) / static_cast<float>(NUM_CASCADES);
        float logSplit = minZ * std::pow(ratio, p);
        float uniSplit = minZ + range * p;
        float d = m_cascadeLambda * (logSplit - uniSplit) + uniSplit;
        cascadeSplitsScalar[i] = (d - nearClip) / clipRange;
    }

    glm::mat4 camView = view;
    glm::mat4 invView = glm::inverse(camView);

    float fovY = glm::radians(camera->fov);
    float tanHalfFovY = std::tan(fovY * 0.5f);
    float tanHalfFovX = tanHalfFovY * aspect;

    // Direction the light comes FROM (same as used in shading: L = normalize(-lightDir))
    // glm::vec3 lightDir = glm::normalize(-fragmentUniforms.lightDir);

    glm::vec4 cascadeFarPlanes(0.0f);

    for (int cascadeIndex = 0; cascadeIndex < NUM_CASCADES; ++cascadeIndex)
    {
        float prevSplitDist = (cascadeIndex == 0) ? 0.0f : cascadeSplitsScalar[cascadeIndex - 1];
        float splitDist = cascadeSplitsScalar[cascadeIndex];

        float nearDist = nearClip + prevSplitDist * clipRange;
        float farDist = nearClip + splitDist * clipRange;

        glm::vec3 frustumCornersWS[8];

        // Frustum corners in view space (right-handed, camera looks down -Z)
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

        for (int i = 0; i < 8; ++i)
        {
            glm::vec4 vWorld = invView * glm::vec4(frustumCornersVS[i], 1.0f);
            frustumCornersWS[i] = glm::vec3(vWorld);
        }

        // Center of the frustum slice
        glm::vec3 center(0.0f);
        for (int i = 0; i < 8; ++i)
            center += frustumCornersWS[i];
        center /= 8.0f;

        // Build light view matrix
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(up, lightDir)) > 0.9f)
            up = glm::vec3(0.0f, 0.0f, 1.0f);

        float dist = farDist * 2.0f;
        glm::vec3 lightPos = center - lightDir * dist;

        glm::mat4 lightView = glm::lookAt(lightPos, center, up);

        // Find bounds in light-space
        float minX = std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();
        float minZ = std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();

        for (int i = 0; i < 8; ++i)
        {
            glm::vec4 trf = lightView * glm::vec4(frustumCornersWS[i], 1.0f);
            minX = std::min(minX, trf.x);
            maxX = std::max(maxX, trf.x);
            minY = std::min(minY, trf.y);
            maxY = std::max(maxY, trf.y);
            minZ = std::min(minZ, trf.z);
            maxZ = std::max(maxZ, trf.z);
        }

        // Expand depth range a bit to accommodate moving objects & avoid clipping
        const float zMult = 10.0f;
        if (minZ < 0.0f)
            minZ *= zMult;
        else
            minZ /= zMult;
        if (maxZ < 0.0f)
            maxZ /= zMult;
        else
            maxZ *= zMult;

        glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, -minZ, -maxZ);

        static const glm::mat4 m_biasMatrix = glm::mat4(
            0.5, 0.0, 0.0, 0.0,
            0.0, -0.5, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0, // Changed from 0.5
            0.5, 0.5, 0.0, 1.0  // Changed from 0.5
        );
        m_shadowUniforms.depthBiasVP[cascadeIndex] = m_biasMatrix * lightProj * lightView;

        m_cascades[cascadeIndex].view = lightView;
        m_cascades[cascadeIndex].projection = lightProj;

        // Store view-space far distance for this cascade
        float cascadeFar = nearClip + splitDist * clipRange;
        (&cascadeFarPlanes.x)[cascadeIndex] = cascadeFar;
    }

    m_shadowUniforms.cameraView = camView;
    m_shadowUniforms.cascadeSplits = cascadeFarPlanes;
}
