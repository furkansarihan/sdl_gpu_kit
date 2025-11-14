#version 450

// Vertex attributes
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// Vertex shader outputs
layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;

// Uniform buffer for per-draw transforms
layout(binding = 0) uniform VertexUniformBlock {
    mat4 model;
    mat4 view;
    mat4 projection;
    mat4 normalMatrix;
} ubo;

void main() {
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    fragPos = worldPos.xyz;
    
    // Transform normal to world space
    fragNormal = normalize((ubo.normalMatrix * vec4(inNormal, 0.0)).xyz);
    
    fragUV = inUV;
    
    gl_Position = ubo.projection * ubo.view * worldPos;
}
