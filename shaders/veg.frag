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
    vec4 quality;
} u;
layout(set = 0, binding = 4) uniform sampler2DArray matAlbedo;
layout(set = 0, binding = 6) uniform sampler2D ecoTex;

layout(location = 0) out vec4 outColor;
const float WS = 165.0;

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
    bool leaf = (vType < 1.5) && (vColor.g > vColor.r * 1.25);      // tree foliage vs bark
    bool rockLike = (vType > 1.5 && vType < 2.5) || vType > 5.5 && vType < 6.5;
    if (rockLike) {
        // Boulders: rock photo texture, triplanar, with moss where the surface faces up.
        vec3 w = pow(abs(n), vec3(4.0)); w /= (w.x + w.y + w.z);
        vec3 pw = vWorldPos * 6.0;
        vec3 rock = texture(matAlbedo, vec3(pw.zy, 0.0)).rgb * w.x
                  + texture(matAlbedo, vec3(pw.xz, 0.0)).rgb * w.y
                  + texture(matAlbedo, vec3(pw.xy, 0.0)).rgb * w.z;
        float rl = dot(rock, vec3(0.3, 0.59, 0.11));
        rock = mix(vec3(rl), rock, 0.4) * vec3(0.78, 0.80, 0.82);
        float mossAmt = clamp(n.y, 0.0, 1.0) * (0.3 + 0.7 * vFade);
        float mossNoise = hash(floor(vWorldPos.xz * 90.0) + floor(vWorldPos.y * 90.0));
        vec3 moss = texture(matAlbedo, vec3(pw.xz * 1.7, 2.0)).rgb * vec3(0.45, 0.85, 0.35);
        albedo = mix(rock, moss, smoothstep(0.35, 0.75, mossAmt + (mossNoise - 0.5) * 0.35));
    } else if (vType < 1.5 && !leaf) {
        // Bark: vertical fissures from a cheap stripe hash.
        float ang = atan(vWorldPos.z * 40.0, vWorldPos.x * 40.0);
        float stripe = hash(vec2(floor(ang * 5.0), floor(vWorldPos.y * 120.0 + hash(vec2(floor(ang * 5.0))) * 7.0)));
        albedo *= 0.65 + 0.7 * stripe;
    }
    float hv = (vType > 2.5 && vType < 3.5) ? 0.5 : hash(floor(vWorldPos.xz * 37.0));
    if (!rockLike) {
        albedo *= 0.8 + 0.4 * hv;
        albedo = mix(albedo, albedo * vec3(1.15, 1.0, 0.7), (hv - 0.5) * 0.4);
    }
    float daylight = smoothstep(-0.08, 0.20, L.y);
    vec3 ambient = mix(vec3(0.020, 0.028, 0.048), vec3(0.15, 0.18, 0.22), daylight) * (0.6 + 0.4 * clamp(n.y, 0.0, 1.0));
    if (leaf && dot(n, V) < 0.0) n = -n;
    float NoL = clamp(dot(n, L) * 0.7 + 0.3, 0.0, 1.0);
    float ao = mix(vType > 3.5 ? 0.55 : 0.45, 1.0, vHeight01);
    vec3 color = albedo * (ambient * 0.85 + u.sunColor.xyz * NoL * vShadow * 0.62) * ao;
    // Thin foliage lets light through: backlit leaves and blades glow.
    if (leaf || (vType > 3.5 && vType < 5.5) || vType > 6.5) {
        float through = pow(clamp(dot(V, L), 0.0, 1.0), 3.0);
        color += albedo * vec3(1.0, 1.0, 0.7) * u.sunColor.xyz * through * vShadow * (leaf ? 0.5 : 0.12);
    }
    // Forest haze: denser air under canopy so trunks recede into mist.
    float haze = 0.0;
    {
        float R = u.material.w;
        vec2 rel = clamp((vWorldPos.xz + WS * 0.5) / WS, 0.0, 1.0);
        vec2 tc = (rel * (R - 1.0) + 0.5) / R;
        haze = texture(ecoTex, tc).b;
    }

    float dist = length(u.cameraPos.xyz - vWorldPos);
    float fogDensity = u.terrain.w + haze * 0.0025;
    float fog = fogDensity <= 0.00001 ? 0.0 : clamp(1.0 - exp(-dist * fogDensity), 0.0, 0.92);
    float sunAmount = max(dot(-V, L), 0.0);
    vec3 fogCol = mix(u.fogColor.xyz, u.sunColor.xyz, 0.55 * pow(sunAmount, 6.0));
    color = mix(color, fogCol, fog);
    color = aces(color * 1.25);
    outColor = vec4(color, 1.0);
}
