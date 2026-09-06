#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec4 inPosScale;
layout(location = 4) in vec4 inRotType;
layout(location = 5) in vec4 inUvLayer;
layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view; mat4 proj; vec4 cameraPos; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 terrain; vec4 effects;
    mat4 invViewProj; vec4 volMin; vec4 volMax; vec4 cloudGrid; vec4 cloudParams; vec4 lightning; vec4 shadowParams;
    vec4 material; vec4 quality; mat4 lightViewProj;
} u;
layout(location = 0) out vec3 vUvLayer;
float hash2(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main()
{
    // Same instance transform as veg.vert (girth, lean, rotation, wind) so shadows match.
    float s = inPosScale.w;
    float c = cos(inRotType.x), sn = sin(inRotType.x);
    vec3 p = inPosition * s;
    float t0 = inRotType.y;
    bool treeLike = (t0 < 1.5) || (t0 > 9.5 && t0 < 10.5);
    if (treeLike) {
        float h1 = hash2(inPosScale.xz + 3.1);
        float h2 = hash2(inPosScale.zx * 1.7 + 9.3);
        p.xz *= 0.72 + 0.7 * h1;
        float h01 = clamp(inPosition.y * 60.0, 0.0, 1.0);
        float lean = (h2 - 0.5) * 0.22 * h01;
        float la = h1 * 6.2831853;
        vec3 axis = vec3(cos(la), 0.0, sin(la));
        float cl = cos(lean), sl = sin(lean);
        p = p * cl + cross(axis, p) * sl + axis * dot(axis, p) * (1.0 - cl);
    }
    vec3 rp = vec3(c * p.x + sn * p.z, p.y, -sn * p.x + c * p.z);
    vec3 world = inPosScale.xyz + rp;
    {
        float time = u.quality.w;
        vec2 windDir = normalize(vec2(0.8, 0.45));
        float phase = dot(inPosScale.xz, windDir) * 0.35;
        float gust = 0.55 + 0.45 * sin(time * 0.7 - phase) * sin(time * 0.23 - phase * 0.3 + 1.7);
        float h01 = clamp(inPosition.y * 60.0, 0.0, 1.0);
        float t = inRotType.y;
        float amp, flutter;
        if (t < 1.5) { amp = (s / 60.0) * 0.025; flutter = sin(time * 2.1 + hash2(inPosScale.xz) * 6.28 + inPosition.y * 60.0 * 0.4) * 0.25; }
        else if ((t > 3.5 && t < 5.5) || (t > 6.5 && t < 7.5)) { amp = 0.0045; flutter = sin(time * 3.3 + hash2(inPosScale.xz) * 6.28) * 0.6; }
        else { amp = 0.0; flutter = 0.0; }
        float bend = h01 * h01 * amp * (gust + flutter);
        world.xz += windDir * bend;
        if (t < 1.5) world.y -= bend * 0.3;
    }
    vUvLayer = inUvLayer.xyz;
    gl_Position = u.lightViewProj * vec4(world, 1.0);
}
