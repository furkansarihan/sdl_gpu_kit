#version 450

layout(location = 0) in vec3 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform SkyboxFragUniformBlock {
    float exposure;
    float lod;
} ubo;

layout(binding = 0) uniform samplerCube environmentMap;

// ACES filmic tone mapping curve
// Source: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
vec3 aces_tonemap(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 color = textureLod(environmentMap, fragTexCoord, ubo.lod).rgb;
    color *= ubo.exposure;
    color = aces_tonemap(color);
    const float gamma = 2.2;
    color = pow(color, vec3(1.0 / gamma));
    outColor = vec4(color, 1.0);
}
