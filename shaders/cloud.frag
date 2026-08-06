#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform SceneUniforms {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    vec4 sunDir;
    vec4 sunColor;
    vec4 fogColor;
    vec4 terrain;
    vec4 effects;
    mat4 invViewProj;
    vec4 volMin;      // world pos of cloud cell-center (0,0,0)
    vec4 volMax;      // world pos of cell-center (nx-1,ny-1,nz-1)
    vec4 cloudGrid;   // nx, ny, nz, heightmapRes
    vec4 cloudParams; // densityScale, steps, sunAbsorption, coverage
} u;

// R16F textures with hardware filtering: one trilinear sample replaces the eight raw
// buffer fetches the old SSBO path needed, and one bilinear sample replaces four.
layout(binding = 1) uniform sampler3D cloudTex;   // cloud water qc (kg/kg)
layout(binding = 2) uniform sampler2D heightTex;  // terrain surface height

const float WS = 165.0; // kTerrainWorldSize
// Allocated 3D texture extent (the grid slider maxima, kCloudTex* in vulkan_app.h);
// only the active cloudGrid.xyz sub-region holds data and is ever sampled.
const vec3 TEXDIM = vec3(144.0, 96.0, 144.0);

// Cloud water at a world point, trilinearly interpolated. 0 outside the box.
float sampleQc(vec3 p)
{
    vec3 rel = (p - u.volMin.xyz) / (u.volMax.xyz - u.volMin.xyz);
    if (any(lessThan(rel, vec3(0.0))) || any(greaterThan(rel, vec3(1.0)))) return 0.0;
    // rel [0,1] spans the centers of cells 0..n-1 of the active sub-region.
    vec3 tc = (rel * (u.cloudGrid.xyz - 1.0) + 0.5) / TEXDIM;
    return texture(cloudTex, tc).r;
}

// Terrain surface height at a world (x,z), bilinear over the uploaded heightmap.
float terrainHeight(vec2 xz)
{
    float R = u.cloudGrid.w;
    vec2 rel = clamp((xz + WS * 0.5) / WS, 0.0, 1.0);
    vec2 tc = (rel * (R - 1.0) + 0.5) / R;
    return texture(heightTex, tc).r;
}

float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main()
{
    // Reconstruct the world-space view ray for this pixel.
    vec2 ndc = vUv * 2.0 - 1.0;
    vec4 nh = u.invViewProj * vec4(ndc, 0.0, 1.0);
    vec4 fh = u.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 ro = u.cameraPos.xyz;
    vec3 rd = normalize(fh.xyz / fh.w - nh.xyz / nh.w);

    // Intersect the cloud volume AABB.
    vec3 t0 = (u.volMin.xyz - ro) / rd;
    vec3 t1 = (u.volMax.xyz - ro) / rd;
    vec3 tmin = min(t0, t1), tmax = max(t0, t1);
    float tN = max(max(tmin.x, tmin.y), tmin.z);
    float tF = min(min(tmax.x, tmax.y), tmax.z);
    tN = max(tN, 0.0);
    if (tF <= tN) { outColor = vec4(0.0); return; }

    int steps = int(u.cloudParams.y);
    float stepLen = (tF - tN) / float(steps);
    float densityScale = u.cloudParams.x;
    float sunAbsorb = u.cloudParams.z;
    float coverage = u.cloudParams.w;

    // Jitter the start to break up slice banding.
    float jitter = hash21(gl_FragCoord.xy) * stepLen;
    float t = tN + jitter;

    vec3 sunlit = u.sunColor.rgb;
    vec3 shadow = u.fogColor.rgb * 0.55 + vec3(0.12, 0.14, 0.18);

    float transmittance = 1.0;
    vec3 scattered = vec3(0.0);

    for (int s = 0; s < steps; ++s, t += stepLen) {
        vec3 p = ro + rd * t;
        // Terrain occlusion: once the ray is underground everything beyond is hidden.
        if (p.y < terrainHeight(p.xz)) break;

        float d = max(0.0, sampleQc(p) * densityScale - coverage);
        if (d <= 0.0) continue;

        // Short march toward the sun for self-shadowing.
        float ld = 0.0;
        float lstep = stepLen * 1.5;
        vec3 lp = p;
        for (int l = 0; l < 4; ++l) {
            lp += u.sunDir.xyz * lstep;
            ld += max(0.0, sampleQc(lp) * densityScale);
        }
        float lightT = exp(-ld * lstep * sunAbsorb);

        float a = 1.0 - exp(-d * stepLen);
        vec3 col = mix(shadow, sunlit, lightT);
        scattered += transmittance * a * col;     // premultiplied
        transmittance *= (1.0 - a);
        if (transmittance < 0.02) break;
    }

    outColor = vec4(scattered, 1.0 - transmittance); // premultiplied-alpha over scene
}
