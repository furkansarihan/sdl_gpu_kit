#version 450

layout(location = 0) out float outColor;

layout(binding = 0) uniform sampler2D ssaoRaw;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(ssaoRaw, 0));
    float result = 0.0;
    
    // Simple 4x4 Box Blur
    // Matches the noise texture dimension logic
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            result += texture(ssaoRaw, gl_FragCoord.xy * texelSize + offset).r;
        }
    }
    
    outColor = result / 16.0;
}
