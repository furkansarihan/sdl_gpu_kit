#pragma once

#include <string>
#include <vector>

#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include "../animation/animation.h"

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent; // xyz = tangent direction, w = handedness (+1 or -1)
    glm::uvec4 joints;
    glm::vec4 weights;
};

struct Texture
{
    SDL_GPUTexture *id = nullptr;
    int width, height, component;
    glm::vec2 uvScale = glm::vec2(1.f);
};

enum class AlphaMode
{
    Opaque,
    Mask,
    Blend
};

class Material
{
public:
    std::string name;

    Texture albedoTexture;
    Texture normalTexture;
    Texture metallicRoughnessTexture;
    Texture occlusionTexture;
    Texture emissiveTexture;
    Texture opacityTexture;

    glm::vec2 uvScale;
    glm::vec4 albedo;
    float metallic;
    float roughness;
    float opacity;
    glm::vec4 emissiveColor; // (r, g, b, strength) - strength in alpha

    AlphaMode alphaMode;
    float alphaCutoff;

    int doubleSided;
    int receiveShadow;

    Material(const std::string &name)
        : name(name),
          uvScale(glm::vec2(1.f)),
          albedo(glm::vec4(1.f)),
          metallic(0.f),
          roughness(1.f),
          opacity(1.f),
          emissiveColor(glm::vec4(0.f, 0.f, 0.f, 0.f)),
          alphaMode(AlphaMode::Opaque),
          alphaCutoff(0.f),
          doubleSided(0),
          receiveShadow(1)
    {
    }
};

struct PrimitiveData
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::string name;
    Material *material;

    SDL_GPUBuffer *vertexBuffer = NULL;
    SDL_GPUBuffer *indexBuffer = NULL;
    SDL_GPUTransferBuffer *vertexTransferBuffer = NULL;
    SDL_GPUTransferBuffer *indexTransferBuffer = NULL;

    glm::vec3 aabbMin{std::numeric_limits<float>::max()};
    glm::vec3 aabbMax{-std::numeric_limits<float>::max()};
    glm::vec3 sphereCenter{0.0f};
    float sphereRadius = 0.0f;
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
    std::vector<Material *> materials;
    std::vector<Texture> textures;
    std::vector<Animation *> animations;
};

enum class TextureDataType
{
    UnsignedByte,
    UnsignedByteSRGB,
    Float16,
    Float32
};

struct TextureParams
{
    TextureDataType dataType;
    bool generateMipmaps;
    bool sample;
    bool colorTarget;
    bool depthTarget;

    TextureParams(TextureDataType dataType = TextureDataType::UnsignedByte,
                  bool generateMipmaps = false,
                  bool sample = false,
                  bool colorTarget = false,
                  bool depthTarget = false)
        : dataType(dataType),
          generateMipmaps(generateMipmaps),
          sample(sample),
          colorTarget(colorTarget),
          depthTarget(depthTarget)
    {
    }
};

class ResourceManager
{
public:
    ResourceManager(SDL_GPUDevice *device);
    ~ResourceManager();

    SDL_GPUDevice *m_device = nullptr;

    void dispose(ModelData *model);
    void dispose(const Texture &texture);

    ModelData *loadModel(const std::string &path);
    Texture loadTextureFromMemory(const TextureParams &params, void *buffer, size_t bufferSize);
    Texture loadTextureFromFile(const TextureParams &params, const std::string &path);

private:
    bool convertAndLoadTexture(Texture &texture, const TextureParams &params,
                               void *data, int originalComponents);
    bool loadTexture(Texture &texture, const TextureParams &params,
                     void *data, Uint32 bytesPerComponent);
};
