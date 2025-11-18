#version 450

layout(location = 0) out float outDepth;

layout(set = 0, binding = 0) uniform sampler2D uDepthTexture;

void main()
{
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(uDepthTexture, 0));
    float depth = texture(uDepthTexture, uv).r;
    outDepth = depth;
}
