#version 450

layout(location = 0) in vec3 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform SkyboxFragmentUBO {
    vec3 sunPosition;
    float turbidity;

    vec3 cameraPos;
    float rayleigh;

    float mieCoefficient;
    float mieDirectionalG;
    float sunIntensityFactor;
    float padding0;

    float sunIntensityFalloffSteepness;
    float sunAngularDiameterDegrees;
    float rayleighZenithLength;
    float mieZenithLength;

    float mieV;
    float numMolecules;
    float refractiveIndex;
    float depolarizationFactor;

    vec3 primaries;
    float padding1;

    vec3 mieKCoefficient;
    float padding2;

    float time;
    float cloudScale;
    float cloudCoverage;
    float padding3;
} ubo;

layout(binding = 0) uniform sampler2D noiseTexture;

const float PI = 3.141592653589793;
const vec3 UP = vec3(0.0, 1.0, 0.0);

vec3 totalRayleigh(vec3 lambda) {
    return (8.0 * pow(PI, 3.0) * pow(pow(ubo.refractiveIndex, 2.0) - 1.0, 2.0) * (6.0 + 3.0 * ubo.depolarizationFactor)) / 
           (3.0 * ubo.numMolecules * pow(lambda, vec3(4.0)) * (6.0 - 7.0 * ubo.depolarizationFactor));
}

vec3 totalMie(vec3 lambda, vec3 K, float T) {
    float c = 0.2 * T * 10e-18;
    return 0.434 * c * PI * pow((2.0 * PI) / lambda, vec3(ubo.mieV - 2.0)) * K;
}

float rayleighPhase(float cosTheta) {
    return (3.0 / (16.0 * PI)) * (1.0 + pow(cosTheta, 2.0));
}

float henyeyGreensteinPhase(float cosTheta, float g) {
    return (1.0 / (4.0 * PI)) * ((1.0 - pow(g, 2.0)) / pow(1.0 - 2.0 * g * cosTheta + pow(g, 2.0), 1.5));
}

float sunIntensity(float zenithAngleCos) {
    float cutoffAngle = PI / 1.95;
    return ubo.sunIntensityFactor * max(0.0, 1.0 - exp(-((cutoffAngle - acos(zenithAngleCos)) / ubo.sunIntensityFalloffSteepness)));
}

float getCloudAlpha(vec3 cloudPos) {
    float cloudCeiling = 1000.0;
    vec3 planarPos = cloudPos * (cloudCeiling / max(cloudPos.y, 0.05));

    vec2 noiseCoord = planarPos.xz * (0.001 * ubo.cloudScale);
    noiseCoord.x += ubo.time * 0.006;

    float baseCloud = texture(noiseTexture, noiseCoord, 3.0).r;

    vec2 detailCoord = noiseCoord * 2.0; 
    detailCoord.x -= ubo.time * 0.002;
    float detailCloud = texture(noiseTexture, detailCoord, 1.0).r;

    float cloudVal = baseCloud * 0.7 + detailCloud * 0.3;
    float cloudAlpha = smoothstep(1.0 - ubo.cloudCoverage, 1.0, cloudVal);

    // Simple horizon fade
    return cloudAlpha * clamp((cloudPos.y - 0.05) * 5.0, 0.0, 1.0);
}

void main() {
    vec3 vWorldPosition = fragTexCoord;
    
    // 
    vWorldPosition.y *= -1.0;
    
    // Rayleigh coefficient
    float sunfade = 1.0 - clamp(1.0 - exp((ubo.sunPosition.y / 450000.0)), 0.0, 1.0);
    float rayleighCoefficient = ubo.rayleigh - (1.0 * (1.0 - sunfade));
    vec3 betaR = totalRayleigh(ubo.primaries) * rayleighCoefficient;
    
    // Mie coefficient
    vec3 betaM = totalMie(ubo.primaries, ubo.mieKCoefficient, ubo.turbidity) * ubo.mieCoefficient;
    
    // Optical length
    vec3 viewDir = normalize(vWorldPosition - ubo.cameraPos);
    float zenithAngle = acos(max(0.0, dot(UP, viewDir)));
    float denom = cos(zenithAngle) + 0.15 * pow(93.885 - ((zenithAngle * 180.0) / PI), -1.253);
    float sR = ubo.rayleighZenithLength / denom;
    float sM = ubo.mieZenithLength / denom;
    
    // Combined extinction factor
    vec3 Fex = exp(-(betaR * sR + betaM * sM));
    
    // In-scattering
    vec3 sunDirection = normalize(ubo.sunPosition);
    float cosTheta = dot(viewDir, sunDirection);
    vec3 betaRTheta = betaR * rayleighPhase(cosTheta * 0.5 + 0.5);
    vec3 betaMTheta = betaM * henyeyGreensteinPhase(cosTheta, ubo.mieDirectionalG);
    float sunE = sunIntensity(dot(sunDirection, UP));
    vec3 Lin = pow(sunE * ((betaRTheta + betaMTheta) / (betaR + betaM)) * (1.0 - Fex), vec3(1.5));
    Lin *= mix(vec3(1.0), pow(sunE * ((betaRTheta + betaMTheta) / (betaR + betaM)) * Fex, vec3(0.5)), 
               clamp(pow(1.0 - dot(UP, sunDirection), 5.0), 0.0, 1.0));
    
    // Solar disc
    float sunAngularDiameterCos = cos(ubo.sunAngularDiameterDegrees);
    float sundisk = smoothstep(sunAngularDiameterCos, sunAngularDiameterCos + 0.00002, cosTheta);
    vec3 L0 = vec3(0.1) * Fex;
    L0 += sunE * 19000.0 * Fex * sundisk;

    // Sky color
    vec3 skyColor = (Lin + L0) * 0.04;
    skyColor += vec3(0.0, 0.001, 0.0025) * 0.3;

    // Cloud color
    vec3 cloudColor = vec3(0.9);
    float cloudAlpha = getCloudAlpha(viewDir);
    
    // Final color
    vec3 finalColor = mix(skyColor, cloudColor, cloudAlpha);

    outColor = vec4(finalColor, 1.0);
}
