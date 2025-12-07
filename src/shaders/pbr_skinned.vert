#version 450

// Vertex attributes
layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inUV;
layout(location = 3) in vec4  inTangent; // xyz = tangent, w = handedness

layout(location = 4) in uvec4 inJoints;   // JOINTS_0
layout(location = 5) in vec4  inWeights;  // WEIGHTS_0

// Outputs
layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out vec3 fragTangent;
layout(location = 4) out vec3 fragBitangent;

// Per-draw transforms
layout(binding = 0) uniform VertexUniformBlock {
    mat4 model;
    mat4 view;
    mat4 projection;
    mat4 normalMatrix;
} ubo;

const int MAX_JOINTS = 128;
layout(binding = 1) uniform SkinningBlock {
    mat4 jointMatrices[MAX_JOINTS];
} skin;

// layout(binding = 2) uniform SkinningNormalBlock {
//     mat3 jointNormalMatrices[MAX_JOINTS];
// } skinNormal;

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

/* float getJointWeight(int joint)
{
    float w = 0.0;
    if (inJoints.x == uint(joint)) w += inWeights.x;
    if (inJoints.y == uint(joint)) w += inWeights.y;
    if (inJoints.z == uint(joint)) w += inWeights.z;
    if (inJoints.w == uint(joint)) w += inWeights.w;
    return w;
} */

void main()
{
    mat4 skinMat = getSkinMatrix();

    vec4 worldPos = skinMat * vec4(inPosition, 1.0);
    fragPos = worldPos.xyz;

    mat3 normalWorld = mat3(skinMat);
    fragNormal          = normalize(normalWorld * inNormal);
    fragTangent         = normalize(normalWorld * inTangent.xyz);
    fragBitangent       = cross(fragNormal, fragTangent) * inTangent.w;

    fragUV = inUV;

    gl_Position = ubo.projection * ubo.view * worldPos;
}
