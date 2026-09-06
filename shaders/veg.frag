#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;
layout(location = 3) in float vShadow;
layout(location = 4) in float vHeight01;
layout(location = 5) in float vType;
layout(location = 6) in float vFade;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    vec4 sunDir;
    vec4 sunColor;
    vec4 fogColor;
    vec4 terrain;
    vec4 effects;
    mat4 invViewProj;
    vec4 volMin;
    vec4 volMax;
    vec4 cloudGrid;
    vec4 cloudParams;
    vec4 lightning;
    vec4 shadowParams;
    vec4 material;
} u;

layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

void main()
{
    vec3 L = normalize(u.sunDir.xyz);
    vec3 V = normalize(u.cameraPos.xyz - vWorldPos);
    vec3 n = normalize(vNormal);
    if (vType >= 2.5 && vType < 5.5 && dot(n, V) < 0.0) n = -n;   // billboards, grass, ferns are two-sided
    if (vType > 3.5 && vType < 5.5) n = normalize(mix(n, vec3(0.0, 1.0, 0.0), 0.6)); // grass/ferns light like the ground
    if (vType > 2.5 && vType < 3.5) n = normalize(vec3(0.0, 1.0, 0.0) * 0.8 + L * 0.35); // far trees: average canopy lighting

    vec3 albedo = vColor;
    if (vType > 5.5) {
        // Moss rock: grey stone with moss on top, more moss under forest (fade carries forest cover).
        float moss = clamp(n.y, 0.0, 1.0) * (0.35 + 0.65 * vFade);
        albedo = mix(vColor, vec3(0.16, 0.30, 0.12), smoothstep(0.35, 0.8, moss));
    }
    float hv = (vType > 2.5 && vType < 3.5) ? 0.5 : hash(floor(vWorldPos.xz * 37.0));
    albedo *= 0.75 + 0.5 * hv;
    albedo = mix(albedo, albedo * vec3(1.15, 1.0, 0.7), (hv - 0.5) * 0.5);

    float daylight = smoothstep(-0.08, 0.20, L.y);
    vec3 ambient = mix(vec3(0.020, 0.028, 0.048), vec3(0.15, 0.18, 0.22), daylight) * (0.6 + 0.4 * clamp(n.y, 0.0, 1.0));
    float NoL = clamp(dot(n, L) * 0.7 + 0.3, 0.0, 1.0);
    float ao = mix(vType > 3.5 ? 0.55 : 0.45, 1.0, vHeight01);
    vec3 color = albedo * (ambient * 0.85 + u.sunColor.xyz * NoL * vShadow * 0.62) * ao;

    float dist = length(u.cameraPos.xyz - vWorldPos);
    float fogDensity = u.terrain.w;
    float fog = fogDensity <= 0.00001 ? 0.0 : clamp(1.0 - exp(-dist * fogDensity), 0.0, 0.92);
    float sunAmount = max(dot(-V, L), 0.0);
    vec3 fogCol = mix(u.fogColor.xyz, u.sunColor.xyz, 0.55 * pow(sunAmount, 6.0));
    color = mix(color, fogCol, fog);
    color = aces(color * 1.25);
    outColor = vec4(color, 1.0);
}
