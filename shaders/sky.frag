#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform samplerCube uSkybox;

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

void main()
{
    float y = clamp(vUv.y, 0.0, 1.0);
    vec2 ndc = vUv * 2.0 - 1.0;
    vec4 clip = vec4(ndc, 1.0, 1.0);
    vec4 viewDir4 = inverse(u.proj) * clip;
    viewDir4 = vec4(viewDir4.xy, -1.0, 0.0);
    vec3 dir = normalize((inverse(u.view) * viewDir4).xyz);

    vec3 low = vec3(0.38, 0.50, 0.58);
    vec3 horizon = vec3(0.52, 0.64, 0.70);
    vec3 zenith = vec3(0.11, 0.22, 0.34);
    vec3 sky = mix(horizon, zenith, smoothstep(0.16, 1.0, y));
    sky = mix(low, sky, smoothstep(0.0, 0.24, y));
    vec3 cube = texture(uSkybox, dir).rgb;
    cube = mix(cube, sky, smoothstep(-0.10, 0.18, abs(dir.y)) * 0.35);
    float horizonFade = smoothstep(-0.04, 0.22, abs(dir.y));
    sky = mix(sky, cube, horizonFade);

    vec2 sunPos = vec2(0.5 + normalize(u.sunDir.xz).x * 0.22, 0.62 + u.sunDir.y * 0.30);
    float sun = 1.0 - smoothstep(0.0, 0.34, distance(vUv, sunPos));
    sky += u.sunColor.xyz * sun * 0.11;

    float vignette = smoothstep(0.95, 0.10, distance(vUv, vec2(0.5, 0.42)));
    sky *= mix(0.78, 1.0, vignette);
    outColor = vec4(sky, 1.0);
}
