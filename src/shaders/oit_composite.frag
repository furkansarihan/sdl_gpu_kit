#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D accumTex;
layout(binding = 1) uniform sampler2D revealTex;

void main() {
    float reveal = texture(revealTex, uv).r;

    // If reveal is ~1.0, no transparent objects were drawn here.
    if (reveal >= 0.99999) {
        discard; 
    }

    vec4 accum = texture(accumTex, uv);
    
    // Avoid divide by zero
    float epsilon = 0.00001;
    vec3 averageColor = accum.rgb / max(accum.a, epsilon);

    // Blending Formula:
    // Dest (Background) * Reveal + Src (Transparent) * (1 - Reveal)
    // We output (Src, 1-Reveal) and use glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
    // resulting in: Src * (1-Reveal) + Dest * (1 - (1-Reveal)) -> Src*(1-R) + Dest*R
    
    outColor = vec4(averageColor, 1.0 - reveal);
}
