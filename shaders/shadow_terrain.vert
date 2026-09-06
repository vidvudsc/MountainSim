#version 450
layout(location = 0) in vec3 inPosition;
layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view; mat4 proj; vec4 cameraPos; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 terrain; vec4 effects;
    mat4 invViewProj; vec4 volMin; vec4 volMax; vec4 cloudGrid; vec4 cloudParams; vec4 lightning; vec4 shadowParams;
    vec4 material; vec4 quality; mat4 lightViewProj;
} u;
void main() { gl_Position = u.lightViewProj * vec4(inPosition, 1.0); }
