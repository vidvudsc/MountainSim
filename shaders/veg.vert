#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec4 inPosScale;   // per instance
layout(location = 4) in vec4 inRotType;    // per instance
layout(location = 5) in vec4 inUvLayer;

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
    vec4 quality;   // w = time
    mat4 lightViewProj;
} u;
layout(set = 0, binding = 3) uniform sampler2D heightTex;

float hash2(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;
layout(location = 3) out float vShadow;
layout(location = 4) out float vHeight01;
layout(location = 5) out float vType;
layout(location = 6) out float vFade;
layout(location = 7) out vec3 vUvLayer;
layout(location = 8) out float vSub;

const float WS = 165.0;

float terrainH(vec2 xz)
{
    float R = u.cloudGrid.w;
    vec2 rel = clamp((xz + WS * 0.5) / WS, 0.0, 1.0);
    vec2 tc = (rel * (R - 1.0) + 0.5) / R;
    return texture(heightTex, tc).r;
}

float sunTerrainShadow(vec3 wp, vec3 sd)
{
    if (u.shadowParams.x < 0.5 || sd.y <= 0.03) return 1.0;
    float res = 1.0;
    float t = 1.0;
    for (int i = 0; i < 16; ++i) {
        vec3 p = wp + sd * t;
        if (p.y > u.terrain.x + 8.0) break;
        float h = terrainH(p.xz);
        res = min(res, 6.0 * (p.y - h + 0.6) / t);
        if (res < 0.0) break;
        t += clamp(t * 0.4, 1.0, 8.0);
    }
    return mix(1.0, clamp(res, 0.0, 1.0), smoothstep(0.03, 0.14, sd.y));
}

void main()
{
    float s = inPosScale.w;
    float c = cos(inRotType.x), sn = sin(inRotType.x);
    vec3 p = inPosition * s;
    float t0 = inRotType.y;
    bool treeLike = (t0 < 1.5) || (t0 > 9.5 && t0 < 10.5);
    if (treeLike) {
        // Individual trees: girth varies independently of height, and each leans a little,
        // bending progressively from a planted base.
        float h1 = hash2(inPosScale.xz + 3.1);
        float h2 = hash2(inPosScale.zx * 1.7 + 9.3);
        float girth = 0.72 + 0.7 * h1;
        p.xz *= girth;
        float h01 = clamp(inPosition.y * 60.0, 0.0, 1.0);
        float lean = (h2 - 0.5) * 0.22 * h01;          // up to ~6 degrees at the crown
        float la = h1 * 6.2831853;
        vec3 axis = vec3(cos(la), 0.0, sin(la));
        float cl = cos(lean), sl = sin(lean);
        p = p * cl + cross(axis, p) * sl + axis * dot(axis, p) * (1.0 - cl);
    }
    vec3 rp = vec3(c * p.x + sn * p.z, p.y, -sn * p.x + c * p.z);
    vec3 world = inPosScale.xyz + rp;
    // Wind: a slow gust field over the map plus a faster flutter, bending with height.
    {
        float time = u.quality.w;
        vec2 windDir = normalize(vec2(0.8, 0.45));
        float phase = dot(inPosScale.xz, windDir) * 0.35;
        float gust = 0.55 + 0.45 * sin(time * 0.7 - phase) * sin(time * 0.23 - phase * 0.3 + 1.7);
        float h01 = clamp(inPosition.y * 60.0, 0.0, 1.0);
        float t = inRotType.y;
        float amp, flutter;
        if (t < 1.5) {          // trees: crown sways, trunk base fixed
            amp = (s / 60.0) * 0.025;               // crown moves ~2.5% of the tree height
            flutter = sin(time * 2.1 + hash2(inPosScale.xz) * 6.28 + inPosition.y * 60.0 * 0.4) * 0.25;
        } else if ((t > 3.5 && t < 5.5) || (t > 6.5 && t < 7.5)) {   // grass, ferns, flowers
            amp = 0.0045;
            flutter = sin(time * 3.3 + hash2(inPosScale.xz) * 6.28) * 0.6;
        } else { amp = 0.0; flutter = 0.0; }
        float bend = h01 * h01 * amp * (gust + flutter);
        world.xz += windDir * bend;
        if (t < 1.5) world.y -= bend * 0.3;
    }
    vec3 n = inNormal;
    vNormal = normalize(vec3(c * n.x + sn * n.z, n.y, -sn * n.x + c * n.z));
    vWorldPos = world;
    vColor = inColor;
    // Billboards: tint by tree subtype (0 conifer darker, 1 broadleaf).
    if (inRotType.y > 2.5 && inRotType.y < 3.5) vColor = (inRotType.w < 0.5) ? vec3(0.55, 0.62, 0.55) : vec3(0.95, 0.95, 0.9);
    vHeight01 = clamp(inPosition.y * 60.0, 0.0, 1.0);   // unit object is 1 m tall (1/kMetersPerUnit units)
    vType = inRotType.y;
    vFade = inRotType.z;
    vUvLayer = inUvLayer.xyz;
    vSub = inRotType.w;
    // Far billboards: cheap 4-step march; meshes get the full one.
    vec3 sd = normalize(u.sunDir.xyz);
    if (inRotType.y > 2.5 && inRotType.y < 3.5) {
        float res = 1.0;
        float t = 2.0;
        for (int i = 0; i < 4; ++i) {
            vec3 pp = inPosScale.xyz + vec3(0.0, 0.3, 0.0) + sd * t;
            res = min(res, 6.0 * (pp.y - terrainH(pp.xz) + 0.9) / t);
            t *= 2.2;
        }
        vShadow = (u.shadowParams.x < 0.5 || sd.y <= 0.03) ? 1.0 : mix(1.0, clamp(res, 0.0, 1.0), smoothstep(0.03, 0.14, sd.y));
    } else {
        vShadow = sunTerrainShadow(inPosScale.xyz + vec3(0.0, 0.05, 0.0), sd);
    }
    gl_Position = u.proj * u.view * vec4(world, 1.0);
}
