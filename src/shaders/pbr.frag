#version 450

// Vertex shader inputs
layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec3 fragTangent;
layout(location = 4) in vec3 fragBitangent;

// Fragment shader output
layout(location = 0) out vec4 outColor;

// Scene-wide uniforms
layout(binding = 0) uniform FragmentUniformBlock {
    vec3 lightDir;
    vec3 viewPos;
    vec3 lightColor;
    float exposure;
} ubo;

// Per-Material uniforms
layout(binding = 1) uniform MaterialUniformBlock {
    vec4 albedoFactor;
    vec4 emissiveFactor; // .rgb = color, .a = strength
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    int hasAlbedoTexture;
    int hasNormalTexture;
    int hasMetallicRoughnessTexture;
    int hasOcclusionTexture;
    int hasEmissiveTexture;
    vec2 uvScale;
} material;

// Textures
layout(binding = 0) uniform sampler2D albedoMap;
layout(binding = 1) uniform sampler2D normalMap;
layout(binding = 2) uniform sampler2D metallicRoughnessMap;
layout(binding = 3) uniform sampler2D occlusionMap;
layout(binding = 4) uniform sampler2D emissiveMap;
layout(binding = 5) uniform samplerCube irradianceMap;
layout(binding = 6) uniform samplerCube prefilterMap;
layout(binding = 7) uniform sampler2D brdfLUT;

const float PI = 3.14159265359;
const float MAX_REFLECTION_LOD = 4.0;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 getNormalFromMap(vec2 uv, vec3 T, vec3 B, vec3 N)
{
    // Sample the normal map
    vec3 tangentNormal = texture(normalMap, uv).xyz * 2.0 - 1.0;
    
    // Construct the TBN matrix
    mat3 TBN = mat3(T, B, N);
    
    // Transform normal from tangent space to world space
    return normalize(TBN * tangentNormal);
}

void main()
{
    // --- Material Properties ---
    vec2 uv = fragUV * material.uvScale;
    
    vec3 albedo;
    float ao, roughness, metallic;

    // Albedo
    if (material.hasAlbedoTexture > 0) {
        vec4 diffuse = texture(albedoMap, uv);
        albedo = diffuse.rgb;
        albedo *= material.albedoFactor.rgb;
    } else {
        albedo = material.albedoFactor.rgb;
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
        // Use TBN matrix for proper normal mapping
        N = getNormalFromMap(uv, fragTangent, fragBitangent, fragNormal);
    } else {
        N = normalize(fragNormal);
    }

    vec3 V = normalize(ubo.viewPos - fragPos);

    // Calculate reflectance at normal incidence
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // --- Direct Lighting ---
    vec3 Lo = vec3(0.0);
    vec3 L = normalize(-ubo.lightDir);
    float NdotL = max(dot(N, L), 0.0);

    if (NdotL > 0.0)
    {
        vec3 H = normalize(V + L);
        vec3 radiance = ubo.lightColor;

        // Cook-Torrance BRDF
        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0);

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }

    // --- Image-Based Lighting (IBL) ---

    // Diffuse IBL
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuse = irradiance * albedo;

    // Specular IBL
    vec3 R = reflect(-V, N);
    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);

    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBRDF = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

    vec3 ambient = (kD * diffuse + specular) * ao;

    // --- Final Color ---
    vec3 color = ambient + Lo;

    outColor = vec4(color, 1.0);
    // outColor = vec4(emissive, 1.0);
    // outColor = vec4(albedo, 1.0);
    // outColor = vec4(N, 1.0);
    // outColor = vec4(fragTangent, 1.0);
    // outColor = vec4(vec3(envBRDF.x), 1.0);
    // outColor = vec4(vec3(envBRDF.y), 1.0);
    // outColor = vec4(vec3(roughness), 1.0);
    // outColor = vec4(vec3(metallic), 1.0);

    // outColor = vec4(texture(albedoMap, uv).rgb, 1.0);
    // outColor = vec4(texture(emissiveMap, uv).rgb, 1.0);
    // outColor = vec4(textureLod(irradianceMap, N, 0).rgb, 1.0);
    // outColor = vec4(textureLod(irradianceMap, fragNormal, 0).rgb, 1.0);
    // outColor = vec4(textureLod(prefilterMap, N, 0).rgb, 1.0);
    // outColor = vec4(texture(brdfLUT, N.xy).rgb, 1.0);
    
    // outColor = vec4(textureLod(albedoMap, uv, 0.0).rgb, 1.0);
}
