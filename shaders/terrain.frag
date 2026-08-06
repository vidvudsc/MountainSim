#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec2 vHydro;
layout(location = 4) in float vHeight;
layout(location = 5) in vec4 vSurface;

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
    vec4 lightning;    // xyz flash world pos, w intensity
    vec4 shadowParams; // x sun-shadows on, y cloud-shadow strength
} u;

// Shared with the cloud pass: cloud water volume + terrain height, for shadow marches.
layout(set = 0, binding = 2) uniform sampler3D cloudTex;
layout(set = 0, binding = 3) uniform sampler2D heightTex;
// Photo-based material arrays: layer 0 grass, 1 rock face, 2 scree, 3 snow, 4 forest.
layout(set = 0, binding = 4) uniform sampler2DArray matAlbedo;
layout(set = 0, binding = 5) uniform sampler2DArray matNormal;

layout(location = 0) out vec4 outColor;

float texLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

const float WS = 165.0;                       // kTerrainWorldSize
const vec3 TEXDIM = vec3(144.0, 96.0, 144.0); // allocated cloud texture extent

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

float terrainH(vec2 xz)
{
    float R = u.cloudGrid.w;
    vec2 rel = clamp((xz + WS * 0.5) / WS, 0.0, 1.0);
    vec2 tc = (rel * (R - 1.0) + 0.5) / R;
    return texture(heightTex, tc).r;
}

float qcAt(vec3 p)
{
    vec3 rel = (p - u.volMin.xyz) / (u.volMax.xyz - u.volMin.xyz);
    if (any(lessThan(rel, vec3(0.0))) || any(greaterThan(rel, vec3(1.0)))) return 0.0;
    vec3 tc = (rel * (u.cloudGrid.xyz - 1.0) + 0.5) / TEXDIM;
    return texture(cloudTex, tc).r;
}

// Soft terrain self-shadow: march the heightmap toward the sun, tracking the minimum
// angular clearance (classic penumbra approximation). Growing steps keep it ~20 taps.
float sunTerrainShadow(vec3 wp, vec3 sd)
{
    if (u.shadowParams.x < 0.5 || sd.y <= 0.03) return 1.0;
    float res = 1.0;
    float t = 2.2;
    for (int i = 0; i < 22; ++i) {
        vec3 p = wp + sd * t;
        if (p.y > u.terrain.x + 8.0) break; // above any possible terrain
        float h = terrainH(p.xz);
        res = min(res, 6.0 * (p.y - h + 0.9) / t);
        if (res < 0.0) break;
        t += clamp(t * 0.32, 1.1, 7.0);
    }
    // Fade out at grazing sun angles: the coarse heightmap aliases into dappled noise
    // there, and direct light is nearly gone at the horizon anyway.
    return mix(1.0, clamp(res, 0.0, 1.0), smoothstep(0.03, 0.14, sd.y));
}

// Cloud shadow: accumulate cloud water along the sun ray up to the volume lid.
float cloudShadow(vec3 wp, vec3 sd)
{
    float strength = u.shadowParams.y;
    if (strength <= 0.0 || sd.y <= 0.05) return 1.0;
    // Cap the path length: at grazing sun the ray would otherwise sweep the whole
    // volume diagonally and alias the coarse cloud grid into patchy bands.
    float tTop = min((u.volMax.y - wp.y) / max(sd.y, 0.10), 300.0);
    float dt = tTop / 8.0;
    vec3 p = wp + sd * (0.5 * dt);
    float od = 0.0;
    for (int i = 0; i < 8; ++i) { od += qcAt(p); p += sd * dt; }
    float trans = exp(-od * dt * u.cloudParams.x * 0.9);
    // keep a floor: even under a storm deck skylight leaks in
    return mix(1.0, max(trans, 0.22), strength * smoothstep(0.05, 0.18, sd.y));
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    float heightScale = max(u.terrain.x, 0.001);
    float waterLevel = u.terrain.z;
    float fogDensity = u.terrain.w;
    float waterTint = u.effects.x;
    float sedimentTint = u.effects.y;
    float showWater = u.effects.z;
    float showSediment = u.effects.w;
    float cutFace = vHydro.x < -0.5 ? 1.0 : 0.0;
    float bottomFace = vHydro.x < -1.5 ? 1.0 : 0.0;

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
    vec3 sediment = mix(vec3(0.42, 0.36, 0.27), vec3(0.70, 0.64, 0.51), medium);

    float rockMask = smoothstep(0.24, 0.58, slope);
    float cliffMask = smoothstep(0.48, 0.82, slope);
    float alpineMask = smoothstep(0.24, 0.52, height01);
    float materialId = vSurface.x;
    float snowMask = clamp(vSurface.y, 0.0, 1.0);
    float iceMask = clamp(vSurface.z, 0.0, 1.0);
    float surfaceTempC = vSurface.w;
    float sedimentMask = smoothstep(1.5, 2.5, materialId) * (1.0 - smoothstep(2.5, 3.5, materialId));
    // Valley forest band: low, gentle, snow-free ground reads as conifer cover.
    float forestW = smoothstep(0.26, 0.10, height01) * smoothstep(0.42, 0.20, slope) * (1.0 - snowMask);

    // ---- photo material detail -----------------------------------------------------
    // The procedural colors above stay in charge of the palette; the textures supply
    // photographic structure (albedo detail + normal-map relief) modulated on top.
    float texScale = max(u.shadowParams.w, 1.0);
    vec2 uvG = p / texScale;
    vec3 nGeom = n;
    vec3 an = abs(nGeom);
    float camDist = length(u.cameraPos.xyz - vWorldPos);

    vec3 grassT = texture(matAlbedo, vec3(uvG, 0.0)).rgb;
    // anti-tiling: second scale blended by the broad noise so repeats never line up
    grassT = mix(grassT, texture(matAlbedo, vec3(uvG * 0.27, 0.0)).rgb, smoothstep(0.30, 0.70, broad));
    vec3 screeT = texture(matAlbedo, vec3(uvG * 1.4, 2.0)).rgb;
    vec3 snowT = texture(matAlbedo, vec3(uvG * 1.1, 3.0)).rgb;
    vec3 forestT = texture(matAlbedo, vec3(uvG * 0.8, 4.0)).rgb;
    // cliff rock: triplanar side projection so faces get vertical structure
    vec2 uvZY = vWorldPos.zy / texScale;
    vec2 uvXY = vWorldPos.xy / texScale;
    float sideW = an.x / max(an.x + an.z, 1e-4);
    vec3 rockT = mix(texture(matAlbedo, vec3(uvXY, 1.0)).rgb, texture(matAlbedo, vec3(uvZY, 1.0)).rgb, sideW);
    float topBlend = smoothstep(0.55, 0.85, nGeom.y);
    rockT = mix(rockT, texture(matAlbedo, vec3(uvG, 1.0)).rgb, topBlend);

    float screeW = max(alpineMask * 0.6, sedimentMask);
    vec3 texDetail = grassT;
    texDetail = mix(texDetail, forestT, forestW);
    texDetail = mix(texDetail, screeT, screeW);
    texDetail = mix(texDetail, rockT, rockMask);
    texDetail = mix(texDetail, snowT, snowMask);
    // structure mostly from luminance so the procedural palette keeps steering the hue
    texDetail = mix(vec3(texLuma(texDetail)), texDetail, 0.45);

    // matching normal-map relief (grass shared for forest; deltas per projection plane)
    vec3 nmTop = texture(matNormal, vec3(uvG, 0.0)).xyz * 2.0 - 1.0;
    nmTop = mix(nmTop, texture(matNormal, vec3(uvG * 1.4, 2.0)).xyz * 2.0 - 1.0, screeW);
    nmTop = mix(nmTop, texture(matNormal, vec3(uvG * 1.1, 3.0)).xyz * 2.0 - 1.0, snowMask);
    vec3 nmXY = texture(matNormal, vec3(uvXY, 1.0)).xyz * 2.0 - 1.0;
    vec3 nmZY = texture(matNormal, vec3(uvZY, 1.0)).xyz * 2.0 - 1.0;
    vec3 dTop = vec3(nmTop.x, 0.0, nmTop.y);
    vec3 dSide = mix(vec3(nmXY.x, nmXY.y, 0.0), vec3(0.0, nmZY.y, nmZY.x), sideW);
    vec3 dTotal = mix(dTop, dSide, rockMask * (1.0 - topBlend * 0.5));
    // fade relief with distance so it never shimmers at the horizon
    float nStr = 0.85 * clamp(80.0 / max(camDist, 1.0), 0.25, 1.0);
    // ---------------------------------------------------------------------------------

    vec3 base = mix(grass, alpine, alpineMask);
    base = mix(base, rock, rockMask);
    base = mix(base, cliff, cliffMask);
    base = mix(base, sediment, sedimentMask);
    base = mix(base, snow, snowMask);
    base = mix(base, vec3(0.70, 0.86, 0.96), iceMask * 0.72);
    base = mix(base, base * vec3(0.52, 0.68, 0.52), forestW); // conifer-dark valleys

    float stratum = smoothstep(0.42, 0.58, noise(vec2(vWorldPos.y * 1.15, vUv.x * 8.0 + vUv.y * 5.0)));
    float layer = smoothstep(0.47, 0.53, sin(vWorldPos.y * 1.7 + broad * 2.8) * 0.5 + 0.5);
    float cutDepth = clamp((vWorldPos.y + 5.0) / (heightScale + 8.0), 0.0, 1.0);
    vec3 compactEarth = vec3(0.20, 0.18, 0.15);
    vec3 weatheredStone = vec3(0.29, 0.27, 0.225);
    vec3 paleLayer = vec3(0.44, 0.40, 0.31);
    vec3 darkLayer = vec3(0.13, 0.14, 0.13);
    vec3 cutMaterial = mix(compactEarth, weatheredStone, cutDepth);
    cutMaterial = mix(cutMaterial, paleLayer, layer * 0.24);
    cutMaterial = mix(cutMaterial, darkLayer, (1.0 - cutDepth) * 0.22 + stratum * 0.12);
    cutMaterial *= 0.86 + fine * 0.12;
    // Dark topsoil band just under the surface so the wall meets the terrain with soil,
    // not a pale weathered-stone stripe.
    float below = max(terrainH(vWorldPos.xz) - vWorldPos.y, 0.0);
    cutMaterial = mix(vec3(0.135, 0.112, 0.085), cutMaterial, smoothstep(0.4, 3.0, below));
    cutMaterial = mix(cutMaterial, vec3(0.08, 0.085, 0.08), bottomFace);

    float wet = 0.0;
    if (cutFace > 0.5) {
        base = cutMaterial;
    } else {
        base = mix(base, sediment, clamp(vHydro.y * sedimentTint * showSediment, 0.0, 0.46));
        base *= 0.86 + fine * 0.22 + broad * 0.10;

        float routeWater = smoothstep(0.22, 0.92, vHydro.x) * waterTint;
        float pocketWater = smoothstep(waterLevel - 0.006, waterLevel + 0.003, waterLevel - height01)
                          * smoothstep(0.48, 0.82, vHydro.x) * 0.18;
        wet = clamp((routeWater + pocketWater) * showWater, 0.0, 1.0);
        vec3 washColor = mix(vec3(0.58, 0.62, 0.60), vec3(0.72, 0.76, 0.73), broad);
        vec3 channelColor = mix(washColor, vec3(0.44, 0.55, 0.57), smoothstep(0.78, 1.0, vHydro.x));
        base = mix(base, channelColor, wet * 0.54);
    }

    // Cut faces are vertical: pure side-projected rock structure over the strata colors.
    if (cutFace > 0.5) {
        texDetail = mix(vec3(texLuma(rockT)), rockT, 0.35);
        dTotal = dSide;
    }
    base = clamp(base * texDetail * 2.15, 0.0, 1.5);

    // Apply the material relief to the shading normal.
    n = normalize(nGeom + dTotal * nStr);

    float diff = max(dot(n, lightDir), 0.0);
    float rim = pow(max(1.0 - dot(n, viewDir), 0.0), 2.2);
    float coldSheen = smoothstep(2.0, -8.0, surfaceTempC) * (snowMask + iceMask);
    float spec = pow(max(dot(n, halfDir), 0.0), mix(22.0, 112.0, wet + snowMask * 0.5 + iceMask)) * (wet + snowMask * 0.10 + iceMask * 0.45 + coldSheen * 0.08);
    float shade = mix(0.58, 1.0, smoothstep(-0.12, 0.36, dot(n, lightDir)));

    // Cast shadows: terrain self-shadowing + cloud shadows attenuate direct sun only.
    float sunVis = sunTerrainShadow(vWorldPos, lightDir) * cloudShadow(vWorldPos, lightDir);

    // Sky ambient follows the day/night clock, and the moon brightens the night in
    // proportion to its phase and elevation.
    float daylight = smoothstep(-0.08, 0.20, lightDir.y);
    vec3 md = normalize(vec3(-lightDir.x + 0.24, -lightDir.y + 0.16, -lightDir.z - 0.30));
    float litFrac = 0.5 - 0.5 * cos(6.2831853 * u.shadowParams.z);
    float moonAmb = litFrac * smoothstep(0.0, 0.25, md.y) * (1.0 - daylight);
    vec3 ambient = mix(vec3(0.020, 0.028, 0.048) * (0.7 + 2.4 * moonAmb), vec3(0.15, 0.18, 0.22), daylight);
    // slight extra skylight on upward-facing surfaces
    ambient *= 0.75 + 0.25 * clamp(n.y, 0.0, 1.0);

    vec3 color = base * (ambient + u.sunColor.xyz * diff * shade * sunVis);
    // faint directional moonlight so full-moon nights model the peaks
    color += base * vec3(0.085, 0.10, 0.15) * max(dot(n, md), 0.0) * moonAmb;
    color += u.sunColor.xyz * spec * 0.9 * sunVis;
    color += vec3(0.18, 0.25, 0.30) * rim * 0.045 * (0.3 + 0.7 * daylight);

    // Lightning flash as a real light source (cool white, distance falloff).
    if (u.lightning.w > 0.001) {
        vec3 toFlash = u.lightning.xyz - vWorldPos;
        float d2 = dot(toFlash, toFlash);
        float ndl = max(dot(n, normalize(toFlash)), 0.12);
        color += base * vec3(0.62, 0.72, 1.0) * (u.lightning.w * 2.4 * ndl / (1.0 + d2 * 0.0022));
    }

    // Fog with sun-tinted inscatter: haze glows warm when looking toward the sun.
    float fog = fogDensity <= 0.00001 ? 0.0 : clamp(1.0 - exp(-camDist * fogDensity), 0.0, 0.92);
    float sunAmount = max(dot(-viewDir, lightDir), 0.0);
    vec3 fogCol = mix(u.fogColor.xyz, u.sunColor.xyz, 0.55 * pow(sunAmount, 6.0));
    color = mix(color, fogCol, fog);

    color = aces(color * 1.25);
    outColor = vec4(color, 1.0);
}
