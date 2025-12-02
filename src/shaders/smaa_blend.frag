#version 450
#extension GL_GOOGLE_include_directive : enable

// 0: Edges from edge pass
layout(set = 0, binding = 0) uniform sampler2D uEdgesTex;
// 1: Precomputed area texture
layout(set = 0, binding = 1) uniform sampler2D uAreaTex;
// 2: Precomputed search texture
layout(set = 0, binding = 2) uniform sampler2D uSearchTex;

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

layout(location = 0) out vec4 oBlendWeights;

void main()
{
    // Compute per-pixel coordinates and offsets
    vec2 pixcoord;
    vec4 offsets[3];
    SMAABlendingWeightCalculationVS(vTexCoord, pixcoord, offsets);

    // For SMAA 1x just pass 0 as subsample indices
    vec4 subsampleIndices = vec4(0.0);

    vec4 weights = SMAABlendingWeightCalculationPS(
        vTexCoord,
        pixcoord,
        offsets,
        uEdgesTex,
        uAreaTex,
        uSearchTex,
        subsampleIndices
    );

    oBlendWeights = weights;
}
