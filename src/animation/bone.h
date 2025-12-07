#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>

namespace tinygltf
{
class Model;
class Animation;
class Accessor;
} // namespace tinygltf

struct KeyVec3
{
    float timestamp;
    glm::vec3 value;
};

struct KeyQuat
{
    float timestamp;
    glm::quat value;
};

class Bone
{
public:
    Bone(const std::string &name,
         int id,
         const tinygltf::Model &model,
         const tinygltf::Animation &animation,
         int nodeIndex);
    Bone(const std::string &name, int id, const glm::mat4 &localTransform);

    ~Bone();

    std::vector<KeyVec3> m_positions;
    std::vector<KeyQuat> m_rotations;
    std::vector<KeyVec3> m_scales;

    glm::vec3 m_translation;
    glm::quat m_rotation;
    glm::vec3 m_scale;
    std::string m_name;
    int m_ID;

    float m_blendFactor = 1.0f;

    void update(float animationTime);
    void updatePose();
    void updateCycle(float animationTime);

    int getPositionIndex(float animationTime) const;
    int getRotationIndex(float animationTime) const;
    int getScaleIndex(float animationTime) const;

    float getScaleFactor(float lastTimeStamp, float nextTimeStamp, float animationTime) const;

    glm::vec3 interpolatePosition(float animationTime) const;
    glm::quat interpolateRotation(float animationTime) const;
    glm::vec3 interpolateScaling(float animationTime) const;

    static const float *GetFloatDataPtr(const tinygltf::Model &model,
                                        const tinygltf::Accessor &acc);
};
