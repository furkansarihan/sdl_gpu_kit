#version 450

layout(location = 0) in vec3 inPosition;

layout(binding = 0) uniform ShadowVertexBlock {
    mat4 lightViewProj;
    mat4 model;
} ubo;

void main()
{
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    gl_Position = ubo.lightViewProj * worldPos;
}
