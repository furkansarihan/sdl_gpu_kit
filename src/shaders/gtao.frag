#version 450

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec2 outOcclusionAndDepth; // R: visibility, G: packed depth

layout(std140, binding = 0) uniform GTAOParams
{
    vec4 resolution;          // xy = size, zw = 1/size
    vec2 positionParams;      // Should be: vec2(tan(fovX/2)*aspect, tan(fovY/2))
    vec2 padding1;

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

#define HALF_PI 1.5707963267948966
#define PI 3.14159265359

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

// Standard linearize (returns positive distance)
float linearizeDepth(float depth) {
    // Convert [0,1] depth back to clip-space z
    float z = depth * 2.0 - 1.0; // back to NDC
    return (2.0 * ubo.nearPlane * ubo.farPlane) /
           (ubo.farPlane + ubo.nearPlane - z * (ubo.farPlane - ubo.nearPlane));
    // This returns a positive view-space distance.
}

vec3 computeViewSpacePositionFromDepth(vec2 uv, float linearDepth, vec2 positionParams) {
    // Use standard NDC reconstruction (uv * 2 - 1)
    // Ensure your CPU positionParams matches this convention
    return vec3((uv * 2.0 - 1.0) * positionParams * linearDepth, -linearDepth);
}

vec3 getViewSpacePosition(vec2 uv, float level) {
    float depth = textureLod(depthTex, uv, level).r;
    float linDepth = linearizeDepth(depth);
    return computeViewSpacePositionFromDepth(uv, linDepth, ubo.positionParams);
}

float sampleDepth(sampler2D depthTexture, vec2 uv, float level) {
    return textureLod(depthTexture, uv, level).r;
}

vec3 faceNormal(vec3 dpdx, vec3 dpdy) {
    return normalize(cross(dpdx, dpdy));
}

// Compute Normal (Updated for standard coordinate system)
vec3 computeViewSpaceNormal(vec2 uv, float depth, vec3 position) {
    vec2 texel = ubo.resolution.zw;
    vec3 pos_c = position;
    vec2 dx = vec2(texel.x, 0.0);
    vec2 dy = vec2(0.0, texel.y);

    // Horizontal
    float d_l = sampleDepth(depthTex, uv - dx, 0.0);
    float d_r = sampleDepth(depthTex, uv + dx, 0.0);
    
    vec3 pos_l = computeViewSpacePositionFromDepth(uv - dx, linearizeDepth(d_l), ubo.positionParams);
    vec3 pos_r = computeViewSpacePositionFromDepth(uv + dx, linearizeDepth(d_r), ubo.positionParams);
    
    // Minimal depth difference check for sharpness
    vec3 dpdx = (abs(d_l - depth) < abs(d_r - depth)) ? (pos_c - pos_l) : (pos_r - pos_c);

    // Vertical
    float d_d = sampleDepth(depthTex, uv - dy, 0.0);
    float d_u = sampleDepth(depthTex, uv + dy, 0.0);

    vec3 pos_d = computeViewSpacePositionFromDepth(uv - dy, linearizeDepth(d_d), ubo.positionParams);
    vec3 pos_u = computeViewSpacePositionFromDepth(uv + dy, linearizeDepth(d_u), ubo.positionParams);

    vec3 dpdy = (abs(d_d - depth) < abs(d_u - depth)) ? (pos_c - pos_d) : (pos_u - pos_c);

    return faceNormal(dpdx, dpdy);
}

float spatialDirectionNoise(vec2 uv) {
    ivec2 position = ivec2(uv * ubo.resolution.xy);
    return (1.0/16.0) * (float(((position.x + position.y) & 3) << 2) + float(position.x & 3));
}

float spatialOffsetsNoise(vec2 uv) {
    ivec2 position = ivec2(uv * ubo.resolution.xy);
    return 0.25 * float((position.y - position.x) & 3);
}

float integrateArcCosWeight(float h, float n) {
    float arc = -cos(2.0 * h - n) + cos(n) + 2.0 * h * sin(n);
    return 0.25 * arc;
}

float updateHorizon(float sampleHorizonCos, float currentHorizonCos, float fallOff) {
    return sampleHorizonCos > currentHorizonCos
        ? mix(sampleHorizonCos, currentHorizonCos, fallOff)
        : mix(currentHorizonCos, sampleHorizonCos, ubo.thicknessHeuristic);
}

float calculateHorizonCos(vec3 sampleDelta, vec3 viewDir, float horizonCos) {
    float sqSampleDist = dot(sampleDelta, sampleDelta);
    float invSampleDist = inversesqrt(sqSampleDist);
    
    float fallOff = saturate(sqSampleDist * ubo.invRadiusSquared * 2.0);
    float shc = dot(sampleDelta, viewDir) * invSampleDist;
    
    return updateHorizon(shc, horizonCos, fallOff);
}

void groundTruthAmbientOcclusion(out float obscurance, vec2 uv, vec3 origin, vec3 normal) {
    vec3 viewDir = normalize(-origin); // Points towards camera (0,0,0)

    float ssRadius = -(ubo.projectionScaleRadius / origin.z);
    
    // Safety to prevent divide by zero or crazy radii close to camera
    ssRadius = clamp(ssRadius, 1.0, 1000.0); 

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
        
        // Screen space direction
        vec3 direction = vec3(cosPhi, sinPhi, 0.0);

        // Ortho direction in View Space
        vec3 orthoDirection = normalize(direction - (dot(direction, viewDir) * viewDir));
        vec3 axis = cross(orthoDirection, viewDir);
        vec3 projNormal = normal - axis * dot(normal, axis);

        float signNorm = sign(dot(orthoDirection, projNormal));
        float projNormalLength = length(projNormal);
        
        float cosNorm = clamp(dot(projNormal, viewDir) / projNormalLength, -1.0, 1.0);
        float n = signNorm * acosFast(cosNorm);
        
        float horizonCos0 = -1.0;
        float horizonCos1 = -1.0;
        
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
            
            horizonCos0 = calculateHorizonCos(sampleDelta0, viewDir, horizonCos0);
            horizonCos1 = calculateHorizonCos(sampleDelta1, viewDir, horizonCos1);
        }
        
        float h0 = -acosFast(horizonCos1);
        float h1 = acosFast(horizonCos0);
        h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);
        h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);

        visibility += projNormalLength * (integrateArcCosWeight(h0, n) + integrateArcCosWeight(h1, n));
    }

    obscurance = 1.0 - saturate(visibility * ubo.sliceCount.y);
}

void main() {
    vec2 uv = vUV;
    float depth = texture(depthTex, uv).r;
    
    // Standard OpenGL/Vulkan depth check
    if (depth == 1.0) {
        outOcclusionAndDepth = vec2(1.0, 0.0);
        return;
    }

    float linDepth = linearizeDepth(depth);
    vec3 origin = computeViewSpacePositionFromDepth(uv, linDepth, ubo.positionParams);
    vec3 normal = computeViewSpaceNormal(uv, depth, origin);
    
    float occlusion = 0.0;
    if (ubo.intensity > 0.0) {
        groundTruthAmbientOcclusion(occlusion, uv, origin, normal);
    }

    // Convert occlusion to visibility
    float aoVisibility = pow(saturate(1.0 - occlusion), ubo.power);
    aoVisibility = mix(1.0, aoVisibility, ubo.intensity);
    
    // Passing linearized depth in G channel for debugging or blurring later
    outOcclusionAndDepth = vec2(aoVisibility, linDepth * ubo.invFarPlane);
}
