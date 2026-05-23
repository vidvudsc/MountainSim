#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec2 vHydro;
layout(location = 4) in float vHeight;

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

layout(location = 0) out vec4 outColor;

float hash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 w = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}

float fbm(vec2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        sum += noise(p) * amp;
        p *= 2.03;
        amp *= 0.5;
    }
    return sum;
}

void main()
{
    float heightScale = max(u.terrain.x, 0.001);
    float snowLevel = u.terrain.y;
    float waterLevel = u.terrain.z;
    float fogDensity = u.terrain.w;
    float waterTint = u.effects.x;
    float sedimentTint = u.effects.y;
    float showWater = u.effects.z;
    float showSediment = u.effects.w;

    vec3 n = normalize(vNormal);
    vec3 lightDir = normalize(u.sunDir.xyz);
    vec3 viewDir = normalize(u.cameraPos.xyz - vWorldPos);
    vec3 halfDir = normalize(lightDir + viewDir);

    float height01 = clamp(vHeight / heightScale, 0.0, 1.0);
    float slope = clamp(1.0 - n.y, 0.0, 1.0);
    vec2 p = vWorldPos.xz;
    float broad = fbm(p * 0.055);
    float medium = fbm(p * 0.23);
    float fine = fbm(p * 1.65);
    float scratch = abs(noise(p * vec2(0.75, 2.8)) - 0.5) * 2.0;
    float detail = broad * 0.44 + medium * 0.36 + fine * 0.20;

    vec3 grass = mix(vec3(0.055, 0.19, 0.065), vec3(0.29, 0.43, 0.13), detail);
    grass = mix(grass, vec3(0.15, 0.28, 0.09), smoothstep(0.58, 0.9, fine) * 0.35);
    vec3 alpine = mix(vec3(0.30, 0.36, 0.25), vec3(0.47, 0.49, 0.35), medium);
    vec3 rock = mix(vec3(0.24, 0.25, 0.25), vec3(0.56, 0.58, 0.57), medium);
    rock = mix(rock, vec3(0.15, 0.155, 0.16), smoothstep(0.62, 0.96, scratch) * 0.38);
    vec3 cliff = mix(vec3(0.19, 0.205, 0.215), vec3(0.48, 0.50, 0.51), scratch * 0.72 + fine * 0.28);
    vec3 snow = mix(vec3(0.78, 0.88, 0.94), vec3(1.0, 0.99, 0.92), detail);
    snow = mix(snow, vec3(0.62, 0.67, 0.69), smoothstep(0.58, 0.88, slope) * 0.24);
    vec3 sediment = mix(vec3(0.37, 0.28, 0.16), vec3(0.64, 0.48, 0.30), medium);

    float rockMask = smoothstep(0.24, 0.58, slope);
    float cliffMask = smoothstep(0.48, 0.82, slope);
    float alpineMask = smoothstep(0.24, 0.52, height01);
    float snowMask = smoothstep(snowLevel - 0.025, snowLevel + 0.075, height01)
                   * (1.0 - smoothstep(0.58, 0.9, slope));

    vec3 base = mix(grass, alpine, alpineMask);
    base = mix(base, rock, rockMask);
    base = mix(base, cliff, cliffMask);
    base = mix(base, snow, snowMask);
    base = mix(base, sediment, clamp(vHydro.y * sedimentTint * showSediment, 0.0, 0.55));
    base *= 0.86 + fine * 0.22 + broad * 0.10;

    float wet = clamp((vHydro.x * waterTint + smoothstep(waterLevel - 0.012, waterLevel + 0.006, waterLevel - height01)) * showWater, 0.0, 1.0);
    vec3 waterColor = mix(vec3(0.035, 0.11, 0.14), vec3(0.12, 0.28, 0.31), broad);
    base = mix(base, waterColor, wet * 0.78);

    float diff = max(dot(n, lightDir), 0.0);
    float rim = pow(max(1.0 - dot(n, viewDir), 0.0), 2.2);
    float spec = pow(max(dot(n, halfDir), 0.0), mix(22.0, 96.0, wet + snowMask * 0.5)) * (wet + snowMask * 0.10);
    float shade = mix(0.58, 1.0, smoothstep(-0.12, 0.36, dot(n, lightDir)));

    vec3 color = base * (vec3(0.15, 0.18, 0.20) + u.sunColor.xyz * diff * shade);
    color += u.sunColor.xyz * spec * 0.9;
    color += vec3(0.18, 0.25, 0.30) * rim * 0.045;

    float dist = length(u.cameraPos.xyz - vWorldPos);
    float fog = fogDensity <= 0.00001 ? 0.0 : clamp(1.0 - exp(-dist * fogDensity), 0.0, 0.92);
    color = mix(color, u.fogColor.xyz, fog);
    color = color / (color + vec3(1.0));
    outColor = vec4(color, 1.0);
}
