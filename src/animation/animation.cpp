#include "animation.h"

#include <vector>

#include <glm/gtc/type_ptr.hpp>

#include <tiny_gltf.h>

glm::mat4 GetLocalMatrix(const tinygltf::Node &node)
{
    glm::mat4 m(1.0f);

    // If node.matrix is present, it overrides TRS
    if (node.matrix.size() == 16)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
        return m;
    }

    glm::vec3 t(0.0f);
    glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 s(1.0f);

    if (node.translation.size() == 3)
        t = glm::vec3(node.translation[0], node.translation[1], node.translation[2]);
    if (node.rotation.size() == 4)
        r = glm::quat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
    if (node.scale.size() == 3)
        s = glm::vec3(node.scale[0], node.scale[1], node.scale[2]);

    m = glm::translate(glm::mat4(1.0f), t) * glm::toMat4(glm::normalize(r)) * glm::scale(glm::mat4(1.0f), s);
    return m;
}

Animation::Animation(const tinygltf::Model &model,
                     int animationIndex,
                     int skinIndex)
{
    const tinygltf::Animation &anim = model.animations[animationIndex];
    const tinygltf::Skin &skin = model.skins[skinIndex];

    m_name = anim.name;
    m_duration = 0.0f;

    // Get duration from largest sampler input time
    for (const auto &sampler : anim.samplers)
    {
        const auto &accTime = model.accessors[sampler.input];
        const float *times = Bone::GetFloatDataPtr(model, accTime);
        for (size_t i = 0; i < accTime.count; ++i)
            m_duration = std::max(m_duration, times[i]);
    }

    // 1) Build boneInfoMap from the skin
    ReadBonesFromSkin(model, skin, m_boneInfoMap);

    // 2) Build hierarchy tree from the scene’s roots
    // Choose the root: in glTF it’s usually the nodes referenced by scenes[0].nodes
    m_rootNode = new GltfNodeData();
    std::unordered_map<std::string, GltfNodeData *> nodeMap;

    // get root index
    int rootNodeIndex;
    if (skin.skeleton >= 0)
        rootNodeIndex = skin.skeleton; // Use the skeleton root from the skin
    else
        rootNodeIndex = model.scenes[model.defaultScene >= 0 ? model.defaultScene : 0].nodes[0];

    ReadHierarchyFromGltf(m_rootNode, model, rootNodeIndex, nodeMap);
    m_nodes.clear();
    for (auto &[name, ptr] : nodeMap)
        m_nodes[name] = reinterpret_cast<GltfNodeData *>(ptr);

    // 3) Build Bone objects from animation channels
    // Build node name -> index map once (outside this loop if possible)
    std::unordered_map<std::string, int> nodeNameToIndex;
    nodeNameToIndex.reserve(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i)
        nodeNameToIndex[model.nodes[i].name] = static_cast<int>(i);

    // Reserve space for bones
    m_bones.reserve(m_boneInfoMap.size());

    // Now iterate bones
    for (const auto &[boneName, boneInfo] : m_boneInfoMap)
    {
        auto it = nodeNameToIndex.find(boneName);
        if (it == nodeNameToIndex.end())
            continue;

        m_bones[boneName] = new Bone(boneName, boneInfo.id, model, anim, it->second);
    }
}

Animation::~Animation()
{
    for (auto iter = m_nodes.begin(); iter != m_nodes.end(); ++iter)
    {
        delete iter->second;
    }
    m_nodes.clear();

    for (auto iter = m_bones.begin(); iter != m_bones.end(); ++iter)
    {
        delete iter->second;
    }
    m_bones.clear();

    // TODO: destruction
}

Bone *Animation::getBone(const std::string &name)
{
    if (m_bones.find(name) != m_bones.end())
    {
        return m_bones[name];
    }

    return nullptr;
}

void Animation::ReadBonesFromSkin(const tinygltf::Model &model,
                                  const tinygltf::Skin &skin,
                                  std::unordered_map<std::string, BoneInfo> &boneInfoMap)
{
    // Read inverse bind matrices accessor (optional)
    std::vector<glm::mat4> inverseBind;
    if (skin.inverseBindMatrices >= 0)
    {
        const auto &acc = model.accessors[skin.inverseBindMatrices];
        const auto &view = model.bufferViews[acc.bufferView];
        const auto &buffer = model.buffers[view.buffer];

        const uint8_t *base = buffer.data.data() + view.byteOffset + acc.byteOffset;
        const float *ptr = reinterpret_cast<const float *>(base);

        inverseBind.resize(acc.count);
        for (size_t i = 0; i < acc.count; ++i)
        {
            inverseBind[i] = glm::make_mat4(ptr + i * 16);
        }
    }

    // Each entry in skin.joints is a node index
    for (size_t j = 0; j < skin.joints.size(); ++j)
    {
        int nodeIndex = skin.joints[j];
        const auto &node = model.nodes[nodeIndex];

        std::string boneName = node.name;
        if (boneName.empty())
            boneName = "joint_" + std::to_string(nodeIndex);

        if (boneInfoMap.find(boneName) == boneInfoMap.end())
        {
            BoneInfo info;
            info.id = static_cast<int>(j);

            if (!inverseBind.empty() && j < inverseBind.size())
                info.offset = inverseBind[j];
            else
                info.offset = glm::mat4(1.0f);

            boneInfoMap[boneName] = info;
        }
    }
}

void Animation::ReadHierarchyFromGltf(GltfNodeData *dest,
                                      const tinygltf::Model &model,
                                      int nodeIndex,
                                      std::unordered_map<std::string, GltfNodeData *> &nodeMap)
{
    const tinygltf::Node &src = model.nodes[nodeIndex];

    std::string nodeName = src.name;
    if (nodeName.empty())
        nodeName = "joint_" + std::to_string(nodeIndex); // <-- same as ReadBonesFromSkin

    dest->name = nodeName;
    dest->transformation = GetLocalMatrix(src);
    nodeMap[dest->name] = dest;

    dest->children.clear();
    dest->children.reserve(src.children.size());
    for (int childIdx : src.children)
    {
        GltfNodeData *child = new GltfNodeData();
        ReadHierarchyFromGltf(child, model, childIdx, nodeMap);
        dest->children.push_back(child);
    }
}

void Animation::setBlendMask(std::unordered_map<std::string, float> blendMask, float defaultValue)
{
    m_blendMask = blendMask;

    for (auto it = m_bones.begin(); it != m_bones.end(); ++it)
    {
        Bone *bone = it->second;
        bone->m_blendFactor = defaultValue;
    }

    for (auto it = m_blendMask.begin(); it != m_blendMask.end(); ++it)
    {
        std::string name = it->first;
        Bone *bone = getBone(name);
        if (bone)
        {
            float blendFactor = it->second;
            bone->m_blendFactor = blendFactor;
        }
    }
}
