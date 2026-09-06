#version 450
layout(location = 0) in vec3 vUvLayer;
layout(set = 0, binding = 7) uniform sampler2DArray foliageTex;
void main()
{
    if (vUvLayer.z >= 0.0) {
        float a = texture(foliageTex, vUvLayer).a;
        if (a < 0.45) discard;
    }
}
