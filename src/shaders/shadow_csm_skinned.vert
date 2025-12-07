#version 450

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inUV;
layout(location = 3) in vec4  inTangent; // xyz = tangent, w = handedness

layout(location = 4) in uvec4 inJoints;   // JOINTS_0
layout(location = 5) in vec4  inWeights;  // WEIGHTS_0

layout(binding = 0) uniform ShadowVertexBlock {
    mat4 lightViewProj;
    mat4 model;
} ubo;

const int MAX_JOINTS = 128;
layout(binding = 1) uniform SkinningBlock {
    mat4 jointMatrices[MAX_JOINTS];
} skin;

mat4 getSkinMatrix()
{
    vec4 w = inWeights;
    
    // normalize weights just in case
    // float sum = w.x + w.y + w.z + w.w;
    // if (sum > 0.0001)
    //     w /= sum;

    mat4 m0 = skin.jointMatrices[inJoints.x];
    mat4 m1 = skin.jointMatrices[inJoints.y];
    mat4 m2 = skin.jointMatrices[inJoints.z];
    mat4 m3 = skin.jointMatrices[inJoints.w];

    return w.x * m0 + w.y * m1 + w.z * m2 + w.w * m3;
}

void main()
{
    mat4 skinMat = getSkinMatrix();
    vec4 worldPos = skinMat * vec4(inPosition, 1.0);
    gl_Position = ubo.lightViewProj * worldPos;
}
