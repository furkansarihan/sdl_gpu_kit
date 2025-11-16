#version 450

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D uSource;

void main()
{
    ivec2 size = textureSize(uSource, 0);
    vec2  invSize = 1.0 / vec2(size);

    // Normalized UV in the *current render target* (same size as uSource in this pass)
    vec2 uv = vUV;

    const float radius = 1.0;
    vec4 d = vec4(invSize, -invSize) * radius;

    vec3 c0 = vec3(0.0);
    c0  = textureLod(uSource, uv + d.zw, 0.0).rgb;
    c0 += textureLod(uSource, uv + d.xw, 0.0).rgb;
    c0 += textureLod(uSource, uv + d.xy, 0.0).rgb;
    c0 += textureLod(uSource, uv + d.zy, 0.0).rgb;
    c0 += 4.0 * textureLod(uSource, uv, 0.0).rgb;

    vec3 c1 = vec3(0.0);
    c1  = textureLod(uSource, uv + vec2(d.z, 0.0), 0.0).rgb;
    c1 += textureLod(uSource, uv + vec2(0.0, d.w), 0.0).rgb;
    c1 += textureLod(uSource, uv + vec2(d.x, 0.0), 0.0).rgb;
    c1 += textureLod(uSource, uv + vec2(0.0, d.y), 0.0).rgb;

    vec3 result = (c0 + 2.0 * c1) * (1.0 / 16.0);

    outColor = vec4(result, 1.0);
}
