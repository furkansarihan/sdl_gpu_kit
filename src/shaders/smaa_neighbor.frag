#version 450
#extension GL_GOOGLE_include_directive : enable

// 0: Original scene color (resolved)
layout(binding = 0) uniform sampler2D uColorTex;
// 1: Blend weight texture from pass 2
layout(binding = 1) uniform sampler2D uBlendTex;

layout(std140, binding = 0) uniform SMAAUniforms
{
    vec4 u_smaaRtMetrics;

    int u_edgeDetectionMode;
    vec3 padding;
};

#define SMAA_RT_METRICS      u_smaaRtMetrics
#define SMAA_GLSL_4          1
#define SMAA_PRESET_HIGH     1
#define SMAA_PREDICATION     0
#define SMAA_REPROJECTION    0
#define SMAA_INCLUDE_VS      1
#define SMAA_INCLUDE_PS      1

#include "SMAA.hlsl"

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 oColor;

void main()
{
    // Get neighborhood offsets for the blend pass
    vec4 offset;
    SMAANeighborhoodBlendingVS(vTexCoord, offset);

    vec4 color = SMAANeighborhoodBlendingPS(
        vTexCoord,
        offset,
        uColorTex,
        uBlendTex
    );

    oColor = color;
}
