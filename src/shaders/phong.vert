#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;

layout(binding = 0) uniform VertexUniforms {
    mat4 model;
    mat4 view;
    mat4 projection;
    mat4 normalMatrix;
} ubo;

void main() {
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    fragPos = worldPos.xyz;
    fragNormal = mat3(ubo.normalMatrix) * inNormal;
    fragTexCoord = inTexCoord;
    
    gl_Position = ubo.projection * ubo.view * worldPos;
}
