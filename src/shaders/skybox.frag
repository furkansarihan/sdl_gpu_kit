#version 450

layout(location = 0) in vec3 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform SkyboxFragmentUBO {
    float lod;
} ubo;

layout(binding = 0) uniform samplerCube environmentMap;

void main() {
    vec3 dir = normalize(fragTexCoord);
    vec3 color = textureLod(environmentMap, dir, ubo.lod).rgb;
    outColor = vec4(color, 1.0);
}
