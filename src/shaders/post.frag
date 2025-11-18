#version 450

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform PostProcessFragmentUBO {
    vec2 screenSize;
    float exposure;
    float gamma;
    float bloomIntensity;
} ubo;

layout(binding = 0) uniform sampler2D sceneTex;
layout(binding = 1) uniform sampler2D bloomTex;
layout(binding = 2) uniform sampler2D ssaoTex; 

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

vec3 fxaa(sampler2D tex, vec2 uv, vec2 screenSize) {
    float FXAA_SPAN_MAX = 8.0;
    float FXAA_REDUCE_MUL = 1.0 / 8.0;
    float FXAA_REDUCE_MIN = 1.0 / 128.0;

    vec3 rgbNW = texture(tex, uv + (vec2(-1.0,-1.0) / screenSize)).xyz;
    vec3 rgbNE = texture(tex, uv + (vec2(1.0,-1.0) / screenSize)).xyz;
    vec3 rgbSW = texture(tex, uv + (vec2(-1.0,1.0) / screenSize)).xyz;
    vec3 rgbSE = texture(tex, uv + (vec2(1.0,1.0) / screenSize)).xyz;
    vec3 rgbM = texture(tex, uv).xyz;

    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);

    float rcpDirMin = 1.0/(min(abs(dir.x), abs(dir.y)) + dirReduce);

    dir = min(vec2( FXAA_SPAN_MAX,  FXAA_SPAN_MAX),
          max(vec2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX),
          dir * rcpDirMin)) / screenSize;

    vec3 rgbA = (1.0 / 2.0) * (
        texture(tex, uv.xy + dir * (1.0 / 3.0 - 0.5)).xyz +
        texture(tex, uv.xy + dir * (2.0 / 3.0 - 0.5)).xyz);
    vec3 rgbB = rgbA * (1.0 / 2.0) + (1.0 / 4.0) * (
        texture(tex, uv.xy + dir * (0.0 / 3.0 - 0.5)).xyz +
        texture(tex, uv.xy + dir * (3.0 / 3.0 - 0.5)).xyz);
    float lumaB = dot(rgbB, luma);

    if ((lumaB < lumaMin) || (lumaB > lumaMax)) {
        return rgbA;
    } else {
        return rgbB;
    }
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(sceneTex, 0));

    // FXAA
    vec3 color = fxaa(sceneTex, uv, ubo.screenSize);

    // SSAO
    float aoFactor = texture(ssaoTex, uv).r;
    color *= aoFactor; 

    // Bloom
    vec3 bloomRaw = texture(bloomTex, uv).rgb;
    color += bloomRaw * ubo.bloomIntensity;

    // Exposure
    color *= ubo.exposure;

    // Tone Mapping
    color = aces_tonemap(color);

    // Gamma Correction
    color = pow(color, vec3(1.0 / ubo.gamma));
    
    outColor = vec4(color, 1.0);
}
