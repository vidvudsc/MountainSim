#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec4 inPosScale;   // per instance
layout(location = 4) in vec4 inRotType;    // per instance

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
layout(set = 0, binding = 3) uniform sampler2D heightTex;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;
layout(location = 3) out float vShadow;
layout(location = 4) out float vHeight01;
layout(location = 5) out float vType;

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
    vec3 rp = vec3(c * p.x + sn * p.z, p.y, -sn * p.x + c * p.z);
    vec3 world = inPosScale.xyz + rp;
    vec3 n = inNormal;
    vNormal = normalize(vec3(c * n.x + sn * n.z, n.y, -sn * n.x + c * n.z));
    vWorldPos = world;
    vColor = inColor;
    // Billboards: colour by tree subtype so the far LOD matches the near meshes' average tone.
    if (inRotType.y > 2.5 && inRotType.y < 3.5) {
        vColor = (inRotType.w < 0.5) ? vec3(0.10, 0.17, 0.09) : vec3(0.13, 0.22, 0.11);
    }
    vHeight01 = clamp(inPosition.y * 60.0, 0.0, 1.0);   // unit object is 1 m tall (1/kMetersPerUnit units)
    vType = inRotType.y;
    vShadow = sunTerrainShadow(inPosScale.xyz + vec3(0.0, 0.05, 0.0), normalize(u.sunDir.xyz));
    gl_Position = u.proj * u.view * vec4(world, 1.0);
}
