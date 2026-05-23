#version 410 core

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
in vec2 vHydro;
in float vHeight;

uniform vec3 uCameraPos;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uAmbientColor;
uniform float uHeightScale;
uniform float uSnowLevel;
uniform float uWaterLevel;
uniform float uWaterTint;
uniform float uSedimentTint;
uniform float uShowWater;
uniform float uShowSediment;
uniform float uFogDensity;
uniform vec3 uFogColor;

out vec4 FragColor;

float hash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        sum += valueNoise(p) * amp;
        p *= 2.03;
        amp *= 0.5;
    }
    return sum;
}

void main()
{
    vec3 n = normalize(vNormal);
    vec3 l = normalize(uSunDir);
    vec3 v = normalize(uCameraPos - vWorldPos);
    vec3 h = normalize(l + v);

    float height01 = clamp(vHeight / max(uHeightScale, 0.001), 0.0, 1.0);
    float slope = clamp(1.0 - n.y, 0.0, 1.0);
    vec2 terrainUv = vWorldPos.xz;
    float broad = fbm(terrainUv * 0.055);
    float medium = fbm(terrainUv * 0.23);
    float fine = fbm(terrainUv * 1.65);
    float scratch = abs(valueNoise(terrainUv * vec2(0.75, 2.8)) - 0.5) * 2.0;
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
    float snowMask = smoothstep(uSnowLevel - 0.025, uSnowLevel + 0.075, height01)
                   * (1.0 - smoothstep(0.58, 0.9, slope));

    vec3 base = mix(grass, alpine, alpineMask);
    base = mix(base, rock, rockMask);
    base = mix(base, cliff, cliffMask);
    base = mix(base, snow, snowMask);
    base = mix(base, sediment, clamp(vHydro.y * uSedimentTint * uShowSediment, 0.0, 0.55));
    base *= 0.86 + fine * 0.22 + broad * 0.10;

    float roughness = mix(0.86, 0.74, max(alpineMask, rockMask));
    roughness = mix(roughness, 0.82, cliffMask);
    roughness = mix(roughness, 0.38, snowMask);

    float wet = clamp((vHydro.x * uWaterTint + smoothstep(uWaterLevel - 0.012, uWaterLevel + 0.006, uWaterLevel - height01)) * uShowWater, 0.0, 1.0);
    vec3 waterColor = mix(vec3(0.035, 0.11, 0.14), vec3(0.12, 0.28, 0.31), broad);
    base = mix(base, waterColor, wet * 0.78);

    float diff = max(dot(n, l), 0.0);
    float rim = pow(max(1.0 - dot(n, v), 0.0), 2.2);
    float specPower = mix(18.0, 110.0, wet + snowMask * 0.5) * max(0.25, 1.0 - roughness);
    float spec = pow(max(dot(n, h), 0.0), specPower) * (wet + snowMask * 0.11);
    float fakeShadow = mix(0.58, 1.0, smoothstep(-0.12, 0.36, dot(n, l)));

    vec3 color = base * (uAmbientColor + uSunColor * diff * fakeShadow);
    color += uSunColor * spec * 0.9;
    color += vec3(0.28, 0.42, 0.52) * rim * 0.12;

    float dist = length(uCameraPos - vWorldPos);
    float fog = 1.0 - exp(-dist * uFogDensity);
    fog = clamp(fog, 0.0, 0.92);
    color = mix(color, uFogColor, fog);

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
