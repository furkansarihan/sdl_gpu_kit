#version 450
#extension GL_GOOGLE_include_directive : enable

// Color input (resolved HDR/LDR scene color, non-sRGB if possible)
layout(binding = 0) uniform sampler2D uColorTex;

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

// RG edges output (R = horizontal edge, G = vertical edge)
layout(location = 0) out vec4 oEdges;

void main()
{
    // Build offsets using the helper "VS" function – we can call it from PS too
    vec4 offsets[3];
    SMAAEdgeDetectionVS(vTexCoord, offsets);

    vec2 edges;

    // Color-based edge detection (best quality)
    if (u_edgeDetectionMode == 0)
        edges = SMAAColorEdgeDetectionPS(vTexCoord, offsets, uColorTex);

    // You could use luma-based instead for speed:
    else
        edges = SMAALumaEdgeDetectionPS(vTexCoord, offsets, uColorTex);

    oEdges = vec4(edges, 0.0, 1.0);
}
