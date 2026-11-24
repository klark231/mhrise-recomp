#version 450
#ifdef SAMPLE_COPY
layout(set=0,binding=0) uniform sampler2D color_image;
layout(set=0,binding=1) uniform sampler2D depth_image;
layout(set=0,binding=2) uniform usampler2D stencil_image;
layout(location=0) out vec4 color;
layout(location=1) out vec4 sampled_ds;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    color = texelFetch(color_image, p, 0);
    sampled_ds = vec4(texelFetch(depth_image, p, 0).r, float(texelFetch(stencil_image, p, 0).r) / 255.0, 0, 1);
}
#elif defined(SECOND_OUTPUT)
layout(location=1) out vec4 sibling;
void main() {
    sibling = vec4(0, 1, 0, 1);
}
#else
layout(location=0) out vec4 color;
void main() {
    color = vec4(1, 0, 0, .5);
}
#endif
