#include "resource_manager.h"

#include <cstddef>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

#include <glm/gtc/type_ptr.hpp>

#include <tiny_gltf.h>

#include "stb_image.h"

#include "../utils/utils.h"

ResourceManager::ResourceManager(SDL_GPUDevice *device)
    : m_device(device)
{
}

ResourceManager::~ResourceManager()
{
}

void ResourceManager::dispose(ModelData *model)
{
    for (auto &mesh : model->meshes)
    {
        for (auto &prim : mesh.primitives)
        {
            if (prim.vertexBuffer)
                SDL_ReleaseGPUBuffer(Utils::device, prim.vertexBuffer);
            if (prim.indexBuffer)
                SDL_ReleaseGPUBuffer(Utils::device, prim.indexBuffer);
        }
    }

    for (auto material : model->materials)
        delete material;

    for (auto texture : model->textures)
        dispose(texture);

    delete model;
}

void ResourceManager::dispose(const Texture &texture)
{
    if (texture.id)
        SDL_ReleaseGPUTexture(Utils::device, texture.id);
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

ModelData *ResourceManager::loadModel(const std::string &path)
{
    const char *filename = path.c_str();

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    bool ret = false;

    // Explicitly check the file extension
    std::string extension = Utils::getFileExtension(path);

    if (extension == "glb")
    {
        // Use LoadBinaryFromFile for .glb (binary) files
        SDL_Log("Loading GLB binary file: %s", filename);
        ret = loader.LoadBinaryFromFile(&model, &err, &warn, filename);
    }
    else if (extension == "gltf")
    {
        // Use LoadASCIIFromFile for .gltf (ASCII/JSON) files
        SDL_Log("Loading GLTF ASCII file: %s", filename);
        ret = loader.LoadASCIIFromFile(&model, &err, &warn, filename);
    }
    else
    {
        // Handle unknown or unsupported extension
        SDL_Log("Unsupported model file extension: %s", extension.c_str());
        return NULL;
    }

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

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);

    // --- 1. Load Textures ---
    std::vector<SDL_GPUTransferBuffer *> textureTransferBuffers;
    std::string baseDir = Utils::getBasePath(filename); // Get base dir for external files

    // define texture formats
    std::vector<TextureDataType> textureFormats(model.textures.size(), TextureDataType::UnsignedByte);
    for (const auto &mat : model.materials)
    {
        // Check Base Color (Albedo) - Needs sRGB
        int baseColorIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
        if (baseColorIndex >= 0 && baseColorIndex < textureFormats.size())
            textureFormats[baseColorIndex] = TextureDataType::UnsignedByteSRGB;

        // Check Emissive - Needs sRGB
        int emissiveIndex = mat.emissiveTexture.index;
        if (emissiveIndex >= 0 && emissiveIndex < textureFormats.size())
            textureFormats[emissiveIndex] = TextureDataType::UnsignedByteSRGB;
    }

    for (size_t i = 0; i < model.textures.size(); ++i)
    {
        const auto &gltfTex = model.textures[i];
        if (gltfTex.source < 0 || gltfTex.source >= model.images.size())
        {
            SDL_LogWarn(0, "Texture %zu has invalid source index", i);
            modelData->textures.push_back(Texture{}); // Push empty texture
            continue;
        }

        const auto &gltfImage = model.images[gltfTex.source];
        TextureParams params;
        params.dataType = (i < textureFormats.size()) ? textureFormats[i] : TextureDataType::UnsignedByteSRGB;
        params.generateMipmaps = true;
        params.sample = true;

        Texture texture;
        bool loaded = false;

        if (!gltfImage.image.empty())
        {
            // Image is raw uncompressed data
            texture.width = gltfImage.width;
            texture.height = gltfImage.height;
            texture.component = gltfImage.component;

            // Need to convert raw data to match expected format
            Uint32 bytesPerComponent = 1;
            if (params.dataType == TextureDataType::Float16)
                bytesPerComponent = 2;
            else if (params.dataType == TextureDataType::Float32)
                bytesPerComponent = 4;

            loaded = convertAndLoadTexture(texture, params, (void *)gltfImage.image.data(), gltfImage.component);
            SDL_Log("Texture %zu: Loaded from raw data (format: %d, size: %dx%d, components: %d)",
                    i, params.dataType, texture.width, texture.height, texture.component);
        }
        else if (gltfImage.bufferView >= 0)
        {
            // Image is compressed in a buffer (e.g., PNG/JPG in a GLB)
            const auto &view = model.bufferViews[gltfImage.bufferView];
            const auto &buffer = model.buffers[view.buffer];
            const stbi_uc *dataPtr = buffer.data.data() + view.byteOffset;
            texture = loadTextureFromMemory(params, (void *)dataPtr, view.byteLength);
            loaded = (texture.id != nullptr);
            SDL_Log("Texture %zu: Loaded from buffer view (format: %d, size: %dx%d, components: %d)",
                    i, params.dataType, texture.width, texture.height, texture.component);
        }
        else if (!gltfImage.uri.empty())
        {
            // Image is an external file
            std::string imagePath = baseDir + "/" + gltfImage.uri;
            texture = loadTextureFromFile(params, imagePath);
            loaded = (texture.id != nullptr);
            SDL_Log("Texture %zu: Loaded from URI '%s' (format: %d, size: %dx%d, components: %d)",
                    i, gltfImage.uri.c_str(), params.dataType, texture.width, texture.height, texture.component);
        }

        else
        {
            SDL_LogWarn(0, "Texture %zu has no valid image data source", i);
        }

        if (!loaded)
        {
            SDL_LogError(0, "Failed to load texture %zu", i);
        }

        modelData->textures.push_back(texture);
    }

    // --- 2. Load Materials ---
    for (size_t i = 0; i < model.materials.size(); ++i)
    {
        const auto &gltfMat = model.materials[i];
        Material *mat = new Material(gltfMat.name);

        // Load Alpha Mode
        if (gltfMat.alphaMode == "MASK")
        {
            mat->alphaMode = AlphaMode::Mask;
            mat->alphaCutoff = (float)gltfMat.alphaCutoff;
        }
        else if (gltfMat.alphaMode == "BLEND")
            mat->alphaMode = AlphaMode::Blend;
        else
            mat->alphaMode = AlphaMode::Opaque;

        mat->doubleSided = gltfMat.doubleSided;

        const auto &pbr = gltfMat.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() == 4)
            mat->albedo = glm::make_vec4(pbr.baseColorFactor.data());
        if (pbr.baseColorTexture.index >= 0)
            mat->albedoTexture = modelData->textures[pbr.baseColorTexture.index];

        mat->metallic = (float)pbr.metallicFactor;
        mat->roughness = (float)pbr.roughnessFactor;
        if (pbr.metallicRoughnessTexture.index >= 0)
            mat->metallicRoughnessTexture = modelData->textures[pbr.metallicRoughnessTexture.index];

        if (gltfMat.normalTexture.index >= 0)
            mat->normalTexture = modelData->textures[gltfMat.normalTexture.index];

        if (gltfMat.occlusionTexture.index >= 0)
            mat->occlusionTexture = modelData->textures[gltfMat.occlusionTexture.index];

        if (gltfMat.emissiveFactor.size() == 3)
            mat->emissiveColor = glm::vec4(glm::make_vec3(gltfMat.emissiveFactor.data()), 1.0f); // Store strength in alpha

        if (gltfMat.emissiveTexture.index >= 0)
            mat->emissiveTexture = modelData->textures[gltfMat.emissiveTexture.index];

        modelData->materials.push_back(mat);
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
            primData.material = (prim.material >= 0) ? (modelData->materials[prim.material]) : nullptr;

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

                const glm::vec3 &p = primData.vertices[i].position;
                primData.aabbMin = glm::min(primData.aabbMin, p);
                primData.aabbMax = glm::max(primData.aabbMax, p);
            }

            // Compute bounding sphere from AABB
            primData.sphereCenter = (primData.aabbMin + primData.aabbMax) * 0.5f;
            primData.sphereRadius = 0.0f;
            for (const auto &v : primData.vertices)
            {
                primData.sphereRadius = std::max(
                    primData.sphereRadius,
                    glm::length(v.position - primData.sphereCenter));
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

            // --- Create GPU Buffers (Vertices) ---
            SDL_GPUBufferCreateInfo vertexBufferInfo{};
            vertexBufferInfo.size = primData.vertices.size() * sizeof(Vertex);
            vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            primData.vertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexBufferInfo);

            SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
            vertexTransferInfo.size = vertexBufferInfo.size;
            vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            primData.vertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
            Vertex *data = (Vertex *)SDL_MapGPUTransferBuffer(m_device, primData.vertexTransferBuffer, false);
            SDL_memcpy(data, primData.vertices.data(), vertexBufferInfo.size);
            SDL_UnmapGPUTransferBuffer(m_device, primData.vertexTransferBuffer);

            SDL_GPUTransferBufferLocation vertexLocation = {primData.vertexTransferBuffer, 0};
            SDL_GPUBufferRegion vertexRegion = {primData.vertexBuffer, 0, vertexBufferInfo.size};
            SDL_UploadToGPUBuffer(copyPass, &vertexLocation, &vertexRegion, true); // Release transfer buffer

            // --- Create GPU Buffers (Indices) ---
            if (!primData.indices.empty())
            {
                SDL_GPUBufferCreateInfo indexBufferInfo{};
                indexBufferInfo.size = primData.indices.size() * sizeof(uint32_t);
                indexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
                primData.indexBuffer = SDL_CreateGPUBuffer(m_device, &indexBufferInfo);

                SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
                indexTransferInfo.size = indexBufferInfo.size;
                indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                primData.indexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indexTransferInfo);
                uint32_t *indexMap = (uint32_t *)SDL_MapGPUTransferBuffer(m_device, primData.indexTransferBuffer, false);
                SDL_memcpy(indexMap, primData.indices.data(), indexBufferInfo.size);
                SDL_UnmapGPUTransferBuffer(m_device, primData.indexTransferBuffer);

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
    for (size_t i = 0; i < modelData->textures.size(); ++i)
    {
        if (modelData->textures[i].id)
            SDL_GenerateMipmapsForGPUTexture(cmd, modelData->textures[i].id);
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    // SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    // SDL_WaitForGPUFences(m_device, true, &initFence, 1);
    // SDL_ReleaseGPUFence(m_device, initFence);

    // Release texture transfer buffers
    for (auto *transferBuffer : textureTransferBuffers)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
    }

    SDL_Log("Total: %zu meshes, %zu materials, %zu textures loaded for this model",
            modelData->meshes.size(), model.materials.size(), model.textures.size());

    return modelData;
}

// Helper function to convert image data to RGBA format
std::vector<uint8_t> ConvertToRGBA(const void *data, int width, int height, int components)
{
    const size_t pixelCount = width * height;
    std::vector<uint8_t> rgba(pixelCount * 4);
    const uint8_t *src = static_cast<const uint8_t *>(data);

    for (size_t i = 0; i < pixelCount; ++i)
    {
        size_t srcIdx = i * components;
        size_t dstIdx = i * 4;

        if (components >= 1)
            rgba[dstIdx + 0] = src[srcIdx + 0]; // R
        else
            rgba[dstIdx + 0] = 0;

        if (components >= 2)
            rgba[dstIdx + 1] = src[srcIdx + 1]; // G
        else
            rgba[dstIdx + 1] = rgba[dstIdx + 0];

        if (components >= 3)
            rgba[dstIdx + 2] = src[srcIdx + 2]; // B
        else
            rgba[dstIdx + 2] = rgba[dstIdx + 0];

        if (components >= 4)
            rgba[dstIdx + 3] = src[srcIdx + 3]; // A
        else
            rgba[dstIdx + 3] = 255;
    }

    return rgba;
}

Texture ResourceManager::loadTextureFromMemory(const TextureParams &params, void *buffer, size_t bufferSize)
{
    Texture texture;
    int originalComponents = 0;
    int loadedComponents = 4;
    void *data = nullptr;

    // Load image data
    if (params.dataType == TextureDataType::UnsignedByte ||
        params.dataType == TextureDataType::UnsignedByteSRGB)
    {
        data = stbi_load_from_memory(
            (const stbi_uc *)buffer, bufferSize,
            &texture.width, &texture.height, &originalComponents, loadedComponents);
    }
    else
    {
        data = stbi_loadf_from_memory(
            (const stbi_uc *)buffer, bufferSize,
            &texture.width, &texture.height, &originalComponents, loadedComponents);
    }

    if (!data)
    {
        SDL_LogError(0, "Failed to load texture from memory: %s", stbi_failure_reason());
        return texture;
    }

    texture.component = loadedComponents;

    // Determine bytes per component based on data type
    Uint32 bytesPerComponent = 1;
    if (params.dataType == TextureDataType::Float16)
        bytesPerComponent = 2;
    else if (params.dataType == TextureDataType::Float32)
        bytesPerComponent = 4;

    // load
    bool success = loadTexture(texture, params, data, bytesPerComponent);

    stbi_image_free(data);

    if (!success)
    {
        SDL_LogError(0, "Failed to convert and load texture");
        texture.id = nullptr;
    }

    return texture;
}

Texture ResourceManager::loadTextureFromFile(const TextureParams &params, const std::string &path)
{
    const char *filepath = path.c_str();
    Texture texture;
    int originalComponents = 0;
    int loadedComponents = 4;
    void *data = nullptr;

    // Load image data
    if (params.dataType == TextureDataType::UnsignedByte ||
        params.dataType == TextureDataType::UnsignedByteSRGB)
    {
        data = stbi_load(filepath, &texture.width, &texture.height, &originalComponents, loadedComponents);
    }
    else
    {
        data = stbi_loadf(filepath, &texture.width, &texture.height, &originalComponents, loadedComponents);
    }

    if (!data)
    {
        SDL_LogError(0, "Failed to load texture from file '%s': %s", filepath, stbi_failure_reason());
        return texture;
    }

    texture.component = loadedComponents;

    // Determine bytes per component based on data type
    Uint32 bytesPerComponent = 1;
    if (params.dataType == TextureDataType::Float16)
        bytesPerComponent = 2;
    else if (params.dataType == TextureDataType::Float32)
        bytesPerComponent = 4;

    // Convert and load
    bool success = loadTexture(texture, params, data, bytesPerComponent);

    stbi_image_free(data);

    if (!success)
    {
        SDL_LogError(0, "Failed to convert and load texture from file: %s", filepath);
        texture.id = nullptr;
    }

    return texture;
}

bool ResourceManager::convertAndLoadTexture(Texture &texture, const TextureParams &params,
                                            void *data, int originalComponents)
{
    // Determine bytes per component based on data type
    Uint32 bytesPerComponent = 1;
    if (params.dataType == TextureDataType::Float16)
        bytesPerComponent = 2;
    else if (params.dataType == TextureDataType::Float32)
        bytesPerComponent = 4;

    // Convert data to RGBA format if needed
    std::vector<uint8_t> convertedData;
    void *finalData = data;

    // For unsigned byte types, convert if not already RGBA
    if ((params.dataType == TextureDataType::UnsignedByte ||
         params.dataType == TextureDataType::UnsignedByteSRGB) &&
        originalComponents != 4)
    {
        convertedData = ConvertToRGBA(data, texture.width, texture.height, originalComponents);
        finalData = convertedData.data();
        SDL_Log("Converted texture from %d to 4 components", originalComponents);
    }
    // For float types, we need to handle conversion differently
    else if ((params.dataType == TextureDataType::Float16 ||
              params.dataType == TextureDataType::Float32) &&
             originalComponents != 4)
    {
        // stbi_loadf returns float data, convert to RGBA
        const size_t pixelCount = texture.width * texture.height;
        std::vector<float> rgbaFloat(pixelCount * 4);
        const float *srcFloat = static_cast<const float *>(data);

        for (size_t i = 0; i < pixelCount; ++i)
        {
            size_t srcIdx = i * originalComponents;
            size_t dstIdx = i * 4;

            rgbaFloat[dstIdx + 0] = (originalComponents >= 1) ? srcFloat[srcIdx + 0] : 0.0f;
            rgbaFloat[dstIdx + 1] = (originalComponents >= 2) ? srcFloat[srcIdx + 1] : rgbaFloat[dstIdx + 0];
            rgbaFloat[dstIdx + 2] = (originalComponents >= 3) ? srcFloat[srcIdx + 2] : rgbaFloat[dstIdx + 0];
            rgbaFloat[dstIdx + 3] = (originalComponents >= 4) ? srcFloat[srcIdx + 3] : 1.0f;
        }

        convertedData.resize(pixelCount * 4 * sizeof(float));
        std::memcpy(convertedData.data(), rgbaFloat.data(), convertedData.size());
        finalData = convertedData.data();
        SDL_Log("Converted float texture from %d to 4 components", originalComponents);
    }

    // Now load with guaranteed RGBA format
    return loadTexture(texture, params, finalData, bytesPerComponent);
}

bool ResourceManager::loadTexture(Texture &texture, const TextureParams &params,
                                  void *data, Uint32 bytesPerComponent)
{
    SDL_GPUTextureCreateInfo texInfo = {};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;

    // Select format based on data type (always RGBA now)
    if (params.dataType == TextureDataType::UnsignedByte)
    {
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }
    else if (params.dataType == TextureDataType::UnsignedByteSRGB)
    {
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    }
    else if (params.dataType == TextureDataType::Float16)
    {
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    }
    else if (params.dataType == TextureDataType::Float32)
    {
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    }
    else
    {
        SDL_LogError(0, "Unknown texture data type");
        return false;
    }

    // Set usage flags
    texInfo.usage = 0;
    if (params.sample)
        texInfo.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (params.colorTarget)
        texInfo.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if (params.depthTarget)
        texInfo.usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;

    texInfo.width = texture.width;
    texInfo.height = texture.height;
    texInfo.layer_count_or_depth = 1;

    if (params.generateMipmaps)
        texInfo.num_levels = CalcMipLevels(texture.width, texture.height);
    else
        texInfo.num_levels = 1;

    // Calculate buffer size: width * height * 4 components * bytes per component
    Uint32 bufferSize = texture.width * texture.height * 4 * bytesPerComponent;

    // Create transfer buffer
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.size = bufferSize;
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    SDL_GPUTransferBuffer *transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);

    if (!transferBuffer)
    {
        SDL_LogError(0, "Failed to create GPU transfer buffer");
        return false;
    }

    // Map and copy data
    Uint8 *dst = (Uint8 *)SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
    if (!dst)
    {
        SDL_LogError(0, "Failed to map GPU transfer buffer");
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        return false;
    }

    SDL_memcpy(dst, data, bufferSize);
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    // Create GPU texture
    SDL_GPUTexture *gpuTexture = SDL_CreateGPUTexture(m_device, &texInfo);
    if (!gpuTexture)
    {
        SDL_LogError(0, "Failed to create GPU texture");
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        return false;
    }

    texture.id = gpuTexture;

    // Upload to GPU
    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTextureTransferInfo tti = {0};
    SDL_GPUTextureRegion region = {0};
    tti.transfer_buffer = transferBuffer;
    tti.offset = 0;
    region.texture = gpuTexture;
    region.mip_level = 0;
    region.layer = 0;
    region.x = 0;
    region.y = 0;
    region.z = 0;
    region.w = texture.width;
    region.h = texture.height;
    region.d = 1;

    SDL_UploadToGPUTexture(copyPass, &tti, &region, false);
    SDL_EndGPUCopyPass(copyPass);

    // Generate mipmaps if requested
    if (params.generateMipmaps)
        SDL_GenerateMipmapsForGPUTexture(commandBuffer, gpuTexture);

    // Submit and wait
    SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    SDL_WaitForGPUFences(m_device, true, &initFence, 1);
    SDL_ReleaseGPUFence(m_device, initFence);

    SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);

    // Set final component count to 4 since we always use RGBA
    texture.component = 4;

    return true;
}
