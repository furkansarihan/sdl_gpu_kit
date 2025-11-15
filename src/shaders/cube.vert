#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(binding = 0) uniform CubemapViewUBO {
    mat4 projection;
    mat4 view;
    mat4 model;
} ubo;

layout(location = 0) out vec3 localPos;

void main()
{
    localPos = aPos;
    mat4 viewNoTranslation = mat4(mat3(ubo.view));
    gl_Position = ubo.projection * viewNoTranslation * ubo.model * vec4(aPos, 1.0);
    gl_Position.z = gl_Position.w;
}
