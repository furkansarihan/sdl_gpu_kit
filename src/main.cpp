#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif

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

#include "stb_image.h"

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
    float exposure;
};

struct CubemapViewUBO
{
    glm::mat4 projection;
    glm::mat4 view;
    glm::mat4 model;
};

struct PrefilterUBO
{
    float roughness;
    float cubemapSize;
    float padding[2];
};

struct MaterialUniforms
{
    glm::vec4 albedoFactor;   // w component is unused
    glm::vec4 emissiveFactor; // w component is emissiveStrength
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

SDL_GPUTexture *depthTex = nullptr;

SDL_GPUGraphicsPipeline *graphicsPipeline;
SDL_GPUGraphicsPipeline *brdfPipeline;
SDL_GPUGraphicsPipeline *cubemapPipeline;
SDL_GPUGraphicsPipeline *irradiancePipeline;
SDL_GPUGraphicsPipeline *prefilterPipeline;

SDL_GPUSampler *hdrSampler;
SDL_GPUSampler *brdfSampler;

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
    int materialIndex = -1; // Index into loadedMaterials

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
    int baseMaterialIndex = 0; // Offset in the global loadedMaterials vector
    int baseTextureIndex = 0;  // Offset in the global loadedTextures vector
};

struct Texture
{
    SDL_GPUTexture *texture = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    int width, height, nrComponents;
    glm::vec2 uvScale = glm::vec2(1.f);
};

class Material
{
public:
    std::string name;

    Texture *albedoTexture = nullptr;
    Texture *normalTexture = nullptr;
    Texture *metallicRoughnessTexture = nullptr;
    Texture *occlusionTexture = nullptr;
    Texture *emissiveTexture = nullptr;

    glm::vec2 uvScale;
    glm::vec4 albedo;
    float roughness;
    float metallic;
    float opacity;
    glm::vec4 emissiveColor; // (r, g, b, strength) - strength in alpha

    Material(const std::string &name)
        : name(name),
          uvScale(glm::vec2(1.f)),
          albedo(glm::vec4(1.f)),
          metallic(0.f),
          roughness(1.f),
          opacity(1.f),
          emissiveColor(glm::vec4(0.f, 0.f, 0.f, 0.f))
    {
    }
};

// Global resource vectors
std::vector<ModelData *> loadedModels;
std::vector<Material *> loadedMaterials;
std::vector<Texture *> loadedTextures;
std::vector<SDL_GPUSampler *> loadedSamplers;

// Default resources
SDL_GPUTexture *defaultWhiteTexture = nullptr;
SDL_GPUTexture *defaultBlackTexture = nullptr;
SDL_GPUTexture *defaultNormalTexture = nullptr;
Material *defaultMaterial = nullptr;

SDL_GPUTexture *hdrTexture = nullptr;
SDL_GPUTexture *brdfTexture = nullptr;
SDL_GPUTexture *cubemapTexture = nullptr;
SDL_GPUTexture *irradianceTexture = nullptr;
SDL_GPUTexture *prefilterTexture = nullptr;

ModelData *cubeModel;

// Helper to create a default 1x1 texture
SDL_GPUTexture *CreateDefaultTexture(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_GPUTextureCreateInfo texInfo{};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width = 1;
    texInfo.height = 1;
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels = 1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texInfo);

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.size = 4;
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    SDL_GPUTransferBuffer *transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

    Uint8 *data = (Uint8 *)SDL_MapGPUTransferBuffer(device, transferBuffer, false);
    data[0] = r;
    data[1] = g;
    data[2] = b;
    data[3] = a;
    SDL_UnmapGPUTransferBuffer(device, transferBuffer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo tti = {0};
    SDL_GPUTextureRegion region = {0};
    tti.transfer_buffer = transferBuffer;
    region.texture = texture;
    region.w = 1;
    region.h = 1;
    region.d = 1;
    SDL_UploadToGPUTexture(copyPass, &tti, &region, 0);

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);

    // We can destroy the transfer buffer now
    SDL_ReleaseGPUTransferBuffer(device, transferBuffer);

    return texture;
}

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

// Helper function to get the base directory from a file path
static std::string GetBasePath(const std::string &path)
{
    size_t last_slash = path.find_last_of("/\\");
    if (last_slash != std::string::npos)
    {
        return path.substr(0, last_slash);
    }
    return "."; // Use current directory if no path found
}

ModelData *LoadGLTFModel(const char *filename)
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
        return NULL;
    }

    SDL_Log("Loaded GLTF: %s (%zu meshes, %zu materials, %zu textures)",
            filename, model.meshes.size(), model.materials.size(), model.textures.size());

    ModelData *modelData = new ModelData();

    // Store base offsets
    modelData->baseTextureIndex = (int)loadedTextures.size();
    modelData->baseMaterialIndex = (int)loadedMaterials.size();

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);

    // --- 1. Load Textures ---
    std::vector<SDL_GPUTransferBuffer *> textureTransferBuffers;
    std::string baseDir = GetBasePath(filename); // Get base dir for external files

    for (size_t i = 0; i < model.textures.size(); ++i)
    {
        const auto &gltfTex = model.textures[i];
        if (gltfTex.source < 0)
            continue;

        const auto &gltfImage = model.images[gltfTex.source];

        stbi_uc *loaded_pixels = nullptr;
        const stbi_uc *pixel_source = nullptr;
        int width = 0, height = 0, components = 0;
        bool stb_loaded = false;

        if (gltfImage.bufferView >= 0)
        {
            // Image is compressed in a buffer (e.g., PNG/JPG in a GLB)
            const auto &view = model.bufferViews[gltfImage.bufferView];
            const auto &buffer = model.buffers[view.buffer];
            const stbi_uc *dataPtr = buffer.data.data() + view.byteOffset;
            int dataLen = (int)view.byteLength;

            loaded_pixels = stbi_load_from_memory(dataPtr, dataLen, &width, &height, &components, 4); // Force 4 components
            stb_loaded = true;
        }
        else if (!gltfImage.uri.empty())
        {
            // Image is an external file
            std::string imagePath = baseDir + "/" + gltfImage.uri;
            loaded_pixels = stbi_load(imagePath.c_str(), &width, &height, &components, 4); // Force 4 components
            stb_loaded = true;
        }
        else if (!gltfImage.image.empty())
        {
            // Image is raw, uncompressed data (original code's path)
            pixel_source = gltfImage.image.data();
            width = gltfImage.width;
            height = gltfImage.height;
            components = gltfImage.component;
        }

        if (!loaded_pixels && !pixel_source)
        {
            SDL_Log("Texture %zu has no image data, skipping", i);
            loadedTextures.push_back(nullptr); // Push a null placeholder
            continue;
        }

        SDL_GPUTextureCreateInfo texInfo{};
        texInfo.type = SDL_GPU_TEXTURETYPE_2D;
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texInfo.width = width;
        texInfo.height = height;
        texInfo.layer_count_or_depth = 1;
        texInfo.num_levels = 1;

        Uint32 bufferSize = width * height * 4;

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.size = bufferSize;
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        SDL_GPUTransferBuffer *transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        textureTransferBuffers.push_back(transferBuffer); // Store for later release

        // Map buffer and convert/copy image data
        Uint8 *dst = (Uint8 *)SDL_MapGPUTransferBuffer(device, transferBuffer, false);

        if (stb_loaded)
        {
            // STB already loaded and converted to RGBA
            SDL_memcpy(dst, loaded_pixels, bufferSize);
        }
        SDL_UnmapGPUTransferBuffer(device, transferBuffer);

        if (stb_loaded)
        {
            stbi_image_free(loaded_pixels); // Don't forget to free STB's data
        }

        // Create the GPU texture
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texInfo);

        // Add to copy pass
        SDL_GPUTextureTransferInfo tti = {0};
        SDL_GPUTextureRegion region = {0};
        tti.transfer_buffer = transferBuffer;
        region.texture = texture;
        region.w = width;
        region.h = height;
        region.d = 1;
        SDL_UploadToGPUTexture(copyPass, &tti, &region, true); // Don't release yet

        // Create our wrapper Texture object
        Texture *tex = new Texture();
        tex->texture = texture;
        tex->width = width;
        tex->height = height;
        tex->nrComponents = 4; // We converted all to 4
        // tex->sampler will be set from loadedSamplers[0] when rendering

        loadedTextures.push_back(tex);
    }

    // --- 2. Load Materials ---
    for (size_t i = 0; i < model.materials.size(); ++i)
    {
        const auto &gltfMat = model.materials[i];
        Material *mat = new Material(gltfMat.name);

        const auto &pbr = gltfMat.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() == 4)
            mat->albedo = glm::make_vec4(pbr.baseColorFactor.data());
        if (pbr.baseColorTexture.index >= 0)
            mat->albedoTexture = loadedTextures[pbr.baseColorTexture.index + modelData->baseTextureIndex];

        mat->metallic = (float)pbr.metallicFactor;
        mat->roughness = (float)pbr.roughnessFactor;
        if (pbr.metallicRoughnessTexture.index >= 0)
            mat->metallicRoughnessTexture = loadedTextures[pbr.metallicRoughnessTexture.index + modelData->baseTextureIndex];

        if (gltfMat.normalTexture.index >= 0)
            mat->normalTexture = loadedTextures[gltfMat.normalTexture.index + modelData->baseTextureIndex];

        if (gltfMat.occlusionTexture.index >= 0)
            mat->occlusionTexture = loadedTextures[gltfMat.occlusionTexture.index + modelData->baseTextureIndex];

        if (gltfMat.emissiveFactor.size() == 3)
            mat->emissiveColor = glm::vec4(glm::make_vec3(gltfMat.emissiveFactor.data()), 1.0f); // Store strength in alpha

        if (gltfMat.emissiveTexture.index >= 0)
            mat->emissiveTexture = loadedTextures[gltfMat.emissiveTexture.index + modelData->baseTextureIndex];

        loadedMaterials.push_back(mat);
    }

    // --- 3. Process Nodes (Scene Hierarchy) ---
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

    // --- 4. Process Meshes and Primitives (Geometry) ---
    for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx)
    {
        const auto &mesh = model.meshes[meshIdx];
        MeshData meshData;

        // Process each primitive in the mesh
        for (size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx)
        {
            const auto &prim = mesh.primitives[primIdx];
            if (prim.attributes.find("POSITION") == prim.attributes.end())
            {
                SDL_Log("Primitive has no POSITION attribute, skipping.");
                continue;
            }

            PrimitiveData primData;
            primData.name = mesh.name.empty() ? ("mesh_" + std::to_string(meshIdx) + "_prim_" + std::to_string(primIdx)) : (mesh.name + "_prim_" + std::to_string(primIdx));

            // Link material
            primData.materialIndex = (prim.material >= 0) ? (prim.material + modelData->baseMaterialIndex) : -1;

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
                primData.vertices[i].position = glm::make_vec3(positions + i * posStride);
                primData.vertices[i].normal = hasNormal ? glm::make_vec3(normals + i * normStride) : glm::vec3(0.0f, 1.0f, 0.0f);
                primData.vertices[i].uv = hasUV ? glm::make_vec2(uvs + i * uvStride) : glm::vec2(0.0f, 0.0f);
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

            SDL_Log("Loaded mesh '%s': %zu vertices, %zu indices, material: %d",
                    primData.name.c_str(), primData.vertices.size(), primData.indices.size(), primData.materialIndex);

            // --- Create GPU Buffers (Vertices) ---
            SDL_GPUBufferCreateInfo vertexBufferInfo{};
            vertexBufferInfo.size = primData.vertices.size() * sizeof(Vertex);
            vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            primData.vertexBuffer = SDL_CreateGPUBuffer(device, &vertexBufferInfo);

            SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
            vertexTransferInfo.size = vertexBufferInfo.size;
            vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            primData.vertexTransferBuffer = SDL_CreateGPUTransferBuffer(device, &vertexTransferInfo);
            Vertex *data = (Vertex *)SDL_MapGPUTransferBuffer(device, primData.vertexTransferBuffer, false);
            SDL_memcpy(data, primData.vertices.data(), vertexBufferInfo.size);
            SDL_UnmapGPUTransferBuffer(device, primData.vertexTransferBuffer);

            SDL_GPUTransferBufferLocation vertexLocation = {primData.vertexTransferBuffer, 0};
            SDL_GPUBufferRegion vertexRegion = {primData.vertexBuffer, 0, vertexBufferInfo.size};
            SDL_UploadToGPUBuffer(copyPass, &vertexLocation, &vertexRegion, true); // Release transfer buffer

            // --- Create GPU Buffers (Indices) ---
            if (!primData.indices.empty())
            {
                SDL_GPUBufferCreateInfo indexBufferInfo{};
                indexBufferInfo.size = primData.indices.size() * sizeof(uint32_t);
                indexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
                primData.indexBuffer = SDL_CreateGPUBuffer(device, &indexBufferInfo);

                SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
                indexTransferInfo.size = indexBufferInfo.size;
                indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                primData.indexTransferBuffer = SDL_CreateGPUTransferBuffer(device, &indexTransferInfo);
                uint32_t *indexMap = (uint32_t *)SDL_MapGPUTransferBuffer(device, primData.indexTransferBuffer, false);
                SDL_memcpy(indexMap, primData.indices.data(), indexBufferInfo.size);
                SDL_UnmapGPUTransferBuffer(device, primData.indexTransferBuffer);

                SDL_GPUTransferBufferLocation indexLocation = {primData.indexTransferBuffer, 0};
                SDL_GPUBufferRegion indexRegion = {primData.indexBuffer, 0, indexBufferInfo.size};
                SDL_UploadToGPUBuffer(copyPass, &indexLocation, &indexRegion, true); // Release transfer buffer
            }

            meshData.primitives.push_back(std::move(primData));
        }

        modelData->meshes.push_back(meshData);
    }

    // --- 5. Finalize Copy Pass ---
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);

    // Release texture transfer buffers
    for (auto *transferBuffer : textureTransferBuffers)
    {
        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
    }

    SDL_Log("Total: %zu meshes, %zu materials, %zu textures loaded for this model",
            modelData->meshes.size(), model.materials.size(), model.textures.size());

    return modelData;
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

static std::string getExecutablePath()
{
    std::string path;
    size_t lastSeparator;

#if defined(_WIN32) || defined(_WIN64)
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    path = buffer;
    lastSeparator = path.find_last_of("\\");
#elif defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1)
    {
        buffer[len] = '\0';
        path = buffer;
    }
    lastSeparator = path.find_last_of("/");
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0)
    {
        path = buffer;
    }
    lastSeparator = path.find_last_of("/");
#endif

    return path.substr(0, lastSeparator);
}

SDL_GPUShader *LoadShaderFromFile(
    const char *filepath,
    Uint32 numSamplers,
    Uint32 numUniformBuffers,
    SDL_GPUShaderStage stage,
    SDL_GPUShaderFormat shaderFormat,
    SDL_GPUDevice *device,
    const char *entrypoint)
{
    std::string exePath = getExecutablePath();

    size_t codeSize;
    void *shaderCode = SDL_LoadFile(std::string(exePath + "/" + filepath).c_str(), &codeSize);
    if (!shaderCode)
    {
        SDL_Log("Failed to load shader!");
        return NULL;
    }

    SDL_GPUShaderCreateInfo shaderInfo = {};
    shaderInfo.code_size = codeSize;
    shaderInfo.code = (Uint8 *)shaderCode;
    shaderInfo.entrypoint = entrypoint;
    shaderInfo.format = shaderFormat;
    shaderInfo.stage = stage;
    shaderInfo.num_samplers = numSamplers;
    shaderInfo.num_storage_textures = 0;
    shaderInfo.num_storage_buffers = 0;
    shaderInfo.num_uniform_buffers = numUniformBuffers;

    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &shaderInfo);

    SDL_free(shaderCode);

    return shader;
}

SDL_GPUGraphicsPipeline *CreatePbrPipeline(
    SDL_GPUDevice *device,
    SDL_GPUShader *vertexShader,
    SDL_GPUShader *fragmentShader,
    SDL_GPUTextureFormat targetFormat,
    SDL_GPUSampleCount sampleCount = SDL_GPU_SAMPLECOUNT_1)
{
    SDL_GPUVertexAttribute vertexAttributes[3]{};
    vertexAttributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};                 // pos
    vertexAttributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3}; // normal
    vertexAttributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6}; // uv

    // --- Rasterizer State ---
    SDL_GPURasterizerState rasterizerState = {};
    rasterizerState.cull_mode = SDL_GPU_CULLMODE_NONE; // Render inside of cube
    rasterizerState.fill_mode = SDL_GPU_FILLMODE_FILL;
    rasterizerState.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    // --- No Depth/Stencil ---
    SDL_GPUDepthStencilState depthStencilState = {};
    depthStencilState.enable_depth_test = false;
    depthStencilState.enable_depth_write = false;

    // --- Color Target ---
    SDL_GPUColorTargetDescription colorTargetDesc = {};
    colorTargetDesc.format = targetFormat;
    colorTargetDesc.blend_state.enable_blend = false; // No blending

    SDL_GPUGraphicsPipelineTargetInfo targetInfo = {};
    targetInfo.color_target_descriptions = &colorTargetDesc;
    targetInfo.num_color_targets = 1;
    targetInfo.has_depth_stencil_target = false;

    SDL_GPUVertexBufferDescription vertexBufferDesctiptions[1];
    vertexBufferDesctiptions[0].slot = 0;
    vertexBufferDesctiptions[0].pitch = sizeof(Vertex);
    vertexBufferDesctiptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    // --- Pipeline Create Info ---
    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDesctiptions;
    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 3;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipelineInfo.rasterizer_state = rasterizerState;
    pipelineInfo.multisample_state.sample_count = sampleCount;
    pipelineInfo.depth_stencil_state = depthStencilState;
    pipelineInfo.target_info = targetInfo;

    return SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
}

/**
 * @brief Helper to load an HDR texture.
 */
SDL_GPUTexture *LoadHdrTexture(SDL_GPUDevice *device, const char *filepath)
{
    int width, height, nrComponents;
    // 1. Use stbi_loadf to load data as floats
    float *data = stbi_loadf(filepath, &width, &height, &nrComponents, 4);
    if (!data)
    {
        SDL_LogError(0, "Failed to load HDR image: %s", filepath);
        return nullptr;
    }

    SDL_GPUTextureCreateInfo texInfo = {};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;
    // 2. Use a floating-point texture format
    // R32G32B32A32_SFLOAT matches the 'float' data from stbi_loadf
    // You could also use R16G16B16A16_SFLOAT to save memory, but it requires data conversion
    texInfo.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width = width;
    texInfo.height = height;
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels = 1;

    // 3. Calculate the correct buffer size: width * height * components * type_size
    // We forced 4 components (RGBA) and the type is float (4 bytes)
    Uint32 bufferSize = width * height * 4 * sizeof(float);

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.size = bufferSize;
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    SDL_GPUTransferBuffer *transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

    Uint8 *dst = (Uint8 *)SDL_MapGPUTransferBuffer(device, transferBuffer, false);

    SDL_memcpy(dst, data, bufferSize);
    SDL_UnmapGPUTransferBuffer(device, transferBuffer);

    stbi_image_free(data);

    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texInfo);

    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTextureTransferInfo tti = {0};
    SDL_GPUTextureRegion region = {0};
    tti.transfer_buffer = transferBuffer;
    region.texture = texture;
    region.w = width;
    region.h = height;
    region.d = 1;
    SDL_UploadToGPUTexture(copyPass, &tti, &region, true); // True = release transfer buffer after copy

    SDL_EndGPUCopyPass(copyPass);

    SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    SDL_WaitForGPUFences(device, true, &initFence, 1);
    SDL_ReleaseGPUFence(device, initFence);

    return texture;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
#if defined(__APPLE__)
    // macOS and iOS use Metal
    const char *deviceName = "Metal";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
    const char *vertexShaderPath = "src/shaders/pbr.vert.metallib";
    const char *fragmentShaderPath = "src/shaders/pbr.frag.metallib";
    const char *entryPoint = "main0";
#else
    // Windows, Linux, Android, etc. use SPIR-V
    const char *deviceName = "Vulkan";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
    const char *vertexShaderPath = "src/shaders/pbr.vert.spv";
    const char *fragmentShaderPath = "src/shaders/pbr.frag.spv";
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

    std::string exePath = getExecutablePath();

    // create default resources
    defaultWhiteTexture = CreateDefaultTexture(255, 255, 255, 255);
    defaultBlackTexture = CreateDefaultTexture(0, 0, 0, 255);
    defaultNormalTexture = CreateDefaultTexture(128, 128, 255, 255); // (0.5, 0.5, 1.0) normal
    defaultMaterial = new Material("default");
    loadedMaterials.push_back(defaultMaterial); // Add to global list, at index 0

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.max_anisotropy = 1.0f;
    SDL_GPUSampler *sampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!sampler)
    {
        SDL_Log("Failed to create nearest sampler: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    loadedSamplers.push_back(sampler);

    // create shaders
    size_t vertexCodeSize, fragmentCodeSize;
    void *vertexCode = SDL_LoadFile(std::string(exePath + "/" + vertexShaderPath).c_str(), &vertexCodeSize);
    void *fragmentCode = SDL_LoadFile(std::string(exePath + "/" + fragmentShaderPath).c_str(), &fragmentCodeSize);
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
    vertexInfo.num_uniform_buffers = 1; // 1 for VertexUniforms

    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code_size = fragmentCodeSize;
    fragmentInfo.code = (Uint8 *)fragmentCode;
    fragmentInfo.entrypoint = entryPoint;
    fragmentInfo.format = shaderFormat;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 8;
    fragmentInfo.num_storage_buffers = 0;
    fragmentInfo.num_storage_textures = 0;
    fragmentInfo.num_uniform_buffers = 2; // 1 for SceneUniforms, 1 for MaterialUniforms

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
    vertexAttributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};                 // pos
    vertexAttributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3}; // normal
    vertexAttributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6}; // uv
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
    const char *modelPath = "assets/models/DamagedHelmet.glb";
    // const char *modelPath = "assets/models/ABeautifulGame.glb";
    ModelData *model = LoadGLTFModel(std::string(exePath + "/" + modelPath).c_str());
    loadedModels.push_back(model);

    SDL_GPUSamplerCreateInfo brdfSamplerInfo{};
    brdfSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    brdfSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    brdfSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;           // No mipmaps
    brdfSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; // IMPORTANT!
    brdfSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; // IMPORTANT!
    brdfSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    brdfSamplerInfo.max_anisotropy = 1.0f;
    brdfSampler = SDL_CreateGPUSampler(device, &brdfSamplerInfo);

    // setup uniform values
    fragmentUniforms.lightPos = glm::vec3(5.0f, 5.0f, 5.0f);
    fragmentUniforms.lightColor = glm::vec3(1.0f, 1.0f, 1.0f) * 10.f;
    fragmentUniforms.exposure = 1.f;

    int m_prefilterMipLevels = 5;
    int m_prefilterSize = 128;

    ModelData *quadModel = LoadGLTFModel(std::string(exePath + "/assets/models/quad.glb").c_str());
    // loadedModels.push_back(quadModel);
    cubeModel = LoadGLTFModel(std::string(exePath + "/assets/models/cube.glb").c_str());
    // loadedModels.push_back(cubeModel);

    // shaders
    SDL_GPUShader *quadVert = LoadShaderFromFile("src/shaders/quad.vert.metallib", 0, 0, SDL_GPU_SHADERSTAGE_VERTEX, shaderFormat, device, entryPoint);
    SDL_GPUShader *cubeVert = LoadShaderFromFile("src/shaders/cube.vert.metallib", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX, shaderFormat, device, entryPoint);
    SDL_GPUShader *hdrToCubeFrag = LoadShaderFromFile("src/shaders/hdr_to_cube.frag.metallib", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT, shaderFormat, device, entryPoint);
    SDL_GPUShader *irradianceFrag = LoadShaderFromFile("src/shaders/irradiance.frag.metallib", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT, shaderFormat, device, entryPoint);
    SDL_GPUShader *prefilterFrag = LoadShaderFromFile("src/shaders/prefilter.frag.metallib", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT, shaderFormat, device, entryPoint);
    SDL_GPUShader *brdfFrag = LoadShaderFromFile("src/shaders/brdf.frag.metallib", 0, 0, SDL_GPU_SHADERSTAGE_FRAGMENT, shaderFormat, device, entryPoint);

    SDL_GPUSamplerCreateInfo hdrSamplerInfo{};
    hdrSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    hdrSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    hdrSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    hdrSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    hdrSampler = SDL_CreateGPUSampler(device, &hdrSamplerInfo);

    // textures
    {
        // brdf
        SDL_GPUTextureCreateInfo brdfInfo{};
        brdfInfo.type = SDL_GPU_TEXTURETYPE_2D;
        brdfInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
        brdfInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        brdfInfo.width = 512;
        brdfInfo.height = 512;
        brdfInfo.layer_count_or_depth = 1;
        brdfInfo.num_levels = 1;
        brdfInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        brdfTexture = SDL_CreateGPUTexture(device, &brdfInfo);

        // cubemap
        SDL_GPUTextureCreateInfo cubemapInfo = {};
        cubemapInfo.type = SDL_GPU_TEXTURETYPE_CUBE;
        cubemapInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; // Good HDR format
        cubemapInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        cubemapInfo.layer_count_or_depth = 6;
        cubemapInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        cubemapInfo.width = 1024;
        cubemapInfo.height = 1024;
        cubemapInfo.num_levels = 1; // Only base level
        cubemapTexture = SDL_CreateGPUTexture(device, &cubemapInfo);

        // irradiance
        cubemapInfo.width = 64;
        cubemapInfo.height = 64;
        irradianceTexture = SDL_CreateGPUTexture(device, &cubemapInfo);

        // prefilter
        cubemapInfo.width = m_prefilterSize;
        cubemapInfo.height = m_prefilterSize;
        cubemapInfo.num_levels = m_prefilterMipLevels;
        prefilterTexture = SDL_CreateGPUTexture(device, &cubemapInfo);

        const char *hdriPath = "/assets/hdris/kloofendal_43d_clear_2k.hdr";
        // const char *hdriPath = "/assets/hdris/TCom_IcelandGolfCourse_2K_hdri_sphere.hdr";
        hdrTexture = LoadHdrTexture(device, std::string(exePath + hdriPath).c_str());
    }

    // pipelines
    {
        brdfPipeline = CreatePbrPipeline(device, quadVert, brdfFrag, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT);
        cubemapPipeline = CreatePbrPipeline(device, cubeVert, hdrToCubeFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        irradiancePipeline = CreatePbrPipeline(device, cubeVert, irradianceFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
        prefilterPipeline = CreatePbrPipeline(device, cubeVert, prefilterFrag, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    }

    {
        SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUColorTargetInfo colorTargetInfo{};
        colorTargetInfo.clear_color = {0.f, 0.f, 0.f, 1.0f};
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.texture = brdfTexture;

        SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, nullptr);
        if (!renderPass)
        {
            SDL_Log("Failed to begin render pass: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }

        SDL_BindGPUGraphicsPipeline(renderPass, brdfPipeline);

        const PrimitiveData &prim = quadModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vertexBinding{prim.vertexBuffer, 0};
        SDL_GPUBufferBinding indexBinding = {prim.indexBuffer, 0};

        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_DrawGPUIndexedPrimitives(renderPass, (Uint32)prim.indices.size(), 1, 0, 0, 0);

        SDL_EndGPURenderPass(renderPass);

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
        SDL_WaitForGPUFences(device, true, &initFence, 1);
        SDL_ReleaseGPUFence(device, initFence);
    }

    glm::mat4 m_captureViews[6] = {
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
    };
    glm::mat4 m_captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

    // cubemap
    {
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = cubemapTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPUViewport viewport = {0, 0, (float)1024, (float)1024, 0.0f, 1.0f};

        // Bind the input HDR texture
        SDL_GPUTextureSamplerBinding hdrBinding = {hdrTexture, hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = cubeModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vtxBinding = {prim.vertexBuffer, 0};
        SDL_GPUBufferBinding idxBinding = {prim.indexBuffer, 0};

        CubemapViewUBO uniforms = {};
        uniforms.projection = m_captureProjection;
        uniforms.model = glm::scale(glm::mat4(1.0), glm::vec3(1.f, -1.f, 1.f));

        for (unsigned int i = 0; i < 6; ++i)
        {
            colorTargetInfo.layer_or_depth_plane = i;
            uniforms.view = m_captureViews[i];

            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, nullptr);
            {
                SDL_BindGPUGraphicsPipeline(pass, cubemapPipeline);
                SDL_SetGPUViewport(pass, &viewport);

                SDL_PushGPUVertexUniformData(cmdbuf, 0, &uniforms, sizeof(uniforms));
                SDL_BindGPUFragmentSamplers(pass, 0, &hdrBinding, 1);

                SDL_BindGPUVertexBuffers(pass, 0, &vtxBinding, 1);
                SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                SDL_DrawGPUIndexedPrimitives(pass, prim.indices.size(), 1, 0, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
        SDL_WaitForGPUFences(device, true, &initFence, 1);
        SDL_ReleaseGPUFence(device, initFence);
    }

    // irradiance
    {
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = irradianceTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 1.0f, 0.0f, 1.0f};

        SDL_GPUViewport viewport = {0, 0, (float)64, (float)64, 0.0f, 1.0f};

        SDL_GPUTextureSamplerBinding hdrBinding = {cubemapTexture, hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = cubeModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vtxBinding = {prim.vertexBuffer, 0};
        SDL_GPUBufferBinding idxBinding = {prim.indexBuffer, 0};

        CubemapViewUBO uniforms = {};
        uniforms.projection = m_captureProjection;
        uniforms.model = glm::mat4(1.0);

        for (unsigned int i = 0; i < 6; ++i)
        {
            colorTargetInfo.layer_or_depth_plane = i;
            uniforms.view = m_captureViews[i];

            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, nullptr);
            {
                SDL_BindGPUGraphicsPipeline(pass, irradiancePipeline);
                SDL_SetGPUViewport(pass, &viewport);

                SDL_PushGPUVertexUniformData(cmdbuf, 0, &uniforms, sizeof(uniforms));
                SDL_BindGPUFragmentSamplers(pass, 0, &hdrBinding, 1);

                SDL_BindGPUVertexBuffers(pass, 0, &vtxBinding, 1);
                SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                SDL_DrawGPUIndexedPrimitives(pass, prim.indices.size(), 1, 0, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
        SDL_WaitForGPUFences(device, true, &initFence, 1);
        SDL_ReleaseGPUFence(device, initFence);
    }

    // prefilter
    {
        SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = prefilterTexture;
        colorTargetInfo.mip_level = 0;
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        colorTargetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};

        SDL_GPUTextureSamplerBinding hdrBinding = {cubemapTexture, hdrSampler};

        // Bind the cube mesh
        const PrimitiveData &prim = cubeModel->meshes[0].primitives[0];

        SDL_GPUBufferBinding vtxBinding = {prim.vertexBuffer, 0};
        SDL_GPUBufferBinding idxBinding = {prim.indexBuffer, 0};

        CubemapViewUBO uniforms = {};
        uniforms.projection = m_captureProjection;
        uniforms.model = glm::mat4(1.0);

        PrefilterUBO fragmentUniform;
        fragmentUniform.roughness = 0.5f;
        fragmentUniform.cubemapSize = 1024.f;

        // Render to each mip level
        for (unsigned int mip = 0; mip < m_prefilterMipLevels; ++mip)
        {
            unsigned int mipWidth = static_cast<unsigned int>(m_prefilterSize * std::pow(0.5, mip));
            unsigned int mipHeight = static_cast<unsigned int>(m_prefilterSize * std::pow(0.5, mip));

            SDL_GPUViewport viewport = {0, 0, (float)mipWidth, (float)mipHeight, 0.0f, 1.0f};

            fragmentUniform.roughness = (float)mip / (float)(m_prefilterMipLevels - 1);

            colorTargetInfo.mip_level = mip;

            for (unsigned int i = 0; i < 6; ++i)
            {
                colorTargetInfo.layer_or_depth_plane = i;
                uniforms.view = m_captureViews[i];

                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, nullptr);
                {
                    SDL_BindGPUGraphicsPipeline(pass, prefilterPipeline);
                    SDL_SetGPUViewport(pass, &viewport);

                    // Push uniforms to both vertex and fragment stages
                    SDL_PushGPUVertexUniformData(cmdbuf, 0, &uniforms, sizeof(uniforms));
                    SDL_PushGPUFragmentUniformData(cmdbuf, 0, &fragmentUniform, sizeof(fragmentUniform));

                    SDL_BindGPUFragmentSamplers(pass, 0, &hdrBinding, 1);

                    SDL_BindGPUVertexBuffers(pass, 0, &vtxBinding, 1);
                    SDL_BindGPUIndexBuffer(pass, &idxBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

                    SDL_DrawGPUIndexedPrimitives(pass, prim.indices.size(), 1, 0, 0, 0);
                }
                SDL_EndGPURenderPass(pass);
            }
        }

        SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
        SDL_WaitForGPUFences(device, true, &initFence, 1);
        SDL_ReleaseGPUFence(device, initFence);
    }

    SDL_ReleaseGPUShader(device, quadVert);
    SDL_ReleaseGPUShader(device, brdfFrag);

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

    // Push Scene-wide uniforms (Slot 0 for VS, Slot 0 for FS)
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniforms, sizeof(vertexUniforms));       // VS binding 0
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragmentUniforms, sizeof(fragmentUniforms)); // FS binding 0

    // --- Render Models ---
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
                SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniforms, sizeof(vertexUniforms)); // Update VS UBO

                // Get material (fallback to default)
                Material *material = (prim.materialIndex >= 0) ? loadedMaterials[prim.materialIndex] : defaultMaterial;

                // Populate material uniform struct
                MaterialUniforms materialUniforms{};
                materialUniforms.albedoFactor = material->albedo;
                materialUniforms.emissiveFactor = material->emissiveColor;
                materialUniforms.metallicFactor = material->metallic;
                materialUniforms.roughnessFactor = material->roughness;
                materialUniforms.occlusionStrength = 1.0f; // TODO: from texture
                materialUniforms.uvScale = material->uvScale;
                materialUniforms.hasAlbedoTexture = (material->albedoTexture != nullptr);
                materialUniforms.hasNormalTexture = (material->normalTexture != nullptr);
                materialUniforms.hasMetallicRoughnessTexture = (material->metallicRoughnessTexture != nullptr);
                materialUniforms.hasOcclusionTexture = (material->occlusionTexture != nullptr);
                materialUniforms.hasEmissiveTexture = (material->emissiveTexture != nullptr);

                // Push Material uniforms (Slot 1 for FS)
                SDL_PushGPUFragmentUniformData(commandBuffer, 1, &materialUniforms, sizeof(materialUniforms)); // FS binding 1

                // --- Bind Textures (Slots 0-4 for FS) ---
                SDL_GPUTextureSamplerBinding texBindings[8];

                // Binding 0: Albedo
                texBindings[0].texture = material->albedoTexture ? material->albedoTexture->texture : defaultWhiteTexture;
                texBindings[0].sampler = loadedSamplers[0];

                // Binding 1: Normal
                texBindings[1].texture = material->normalTexture ? material->normalTexture->texture : defaultNormalTexture;
                texBindings[1].sampler = loadedSamplers[0];

                // Binding 2: Metallic-Roughness
                texBindings[2].texture = material->metallicRoughnessTexture ? material->metallicRoughnessTexture->texture : defaultWhiteTexture; // Use white (1,1,1) -> (metallic=1, rough=1)
                texBindings[2].sampler = loadedSamplers[0];

                // Binding 3: Occlusion
                texBindings[3].texture = material->occlusionTexture ? material->occlusionTexture->texture : defaultWhiteTexture; // Use white (1.0) -> no occlusion
                texBindings[3].sampler = loadedSamplers[0];

                // Binding 4: Emissive
                texBindings[4].texture = material->emissiveTexture ? material->emissiveTexture->texture : defaultBlackTexture; // Use black -> no emission
                texBindings[4].sampler = loadedSamplers[0];

                // Binding 5: Irradiance
                texBindings[5].texture = irradianceTexture;
                texBindings[5].sampler = loadedSamplers[0];

                // Binding 6: Prefilter
                texBindings[6].texture = prefilterTexture;
                texBindings[6].sampler = loadedSamplers[0];

                // Binding 7: Lut
                texBindings[7].texture = brdfTexture;
                texBindings[7].sampler = brdfSampler;

                SDL_BindGPUFragmentSamplers(renderPass, 0, texBindings, 8);

                // bind vertex buffer
                SDL_GPUBufferBinding bufferBinding{prim.vertexBuffer, 0};
                SDL_BindGPUVertexBuffers(renderPass, 0, &bufferBinding, 1);

                // bind the index buffer
                if (!prim.indices.empty())
                {
                    SDL_GPUBufferBinding indexBinding = {prim.indexBuffer, 0};
                    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                }

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
            }
        }

        delete model;
    }

    for (const auto &texture : loadedTextures)
    {
        if (texture && texture->texture)
            SDL_ReleaseGPUTexture(device, texture->texture);
        delete texture;
    }

    for (const auto &material : loadedMaterials)
    {
        delete material;
    }

    for (const auto &sampler : loadedSamplers)
    {
        SDL_ReleaseGPUSampler(device, sampler);
    }

    if (defaultWhiteTexture)
        SDL_ReleaseGPUTexture(device, defaultWhiteTexture);
    if (defaultBlackTexture)
        SDL_ReleaseGPUTexture(device, defaultBlackTexture);
    if (defaultNormalTexture)
        SDL_ReleaseGPUTexture(device, defaultNormalTexture);

    if (graphicsPipeline)
        SDL_ReleaseGPUGraphicsPipeline(device, graphicsPipeline);
    if (device)
        SDL_DestroyGPUDevice(device);
    if (window)
        SDL_DestroyWindow(window);
}
