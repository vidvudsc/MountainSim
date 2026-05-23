#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec2 inHydro;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    vec4 sunDir;
    vec4 sunColor;
    vec4 fogColor;
    vec4 terrain;
    vec4 effects;
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec2 vHydro;
layout(location = 4) out float vHeight;

void main()
{
    vWorldPos = inPosition;
    vNormal = inNormal;
    vUv = inUv;
    vHydro = inHydro;
    vHeight = inPosition.y;
    gl_Position = u.proj * u.view * vec4(inPosition, 1.0);
}

