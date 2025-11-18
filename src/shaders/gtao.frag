#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec2 outOcclusionAndDepth; // R: visibility, G: packed depth

layout(std140, binding = 0) uniform GTAOParams
{
    vec4 resolution;          // xy = size, zw = 1/size
    vec2 positionParams;      // invProj[0][0] * 2, invProj[1][1] * 2
    vec2 padding1;            // matches float padding1[2]

    float invFarPlane;
    int   maxLevel;
    float projectionScale;
    float intensity;

    vec2  sliceCount;         // (sliceCount, 1/sliceCount)
    float stepsPerSlice;
    float radius;
    
    float invRadiusSquared;
    float projectionScaleRadius;
    float power;
    float thicknessHeuristic;
    
    float constThickness;
    float nearPlane;
    float farPlane;
    float padding2;
} ubo;

layout(binding = 0) uniform sampler2D depthTex;

// Constants
#define HALF_PI 1.5707963267948966
#define PI 3.14159265359
#define SECTOR_COUNT 32u

const float kLog2LodRate = 3.0;

// Utility functions
float saturate(float x) {
    return clamp(x, 0.0, 1.0);
}

// Fast approximation of acos
float acosFast(float x) {
    float res = -0.156583 * abs(x) + HALF_PI;
    res *= sqrt(1.0 - abs(x));
    return x >= 0.0 ? res : PI - res;
}

// TODO:
float pack(float depth) {
    return depth;
}

float linearizeDepth(float depth)
{
    // Convert [0,1] depth back to clip-space z
    float z = depth * 2.0 - 1.0; // back to NDC
    return (2.0 * ubo.nearPlane * ubo.farPlane) /
           (ubo.farPlane + ubo.nearPlane - z * (ubo.farPlane - ubo.nearPlane));
    // This returns a positive view-space distance.
}

// Sample depth with mip level
float sampleDepthLinear(sampler2D depthTexture, vec2 uv, float level) {
    float depth = textureLod(depthTexture, uv, level).r;
    return linearizeDepth(depth);
}

// Compute view-space position from depth
vec3 computeViewSpacePositionFromDepth(vec2 uv, float linearDepth, vec2 positionParams) {
    return vec3((0.5 - uv) * positionParams * linearDepth, linearDepth);
}

// Get view space position at UV with mip level
vec3 getViewSpacePosition(vec2 uv, float level) {
    float depth = sampleDepthLinear(depthTex, uv, level);
    return computeViewSpacePositionFromDepth(uv, depth, ubo.positionParams);
}

// Sample raw depth (non-linear) at a given mip level
float sampleDepth(sampler2D depthTexture, vec2 uv, float level)
{
    return textureLod(depthTexture, uv, level).r;
}

// Build a face normal from derivatives
vec3 faceNormal(vec3 dpdx, vec3 dpdy)
{
    // If your normals come out flipped, swap the cross arguments
    return normalize(cross(dpdx, dpdy));
}

// Compute view-space normal using depth derivatives
vec3 computeViewSpaceNormal(vec2 uv, float depth, vec3 position)
{
    vec2 texel = ubo.resolution.zw; // 1 / resolution

    vec3 pos_c = position;
    vec2 dx = vec2(texel.x, 0.0);
    vec2 dy = vec2(0.0, texel.y);

    // ---- Horizontal derivative (dpdx) ----
    vec4 H;
    H.x = sampleDepth(depthTex, uv - dx,      0.0);
    H.y = sampleDepth(depthTex, uv + dx,      0.0);
    H.z = sampleDepth(depthTex, uv - dx * 2.0, 0.0);
    H.w = sampleDepth(depthTex, uv + dx * 2.0, 0.0);

    // Error term: choose the better one-sided derivative
    vec2 he = abs((2.0 * H.xy - H.zw) - depth);

    vec3 pos_l = computeViewSpacePositionFromDepth(
        uv - dx,
        linearizeDepth(H.x),
        ubo.positionParams
    );
    vec3 pos_r = computeViewSpacePositionFromDepth(
        uv + dx,
        linearizeDepth(H.y),
        ubo.positionParams
    );

    vec3 dpdx = (he.x < he.y) ? (pos_c - pos_l) : (pos_r - pos_c);

    // ---- Vertical derivative (dpdy) ----
    vec4 V;
    V.x = sampleDepth(depthTex, uv - dy,      0.0);
    V.y = sampleDepth(depthTex, uv + dy,      0.0);
    V.z = sampleDepth(depthTex, uv - dy * 2.0, 0.0);
    V.w = sampleDepth(depthTex, uv + dy * 2.0, 0.0);

    vec2 ve = abs((2.0 * V.xy - V.zw) - depth);

    vec3 pos_d = computeViewSpacePositionFromDepth(
        uv - dy,
        linearizeDepth(V.x),
        ubo.positionParams
    );
    vec3 pos_u = computeViewSpacePositionFromDepth(
        uv + dy,
        linearizeDepth(V.y),
        ubo.positionParams
    );

    vec3 dpdy = (ve.x < ve.y) ? (pos_c - pos_d) : (pos_u - pos_c);

    return faceNormal(dpdx, dpdy);
}

// Spatial noise functions
float spatialDirectionNoise(vec2 uv) {
    ivec2 position = ivec2(uv * ubo.resolution.xy);
    return (1.0/16.0) * (float(((position.x + position.y) & 3) << 2) + float(position.x & 3));
}

float spatialOffsetsNoise(vec2 uv) {
    ivec2 position = ivec2(uv * ubo.resolution.xy);
    return 0.25 * float((position.y - position.x) & 3);
}

// Integrate arc cosine weight
float integrateArcCosWeight(float h, float n) {
    float arc = -cos(2.0 * h - n) + cos(n) + 2.0 * h * sin(n);
    return 0.25 * arc;
}

// Update horizon angle
float updateHorizon(float sampleHorizonCos, float currentHorizonCos, float fallOff) {
    return sampleHorizonCos > currentHorizonCos
        ? mix(sampleHorizonCos, currentHorizonCos, fallOff)
        : mix(currentHorizonCos, sampleHorizonCos, ubo.thicknessHeuristic);
}

// Calculate horizon cosine
float calculateHorizonCos(vec3 sampleDelta, vec3 viewDir, float horizonCos) {
    float sqSampleDist = dot(sampleDelta, sampleDelta);
    float invSampleDist = inversesqrt(sqSampleDist);
    
    float fallOff = saturate(sqSampleDist * ubo.invRadiusSquared * 16.0);
    float shc = dot(sampleDelta, viewDir) * invSampleDist;
    
    return updateHorizon(shc, horizonCos, fallOff);
}

// Main GTAO computation
void groundTruthAmbientOcclusion(out float obscurance, vec2 uv, vec3 origin, vec3 normal) {
    vec3 viewDir = normalize(-origin);

    // float ssRadius = -(ubo.projectionScaleRadius / origin.z);
    // float stepRadius = ssRadius / (ubo.stepsPerSlice + 1.0);
    float ssRadius   =  ubo.projectionScaleRadius / max(origin.z, 1e-3);
    float stepRadius = ssRadius / (ubo.stepsPerSlice + 1.0);
    
    float noiseOffset = spatialOffsetsNoise(uv);
    float noiseDirection = spatialDirectionNoise(uv);
    float initialRayStep = fract(noiseOffset);

    float visibility = 0.0;    
    for (float i = 0.0; i < ubo.sliceCount.x; i += 1.0) {
        float slice = (i + noiseDirection) * ubo.sliceCount.y;
        float phi = slice * PI;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);
        vec2 omega = vec2(cosPhi, sinPhi);
        
        vec3 direction = vec3(cosPhi, sinPhi, 0.0);
        vec3 orthoDirection = normalize(direction - (dot(direction, viewDir) * viewDir));
        vec3 axis = cross(orthoDirection, viewDir);
        vec3 projNormal = normal - axis * dot(normal, axis);
        
        float signNorm = sign(dot(orthoDirection, projNormal));
        float projNormalLength = length(projNormal);
        float cosNorm = saturate(dot(projNormal, viewDir) / projNormalLength);
        float n = signNorm * acosFast(cosNorm);
        
        float horizonCos0 = -1.0;
        float horizonCos1 = -1.0;
        // uint globalOccludedBitfield = 0u;
        
        for (float j = 0.0; j < ubo.stepsPerSlice; j += 1.0) {
            vec2 sampleOffset = max((j + initialRayStep) * stepRadius, 1.0 + j) * omega;
            float sampleOffsetLength = length(sampleOffset);
            
            float level = clamp(floor(log2(sampleOffsetLength)) - kLog2LodRate, 0.0, float(ubo.maxLevel));
            vec2 uvSampleOffset = sampleOffset * ubo.resolution.zw;
            
            vec2 sampleScreenPos0 = uv + uvSampleOffset;
            vec2 sampleScreenPos1 = uv - uvSampleOffset;
            
            vec3 samplePos0 = getViewSpacePosition(sampleScreenPos0, level);
            vec3 samplePos1 = getViewSpacePosition(sampleScreenPos1, level);
            
            vec3 sampleDelta0 = samplePos0 - origin;
            vec3 sampleDelta1 = samplePos1 - origin;
            
            // if (ubo.useVisibilityBitmasks != 0) {
            //     globalOccludedBitfield = calculateVisibilityMask(sampleDelta0, viewDir, 1.0, globalOccludedBitfield, n, origin);
            //     globalOccludedBitfield = calculateVisibilityMask(sampleDelta1, viewDir, -1.0, globalOccludedBitfield, n, origin);
            // } else {
                   horizonCos0 = calculateHorizonCos(sampleDelta0, viewDir, horizonCos0);
                   horizonCos1 = calculateHorizonCos(sampleDelta1, viewDir, horizonCos1);
            // }
        }
        
        // if (ubo.useVisibilityBitmasks != 0) {
        //     visibility += 1.0 - float(bitCount(globalOccludedBitfield)) / float(SECTOR_COUNT);
        // } else {
               float h0 = -acosFast(horizonCos1);
               float h1 = acosFast(horizonCos0);
               h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);
               h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);

               // float angle = 0.5 * (h0 + h1);
               // bentNormal += viewDir * cos(angle) - orthoDirection * sin(angle);
            
               visibility += projNormalLength * (integrateArcCosWeight(h0, n) + integrateArcCosWeight(h1, n));
        // }
    }

    obscurance = 1.0 - saturate(visibility * ubo.sliceCount.y);
}

void main() {
    vec2 uv = vUV;
    
    float depth = texture(depthTex, uv).r;
    float z = linearizeDepth(depth);
    vec3 origin = computeViewSpacePositionFromDepth(uv, z, ubo.positionParams);

    if (depth >= 0.9999)
    {
        outOcclusionAndDepth = vec2(1.0, pack(origin.z * ubo.invFarPlane));
        return;
    }
    
    vec3 normal = computeViewSpaceNormal(uv, depth, origin);
    
    float occlusion = 0.0;
    
    if (ubo.intensity > 0.0) {
        groundTruthAmbientOcclusion(occlusion, uv, origin, normal);
    }
    
    // Convert occlusion to visibility
    float aoVisibility = pow(saturate(1.0 - occlusion), ubo.power);
    aoVisibility = mix(1.0, aoVisibility, ubo.intensity);
    
    outOcclusionAndDepth = vec2(aoVisibility, pack(origin.z * ubo.invFarPlane));

    // debug
    // outOcclusionAndDepth.r = clamp(origin.z * ubo.invFarPlane, 0.0, 1.0);
    // outOcclusionAndDepth.g = 0.0;
    // outOcclusionAndDepth = vec2(normal);
    // outOcclusionAndDepth = vec2(aoVisibility, 0.0);
    // outOcclusionAndDepth = vec2(normal.y);
}
