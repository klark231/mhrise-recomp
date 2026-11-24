#version 450
layout(set=0,binding=0) uniform sampler2D foliage;
layout(location=0) in vec2 uv;
#ifndef DEPTH_PREPASS
layout(location=0) out vec4 color;
#endif
void main() {
    vec4 value = texture(foliage,uv);
#ifdef DEPTH_PREPASS
    if(value.a < 0.5) discard;
#else
    color = vec4(value.rgb,1);
#endif
}
