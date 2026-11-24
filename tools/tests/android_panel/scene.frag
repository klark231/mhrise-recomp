#version 450
layout(set=0, binding=0) uniform sampler2D mask;
#ifndef DEPTH_PREPASS
layout(location=0) out vec4 color;
#endif
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) % textureSize(mask, 0);
    vec4 texel = texelFetch(mask, p, 0);
#ifdef DEPTH_PREPASS
    if (texel.a < 0.5) discard;
#else
    color = vec4(texel.rgb, 0.5);
#endif
}
