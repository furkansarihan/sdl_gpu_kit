#version 450

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform DownsampleUBO { 
    int mipLevel;
    float highlight;
} ubo;

layout(binding = 0) uniform sampler2D uSource;

float max3(const vec3 v) {
    return max(v.x, max(v.y, v.z));
}

void threshold(inout vec3 c) {
    // threshold everything below 1.0
    c = max(vec3(0.0), c - 1.0);
    // crush everything above 1
    highp float f = max3(c);
    c *= 1.0 / (1.0 + f * (1.0 / ubo.highlight));
}

vec3 karisAverage(vec3 c0, vec3 c1, vec3 c2, vec3 c3) {
    float w0 = 1.0 / (1.0 + max3(c0));
    float w1 = 1.0 / (1.0 + max3(c1));
    float w2 = 1.0 / (1.0 + max3(c2));
    float w3 = 1.0 / (1.0 + max3(c3));
    
    return (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / (w0 + w1 + w2 + w3);
}

vec3 box4x4(vec3 s0, vec3 s1, vec3 s2, vec3 s3)
{
    return (s0 + s1 + s2 + s3) * 0.25;
}

void main()
{
    vec2 uv = vUV;

    // Center texel
    vec3 c = textureLod(uSource, uv, 0.0).rgb;

    // Offsets are expressed in texel units
    vec3 lt  = textureLodOffset(uSource, uv, 0.0, ivec2(-1, -1)).rgb;
    vec3 rt  = textureLodOffset(uSource, uv, 0.0, ivec2( 1, -1)).rgb;
    vec3 rb  = textureLodOffset(uSource, uv, 0.0, ivec2( 1,  1)).rgb;
    vec3 lb  = textureLodOffset(uSource, uv, 0.0, ivec2(-1,  1)).rgb;

    vec3 lt2 = textureLodOffset(uSource, uv, 0.0, ivec2(-2, -2)).rgb;
    vec3 rt2 = textureLodOffset(uSource, uv, 0.0, ivec2( 2, -2)).rgb;
    vec3 rb2 = textureLodOffset(uSource, uv, 0.0, ivec2( 2,  2)).rgb;
    vec3 lb2 = textureLodOffset(uSource, uv, 0.0, ivec2(-2,  2)).rgb;

    vec3 l   = textureLodOffset(uSource, uv, 0.0, ivec2(-2,  0)).rgb;
    vec3 t   = textureLodOffset(uSource, uv, 0.0, ivec2( 0, -2)).rgb;
    vec3 r   = textureLodOffset(uSource, uv, 0.0, ivec2( 2,  0)).rgb;
    vec3 b   = textureLodOffset(uSource, uv, 0.0, ivec2( 0,  2)).rgb;

    vec3 c0;
    vec3 c1;
    if (ubo.mipLevel == 0) {
        c0 = karisAverage(lt, rt, rb, lb);
        c1 = karisAverage(c, l, t, lt2);
        c1 += karisAverage(c, r, t, rt2);
        c1 += karisAverage(c, r, b, rb2);
        c1 += karisAverage(c, l, b, lb2);
    } else {
        c0 = box4x4(lt, rt, rb, lb);
        c1 = box4x4(c, l, t, lt2);
        c1 += box4x4(c, r, t, rt2);
        c1 += box4x4(c, r, b, rb2);
        c1 += box4x4(c, l, b, lb2);
    }

    // weighted average of the five boxes
    vec3 result = c0 * 0.5 + c1 * 0.125;

    if (ubo.mipLevel == 0) {
        threshold(result);
    }

    outColor = vec4(result, 1.0);
}
