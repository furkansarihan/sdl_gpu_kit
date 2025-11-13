#version 450

// Vertex shader inputs
layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;

// Fragment shader output
layout(location = 0) out vec4 outColor;

// Scene-wide uniforms
layout(binding = 0) uniform FragmentUniformBlock {
    vec3 lightPos;
    vec3 viewPos;
    vec3 lightColor;
} ubo;

// Per-Material uniforms
layout(binding = 1) uniform MaterialUniformBlock {
    vec4 albedoFactor;
    vec4 emissiveFactor; // .rgb = color, .a = strength
    float metallicFactor; // Used as specular intensity
    float roughnessFactor; // Used as inverse shininess
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

void main() {
    // --- 1. Get Material Properties ---
    vec2 uv = fragUV /* * material.uvScale */;
    
    // Diffuse color (albedo)
    vec3 diffuseColor = material.albedoFactor.rgb;
    if (material.hasAlbedoTexture > 0)
    {
        diffuseColor = texture(albedoMap, uv).rgb;
    }
    
    // Normal
    vec3 N = normalize(fragNormal);
    if (material.hasNormalTexture > 0)
    {
        // TODO: Need TBN matrix for proper normal mapping
        // For now, just use vertex normal
        N = normalize(fragNormal);
    }
    
    // Specular intensity and shininess
    // Reuse metallic as specular intensity (0-1)
    // Roughness maps to shininess: rough = low shininess, smooth = high shininess
    float specularIntensity = material.metallicFactor;
    float shininess = (1.0 - material.roughnessFactor) * 128.0 + 1.0; // Map to 1-129
    
    if (material.hasMetallicRoughnessTexture > 0)
    {
        vec3 mr = texture(metallicRoughnessMap, uv).rgb;
        specularIntensity = mr.b * specularIntensity; // Metallic channel as specular
        float roughness = mr.g * material.roughnessFactor;
        shininess = (1.0 - roughness) * 128.0 + 1.0;
    }
    
    // Ambient Occlusion
    float ao = 1.0;
    if (material.hasOcclusionTexture > 0)
    {
        ao = texture(occlusionMap, uv).r;
        ao = mix(1.0, ao, material.occlusionStrength);
    }
    
    // Emissive
    vec3 emissive = material.emissiveFactor.rgb;
    if (material.hasEmissiveTexture != 0) {
        vec3 emissiveTexel = texture(emissiveMap, uv).rgb;
        emissive *= emissiveTexel;
    }
    emissive *= material.emissiveFactor.a; // strength is in alpha channel

    // --- 2. Phong Lighting Calculation ---
    
    // Light and view vectors
    vec3 L = normalize(ubo.lightPos - fragPos); // Light direction
    vec3 V = normalize(ubo.viewPos - fragPos);  // View direction
    vec3 R = reflect(-L, N);                      // Reflected light direction
    
    // Attenuation
    float distance = length(ubo.lightPos - fragPos);
    float attenuation = 1.0 / (distance * distance);
    
    // Ambient component
    vec3 ambient = vec3(0.1) * diffuseColor * ao;
    
    // Diffuse component (Lambertian)
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = diffuseColor * ubo.lightColor * NdotL * attenuation;
    
    // Specular component (Phong)
    float RdotV = max(dot(R, V), 0.0);
    float spec = pow(RdotV, shininess);
    vec3 specular = specularIntensity * spec * ubo.lightColor * attenuation;
    
    // --- 3. Combine Components ---
    vec3 color = ambient + diffuse + specular + emissive;
    
    // --- 4. Gamma Correction ---
    color = pow(color, vec3(1.0 / 2.2)); 

    outColor = vec4(color, 1.0);
}
