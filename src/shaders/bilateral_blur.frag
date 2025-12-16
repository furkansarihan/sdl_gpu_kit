#version 450

layout(location = 0) out float outColor;

layout(binding = 0) uniform sampler2D ssaoRaw;  // SSAO values in .r, depth in .g

// Uniforms to reconstruct linear depth (copy from GTAO pass)
layout(std140, binding = 0) uniform BlurFragUBO {
    vec2 invResolutionDirection;
    float sharpness;                // Higher = sharper edges (try 20-80)
    float padding0;
} ubo;

const float KERNEL_RADIUS = 3.0;

vec4 BlurFunction(vec2 uv, float r, float center_ao, float center_d, inout float w_total)
{
    float ao = texture(ssaoRaw, uv).r;
    float d = texture(ssaoRaw, uv).g;
    
    // Gaussian weight based on distance
    const float BlurSigma = KERNEL_RADIUS * 0.5;
    const float BlurFalloff = 1.0 / (2.0 * BlurSigma * BlurSigma);
    
    // Depth difference weight (bilateral filter)
    float ddiff = (d - center_d) * ubo.sharpness;
    float w = exp2(-r * r * BlurFalloff - ddiff * ddiff);
    
    w_total += w;
    return vec4(ao * w, 0.0, 0.0, 0.0);
}

void main()
{
    vec2 texelSize = 1.0 / vec2(textureSize(ssaoRaw, 0));
    vec2 texCoord = gl_FragCoord.xy * texelSize;
    
    // Read center values
    float center_ao = texture(ssaoRaw, texCoord).r;
    float center_d = texture(ssaoRaw, texCoord).g;
    
    // Early exit for sky pixels
    if (center_d >= 0.999) {
        outColor = 1.0;
        return;
    }
    
    float ao_total = center_ao;
    float w_total = 1.0;

    // Blur in positive direction
    for (float r = 1.0; r <= KERNEL_RADIUS; r += 1.0)
    {
        vec2 uv = texCoord + ubo.invResolutionDirection * r;
        ao_total += BlurFunction(uv, r, center_ao, center_d, w_total).r;
    }
    
    // Blur in negative direction
    for (float r = 1.0; r <= KERNEL_RADIUS; r += 1.0)
    {
        vec2 uv = texCoord - ubo.invResolutionDirection * r;
        ao_total += BlurFunction(uv, r, center_ao, center_d, w_total).r;
    }
    
    outColor = ao_total / w_total;
}
