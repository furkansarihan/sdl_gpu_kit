#version 460

layout (location = 0) in vec4 v_color;

layout (location = 0) out vec4 FragColor;

layout(std140, set = 3, binding = 0) uniform UniformBlock {
    float time;
};

void main()
{
    float pulse = sin(time * 1.0) * 0.5 + 0.5; // range [0, 1]

    FragColor = vec4(v_color.rgb * (0.8 + pulse * 0.5), v_color.a);
}
