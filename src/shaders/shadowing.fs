// Shadowing implementation from Filament engine.
// https://github.com/google/filament
// 
// Apache License, Version 2.0

#include "constants.glsl"

float saturate(float x) {
    return clamp(x, 0.0, 1.0);
}

vec4 mulMat4x4Float3(const mat4 m, const vec3 v) {
    return v.x * m[0] + (v.y * m[1] + (v.z * m[2] + m[3]));
}

vec2 poissonDisk[64] = vec2[]( // don't use 'const' b/c of OSX GL compiler bug
    vec2(0.511749, 0.547686), vec2(0.58929, 0.257224), vec2(0.165018, 0.57663), vec2(0.407692, 0.742285),
    vec2(0.707012, 0.646523), vec2(0.31463, 0.466825), vec2(0.801257, 0.485186), vec2(0.418136, 0.146517),
    vec2(0.579889, 0.0368284), vec2(0.79801, 0.140114), vec2(-0.0413185, 0.371455), vec2(-0.0529108, 0.627352),
    vec2(0.0821375, 0.882071), vec2(0.17308, 0.301207), vec2(-0.120452, 0.867216), vec2(0.371096, 0.916454),
    vec2(-0.178381, 0.146101), vec2(-0.276489, 0.550525), vec2(0.12542, 0.126643), vec2(-0.296654, 0.286879),
    vec2(0.261744, -0.00604975), vec2(-0.213417, 0.715776), vec2(0.425684, -0.153211), vec2(-0.480054, 0.321357),
    vec2(-0.0717878, -0.0250567), vec2(-0.328775, -0.169666), vec2(-0.394923, 0.130802), vec2(-0.553681, -0.176777),
    vec2(-0.722615, 0.120616), vec2(-0.693065, 0.309017), vec2(0.603193, 0.791471), vec2(-0.0754941, -0.297988),
    vec2(0.109303, -0.156472), vec2(0.260605, -0.280111), vec2(0.129731, -0.487954), vec2(-0.537315, 0.520494),
    vec2(-0.42758, 0.800607), vec2(0.77309, -0.0728102), vec2(0.908777, 0.328356), vec2(0.985341, 0.0759158),
    vec2(0.947536, -0.11837), vec2(-0.103315, -0.610747), vec2(0.337171, -0.584), vec2(0.210919, -0.720055),
    vec2(0.41894, -0.36769), vec2(-0.254228, -0.49368), vec2(-0.428562, -0.404037), vec2(-0.831732, -0.189615),
    vec2(-0.922642, 0.0888026), vec2(-0.865914, 0.427795), vec2(0.706117, -0.311662), vec2(0.545465, -0.520942),
    vec2(-0.695738, 0.664492), vec2(0.389421, -0.899007), vec2(0.48842, -0.708054), vec2(0.760298, -0.62735),
    vec2(-0.390788, -0.707388), vec2(-0.591046, -0.686721), vec2(-0.769903, -0.413775), vec2(-0.604457, -0.502571),
    vec2(-0.557234, 0.00451362), vec2(0.147572, -0.924353), vec2(-0.0662488, -0.892081), vec2(0.863832, -0.407206)
);

/*
 * Random number between 0 and 1, using interleaved gradient noise.
 * w must not be normalized (e.g. window coordinates)
 */
float interleavedGradientNoise(vec2 w) {
    const vec3 m = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(m.z * fract(dot(w, m.xy)));
}

// tap count up can go up to 64
// const uint DPCF_SHADOW_TAP_COUNT                = 12u;
// more samples lead to better "shape" of the hardened shadow
const uint PCSS_SHADOW_BLOCKER_SEARCH_TAP_COUNT = 16u;
// less samples lead to noisier shadows (can be mitigated with TAA)
const uint PCSS_SHADOW_FILTER_TAP_COUNT         = 16u;

//------------------------------------------------------------------------------
// PCF Shadow Sampling
//------------------------------------------------------------------------------

float hardenedKernel(float x) {
    // this is basically a stronger smoothstep()
    x = 2.0 * x - 1.0;
    float s = sign(x);
    x = 1.0 - s * x;
    x = x * x * x;
    x = s - x * s;
    return 0.5 * x + 0.5;
}

vec2 computeReceiverPlaneDepthBias(const vec3 position) {
    // see: GDC '06: Shadow Mapping: GPU-based Tips and Techniques
    // Chain rule to compute dz/du and dz/dv
    // |dz/du|   |du/dx du/dy|^-T   |dz/dx|
    // |dz/dv| = |dv/dx dv/dy|    * |dz/dy|
    vec3 duvz_dx = dFdx(position);
    vec3 duvz_dy = dFdy(position);
    vec2 dz_duv = inverse(transpose(mat2(duvz_dx.xy, duvz_dy.xy))) * vec2(duvz_dx.z, duvz_dy.z);
    return dz_duv;
}

mat2 getRandomRotationMatrix(vec2 fragCoord) {
    // rotate the poisson disk randomly
    float temporalNoise = 0.0;
    // fragCoord += vec2(frameUniforms.temporalNoise); // 0 when TAA is not used
    fragCoord += vec2(temporalNoise); // 0 when TAA is not used
    float randomAngle = interleavedGradientNoise(fragCoord) * (2.0 * PI);
    vec2 randomBase = vec2(cos(randomAngle), sin(randomAngle));
    mat2 R = mat2(randomBase.x, randomBase.y, -randomBase.y, randomBase.x);
    return R;
}

float getPenumbraLs(const bool DIRECTIONAL, const int index, const float zLight) {
    float penumbraScale = 1.0;
    float shadowBulbRadius = 0.02;
    float wsTexelSize = 0.02;
    // s.shadows[shadowIndex].bulbRadiusLs = mSoftShadowOptions.penumbraScale * 
    //                                       options.shadowBulbRadius / wsTexelSize;
    float bulbRadiusLs = penumbraScale * shadowBulbRadius / wsTexelSize;

    float penumbra;
    // This conditional is resolved at compile time
    if (DIRECTIONAL) {
        // penumbra = shadowUniforms.shadows[index].bulbRadiusLs;
        penumbra = bulbRadiusLs;
    } else {
        // the penumbra radius depends on the light-space z for spotlights
        // penumbra = shadowUniforms.shadows[index].bulbRadiusLs / zLight;
        penumbra = bulbRadiusLs / zLight;
    }
    return penumbra;
}

float getPenumbraRatio(const bool DIRECTIONAL, const int index,
        float z_receiver, float z_blocker) {
    // z_receiver/z_blocker are not linear depths (i.e. they're not distances)
    // Penumbra ratio for PCSS is given by:  pr = (d_receiver - d_blocker) / d_blocker
    float penumbraRatio;
    // if (DIRECTIONAL) {
        // TODO: take lispsm into account
        // For directional lights, the depths are linear but depend on the position (because of LiSPSM).
        // With:        z_linear = f + z * (n - f)
        // We get:      (r-b)/b ==> (f/(n-f) + r_linear) / (f/(n-f) + b_linear) - 1
        // Assuming f>>n and ignoring LISPSM, we get:
        penumbraRatio = (z_blocker - z_receiver) / (1.0 - z_blocker);
    // } else {
    //     // For spotlights, the depths are congruent to 1/z, specifically:
    //     //      z_linear = (n * f) / (n + z * (f - n))
    //     // replacing in (r - b) / b gives:

    //     // TODO:
    //     float nearOverFarMinusNear = shadowUniforms.shadows[index].nearOverFarMinusNear;
    //     penumbraRatio = (nearOverFarMinusNear + z_blocker) / (nearOverFarMinusNear + z_receiver) - 1.0;
    // }
    // TODO:
    // float shadowPenumbraRatioScale = 1.0;
    // return penumbraRatio * frameUniforms.shadowPenumbraRatioScale;
    return penumbraRatio;
}

void blockerSearchAndFilter(out float occludedCount, out float z_occSum,
        const sampler2DArray map, const vec4 scissorNormalized, const vec2 uv,
        const float z_rec, const uint layer,
        const vec2 filterRadii, const mat2 R, const vec2 dz_duv,
        const uint tapCount) {
    occludedCount = 0.0;
    z_occSum = 0.0;
    for (uint i = 0u; i < tapCount; i++) {
        vec2 duv = R * (poissonDisk[i] * filterRadii);
        vec2 tc = clamp(uv + duv, scissorNormalized.xy, scissorNormalized.zw);

        float z_occ = textureLod(map, vec3(tc, layer), 0.0).r;

        // note: z_occ and z_rec are not necessarily linear here, comparing them is always okay for
        // the regular PCF, but the "distance" is meaningless unless they are actually linear
        // (e.g.: for the directional light).
        // Either way, if we assume that all the samples are close to each other we can take their
        // average regardless, and the average depth value of the occluders
        // becomes: z_occSum / occludedCount.

        // receiver plane depth bias
        float z_bias = dot(dz_duv, duv);
        float dz = z_occ - z_rec; // dz>0 when blocker is between receiver and light
        float occluded = step(z_bias, dz);
        occludedCount += occluded;
        z_occSum += z_occ * occluded;
    }
}

/*
 * DPCF, PCF with contact hardenning simulation.
 * see "Shadow of Cold War", A scalable approach to shadowing -- by Kevin Myers
 */
float ShadowSample_DPCF(
        uint tapCount,
        const bool DIRECTIONAL,
        const sampler2DArray map,
        const vec4 scissorNormalized,
        const uint layer, const int index,
        const vec4 shadowPosition, const float zLight) {
    vec3 position = shadowPosition.xyz * (1.0 / shadowPosition.w);
    vec2 texelSize = vec2(1.0) / vec2(textureSize(map, 0));

    // We need to use the shadow receiver plane depth bias to combat shadow acne due to the
    // large kernel.
    vec2 dz_duv = computeReceiverPlaneDepthBias(position);

    float penumbra = getPenumbraLs(DIRECTIONAL, index, zLight);

    // rotate the poisson disk randomly
    mat2 R = getRandomRotationMatrix(gl_FragCoord.xy);

    float occludedCount = 0.0;
    float z_occSum = 0.0;

    blockerSearchAndFilter(occludedCount, z_occSum,
            map, scissorNormalized, position.xy, position.z, layer, texelSize * penumbra, R, dz_duv,
            tapCount);

    // early exit if there is no occluders at all, also avoids a divide-by-zero below.
    if (z_occSum == 0.0) {
        return 1.0;
    }

    float penumbraRatio = getPenumbraRatio(DIRECTIONAL, index, position.z, z_occSum / occludedCount);

    // The main way we're diverging from PCSS is that we're not going to sample again, instead
    // we're going to reuse the blocker search samples and we're going to use the penumbra ratio
    // as a parameter to lerp between a hardened PCF kernel and the search PCF kernel.
    // We need a parameter to blend between the the "hardened" kernel and the "soft" kernel,
    // to this end clamp the penumbra ratio between 0 (blocker is close to the receiver) and
    // 1 (blocker is close to the light).
    penumbraRatio = saturate(penumbraRatio);

    // regular PCF weight (i.e. average of samples in shadow)
    float percentageOccluded = occludedCount * (1.0 / float(tapCount));

    // now we just need to lerp between hardened PCF and regular PCF based on alpha
    percentageOccluded = mix(hardenedKernel(percentageOccluded), percentageOccluded, penumbraRatio);
    return 1.0 - percentageOccluded;
}

/**
 * Computes the light space position of the specified world space point.
 * The returned point may contain a bias to attempt to eliminate common
 * shadowing artifacts such as "acne". To achieve this, the world space
 * normal at the point must also be passed to this function.
 * Normal bias is not used for VSM.
 */

vec4 computeLightSpacePosition(vec3 p, const vec3 n,
        const vec3 dir, const float b, mat4 lightFromWorldMatrix) {

// #if !defined(VARIANT_HAS_VSM)
    float cosTheta = saturate(dot(n, dir));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    p += n * (sinTheta * b);
// #endif

    return mulMat4x4Float3(lightFromWorldMatrix, p);
}

int getCascadeIndex(vec3 worldPos) {
    // Transform fragment position to view space
    vec3 viewPos = (shadowUBO.cameraView * vec4(worldPos, 1.0)).xyz;
    float viewDepth = -viewPos.z; // camera looks down -Z, so make it positive

    int index = 0;
    if (viewDepth > shadowUBO.cascadeSplits.x) index = 1;
    if (viewDepth > shadowUBO.cascadeSplits.y) index = 2;
    if (viewDepth > shadowUBO.cascadeSplits.z) index = 3;

    return index;
}

float shadow(vec3 worldPos, vec3 normal, vec3 camPosition, vec3 lightDir, uint tapCount) {
    int index = getCascadeIndex(worldPos);
    float bias = shadowUBO.cascadeBias[index];
    mat4 DepthBiasVP = shadowUBO.depthBiasVP[index];
    vec4 ShadowCoord = computeLightSpacePosition(
            worldPos, normal,
            lightDir,
            bias,
            DepthBiasVP);

    vec4 scissorNormalized = vec4(0.0, 0.0, 1.0, 1.0);
    int layer = index;
    float zLight = 0.0;
    float shadow = 0.0;

    shadow = ShadowSample_DPCF(
        tapCount,
        true,
        shadowMap,
        scissorNormalized,
        layer,
        index,
        ShadowCoord,
        zLight
    );

    float visibility = 1.0 - shadow * shadowUBO.strength;

    /** Distance from the camera after which shadows are clipped. This is used to clip
     * shadows that are too far and wouldn't contribute to the scene much, improving
     * performance and quality. This value is always positive.
     * Use 0.0f to use the camera far distance.
     * This only affect directional lights.
     */
    float shadowFar = shadowUBO.shadowFar;

    // shadow far attenuation
    vec3 v = worldPos - camPosition;
    // (viewFromWorld * v).z == dot(transpose(viewFromWorld), v)
    float z = dot(transpose(shadowUBO.cameraView)[2].xyz, v);
    // vec2 p = frameUniforms.shadowFarAttenuationParams;
    vec2 p = shadowFar > 0.0 ? 0.5 * vec2(10.0, 10.0 / (shadowFar * shadowFar)) : vec2(1.0, 0.0);
    visibility = 1.0 - ((1.0 - visibility) * saturate(p.x - z * z * p.y));

    return visibility;
}
