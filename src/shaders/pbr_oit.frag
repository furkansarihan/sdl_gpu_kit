#version 450

// Vertex shader inputs
layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec3 fragTangent;
layout(location = 4) in vec3 fragBitangent;

layout(location = 0) out vec4 outAccum;
layout(location = 1) out float outReveal;

// Scene-wide uniforms
layout(binding = 0) uniform FragmentUniformBlock {
    vec3 lightDir;
    float padding1;
    vec3 viewPos;
    float padding2;
    vec3 lightColor;
    float padding3;
} ubo;

// Per-Material uniforms
layout(binding = 1) uniform MaterialUniformBlock {
    vec4 albedoFactor;
    vec4 emissiveFactor; // .rgb = color, .a = strength

    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float alphaCutoff;

    int hasAlbedoTexture;
    int hasNormalTexture;
    int hasMetallicRoughnessTexture;
    int hasOcclusionTexture;

    int hasEmissiveTexture;
    int hasOpacityTexture;
    vec2 uvScale;

    int doubleSided;
    int mirrorBackFace;
    int receiveShadow;
    int padding;
} material;

const int MAX_CASCADES = 4;
layout(binding = 2) uniform ShadowUniformBlock {
    mat4 depthBiasVP[MAX_CASCADES];
    mat4 cameraView;
    vec4 cascadeSplits; // view-space far distance per cascade (x, y, z, w)
    vec4 cascadeBias;
    float shadowFar;
    vec3 padding;
} shadowUBO;

layout(binding = 3) uniform FogUniformBlock {
    vec3 fogColor;
    float fogStart;

    float fogEnd;
    float fogMaxOpacity;
    vec2 padding;
} fogUBO;

// Textures
layout(binding = 0) uniform sampler2D albedoMap;
layout(binding = 1) uniform sampler2D normalMap;
layout(binding = 2) uniform sampler2D metallicRoughnessMap;
layout(binding = 3) uniform sampler2D occlusionMap;
layout(binding = 4) uniform sampler2D emissiveMap;
layout(binding = 5) uniform sampler2D opacityMap;
layout(binding = 6) uniform samplerCube irradianceMap;
layout(binding = 7) uniform samplerCube prefilterMap;
layout(binding = 8) uniform sampler2D brdfLUT;
layout(binding = 9) uniform sampler2DArray shadowMap;

vec3 getNormalFromMap(vec2 uv, vec3 T, vec3 B, vec3 N)
{
    // Sample the normal map
    vec3 tangentNormal = texture(normalMap, uv).xyz * 2.0 - 1.0;

    // Construct the TBN matrix
    mat3 TBN = mat3(T, B, N);

    // Transform normal from tangent space to world space
    return normalize(TBN * tangentNormal);
}

#include "shadowing.fs"
#include "pbr.fs"
#include "fog.fs"

void main()
{
    // --- Material Properties ---
    vec2 uv = fragUV * material.uvScale;

    if (material.doubleSided == 1 && material.mirrorBackFace == 1 && !gl_FrontFacing)
    {
        uv.x = 1.0 - uv.x;
    }

    vec3 albedo;
    float ao, roughness, metallic;

    // Base Alpha
    float alpha = material.albedoFactor.a;

    // Albedo
    if (material.hasAlbedoTexture > 0) {
        vec4 diffuse = texture(albedoMap, uv);
        albedo = diffuse.rgb * material.albedoFactor.rgb;
        alpha *= diffuse.a;
    } else {
        albedo = material.albedoFactor.rgb;
    }

    // Opacity
    if (material.hasOpacityTexture > 0) {
        float opacity = texture(opacityMap, uv).r;
        alpha *= opacity;
    }

    // Emissive
    vec3 emissive = material.emissiveFactor.rgb * material.emissiveFactor.a;
    if (material.hasEmissiveTexture > 0) {
        emissive *= texture(emissiveMap, uv).rgb;
    }
    albedo += emissive;

    // Metallic, Roughness, AO
    if (material.hasMetallicRoughnessTexture > 0) {
        vec3 mr = texture(metallicRoughnessMap, uv).rgb;
        ao = 1.0; // AO from separate texture if available
        roughness = mr.g * material.roughnessFactor;
        metallic = mr.b * material.metallicFactor;
    } else {
        ao = 1.0;
        roughness = material.roughnessFactor;
        metallic = material.metallicFactor;
    }

    if (material.hasOcclusionTexture > 0) {
        ao = texture(occlusionMap, uv).r;
        ao = mix(1.0, ao, material.occlusionStrength);
    }

    // Normal with proper tangent space calculation
    vec3 N;
    if (material.hasNormalTexture > 0) {
        N = getNormalFromMap(uv, fragTangent, fragBitangent, fragNormal);
    } else {
        N = normalize(fragNormal);
    }

    if (material.doubleSided > 0 && !gl_FrontFacing) {
        N = -N;
    }

    N = normalize(N);

    // TODO: 
    vec3 L = normalize(-ubo.lightDir);
    float NdotL = max(dot(N, L), 0.0);

    float visibility = 1.0;
    if (material.receiveShadow > 0 && NdotL > 0.0) {
        visibility = shadow(fragPos, N, ubo.viewPos, ubo.lightDir, 2u);
    }

    // debug cascade
    /* {
        int index = getCascadeIndex(fragPos);
        if (index == 0) {
            ambient = vec3(0.0, 1.0, 0.0);
        } else if (index == 1) {
            ambient = vec3(1.0, 1.0, 0.0);
        } else if (index == 2) {
            ambient = vec3(1.0, 1.0, 1.0);
        } else if (index == 3) {
            ambient = vec3(0.0, 1.0, 1.0);
        }
    } */

    
    ShadeResult shade = shadePBR(
        fragPos,
        ubo.viewPos,
        ubo.lightDir,
        ubo.lightColor,
        albedo, metallic, roughness, N, ao);

    // --- Final Color ---
    vec3 color = shade.ambient + shade.Lo * visibility;

    // Fog
    float dist = distance(ubo.viewPos, fragPos);
    color = linearFog(color, dist);

    // Weight function by McGuire and Bavoil
    // Adjust depth range as needed (assuming 0.0 - 1.0 depth)
    float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) * 1e8 * pow(1.0 - gl_FragCoord.z * 0.9, 3.0), 1e-2, 3e3);

    // Output to Accumulation Buffer
    // RGB = premultiplied alpha * weight
    // Alpha = weight
    outAccum = vec4(color * alpha, alpha) * weight;

    // Output to Revealage Buffer
    // 'Zero' blend means we multiply destination by this value.
    // 1.0 = fully revealed (transparent), 0.0 = fully occluded (opaque)
    outReveal = alpha;
}
