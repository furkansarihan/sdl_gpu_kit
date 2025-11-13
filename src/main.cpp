#include <cmath>
#include <cstring>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_scancode.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/norm.hpp>

#include <tiny_gltf.h>

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct VertexUniforms
{
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 normalMatrix;
};

struct FragmentUniforms
{
    glm::vec3 lightPos;
    float padding1;
    glm::vec3 viewPos;
    float padding2;
    glm::vec3 lightColor;
    float padding3;
    glm::vec3 objectColor;
    float padding4;
};

// Camera state
struct Camera
{
    glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f);
    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    float yaw = -90.0f;
    float pitch = 0.0f;
    float speed = 2.5f;
    float sensitivity = 0.1f;
    float lastX = 640.0f;
    float lastY = 360.0f;
    bool firstMouse = true;
} camera;

SDL_Window *window;
SDL_GPUDevice *device;

SDL_GPUGraphicsPipeline *graphicsPipeline;

VertexUniforms vertexUniforms{};
FragmentUniforms fragmentUniforms{};

bool keys[SDL_SCANCODE_COUNT]{};
float deltaTime = 0.0f;
Uint64 lastFrame = 0;

struct PrimitiveData
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::string name;

    SDL_GPUBuffer *vertexBuffer = NULL;
    SDL_GPUBuffer *indexBuffer = NULL;
    SDL_GPUTransferBuffer *vertexTransferBuffer = NULL;
    SDL_GPUTransferBuffer *indexTransferBuffer = NULL;
};

struct MeshData
{
    std::vector<PrimitiveData> primitives;
};

struct NodeData
{
    glm::mat4 localTransform; // Local transformation matrix
    glm::mat4 worldTransform; // World transformation matrix
    std::string name;
    int meshIndex; // Index of mesh attached to this node (-1 if none)
};

struct ModelData
{
    std::vector<NodeData> nodes;
    std::vector<MeshData> meshes;
};

std::vector<ModelData *> loadedModels;

glm::mat4 GetNodeTransform(const tinygltf::Node &node)
{
    glm::mat4 transform(1.0f);

    // If matrix is provided directly, use it
    if (node.matrix.size() == 16)
    {
        transform = glm::make_mat4(node.matrix.data());
    }
    else
    {
        // Otherwise, compose from TRS (Translation, Rotation, Scale)
        glm::vec3 translation(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);

        if (node.translation.size() == 3)
        {
            translation = glm::vec3(node.translation[0],
                                    node.translation[1],
                                    node.translation[2]);
        }

        if (node.rotation.size() == 4)
        {
            rotation = glm::quat(node.rotation[3],  // w comes last in glTF
                                 node.rotation[0],  // x
                                 node.rotation[1],  // y
                                 node.rotation[2]); // z
        }

        if (node.scale.size() == 3)
        {
            scale = glm::vec3(node.scale[0],
                              node.scale[1],
                              node.scale[2]);
        }

        // Compose: T * R * S
        transform = glm::translate(glm::mat4(1.0f), translation) *
                    glm::mat4_cast(rotation) *
                    glm::scale(glm::mat4(1.0f), scale);
    }

    return transform;
}

void ProcessNode(std::vector<NodeData> &nodes, const tinygltf::Model &model, int nodeIndex, const glm::mat4 &parentTransform)
{
    if (nodeIndex < 0)
        return;

    const auto &node = model.nodes[nodeIndex];

    // Get local transform and compute world transform
    glm::mat4 localTransform = GetNodeTransform(node);
    glm::mat4 worldTransform = parentTransform * localTransform;

    // Store transform info
    NodeData nodeData;
    nodeData.name = node.name;
    nodeData.localTransform = localTransform;
    nodeData.worldTransform = worldTransform;
    nodeData.meshIndex = node.mesh;

    nodes.push_back(nodeData);

    SDL_Log("Node '%s': mesh=%d", node.name.c_str(), node.mesh);

    // Process children recursively
    for (int childIndex : node.children)
    {
        ProcessNode(nodes, model, childIndex, worldTransform);
    }
}

bool LoadGLTFModel(const char *filename)
{
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    bool ret = loader.LoadBinaryFromFile(&model, &err, &warn, filename);
    if (!warn.empty())
        SDL_Log("GLTF Warning: %s", warn.c_str());
    if (!err.empty())
        SDL_Log("GLTF Error: %s", err.c_str());
    if (!ret)
    {
        SDL_Log("Failed to load GLTF: %s", filename);
        return false;
    }

    SDL_Log("Loaded GLTF: %s (%zu meshes)", filename, model.meshes.size());

    if (model.meshes.empty())
    {
        SDL_Log("No meshes found!");
        return false;
    }

    ModelData *modelData = new ModelData();
    loadedModels.push_back(modelData);

    if (!model.scenes.empty())
    {
        const auto &scene = model.scenes[model.defaultScene >= 0 ? model.defaultScene : 0];

        // Process root nodes
        for (int nodeIndex : scene.nodes)
        {
            ProcessNode(modelData->nodes, model, nodeIndex, glm::mat4(1.0f));
        }

        SDL_Log("Processed %zu nodes", modelData->nodes.size());
    }

    // Iterate through all meshes
    for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx)
    {
        const auto &mesh = model.meshes[meshIdx];

        MeshData meshData;

        // Process each primitive in the mesh
        for (size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx)
        {
            const auto &prim = mesh.primitives[primIdx];

            PrimitiveData primData;
            primData.name = mesh.name.empty() ? ("mesh_" + std::to_string(meshIdx) + "_prim_" + std::to_string(primIdx)) : (mesh.name + "_prim_" + std::to_string(primIdx));

            // Load positions
            const auto &posAccessor = model.accessors[prim.attributes.at("POSITION")];
            const auto &posView = model.bufferViews[posAccessor.bufferView];
            const auto &posBuffer = model.buffers[posView.buffer];
            const float *positions = reinterpret_cast<const float *>(&posBuffer.data[posView.byteOffset + posAccessor.byteOffset]);
            const int posStride = posView.byteStride ? posView.byteStride / sizeof(float) : 3;

            // Load normals if available
            bool hasNormal = prim.attributes.find("NORMAL") != prim.attributes.end();
            const float *normals = nullptr;
            int normStride = 0;
            if (hasNormal)
            {
                const auto &normAccessor = model.accessors[prim.attributes.at("NORMAL")];
                const auto &normView = model.bufferViews[normAccessor.bufferView];
                const auto &normBuffer = model.buffers[normView.buffer];
                normals = reinterpret_cast<const float *>(&normBuffer.data[normView.byteOffset + normAccessor.byteOffset]);
                normStride = normView.byteStride ? normView.byteStride / sizeof(float) : 3;
            }

            // Load UVs if available
            bool hasUV = prim.attributes.find("TEXCOORD_0") != prim.attributes.end();
            const float *uvs = nullptr;
            int uvStride = 0;
            if (hasUV)
            {
                const auto &uvAccessor = model.accessors[prim.attributes.at("TEXCOORD_0")];
                const auto &uvView = model.bufferViews[uvAccessor.bufferView];
                const auto &uvBuffer = model.buffers[uvView.buffer];
                uvs = reinterpret_cast<const float *>(&uvBuffer.data[uvView.byteOffset + uvAccessor.byteOffset]);
                uvStride = uvView.byteStride ? uvView.byteStride / sizeof(float) : 2;
            }

            // Build vertex data
            primData.vertices.resize(posAccessor.count);
            for (size_t i = 0; i < posAccessor.count; ++i)
            {
                primData.vertices[i].position.x = positions[i * posStride + 0];
                primData.vertices[i].position.y = positions[i * posStride + 1];
                primData.vertices[i].position.z = positions[i * posStride + 2];

                if (hasNormal)
                {
                    primData.vertices[i].normal.x = normals[i * normStride + 0];
                    primData.vertices[i].normal.y = normals[i * normStride + 1];
                    primData.vertices[i].normal.z = normals[i * normStride + 2];
                }
                else
                {
                    primData.vertices[i].normal.x = 0.0f;
                    primData.vertices[i].normal.y = 1.0f;
                    primData.vertices[i].normal.z = 0.0f;
                }

                if (hasUV)
                {
                    primData.vertices[i].uv.x = uvs[i * uvStride + 0];
                    primData.vertices[i].uv.y = uvs[i * uvStride + 1];
                }
                else
                {
                    primData.vertices[i].uv.x = 0.0f;
                    primData.vertices[i].uv.y = 0.0f;
                }
            }

            // Load indices
            if (prim.indices >= 0)
            {
                const auto &indexAccessor = model.accessors[prim.indices];
                const auto &indexView = model.bufferViews[indexAccessor.bufferView];
                const auto &indexBuffer = model.buffers[indexView.buffer];
                const uint8_t *indexData = &indexBuffer.data[indexView.byteOffset + indexAccessor.byteOffset];

                primData.indices.resize(indexAccessor.count);

                if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                {
                    const uint16_t *indices16 = reinterpret_cast<const uint16_t *>(indexData);
                    for (size_t i = 0; i < indexAccessor.count; ++i)
                        primData.indices[i] = indices16[i];
                }
                else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                {
                    const uint32_t *indices32 = reinterpret_cast<const uint32_t *>(indexData);
                    for (size_t i = 0; i < indexAccessor.count; ++i)
                        primData.indices[i] = indices32[i];
                }
                else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                {
                    for (size_t i = 0; i < indexAccessor.count; ++i)
                        primData.indices[i] = indexData[i];
                }
            }

            SDL_Log("Loaded mesh '%s': %zu vertices, %zu indices", primData.name.c_str(), primData.vertices.size(), primData.indices.size());

            {
                SDL_GPUBufferCreateInfo vertexBufferInfo{};
                vertexBufferInfo.size = primData.vertices.size() * sizeof(Vertex);
                vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
                primData.vertexBuffer = SDL_CreateGPUBuffer(device, &vertexBufferInfo);

                SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
                vertexTransferInfo.size = vertexBufferInfo.size;
                vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                primData.vertexTransferBuffer = SDL_CreateGPUTransferBuffer(device, &vertexTransferInfo);

                // fill the vertex transfer buffer
                Vertex *data = (Vertex *)SDL_MapGPUTransferBuffer(device, primData.vertexTransferBuffer, false);
                SDL_memcpy(data, primData.vertices.data(), vertexBufferInfo.size);
                SDL_UnmapGPUTransferBuffer(device, primData.vertexTransferBuffer);

                SDL_GPUBufferCreateInfo indexBufferInfo{};
                if (!primData.indices.empty())
                {
                    indexBufferInfo.size = primData.indices.size() * sizeof(uint32_t);
                    indexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
                    primData.indexBuffer = SDL_CreateGPUBuffer(device, &indexBufferInfo);

                    SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
                    indexTransferInfo.size = indexBufferInfo.size;
                    indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                    primData.indexTransferBuffer = SDL_CreateGPUTransferBuffer(device, &indexTransferInfo);

                    // fill the index transfer buffer
                    uint32_t *indexMap = (uint32_t *)SDL_MapGPUTransferBuffer(device, primData.indexTransferBuffer, false);
                    SDL_memcpy(indexMap, primData.indices.data(), indexBufferInfo.size);
                    SDL_UnmapGPUTransferBuffer(device, primData.indexTransferBuffer);
                }

                // start a copy pass
                SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
                SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);

                // upload vertex data
                SDL_GPUTransferBufferLocation vertexLocation = {primData.vertexTransferBuffer, 0};
                SDL_GPUBufferRegion vertexRegion = {primData.vertexBuffer, 0, vertexBufferInfo.size};
                SDL_UploadToGPUBuffer(copyPass, &vertexLocation, &vertexRegion, true);

                // upload index data
                if (!primData.indices.empty())
                {
                    SDL_GPUTransferBufferLocation indexLocation = {primData.indexTransferBuffer, 0};
                    SDL_GPUBufferRegion indexRegion = {primData.indexBuffer, 0, indexBufferInfo.size};
                    SDL_UploadToGPUBuffer(copyPass, &indexLocation, &indexRegion, true);
                }

                // end the copy pass
                SDL_EndGPUCopyPass(copyPass);
                SDL_SubmitGPUCommandBuffer(commandBuffer);
            }

            meshData.primitives.push_back(std::move(primData));
        }

        modelData->meshes.push_back(meshData);
    }

    SDL_Log("Total: %zu meshes loaded", modelData->meshes.size());

    return true;
}

void UpdateCamera(float dt)
{
    float velocity = camera.speed * dt;

    if (keys[SDL_SCANCODE_LSHIFT])
        velocity *= 0.2f;
    if (keys[SDL_SCANCODE_SPACE])
        velocity *= 5.f;

    glm::vec3 direction(0.f);
    if (keys[SDL_SCANCODE_W])
        direction += camera.front;
    if (keys[SDL_SCANCODE_S])
        direction -= camera.front;
    if (keys[SDL_SCANCODE_A])
        direction -= glm::normalize(glm::cross(camera.front, camera.up));
    if (keys[SDL_SCANCODE_D])
        direction += glm::normalize(glm::cross(camera.front, camera.up));

    if (glm::length2(direction) > 0.f)
        camera.position += glm::normalize(direction) * velocity;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
#if defined(__APPLE__)
    // macOS and iOS use Metal
    const char *deviceName = "Metal";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
    const char *vertexShaderPath = "src/shaders/phong.vert.metallib";
    const char *fragmentShaderPath = "src/shaders/phong.frag.metallib";
    const char *entryPoint = "main0";
#else
    // Windows, Linux, Android, etc. use SPIR-V
    const char *deviceName = "Vulkan";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
    const char *vertexShaderPath = "src/shaders/phong.vert.spv";
    const char *fragmentShaderPath = "src/shaders/phong.frag.spv";
    const char *entryPoint = "main";
#endif

    // create a window
    window = SDL_CreateWindow("SDL_GPU_Kit", 1280, 720, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_SetWindowRelativeMouseMode(window, true);

    // create the device
    device = SDL_CreateGPUDevice(shaderFormat, false, deviceName);
    if (!device)
    {
        SDL_Log("Failed to create device: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_ClaimWindowForGPUDevice(device, window);

    // create shaders
    size_t vertexCodeSize, fragmentCodeSize;
    void *vertexCode = SDL_LoadFile(vertexShaderPath, &vertexCodeSize);
    void *fragmentCode = SDL_LoadFile(fragmentShaderPath, &fragmentCodeSize);

    if (!vertexCode || !fragmentCode)
    {
        SDL_Log("Failed to load shaders!");
        return SDL_APP_FAILURE;
    }

    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code_size = vertexCodeSize;
    vertexInfo.code = (Uint8 *)vertexCode;
    vertexInfo.entrypoint = entryPoint;
    vertexInfo.format = shaderFormat;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_samplers = 0;
    vertexInfo.num_storage_buffers = 0;
    vertexInfo.num_storage_textures = 0;
    vertexInfo.num_uniform_buffers = 1;

    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code_size = fragmentCodeSize;
    fragmentInfo.code = (Uint8 *)fragmentCode;
    fragmentInfo.entrypoint = entryPoint;
    fragmentInfo.format = shaderFormat;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 0;
    fragmentInfo.num_storage_buffers = 0;
    fragmentInfo.num_storage_textures = 0;
    fragmentInfo.num_uniform_buffers = 1;

    SDL_GPUShader *vertexShader = SDL_CreateGPUShader(device, &vertexInfo);
    SDL_GPUShader *fragmentShader = SDL_CreateGPUShader(device, &fragmentInfo);

    SDL_free(vertexCode);
    SDL_free(fragmentCode);

    // create the graphics pipeline
    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    // describe the vertex buffers
    SDL_GPUVertexBufferDescription vertexBufferDesctiptions[1];
    vertexBufferDesctiptions[0].slot = 0;
    vertexBufferDesctiptions[0].pitch = sizeof(Vertex);
    vertexBufferDesctiptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDesctiptions;

    SDL_GPUVertexAttribute vertexAttributes[3]{};

    // inPosition
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[0].offset = 0;

    // inNormal
    vertexAttributes[1].location = 1;
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[1].offset = sizeof(float) * 3;

    // inUV
    vertexAttributes[2].location = 2;
    vertexAttributes[2].buffer_slot = 0;
    vertexAttributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[2].offset = sizeof(float) * 6;

    pipelineInfo.vertex_input_state.num_vertex_attributes = 3;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    // describe the color target
    SDL_GPUColorTargetDescription colorTargetDescriptions[1];
    colorTargetDescriptions[0] = {};
    colorTargetDescriptions[0].format = SDL_GetGPUSwapchainTextureFormat(device, window);

    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = colorTargetDescriptions;
    pipelineInfo.target_info.has_depth_stencil_target = true;
    pipelineInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    pipelineInfo.depth_stencil_state.enable_depth_test = true;
    pipelineInfo.depth_stencil_state.enable_depth_write = true;

    // create the pipeline
    graphicsPipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);

    // we don't need to store the shaders after creating the pipeline
    SDL_ReleaseGPUShader(device, vertexShader);
    SDL_ReleaseGPUShader(device, fragmentShader);

    // load model
    // const char *modelPath = "assets/models/DamagedHelmet.glb";
    const char *modelPath = "assets/models/ABeautifulGame.glb";
    // const char *modelPath = "assets/models/Fox.glb";
    // const char *modelPath = "assets/models/Sponza.glb";
    LoadGLTFModel(modelPath);

    // setup uniform values
    fragmentUniforms.lightPos = glm::vec3(5.0f, 5.0f, 5.0f);
    fragmentUniforms.lightColor = glm::vec3(1.0f, 1.0f, 1.0f);
    fragmentUniforms.objectColor = glm::vec3(0.8f, 0.8f, 0.8f);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    Uint64 currentFrame = SDL_GetTicksNS();
    deltaTime = (currentFrame - lastFrame) / 1e9f;
    lastFrame = currentFrame;

    UpdateCamera(deltaTime);

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);

    // get the swapchain texture
    SDL_GPUTexture *swapchainTexture;
    Uint32 width, height;
    SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, &width, &height);

    // end the frame early if a swapchain texture is not available
    if (swapchainTexture == NULL)
    {
        // you must always submit the command buffer
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return SDL_APP_CONTINUE;
    }

    static SDL_GPUTexture *depthTex = nullptr;
    static Uint32 lastW = 0, lastH = 0;
    if (!depthTex || lastW != width || lastH != height)
    {
        if (depthTex)
            SDL_ReleaseGPUTexture(device, depthTex);

        SDL_GPUTextureCreateInfo depthInfo{};
        depthInfo.type = SDL_GPU_TEXTURETYPE_2D;
        depthInfo.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        depthInfo.width = width;
        depthInfo.height = height;
        depthInfo.layer_count_or_depth = 1;
        depthInfo.num_levels = 1;
        depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        depthTex = SDL_CreateGPUTexture(device, &depthInfo);

        lastW = width;
        lastH = height;
    }

    // update common uniform data
    vertexUniforms.view = glm::lookAt(camera.position, camera.position + camera.front, camera.up);
    vertexUniforms.projection = glm::perspectiveRH_ZO(glm::radians(45.0f), (float)width / (float)height, 0.1f, 1000.0f);
    fragmentUniforms.viewPos = camera.position;

    // create the color target
    SDL_GPUColorTargetInfo colorTargetInfo{};
    colorTargetInfo.clear_color = {0.1f, 0.1f, 0.15f, 1.0f};
    colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
    colorTargetInfo.texture = swapchainTexture;

    // create the depth target
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    depthInfo.texture = depthTex;
    depthInfo.clear_depth = 1.0f;
    depthInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    depthInfo.store_op = SDL_GPU_STOREOP_DONT_CARE;

    // begin a render pass
    SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, &depthInfo);

    // bind the pipeline
    SDL_BindGPUGraphicsPipeline(renderPass, graphicsPipeline);

    for (const auto &model : loadedModels)
        for (const auto &node : model->nodes)
        {
            if (node.meshIndex < 0 || node.meshIndex > model->meshes.size() - 1)
                continue;

            const MeshData &mesh = model->meshes[node.meshIndex];

            for (const auto &prim : mesh.primitives)
            {
                // update per-node uniform data
                vertexUniforms.model = node.worldTransform;
                vertexUniforms.normalMatrix = glm::transpose(glm::inverse(vertexUniforms.model));

                // bind the vertex buffer
                SDL_GPUBufferBinding bufferBinding{prim.vertexBuffer, 0};
                SDL_BindGPUVertexBuffers(renderPass, 0, &bufferBinding, 1);

                // bind the index buffer
                if (!prim.indices.empty())
                {
                    SDL_GPUBufferBinding indexBinding = {prim.indexBuffer, 0};
                    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                }

                // update uniform data
                SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniforms, sizeof(vertexUniforms));
                SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragmentUniforms, sizeof(fragmentUniforms));

                // issue a draw call
                if (!prim.indices.empty())
                    SDL_DrawGPUIndexedPrimitives(renderPass, (Uint32)prim.indices.size(), 1, 0, 0, 0);
                else
                    SDL_DrawGPUPrimitives(renderPass, (Uint32)prim.vertices.size(), 0, 0, 0);
            }
        }

    // end the render pass
    SDL_EndGPURenderPass(renderPass);

    // submit the command buffer
    SDL_SubmitGPUCommandBuffer(commandBuffer);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return SDL_APP_SUCCESS;

    if (event->type == SDL_EVENT_KEY_DOWN)
        keys[event->key.scancode] = true;
    if (event->type == SDL_EVENT_KEY_UP)
        keys[event->key.scancode] = false;

    if (event->type == SDL_EVENT_MOUSE_MOTION)
    {
        float xoffset = event->motion.xrel * camera.sensitivity;
        float yoffset = -event->motion.yrel * camera.sensitivity;

        camera.yaw += xoffset;
        camera.pitch += yoffset;

        if (camera.pitch > 89.0f)
            camera.pitch = 89.0f;
        if (camera.pitch < -89.0f)
            camera.pitch = -89.0f;

        glm::vec3 direction;
        direction.x = cos(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        direction.y = sin(glm::radians(camera.pitch));
        direction.z = sin(glm::radians(camera.yaw)) * cos(glm::radians(camera.pitch));
        camera.front = glm::normalize(direction);
    }

    if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE)
    {
        bool relative = SDL_GetWindowRelativeMouseMode(window);
        SDL_SetWindowRelativeMouseMode(window, !relative);
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    for (const auto &model : loadedModels)
    {
        for (const auto &mesh : model->meshes)
        {
            for (const auto &prim : mesh.primitives)
            {
                if (prim.vertexBuffer)
                    SDL_ReleaseGPUBuffer(device, prim.vertexBuffer);
                if (prim.indexBuffer)
                    SDL_ReleaseGPUBuffer(device, prim.indexBuffer);
                if (prim.vertexTransferBuffer)
                    SDL_ReleaseGPUTransferBuffer(device, prim.vertexTransferBuffer);
                if (prim.indexTransferBuffer)
                    SDL_ReleaseGPUTransferBuffer(device, prim.indexTransferBuffer);
            }
        }

        delete model;
    }
    if (graphicsPipeline)
        SDL_ReleaseGPUGraphicsPipeline(device, graphicsPipeline);
    if (device)
        SDL_DestroyGPUDevice(device);
    if (window)
        SDL_DestroyWindow(window);
}
