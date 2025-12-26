vec3 linearFog(vec3 color, float dist) {
    if (dist < fogUBO.fogStart) {
        return color;
    }

    // Calculate linear fog factor: (dist - start) / (end - start)
    float fogFactor = (dist - fogUBO.fogStart) / (fogUBO.fogEnd - fogUBO.fogStart);
    
    // Clamp to [0, MaxOpacity]
    fogFactor = clamp(fogFactor, 0.0, fogUBO.fogMaxOpacity);

    // Standard Mix: blend current color into fog color based on factor
    return mix(color, fogUBO.fogColor, fogFactor);
}
