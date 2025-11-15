#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/norm.hpp>

#include <tiny_gltf.h>

#include "stb_image.h"

#include "external/imgui/imgui_impl_sdl3.h"
#include "external/imgui/imgui_impl_sdlgpu3.h"
#include "ui/root_ui.h"
#include "ui/system_monitor/system_monitor_ui.h"
#include <imgui.h>

#include "utils/common.h"

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent; // xyz = tangent direction, w = handedness (+1 or -1)
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
    glm::vec3 lightDir;
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
    float speed = 10.f;
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
SDL_GPUGraphicsPipeline *skyboxPipeline;

SDL_GPUSampler *hdrSampler;
SDL_GPUSampler *brdfSampler;

VertexUniforms vertexUniforms{};
FragmentUniforms fragmentUniforms{};

bool keys[SDL_SCANCODE_COUNT]{};
bool mouseButtons[6]{};
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
SDL_GPUSampler *baseSampler;
SDL_GPUSampler *cubeSampler;

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

RootUI *rootUI;
SystemMonitorUI *systemMonitorUI;

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

void CalculateTangents(std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices)
{
    // Reset tangents
    for (auto &v : vertices)
    {
        v.tangent = glm::vec4(0.0f);
    }

    // Calculate tangents per triangle
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        Vertex &v0 = vertices[indices[i]];
        Vertex &v1 = vertices[indices[i + 1]];
        Vertex &v2 = vertices[indices[i + 2]];

        glm::vec3 edge1 = v1.position - v0.position;
        glm::vec3 edge2 = v2.position - v0.position;
        glm::vec2 deltaUV1 = v1.uv - v0.uv;
        glm::vec2 deltaUV2 = v2.uv - v0.uv;

        float f = 1.0f / (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);

        glm::vec3 tangent;
        tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
        tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
        tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);

        // Accumulate tangents for each vertex
        v0.tangent += glm::vec4(tangent, 0.0f);
        v1.tangent += glm::vec4(tangent, 0.0f);
        v2.tangent += glm::vec4(tangent, 0.0f);
    }

    // Orthogonalize and calculate handedness
    for (auto &v : vertices)
    {
        glm::vec3 t = glm::vec3(v.tangent);
        // Gram-Schmidt orthogonalize
        t = glm::normalize(t - v.normal * glm::dot(v.normal, t));

        // Calculate handedness
        glm::vec3 c = glm::cross(v.normal, t);
        float w = (glm::dot(c, glm::vec3(v.tangent)) < 0.0f) ? -1.0f : 1.0f;

        v.tangent = glm::vec4(t, w);
    }
}

// Helper function to determine if a texture should be loaded as sRGB or linear
bool IsTextureLinear(const tinygltf::Model &model, int textureIndex)
{
    if (textureIndex < 0 || textureIndex >= model.textures.size())
        return true; // Default to linear for safety

    // Check all materials to see how this texture is used
    for (const auto &mat : model.materials)
    {
        // Normal maps, metallic-roughness, occlusion, and other data maps should be linear
        if (mat.normalTexture.index == textureIndex)
            return true; // Normal map - LINEAR

        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index == textureIndex)
            return true; // Metallic-roughness - LINEAR

        if (mat.occlusionTexture.index == textureIndex)
            return true; // Occlusion - LINEAR

        // Base color and emissive are typically sRGB
        if (mat.pbrMetallicRoughness.baseColorTexture.index == textureIndex)
            return false; // Albedo/Base color - sRGB

        if (mat.emissiveTexture.index == textureIndex)
            return false; // Emissive - sRGB
    }

    // Default to linear if usage is unknown
    return true;
}

Uint32 CalcMipLevels(int w, int h)
{
    Uint32 levels = 1;
    Uint32 size = (Uint32)SDL_max(w, h);
    while (size > 1)
    {
        size >>= 1;
        ++levels;
    }
    return levels;
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

        // Determine if this texture should be linear or sRGB
        bool isLinear = IsTextureLinear(model, i);

        SDL_GPUTextureCreateInfo texInfo{};
        texInfo.type = SDL_GPU_TEXTURETYPE_2D;
        // Use appropriate format based on texture usage
        texInfo.format = isLinear ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
        texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texInfo.width = width;
        texInfo.height = height;
        texInfo.layer_count_or_depth = 1;
        texInfo.num_levels = CalcMipLevels(width, height);

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
            SDL_memcpy(dst, loaded_pixels, bufferSize);
        }
        else if (pixel_source)
        {
            // Handle raw uncompressed data
            if (components == 4)
            {
                SDL_memcpy(dst, pixel_source, bufferSize);
            }
            else if (components == 3)
            {
                // Convert RGB to RGBA
                for (int p = 0; p < width * height; ++p)
                {
                    dst[p * 4 + 0] = pixel_source[p * 3 + 0];
                    dst[p * 4 + 1] = pixel_source[p * 3 + 1];
                    dst[p * 4 + 2] = pixel_source[p * 3 + 2];
                    dst[p * 4 + 3] = 255;
                }
            }
            else if (components == 2)
            {
                // Convert RG to RGBA (useful for normal maps in RG format)
                for (int p = 0; p < width * height; ++p)
                {
                    dst[p * 4 + 0] = pixel_source[p * 2 + 0];
                    dst[p * 4 + 1] = pixel_source[p * 2 + 1];
                    dst[p * 4 + 2] = 0;
                    dst[p * 4 + 3] = 255;
                }
            }
            else if (components == 1)
            {
                // Convert grayscale to RGBA
                for (int p = 0; p < width * height; ++p)
                {
                    dst[p * 4 + 0] = pixel_source[p];
                    dst[p * 4 + 1] = pixel_source[p];
                    dst[p * 4 + 2] = pixel_source[p];
                    dst[p * 4 + 3] = 255;
                }
            }
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
        region.mip_level = 0;
        region.w = width;
        region.h = height;
        region.d = 1;
        SDL_UploadToGPUTexture(copyPass, &tti, &region, true);

        // Create wrapper Texture object
        Texture *tex = new Texture();
        tex->texture = texture;
        tex->width = width;
        tex->height = height;
        tex->nrComponents = 4; // We converted all to 4
        // tex->sampler will be set from loadedSamplers[0] when rendering
        // tex->isLinear = isLinear;

        SDL_Log("Loaded texture %zu: %dx%d, format: %s", i, width, height, isLinear ? "LINEAR" : "sRGB");

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

            // Load tangents if available
            bool hasTangent = prim.attributes.find("TANGENT") != prim.attributes.end();
            const float *tangents = nullptr;
            int tangentStride = 0;
            if (hasTangent)
            {
                const auto &tangentAccessor = model.accessors[prim.attributes.at("TANGENT")];
                const auto &tangentView = model.bufferViews[tangentAccessor.bufferView];
                const auto &tangentBuffer = model.buffers[tangentView.buffer];
                tangents = reinterpret_cast<const float *>(&tangentBuffer.data[tangentView.byteOffset + tangentAccessor.byteOffset]);
                tangentStride = tangentView.byteStride ? tangentView.byteStride / sizeof(float) : 4; // Tangents are vec4
            }

            // Build vertex data
            primData.vertices.resize(posAccessor.count);
            for (size_t i = 0; i < posAccessor.count; ++i)
            {
                primData.vertices[i].position = glm::make_vec3(positions + i * posStride);
                primData.vertices[i].normal = hasNormal ? glm::make_vec3(normals + i * normStride) : glm::vec3(0.0f, 1.0f, 0.0f);
                primData.vertices[i].uv = hasUV ? glm::make_vec2(uvs + i * uvStride) : glm::vec2(0.0f, 0.0f);
                primData.vertices[i].tangent = hasTangent ? glm::make_vec4(tangents + i * tangentStride) : glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
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

            // Calculate tangents if not present
            if (!hasTangent && hasUV && !primData.indices.empty())
            {
                SDL_Log("Calculating tangents for mesh '%s'", primData.name.c_str());
                CalculateTangents(primData.vertices, primData.indices);
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

    // Generate mipmaps
    for (size_t i = modelData->baseTextureIndex; i < loadedTextures.size(); ++i)
    {
        if (loadedTextures[i] && loadedTextures[i]->texture)
            SDL_GenerateMipmapsForGPUTexture(cmd, loadedTextures[i]->texture);
    }

    SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(device, true, &initFence, 1);
    SDL_ReleaseGPUFence(device, initFence);

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

SDL_GPUShader *LoadShaderFromFile(
    const char *filepath,
    Uint32 numSamplers,
    Uint32 numUniformBuffers,
    SDL_GPUShaderStage stage)
{
#if defined(__APPLE__)
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
    const char *entryPoint = "main0";
    const char *extension = ".metallib";
#else
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
    const char *entryPoint = "main";
    const char *extension = ".spv";
#endif
    std::string exePath = CommonUtil::getExecutablePath();

    size_t codeSize;
    void *shaderCode = SDL_LoadFile(std::string(exePath + "/" + filepath + extension).c_str(), &codeSize);
    if (!shaderCode)
    {
        SDL_Log("Failed to load shader!");
        return NULL;
    }

    SDL_GPUShaderCreateInfo shaderInfo = {};
    shaderInfo.code_size = codeSize;
    shaderInfo.code = (Uint8 *)shaderCode;
    shaderInfo.entrypoint = entryPoint;
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
    rasterizerState.cull_mode = SDL_GPU_CULLMODE_NONE;
    rasterizerState.fill_mode = SDL_GPU_FILLMODE_FILL;
    rasterizerState.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    // --- No Depth/Stencil ---
    SDL_GPUDepthStencilState depthStencilState = {};
    depthStencilState.enable_depth_test = false;
    depthStencilState.enable_depth_write = false;

    // --- Color Target ---
    SDL_GPUColorTargetDescription colorTargetDesc = {};
    colorTargetDesc.format = targetFormat;
    colorTargetDesc.blend_state.enable_blend = false;

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

    SDL_GenerateMipmapsForGPUTexture(commandBuffer, texture);

    SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    SDL_WaitForGPUFences(device, true, &initFence, 1);
    SDL_ReleaseGPUFence(device, initFence);

    return texture;
}

SDL_GPUGraphicsPipeline *CreateSkyboxPipeline(SDL_GPUDevice *device)
{
    // Load shaders
    SDL_GPUShader *skyboxVertShader = LoadShaderFromFile("src/shaders/cube.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *skyboxFragShader = LoadShaderFromFile("src/shaders/skybox.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);

    // Vertex input - reuse your existing Vertex structure
    SDL_GPUVertexAttribute vertexAttributes[4] = {};

    // Position (location = 0)
    vertexAttributes[0].location = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[0].offset = offsetof(Vertex, position);
    vertexAttributes[0].buffer_slot = 0;

    // Normal (location = 1) - not used but must be defined
    vertexAttributes[1].location = 1;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[1].offset = offsetof(Vertex, normal);
    vertexAttributes[1].buffer_slot = 0;

    // UV (location = 2) - not used but must be defined
    vertexAttributes[2].location = 2;
    vertexAttributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[2].offset = offsetof(Vertex, uv);
    vertexAttributes[2].buffer_slot = 0;

    // Tangent (location = 3) - not used but must be defined
    vertexAttributes[3].location = 3;
    vertexAttributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    vertexAttributes[3].offset = offsetof(Vertex, tangent);
    vertexAttributes[3].buffer_slot = 0;

    SDL_GPUVertexBufferDescription vertexBufferDesc = {};
    vertexBufferDesc.slot = 0;
    vertexBufferDesc.pitch = sizeof(Vertex);
    vertexBufferDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexInputState vertexInputState = {};
    vertexInputState.vertex_buffer_descriptions = &vertexBufferDesc;
    vertexInputState.num_vertex_buffers = 1;
    vertexInputState.vertex_attributes = vertexAttributes;
    vertexInputState.num_vertex_attributes = 4;

    // Rasterizer state - disable culling or use front face culling
    SDL_GPURasterizerState rasterizerState = {};
    rasterizerState.fill_mode = SDL_GPU_FILLMODE_FILL;
    rasterizerState.cull_mode = SDL_GPU_CULLMODE_NONE;
    rasterizerState.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    // Depth stencil state
    SDL_GPUDepthStencilState depthStencilState = {};
    depthStencilState.enable_depth_test = true;
    depthStencilState.enable_depth_write = false; // Don't write to depth
    depthStencilState.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;

    // Multisample state
    SDL_GPUMultisampleState multisampleState = {};
    multisampleState.sample_count = SDL_GPU_SAMPLECOUNT_1;

    // Color target
    SDL_GPUColorTargetDescription colorTarget = {};
    colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // Or your swapchain format

    SDL_GPUGraphicsPipelineTargetInfo targetInfo = {};
    targetInfo.color_target_descriptions = &colorTarget;
    targetInfo.num_color_targets = 1;
    targetInfo.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; // Or your depth format
    targetInfo.has_depth_stencil_target = true;

    // Create pipeline
    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.vertex_shader = skyboxVertShader;
    pipelineInfo.fragment_shader = skyboxFragShader;
    pipelineInfo.vertex_input_state = vertexInputState;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipelineInfo.rasterizer_state = rasterizerState;
    pipelineInfo.depth_stencil_state = depthStencilState;
    pipelineInfo.multisample_state = multisampleState;
    pipelineInfo.target_info = targetInfo;

    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);

    // Release shaders
    SDL_ReleaseGPUShader(device, skyboxVertShader);
    SDL_ReleaseGPUShader(device, skyboxFragShader);

    return pipeline;
}

void RenderSkybox(
    SDL_GPUCommandBuffer *commandBuffer,
    SDL_GPURenderPass *renderPass,
    SDL_GPUGraphicsPipeline *skyboxPipeline,
    PrimitiveData *cubePrimitive,
    const glm::mat4 &viewMatrix,
    const glm::mat4 &projectionMatrix,
    SDL_GPUTexture *environmentCubemap,
    SDL_GPUSampler *sampler,
    float exposure = 1.0f,
    float lod = 0.0f)
{
    CubemapViewUBO vertUniforms;
    vertUniforms.model = glm::scale(glm::mat4(1.0), {1.f, -1.f, 1.f});
    vertUniforms.view = viewMatrix;
    vertUniforms.projection = projectionMatrix;

    struct SkyboxFragmentUniforms
    {
        float exposure;
        float lod;
        float padding[2];
    } fragUniforms;
    fragUniforms.exposure = exposure;
    fragUniforms.lod = lod;

    // Push uniforms
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertUniforms, sizeof(vertUniforms));
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragUniforms, sizeof(fragUniforms));

    // Bind pipeline
    SDL_BindGPUGraphicsPipeline(renderPass, skyboxPipeline);

    // Bind vertex buffer (your cube's vertex buffer)
    SDL_GPUBufferBinding vertexBinding;
    vertexBinding.buffer = cubePrimitive->vertexBuffer;
    vertexBinding.offset = 0;
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    // Bind index buffer (your cube's index buffer)
    SDL_GPUBufferBinding indexBinding;
    indexBinding.buffer = cubePrimitive->indexBuffer;
    indexBinding.offset = 0;
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // Bind cubemap texture
    SDL_GPUTextureSamplerBinding textureSamplerBinding;
    textureSamplerBinding.texture = environmentCubemap;
    textureSamplerBinding.sampler = sampler;
    SDL_BindGPUFragmentSamplers(renderPass, 0, &textureSamplerBinding, 1);

    // Draw the cube
    SDL_DrawGPUIndexedPrimitives(renderPass, cubePrimitive->indices.size(), 1, 0, 0, 0);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
#if defined(__APPLE__)
    // macOS and iOS use Metal
    const char *deviceName = "Metal";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
#else
    // Windows, Linux, Android, etc. use SPIR-V
    const char *deviceName = "Vulkan";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
#endif

    // create a window
    window = SDL_CreateWindow("SDL_GPU_Kit", 1280, 720, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // create the device
    device = SDL_CreateGPUDevice(shaderFormat, false, deviceName);
    if (!device)
    {
        SDL_Log("Failed to create device: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_ClaimWindowForGPUDevice(device, window);

    std::string exePath = CommonUtil::getExecutablePath();

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
    samplerInfo.enable_anisotropy = true;
    samplerInfo.max_anisotropy = 16.0f;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 1000.0f;
    baseSampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!baseSampler)
    {
        SDL_Log("Failed to create baseSampler: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_GPUSamplerCreateInfo cubeSamplerInfo{};
    cubeSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    cubeSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    cubeSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    cubeSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    cubeSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    cubeSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    cubeSamplerInfo.enable_anisotropy = true;
    cubeSamplerInfo.max_anisotropy = 16.0f;
    cubeSampler = SDL_CreateGPUSampler(device, &cubeSamplerInfo);
    if (!cubeSampler)
    {
        SDL_Log("Failed to create cube sampler: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // create shaders
    SDL_GPUShader *vertexShader = LoadShaderFromFile("src/shaders/pbr.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fragmentShader = LoadShaderFromFile("src/shaders/pbr.frag", 8, 2, SDL_GPU_SHADERSTAGE_FRAGMENT);

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

    SDL_GPUVertexAttribute vertexAttributes[4]{};
    vertexAttributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0};                 // pos
    vertexAttributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3}; // normal
    vertexAttributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6}; // uv
    vertexAttributes[3] = {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 8}; // uv
    pipelineInfo.vertex_input_state.num_vertex_attributes = 4;
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
    fragmentUniforms.lightDir = glm::normalize(glm::vec3(-0.3f, -0.8f, -0.3f));
    fragmentUniforms.lightColor = glm::vec3(1.0f) * 6.0f;
    fragmentUniforms.exposure = 1.0f;

    int m_prefilterMipLevels = 5;
    int m_prefilterSize = 128;

    ModelData *quadModel = LoadGLTFModel(std::string(exePath + "/assets/models/quad.glb").c_str());
    // loadedModels.push_back(quadModel);
    cubeModel = LoadGLTFModel(std::string(exePath + "/assets/models/cube.glb").c_str());
    // loadedModels.push_back(cubeModel);

    // shaders
    SDL_GPUShader *quadVert = LoadShaderFromFile("src/shaders/quad.vert", 0, 0, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *cubeVert = LoadShaderFromFile("src/shaders/cube.vert", 0, 1, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *hdrToCubeFrag = LoadShaderFromFile("src/shaders/hdr_to_cube.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *irradianceFrag = LoadShaderFromFile("src/shaders/irradiance.frag", 1, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *prefilterFrag = LoadShaderFromFile("src/shaders/prefilter.frag", 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUShader *brdfFrag = LoadShaderFromFile("src/shaders/brdf.frag", 0, 0, SDL_GPU_SHADERSTAGE_FRAGMENT);

    SDL_GPUSamplerCreateInfo hdrSamplerInfo{};
    hdrSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    hdrSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    hdrSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    hdrSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    hdrSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
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
        cubemapInfo.num_levels = 5;
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
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
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

        SDL_GenerateMipmapsForGPUTexture(cmdbuf, cubemapTexture);

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

    skyboxPipeline = CreateSkyboxPipeline(device);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    ImGui::StyleColorsDark();

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    // Setup scaling
    ImGuiStyle &style = ImGui::GetStyle();
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    style.ScaleAllSizes(main_scale); // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
    style.FontScaleDpi = main_scale; // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;                     // Only used in multi-viewports mode.
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR; // Only used in multi-viewports mode.
    init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    ImGui_ImplSDLGPU3_Init(&init_info);

    rootUI = new RootUI();
    systemMonitorUI = new SystemMonitorUI();
    rootUI->add(systemMonitorUI);

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

    glm::vec3 center{0.f, 0.f, 0.f};
    RenderSkybox(
        commandBuffer,
        renderPass,
        skyboxPipeline,
        &cubeModel->meshes[0].primitives[0],
        glm::lookAt(center, center + camera.front, camera.up),
        vertexUniforms.projection,
        cubemapTexture, // Or prefilterMap, or irradianceMap
        cubeSampler,
        1.0,
        0.0f // LOD 0 for sharp skybox, higher for blurred
    );

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
                texBindings[0].sampler = baseSampler;

                // Binding 1: Normal
                texBindings[1].texture = material->normalTexture ? material->normalTexture->texture : defaultNormalTexture;
                texBindings[1].sampler = baseSampler;

                // Binding 2: Metallic-Roughness
                texBindings[2].texture = material->metallicRoughnessTexture ? material->metallicRoughnessTexture->texture : defaultWhiteTexture; // Use white (1,1,1) -> (metallic=1, rough=1)
                texBindings[2].sampler = baseSampler;

                // Binding 3: Occlusion
                texBindings[3].texture = material->occlusionTexture ? material->occlusionTexture->texture : defaultWhiteTexture; // Use white (1.0) -> no occlusion
                texBindings[3].sampler = baseSampler;

                // Binding 4: Emissive
                texBindings[4].texture = material->emissiveTexture ? material->emissiveTexture->texture : defaultBlackTexture; // Use black -> no emission
                texBindings[4].sampler = baseSampler;

                // Binding 5: Irradiance
                texBindings[5].texture = irradianceTexture;
                texBindings[5].sampler = cubeSampler;

                // Binding 6: Prefilter
                texBindings[6].texture = prefilterTexture;
                texBindings[6].sampler = cubeSampler;

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

    // render ui
    rootUI->render(commandBuffer, swapchainTexture);

    // submit the command buffer
    SDL_SubmitGPUCommandBuffer(commandBuffer);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return SDL_APP_SUCCESS;

    if (event->type == SDL_EVENT_KEY_DOWN)
        keys[event->key.scancode] = true;
    if (event->type == SDL_EVENT_KEY_UP)
        keys[event->key.scancode] = false;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        mouseButtons[event->button.button] = true;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP)
        mouseButtons[event->button.button] = false;

    if (event->type == SDL_EVENT_MOUSE_MOTION && mouseButtons[SDL_BUTTON_RIGHT])
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

    SDL_ReleaseGPUSampler(device, baseSampler);
    SDL_ReleaseGPUSampler(device, cubeSampler);

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

    delete rootUI;
    delete systemMonitorUI;
}
