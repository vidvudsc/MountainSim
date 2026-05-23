#version 410 core

in vec3 vDirection;

uniform vec3 uSunDir;
uniform vec3 uHorizonColor;
uniform vec3 uZenithColor;
uniform vec3 uSunColor;
uniform samplerCube uSkybox;
uniform float uUseSkybox;

out vec4 FragColor;

void main()
{
    vec3 dir = normalize(vDirection);
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 gradientSky = mix(uHorizonColor, uZenithColor, pow(up, 1.35));
    vec3 cubeSky = texture(uSkybox, dir).rgb;
    vec3 sky = mix(gradientSky, cubeSky, uUseSkybox);
    float sunDisc = pow(max(dot(dir, normalize(uSunDir)), 0.0), 850.0);
    float sunGlow = pow(max(dot(dir, normalize(uSunDir)), 0.0), 12.0);
    sky += uSunColor * sunGlow * 0.22 + uSunColor * sunDisc * 2.6;
    sky = sky / (sky + vec3(1.0));
    sky = pow(sky, vec3(1.0 / 2.2));
    FragColor = vec4(sky, 1.0);
}
