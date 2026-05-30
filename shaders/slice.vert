#version 450

layout(binding = 0) uniform SceneUniforms {
    mat4 view;
    mat4 proj;
} u;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;

void main()
{
    vColor = inColor;
    gl_Position = u.proj * u.view * vec4(inPos, 1.0);
}
