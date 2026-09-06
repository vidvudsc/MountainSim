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
    vec4 lightning;   // xyz flash world pos, w intensity
    vec4 shadowParams;
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

// Interleaved gradient noise: visibly better step-jitter distribution than a sine hash.
float ign(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

// Henyey-Greenstein phase (unnormalized) for anisotropic scattering.
float hg(float mu, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * mu, 1.5);
}

float hash31(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.11369, 0.13787));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float noise3(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 w = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i + vec3(0, 0, 0));
    float n100 = hash31(i + vec3(1, 0, 0));
    float n010 = hash31(i + vec3(0, 1, 0));
    float n110 = hash31(i + vec3(1, 1, 0));
    float n001 = hash31(i + vec3(0, 0, 1));
    float n101 = hash31(i + vec3(1, 0, 1));
    float n011 = hash31(i + vec3(0, 1, 1));
    float n111 = hash31(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, w.x), mix(n010, n110, w.x), w.y),
               mix(mix(n001, n101, w.x), mix(n011, n111, w.x), w.y), w.z);
}

float fbm3(vec3 p)
{
    float sum = 0.0;
    float amp = 0.56;
    for (int i = 0; i < 3; ++i) {
        sum += noise3(p) * amp;
        p = p * 2.03 + vec3(11.7, -4.2, 6.1);
        amp *= 0.5;
    }
    return sum;
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

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
    float detailStrength = clamp(u.shadowParams.w, 0.0, 1.0);

    // Jitter the start to break up slice banding.
    float jitter = ign(gl_FragCoord.xy) * stepLen;
    float t = tN + jitter;

    vec3 sd = normalize(u.sunDir.xyz);
    float daylight = smoothstep(-0.08, 0.15, sd.y);
    // Anisotropic phase: bright silver lining looking sunward, softer looking away.
    float mu = dot(rd, sd);
    float phase = 0.45 + 0.65 * hg(mu, 0.5) / 3.0;
    vec3 sunlit = u.sunColor.rgb * phase;
    // Sky ambient keeps shadowed cloud bases blue-grey by day, near-black at night.
    vec3 skyAmb = mix(vec3(0.012, 0.016, 0.032), vec3(0.30, 0.37, 0.48), daylight);

    float transmittance = 1.0;
    vec3 scattered = vec3(0.0);

    for (int s = 0; s < steps; ++s, t += stepLen) {
        vec3 p = ro + rd * t;
        // Terrain occlusion: once the ray is underground everything beyond is hidden.
        if (p.y < terrainHeight(p.xz)) break;

        float qc = sampleQc(p);
        float d = max(0.0, qc * densityScale - coverage);
        if (d <= 0.0) continue;
        if (detailStrength > 0.001) {
            float envelope = smoothstep(coverage, coverage + 0.22, qc * densityScale);
            float curlish = fbm3(p * 0.095 + vec3(0.0, p.y * 0.018, 0.0));
            float erode = mix(0.82, 1.18, curlish);
            float edgeFeather = mix(1.0, smoothstep(0.18, 0.92, curlish + envelope * 0.38), detailStrength);
            d *= mix(1.0, erode * edgeFeather, detailStrength);
        }

        // Short march toward the sun for self-shadowing.
        float ld = 0.0;
        float lstep = stepLen * 1.5;
        vec3 lp = p;
        for (int l = 0; l < 4; ++l) {
            lp += sd * lstep;
            ld += max(0.0, sampleQc(lp) * densityScale);
        }
        float lightT = exp(-ld * lstep * sunAbsorb);

        float a = 1.0 - exp(-d * stepLen);
        vec3 col = skyAmb + sunlit * lightT;
        // Lightning glow from inside the storm.
        if (u.lightning.w > 0.001) {
            vec3 toFlash = u.lightning.xyz - p;
            col += vec3(0.60, 0.70, 1.0) * (u.lightning.w * 6.0 / (1.0 + dot(toFlash, toFlash) * 0.004));
        }
        scattered += transmittance * a * col;     // premultiplied
        transmittance *= (1.0 - a);
        if (transmittance < 0.02) break;
    }

    outColor = vec4(aces(scattered * 1.25), 1.0 - transmittance); // premultiplied-alpha over scene
}
