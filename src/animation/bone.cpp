#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <tiny_gltf.h>

#include "bone.h"

Bone::Bone(const std::string &name,
           int id,
           const tinygltf::Model &model,
           const tinygltf::Animation &animation,
           int nodeIndex)
    : m_name(name),
      m_ID(id),
      m_translation(1.0f),
      m_rotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)),
      m_scale(1.0f)
{
    m_positions.clear();
    m_rotations.clear();
    m_scales.clear();

    // 1) Start from the static local transform of the node (pose)
    const tinygltf::Node &node = model.nodes[nodeIndex];

    glm::vec3 baseT(0.0f);
    glm::quat baseR(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 baseS(1.0f);

    if (node.translation.size() == 3)
        baseT = glm::vec3(node.translation[0], node.translation[1], node.translation[2]);
    if (node.rotation.size() == 4)
        baseR = glm::quat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]); // w,x,y,z
    if (node.scale.size() == 3)
        baseS = glm::vec3(node.scale[0], node.scale[1], node.scale[2]);

    // Put base pose as default single keyframe
    m_positions.push_back({0.0f, baseT});
    m_rotations.push_back({0.0f, baseR});
    m_scales.push_back({0.0f, baseS});

    // 2) Fill from animation channels that affect this node
    for (const auto &channel : animation.channels)
    {
        if (channel.target_node != nodeIndex)
            continue;

        const auto &sampler = animation.samplers[channel.sampler];

        const auto &accTime = model.accessors[sampler.input];
        const float *times = GetFloatDataPtr(model, accTime);

        const auto &accValue = model.accessors[sampler.output];
        const float *values = GetFloatDataPtr(model, accValue);

        // glTF times are in seconds
        size_t keyCount = accTime.count;

        if (channel.target_path == "translation")
        {
            m_positions.clear();
            m_positions.reserve(keyCount);
            for (size_t i = 0; i < keyCount; ++i)
            {
                glm::vec3 t(values[i * 3 + 0],
                            values[i * 3 + 1],
                            values[i * 3 + 2]);
                KeyVec3 key;
                key.timestamp = times[i];
                key.value = t;
                m_positions.push_back(key);
            }
        }
        else if (channel.target_path == "rotation")
        {
            m_rotations.clear();
            m_rotations.reserve(keyCount);
            for (size_t i = 0; i < keyCount; ++i)
            {
                glm::quat q(values[i * 4 + 3], // w, x, y, z
                            values[i * 4 + 0],
                            values[i * 4 + 1],
                            values[i * 4 + 2]);
                KeyQuat key;
                key.timestamp = times[i];
                key.value = glm::normalize(q);
                m_rotations.push_back(key);
            }
        }
        else if (channel.target_path == "scale")
        {
            m_scales.clear();
            m_scales.reserve(keyCount);
            for (size_t i = 0; i < keyCount; ++i)
            {
                glm::vec3 s(values[i * 3 + 0],
                            values[i * 3 + 1],
                            values[i * 3 + 2]);
                KeyVec3 key;
                key.timestamp = times[i];
                key.value = s;
                m_scales.push_back(key);
            }
        }
        // (ignore "weights" for now)
    }
}

Bone::Bone(const std::string &name, int id, const glm::mat4 &localTransform)
    : m_name(name),
      m_ID(id)
{
    glm::vec3 scale;
    glm::quat rotation;
    glm::vec3 translation;
    glm::vec3 skew;
    glm::vec4 perspective;

    glm::decompose(localTransform, scale, rotation, translation, skew, perspective);

    m_positions.resize(1);
    m_positions[0].value = translation;
    m_positions[0].timestamp = 0.0f;

    m_rotations.resize(1);
    m_rotations[0].value = rotation;
    m_rotations[0].timestamp = 0.0f;

    m_scales.resize(1);
    m_scales[0].value = scale;
    m_scales[0].timestamp = 0.0f;
}

Bone::~Bone()
{
}

void Bone::update(float animationTime)
{
    // Use .size() and check <= 1 to handle poses
    if (m_positions.size() <= 1 && m_rotations.size() <= 1 && m_scales.size() <= 1)
        updatePose();
    else
        updateCycle(animationTime);
}

void Bone::updatePose()
{
    // Ensure vectors are not empty before accessing
    if (!m_positions.empty())
        m_translation = m_positions[0].value;
    if (!m_rotations.empty())
        m_rotation = m_rotations[0].value;
    if (!m_scales.empty())
        m_scale = m_scales[0].value;
}

void Bone::updateCycle(float animationTime)
{
    m_translation = interpolatePosition(animationTime);
    m_rotation = interpolateRotation(animationTime);
    m_scale = interpolateScaling(animationTime);
}

int Bone::getPositionIndex(float animationTime) const
{
    // Find the first keyframe with a timestamp *greater* than animationTime
    auto it = std::upper_bound(m_positions.begin(), m_positions.end(), animationTime,
                               [](float time, const KeyVec3 &key) {
                                   return time < key.timestamp;
                               });
    // The index we want is the one *before* it
    if (it == m_positions.begin())
        return 0;

    int index = std::distance(m_positions.begin(), it) - 1;
    // Ensure index is not the last element, as we need index+1
    return (index >= m_positions.size() - 1) ? (int)m_positions.size() - 2 : index;
}

int Bone::getRotationIndex(float animationTime) const
{
    auto it = std::upper_bound(m_rotations.begin(), m_rotations.end(), animationTime,
                               [](float time, const KeyQuat &key) {
                                   return time < key.timestamp;
                               });
    if (it == m_rotations.begin())
        return 0;

    int index = std::distance(m_rotations.begin(), it) - 1;
    return (index >= m_rotations.size() - 1) ? (int)m_rotations.size() - 2 : index;
}

int Bone::getScaleIndex(float animationTime) const
{
    auto it = std::upper_bound(m_scales.begin(), m_scales.end(), animationTime,
                               [](float time, const KeyVec3 &key) {
                                   return time < key.timestamp;
                               });
    if (it == m_scales.begin())
        return 0;

    int index = std::distance(m_scales.begin(), it) - 1;
    return (index >= m_scales.size() - 1) ? (int)m_scales.size() - 2 : index;
}

float Bone::getScaleFactor(float lastTimeStamp, float nextTimeStamp, float animationTime) const
{
    float framesDiff = nextTimeStamp - lastTimeStamp;
    // Avoid divide by zero
    if (framesDiff == 0.0f)
        return 0.0f;

    float midWayLength = animationTime - lastTimeStamp;
    return midWayLength / framesDiff;
}

glm::vec3 Bone::interpolatePosition(float animationTime) const
{
    // Handle single-keyframe pose
    if (m_positions.empty())
        return glm::vec3(1.0f);
    if (m_positions.size() == 1)
        return m_positions[0].value;

    int p0Index = getPositionIndex(animationTime);
    int p1Index = p0Index + 1;
    float scaleFactor = getScaleFactor(m_positions[p0Index].timestamp, m_positions[p1Index].timestamp, animationTime);
    return glm::mix(m_positions[p0Index].value, m_positions[p1Index].value, scaleFactor);
}

glm::quat Bone::interpolateRotation(float animationTime) const
{
    if (m_rotations.empty())
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (m_rotations.size() == 1)
        return m_rotations[0].value;

    int p0Index = getRotationIndex(animationTime);
    int p1Index = p0Index + 1;
    float scaleFactor = getScaleFactor(m_rotations[p0Index].timestamp, m_rotations[p1Index].timestamp, animationTime);
    glm::quat finalRotation = glm::slerp(m_rotations[p0Index].value, m_rotations[p1Index].value, scaleFactor);
    return glm::normalize(finalRotation); // Normalizing is good practice
}

glm::vec3 Bone::interpolateScaling(float animationTime) const
{
    if (m_scales.empty())
        return glm::vec3(1.0f);
    if (m_scales.size() == 1)
        return m_scales[0].value;

    int p0Index = getScaleIndex(animationTime);
    int p1Index = p0Index + 1;
    float scaleFactor = getScaleFactor(m_scales[p0Index].timestamp, m_scales[p1Index].timestamp, animationTime);
    return glm::mix(m_scales[p0Index].value, m_scales[p1Index].value, scaleFactor);
}

const float *Bone::GetFloatDataPtr(const tinygltf::Model &model,
                                   const tinygltf::Accessor &acc)
{
    const auto &view = model.bufferViews[acc.bufferView];
    const auto &buffer = model.buffers[view.buffer];
    const uint8_t *base = buffer.data.data() + view.byteOffset + acc.byteOffset;
    return reinterpret_cast<const float *>(base);
}
