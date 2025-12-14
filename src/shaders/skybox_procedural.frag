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
} ubo;

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
    float zenithAngle = acos(max(0.0, dot(UP, normalize(vWorldPosition - ubo.cameraPos))));
    float denom = cos(zenithAngle) + 0.15 * pow(93.885 - ((zenithAngle * 180.0) / PI), -1.253);
    float sR = ubo.rayleighZenithLength / denom;
    float sM = ubo.mieZenithLength / denom;
    
    // Combined extinction factor
    vec3 Fex = exp(-(betaR * sR + betaM * sM));
    
    // In-scattering
    vec3 sunDirection = normalize(ubo.sunPosition);
    float cosTheta = dot(normalize(vWorldPosition - ubo.cameraPos), sunDirection);
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
    
    // Final color
    vec3 color = (Lin + L0) * 0.04;
    color += vec3(0.0, 0.001, 0.0025) * 0.3;

    outColor = vec4(color, 1.0);
}
