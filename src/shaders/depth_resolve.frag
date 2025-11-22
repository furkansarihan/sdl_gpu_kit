#version 450

layout(location = 0) out float outDepth;

layout(binding = 0) uniform sampler2DMS depthTexMS;

void main() {
    ivec2 texCoord = ivec2(gl_FragCoord.xy);
    int samples = textureSamples(depthTexMS);
    
    // Sample all MSAA samples and take the closest (minimum) depth
    float minDepth = 1.0;
    for (int i = 0; i < samples; i++) {
        float depth = texelFetch(depthTexMS, texCoord, i).r;
        minDepth = min(minDepth, depth);
    }
    
    outDepth = minDepth;
}
