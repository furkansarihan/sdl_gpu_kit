#version 450

layout(location = 0) in vec3 localPos;

layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform samplerCube environmentMap;

const float PI = 3.14159265359;

void main()
{
    vec3 normal = normalize(localPos);
    vec3 irradiance = vec3(0.0);  

    vec3 up    = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, normal));
    up         = normalize(cross(normal, right));

    float sampleDelta = 0.025;
    float nrSamples = 0.0; 
    for(float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for(float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // spherical to cartesian (in tangent space)
            vec3 tangentSample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
            // tangent space to world
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * normal; 

            // Use textureLod with 0.0 to sample base mip level
            vec3 sampled = textureLod(environmentMap, sampleVec, 0.0).rgb * cos(theta) * sin(theta);
            
            // eliminate fireflies
            sampled = min(sampled, vec3(500.0));
            
            irradiance += sampled;
            nrSamples++;
        }
    }
    irradiance = PI * irradiance * (1.0 / float(nrSamples));
  
    FragColor = vec4(irradiance, 1.0);
}
