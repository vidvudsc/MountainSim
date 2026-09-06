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
    vec4 material;     // x macro tile, y mid tile, z near tile (world units), w terrain grid res
    vec4 quality;      // x textures, y terrain shadow, z cloud shadow
} u;

// Shared with the cloud pass: cloud water volume + terrain height, for shadow marches.
layout(set = 0, binding = 2) uniform sampler3D cloudTex;
layout(set = 0, binding = 3) uniform sampler2D heightTex;
// Photo materials (Poly Haven CC0): layers rock, scree, grass, forest, snow, marsh.
layout(set = 0, binding = 4) uniform sampler2DArray matAlbedo;
layout(set = 0, binding = 5) uniform sampler2DArray matNormal;
// Ecology map: r flow accumulation, g curvature (0.5 flat), b forest density, a sky view.
layout(set = 0, binding = 6) uniform sampler2D ecoTex;

layout(location = 0) out vec4 outColor;

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
float fbm3(vec2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i) {
        sum += noise(p) * amp;
        p *= 2.03;
        amp *= 0.5;
    }
    return sum / 0.875;
}

// ---- textured materials ------------------------------------------------------------
float gNearW = 0.0;
float gMidW = 0.0;

// Two-tap stochastic anti-tiling (Quilez); v is a slow-varying 0..1 field.
vec4 sampleNT(sampler2DArray t, vec2 uv, float layer, float v)
{
    float l = v * 8.0;
    float f = fract(l);
    float ia = floor(l);
    float ib = ia + 1.0;
    vec2 offa = sin(vec2(3.0, 7.0) * ia);
    vec2 offb = sin(vec2(3.0, 7.0) * ib);
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    vec4 ca = textureGrad(t, vec3(uv + offa, layer), dx, dy);
    vec4 cb = textureGrad(t, vec3(uv + offb, layer), dx, dy);
    vec3 dd = ca.rgb - cb.rgb;
    return mix(ca, cb, smoothstep(0.2, 0.8, f - 0.1 * (dd.x + dd.y + dd.z)));
}

// One planar projection. Budget: anti-tiled macro albedo (2 taps), single detail albedo,
// single macro normal; mid and near tiles only within their camera ranges.
vec3 planar(float layer, vec2 uv, float v, out vec2 tn)
{
    vec2 uvM = uv / u.material.x;
    vec2 uvD = uv / (u.material.x * 0.25);
    vec3 cM = sampleNT(matAlbedo, uvM, layer, v).rgb;
    vec3 cD = texture(matAlbedo, vec3(uvD + sin(vec2(3.0, 7.0) * floor(v * 8.0)), layer)).rgb;
    vec3 nM = texture(matNormal, vec3(uvM, layer)).rgb * 2.0 - 1.0;
    tn = nM.xy * 0.85;
    vec3 c = mix(cM, cD, 0.25);
    if (gMidW > 0.01) {
        vec2 uvMid = uv / u.material.y;
        vec3 cMid = sampleNT(matAlbedo, uvMid, layer, v * 3.1 + 0.83).rgb;
        vec3 nMid = texture(matNormal, vec3(uvMid, layer)).rgb * 2.0 - 1.0;
        c = mix(c, c * (cMid / max(vec3(0.30), vec3(dot(cMid, vec3(0.33))))), 0.7 * gMidW);
        tn += nMid.xy * 0.5 * gMidW;
    }
    if (gNearW > 0.01) {
        vec2 uvN = uv / u.material.z;
        vec3 cN = sampleNT(matAlbedo, uvN, layer, v * 2.3 + 0.57).rgb;
        vec3 nN = texture(matNormal, vec3(uvN, layer)).rgb * 2.0 - 1.0;
        c = mix(c, c * (cN / max(vec3(0.30), vec3(dot(cN, vec3(0.33))))), 0.85 * gNearW);
        tn += nN.xy * 0.8 * gNearW;
    }
    return c;
}

// Triplanar: top projection on gentle ground, side projections on steep faces so
// textures stop smearing into vertical streaks.
vec3 material(float layer, vec3 p, vec3 gN, float v, out vec2 tn)
{
    if (u.quality.x < 0.5) { tn = vec2(0.0); return vec3(0.35, 0.36, 0.30); }
    vec3 w = pow(abs(gN), vec3(6.0));
    w /= (w.x + w.y + w.z);
    vec3 col = vec3(0.0);
    vec2 tAcc = vec2(0.0);
    // Drop side projections under 12% so gentle ground pays for one projection only.
    if (w.x < 0.12) w.x = 0.0;
    if (w.z < 0.12) w.z = 0.0;
    w /= max(w.x + w.y + w.z, 0.0001);
    if (w.y > 0.0) { vec2 t; col += planar(layer, p.xz, v, t) * w.y; tAcc += t * w.y; }
    if (w.x > 0.0) { vec2 t; col += planar(layer, vec2(p.z, p.y), v + 0.13, t) * w.x; tAcc += vec2(t.x, 0.0) * w.x * 0.5; }
    if (w.z > 0.0) { vec2 t; col += planar(layer, vec2(p.x, p.y), v + 0.27, t) * w.z; tAcc += vec2(0.0, t.x) * w.z * 0.5; }
    tn = tAcc;
    return col;
}

vec4 ecoAt(vec2 xz)
{
    float R = u.material.w;
    vec2 rel = clamp((xz + WS * 0.5) / WS, 0.0, 1.0);
    vec2 tc = (rel * (R - 1.0) + 0.5) / R;
    return texture(ecoTex, tc);
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
    if (u.shadowParams.x < 0.5 || sd.y <= 0.03 || u.quality.y < 0.5) return 1.0;
    float res = 1.0;
    float t = 2.2;
    for (int i = 0; i < 14; ++i) {
        vec3 p = wp + sd * t;
        if (p.y > u.terrain.x + 8.0) break; // above any possible terrain
        float h = terrainH(p.xz);
        res = min(res, 6.0 * (p.y - h + 0.9) / t);
        if (res < 0.0) break;
        t += clamp(t * 0.48, 1.4, 10.0);
    }
    // Fade out at grazing sun angles: the coarse heightmap aliases into dappled noise
    // there, and direct light is nearly gone at the horizon anyway.
    return mix(1.0, clamp(res, 0.0, 1.0), smoothstep(0.03, 0.14, sd.y));
}

// Cloud shadow: accumulate cloud water along the sun ray up to the volume lid.
float cloudShadow(vec3 wp, vec3 sd)
{
    float strength = u.shadowParams.y;
    if (strength <= 0.0 || sd.y <= 0.05 || u.quality.z < 0.5) return 1.0;
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

// Per-layer colour grading, shared by whichever two layers won the blend.
vec3 tintLayer(int layer, vec3 c, vec2 p, float height01, float drainage, float medium, float convex, float wet)
{
    if (layer == 0) {          // rock: grey with a cool cast, strata bands keep a little warmth
        float strata = fbm3(vec2(p.x * 0.08, height01 * 24.0));
        float rl = dot(c, vec3(0.3, 0.59, 0.11));
        c = mix(vec3(rl), c, 0.35) * vec3(0.86, 0.88, 0.92);
        c *= mix(0.80, 1.16, strata);
        c = mix(c, c * vec3(1.06, 1.0, 0.94), strata * 0.3);
    } else if (layer == 1) {   // scree
        float sl = dot(c, vec3(0.3, 0.59, 0.11));
        c = mix(vec3(sl), c, 0.30) * vec3(0.80, 0.83, 0.86) * mix(1.0, 0.88, smoothstep(0.5, 0.8, height01));
    } else if (layer == 2) {   // meadow: straw where dry, green in hollows
        float dry = smoothstep(0.35, 0.75, medium * 0.6 + convex * 0.3 + height01 * 0.4 - wet * 0.5);
        vec3 tint = mix(vec3(0.86, 0.98, 0.70), vec3(0.92, 0.86, 0.60), dry);
        float gl = dot(c, vec3(0.3, 0.59, 0.11));
        c = mix(vec3(gl), c, 0.78) * tint;
    } else if (layer == 3) {   // forest floor under canopy
        float canopy = fbm3(p * 0.45);
        c *= vec3(0.22, 0.36, 0.17) * (0.70 + 0.6 * canopy) * (1.0 - drainage * 0.25);
    } else if (layer == 4) {   // snow
        c *= 1.05;
    } else {                   // marsh
        c *= vec3(0.95, 0.98, 0.85);
    }
    return c;
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
    vec3 gN = n;
    float slope = clamp(1.0 - gN.y, 0.0, 1.0);
    vec2 p = vWorldPos.xz;
    float camDist = length(u.cameraPos.xyz - vWorldPos);
    gNearW = 1.0 - smoothstep(4.0, 16.0, camDist);
    gMidW = 1.0 - smoothstep(18.0, 70.0, camDist);

    float broad = fbm(p * 0.055);
    float medium = fbm(p * 0.23);
    float fine = fbm3(p * 1.65);
    float ntVar = fbm3(p * 0.09);

    vec4 eco = ecoAt(p);
    float flow = eco.r;
    float curv = eco.g;
    float forestDensity = eco.b;
    float svf = eco.a;
    float convex = clamp((0.5 - curv) * 2.0, 0.0, 1.0);
    float concave = clamp((curv - 0.5) * 2.0, 0.0, 1.0);

    float materialId = vSurface.x;
    float snowMass = clamp(vSurface.y, 0.0, 1.0);
    float iceMask = clamp(vSurface.z, 0.0, 1.0);
    float surfaceTempC = vSurface.w;

    float horiz = length(gN.xz);
    float northness = (horiz > 0.0001) ? (-gN.z / horiz) : 0.0;
    northness *= clamp(slope * 8.0, 0.0, 1.0);

    float wet = flow * (1.0 - smoothstep(0.06, 0.20, slope));
    float drainage = smoothstep(0.35, 0.75, flow);
    // Masks. Rock on steep and convex ground; scree below cliffs and above the meadows;
    // forest from the ecology map; marsh where water collects on flat ground; snow from
    // the surface state, shed from steep faces and held in gullies.
    float rock = smoothstep(0.30, 0.48, slope + convex * 0.08 + (fine - 0.5) * 0.10);
    float scree = (1.0 - rock) * smoothstep(0.14, 0.28, slope) * smoothstep(0.28, 0.55, height01 + (broad - 0.5) * 0.12);
    scree = max(scree, (1.0 - rock) * smoothstep(0.50, 0.72, height01 + (medium - 0.5) * 0.10) * 0.75);
    float forest = smoothstep(0.10, 0.55, forestDensity) * (1.0 - rock);
    float marsh = smoothstep(0.42, 0.80, flow) * (1.0 - smoothstep(0.02, 0.07, slope)) * (1.0 - forest * 0.6);
    float snow = snowMass * (1.0 - smoothstep(0.42, 0.62, slope + (fine - 0.5) * 0.08 - concave * 0.06));
    snow = max(snow, snowMass * concave * 0.5);
    snow = clamp(snow + smoothstep(0.88, 0.98, height01) * (1.0 - smoothstep(0.55, 0.75, slope)) * 0.6, 0.0, 1.0);

    // Layered "over" stack converted to weights, then only the two heaviest materials are
    // sampled: the texture taps were the biggest cost in the frame.
    float wSnow = snow;
    float wRock = rock * (1.0 - snow);
    float wScree = scree * (1.0 - rock) * (1.0 - snow);
    float wForest = forest * (1.0 - scree) * (1.0 - rock) * (1.0 - snow);
    float wMarsh = marsh * (1.0 - forest) * (1.0 - scree) * (1.0 - rock) * (1.0 - snow);
    float wGrass = max(0.0, 1.0 - wSnow - wRock - wScree - wForest - wMarsh);
    float wts[6] = float[6](wRock, wScree, wGrass, wForest, wSnow, wMarsh);   // layer order
    int a = 0; float wa = -1.0;
    for (int i = 0; i < 6; ++i) if (wts[i] > wa) { wa = wts[i]; a = i; }
    int b = -1; float wb = 0.0;
    for (int i = 0; i < 6; ++i) if (i != a && wts[i] > wb) { wb = wts[i]; b = i; }
    if (wb < 0.06) { b = -1; wb = 0.0; }
    float fb = wb / max(wa + wb, 0.0001);

    vec2 tn;
    vec2 tnAcc;
    vec3 base = material(float(a), vWorldPos, gN, ntVar, tnAcc);
    base = tintLayer(a, base, p, height01, drainage, medium, convex, wet);
    if (b >= 0) {
        vec3 cb = material(float(b), vWorldPos, gN, ntVar, tn);
        cb = tintLayer(b, cb, p, height01, drainage, medium, convex, wet);
        base = mix(base, cb, fb);
        tnAcc = mix(tnAcc, tn, fb);
    }
    base = mix(base, vec3(0.70, 0.86, 0.96), iceMask * 0.72);
    // Cavity shading and wet ground.
    base *= 1.0 - concave * 0.16 + convex * 0.05;
    base *= 1.0 - drainage * 0.10 * (1.0 - snow);
    base *= 1.0 - wet * 0.20 * (1.0 - snow);
    // Texture normals perturb the shading normal (UDN blend).
    n = normalize(vec3(gN.x + tnAcc.x * 0.55, gN.y, gN.z + tnAcc.y * 0.55));
    halfDir = normalize(lightDir + viewDir);

    // Cut faces of the box: rock texture side-projected under strata bands.
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
    {
        float along = dot(vWorldPos.xz, vec2(abs(gN.z), abs(gN.x)));
        vec3 rockTex = texture(matAlbedo, vec3(vec2(along, vWorldPos.y) / (u.material.x * 0.35), 0.0)).rgb;
        float rl = dot(rockTex, vec3(0.3, 0.59, 0.11));
        cutMaterial *= mix(0.7, 1.35, rl);
    }
    float below = max(terrainH(vWorldPos.xz) - vWorldPos.y, 0.0);
    cutMaterial = mix(vec3(0.135, 0.112, 0.085), cutMaterial, smoothstep(0.4, 3.0, below));
    cutMaterial = mix(cutMaterial, vec3(0.08, 0.085, 0.08), bottomFace);

    vec3 sediment = mix(vec3(0.42, 0.36, 0.27), vec3(0.70, 0.64, 0.51), medium);
    float wetWash = 0.0;
    if (cutFace > 0.5) {
        base = cutMaterial;
        n = gN;
        svf = 0.6;
        forestDensity = 0.0;
    } else {
        base = mix(base, sediment, clamp(vHydro.y * sedimentTint * showSediment, 0.0, 0.46));
        float routeWater = smoothstep(0.22, 0.92, vHydro.x) * waterTint;
        float pocketWater = smoothstep(waterLevel - 0.006, waterLevel + 0.003, waterLevel - height01)
                          * smoothstep(0.48, 0.82, vHydro.x) * 0.18;
        wetWash = clamp((routeWater + pocketWater) * showWater, 0.0, 1.0);
        // Water reads blue: shallow wash is a muted slate blue, pools go deeper teal-blue.
        vec3 washColor = mix(vec3(0.30, 0.42, 0.52), vec3(0.40, 0.54, 0.64), broad);
        vec3 channelColor = mix(washColor, vec3(0.14, 0.32, 0.48), smoothstep(0.55, 1.0, vHydro.x));
        base = mix(base, channelColor, wetWash * mix(0.55, 0.85, smoothstep(0.5, 1.0, vHydro.x)));
    }
    wet = max(wet * 0.6, wetWash);
    float snowMask = snow;

    float diff = max(dot(n, lightDir), 0.0);
    float rim = pow(max(1.0 - dot(n, viewDir), 0.0), 2.2);
    float coldSheen = smoothstep(2.0, -8.0, surfaceTempC) * (snowMask + iceMask);
    float spec = pow(max(dot(n, halfDir), 0.0), mix(22.0, 112.0, wet + snowMask * 0.5 + iceMask)) * (wet + snowMask * 0.10 + iceMask * 0.45 + coldSheen * 0.08);
    float shade = mix(0.58, 1.0, smoothstep(-0.12, 0.36, dot(n, lightDir)));

    // Cast shadows: terrain self-shadowing + cloud shadows attenuate direct sun only.
    float sunVis = sunTerrainShadow(vWorldPos, lightDir) * cloudShadow(vWorldPos, lightDir);
    // Forest canopy blocks most direct sun and part of the sky on the floor beneath it.
    float canopyBlock = smoothstep(0.08, 0.45, forestDensity);
    sunVis *= 1.0 - canopyBlock * 0.72;
    svf *= 1.0 - canopyBlock * 0.45;

    // Sky ambient follows the day/night clock, and the moon brightens the night in
    // proportion to its phase and elevation.
    float daylight = smoothstep(-0.08, 0.20, lightDir.y);
    vec3 md = normalize(vec3(-lightDir.x + 0.24, -lightDir.y + 0.16, -lightDir.z - 0.30));
    float litFrac = 0.5 - 0.5 * cos(6.2831853 * u.shadowParams.z);
    float moonAmb = litFrac * smoothstep(0.0, 0.25, md.y) * (1.0 - daylight);
    vec3 ambient = mix(vec3(0.020, 0.028, 0.048) * (0.7 + 2.4 * moonAmb), vec3(0.15, 0.18, 0.22), daylight);
    // slight extra skylight on upward-facing surfaces
    ambient *= (0.75 + 0.25 * clamp(n.y, 0.0, 1.0)) * mix(0.55, 1.0, svf);

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
    float dist = length(u.cameraPos.xyz - vWorldPos);
    fogDensity += forestDensity * 0.0025;   // forest haze under canopy, matched in veg.frag
    float fog = fogDensity <= 0.00001 ? 0.0 : clamp(1.0 - exp(-dist * fogDensity), 0.0, 0.92);
    float sunAmount = max(dot(-viewDir, lightDir), 0.0);
    vec3 fogCol = mix(u.fogColor.xyz, u.sunColor.xyz, 0.55 * pow(sunAmount, 6.0));
    color = mix(color, fogCol, fog);

    color = aces(color * 1.25);
    outColor = vec4(color, 1.0);
}
