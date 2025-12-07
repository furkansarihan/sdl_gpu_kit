#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "bone.h"

namespace tinygltf
{
class Skin;
} // namespace tinygltf

struct GltfNodeData
{
    std::string name;
    glm::mat4 transformation;
    std::vector<GltfNodeData *> children;
};

struct BoneInfo
{
    int id;
    glm::mat4 offset;
};

class Animation
{
public:
    Animation(const tinygltf::Model &model,
              int animationIndex,
              int skinIndex);
    ~Animation();

    std::string m_name;
    float m_duration;

    GltfNodeData *m_rootNode;
    std::map<std::string, GltfNodeData *> m_nodes;

    std::unordered_map<std::string, Bone *> m_bones;
    std::unordered_map<std::string, BoneInfo> m_boneInfoMap;
    std::unordered_map<std::string, float> m_blendMask;

    Bone *getBone(const std::string &name);

    void ReadBonesFromSkin(
        const tinygltf::Model &model,
        const tinygltf::Skin &skin,
        std::unordered_map<std::string, BoneInfo> &boneInfoMap);
    void ReadHierarchyFromGltf(
        GltfNodeData *dest,
        const tinygltf::Model &model,
        int nodeIndex,
        std::unordered_map<std::string, GltfNodeData *> &nodeMap);

    void setBlendMask(std::unordered_map<std::string, float> blendMask, float defaultValue);
};
