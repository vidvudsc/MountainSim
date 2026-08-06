#version 450

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

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
    vec4 shadowParams; // z = moon phase (0 new, 0.5 full)
} u;

float hash13(vec3 p)
{
    p = fract(p * 443.8975);
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec2 ndc = vUv * 2.0 - 1.0;
    vec4 nh = u.invViewProj * vec4(ndc, 0.0, 1.0);
    vec4 fh = u.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 rd = normalize(fh.xyz / fh.w - nh.xyz / nh.w);
    vec3 sd = normalize(u.sunDir.xyz);

    float daylight = smoothstep(-0.10, 0.12, sd.y);
    float night = 1.0 - daylight;
    float duskband = exp(-abs(sd.y) * 7.0); // peaks when the sun crosses the horizon
    float mu = dot(rd, sd);

    // Saturated day palette (kept well below 1.0 so ACES doesn't wash it out).
    vec3 zenith = mix(vec3(0.008, 0.013, 0.030), vec3(0.085, 0.235, 0.560), daylight);
    vec3 horizon = mix(vec3(0.030, 0.042, 0.070), vec3(0.355, 0.500, 0.685), daylight);

    // Dusk/dawn: deep orange band near the horizon on the sun's side, faint purple aloft.
    float sunSide = clamp(mu * 0.5 + 0.5, 0.0, 1.0);
    horizon = mix(horizon, vec3(0.98, 0.30, 0.06), duskband * (0.20 + 0.65 * sunSide));
    zenith = mix(zenith, vec3(0.14, 0.075, 0.16), duskband * 0.40);

    float up = clamp(rd.y, 0.0, 1.0);
    vec3 sky = mix(horizon, zenith, pow(up, 0.50));
    // Dim the whole dome as the sun gets low so dusk reads dark and saturated.
    sky *= 1.0 - 0.55 * daylight * (1.0 - smoothstep(0.02, 0.32, sd.y));

    // Mie halo + sun disc (goes deep orange and dimmer at the horizon).
    sky += u.sunColor.xyz * pow(max(mu, 0.0), 9.0) * (0.06 + 0.28 * duskband);
    float disc = smoothstep(0.99964, 0.99993, mu) * smoothstep(-0.015, 0.02, sd.y);
    vec3 discCol = mix(u.sunColor.xyz * 2.4, vec3(1.0, 0.30, 0.05) * 1.1, duskband);
    sky += discCol * disc;

    // Moon direction and illuminated fraction (used by the void gradient below too).
    vec3 md = normalize(vec3(-sd.x + 0.24, -sd.y + 0.16, -sd.z - 0.30));
    float phase = u.shadowParams.z;
    float litFrac = 0.5 - 0.5 * cos(6.2831853 * phase); // 0 new .. 1 full
    float mcos = dot(rd, md);
    float moonVis = smoothstep(-0.02, 0.08, md.y) * (0.20 + 0.80 * night);

    // Below the horizon: the void gets its own gradient (fog-toned at the seam, deepening
    // downward), a soft sun glow by day, and a cool moon glow at night.
    float belowT = clamp(-rd.y, 0.0, 1.0);
    vec3 deep = mix(vec3(0.010, 0.014, 0.024), vec3(0.115, 0.150, 0.200), daylight);
    deep = mix(deep, vec3(0.28, 0.12, 0.05), duskband * sunSide * 0.35);
    vec3 belowCol = mix(u.fogColor.xyz, deep, smoothstep(0.0, 0.40, belowT));
    belowCol += u.sunColor.xyz * pow(max(mu, 0.0), 10.0) * 0.12 * (1.0 - belowT);
    belowCol += vec3(0.16, 0.19, 0.28) * pow(max(mcos, 0.0), 30.0) * litFrac * night * 0.5;
    sky = mix(sky, belowCol, smoothstep(0.030, -0.030, rd.y));

    // Moonlight halo: the lit moon brightens the sky around it (scales with phase).
    sky += vec3(0.30, 0.36, 0.50) * pow(max(mcos, 0.0), 600.0) * litFrac * moonVis * night;
    sky += vec3(0.10, 0.12, 0.18) * pow(max(mcos, 0.0), 45.0) * litFrac * moonVis * night * 0.55;

    // Moon disc: only the sunlit part is bright; the dark limb is a faint earthshine
    // ghost rather than an opaque grey ball.
    if (mcos > 0.99978 && md.y > -0.02) {
        vec3 axis = abs(md.y) > 0.95 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        vec3 e1 = normalize(cross(md, axis));
        vec3 e2 = cross(e1, md);
        vec2 mc = vec2(dot(rd, e1), dot(rd, e2)) / 0.0148; // unit disc coords
        float r = length(mc);
        float discM = smoothstep(1.0, 0.82, r);
        float c = cos(6.2831853 * phase); // 1 = new, -1 = full
        vec2 toSun = normalize(vec2(dot(sd, e1), dot(sd, e2)) + vec2(1e-4, 0.0));
        float lit = smoothstep(c - 0.22, c + 0.22, dot(mc, toSun));
        // subtle surface mottling so the disc isn't a flat white circle
        float mare = 0.85 + 0.15 * hash13(floor(vec3(mc * 3.0, 1.0)));
        sky = mix(sky, vec3(0.88, 0.90, 0.94) * mare, discM * moonVis * lit);
        sky += vec3(0.045, 0.050, 0.065) * discM * moonVis * (1.0 - lit) * night; // earthshine
    }

    // Stars, fixed in world directions, only after dark — dimmed but present below the
    // horizon too, so the night void reads as space instead of a flat sheet.
    if (night > 0.05) {
        float s = hash13(floor(rd * 420.0));
        float star = smoothstep(0.9985, 1.0, s);
        star *= mix(0.55, 1.0, smoothstep(-0.08, 0.06, rd.y));
        sky += vec3(0.85, 0.9, 1.0) * star * night * (0.35 + 0.65 * hash13(floor(rd * 420.0) + 7.0));
    }

    outColor = vec4(aces(sky), 1.0);
}
