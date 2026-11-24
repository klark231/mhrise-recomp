#version 450
#ifdef READ_BITS
layout(set = 0, binding = 0) uniform usampler2D source_image;
layout(location = 0) out vec4 color;
void main() {
    color = texelFetch(source_image, ivec2(2), 0).r == 0x3800u ? vec4(0,1,0,1) : vec4(1,0,0,1);
}
#else
layout(location = 0) out uint bits;
void main() {
    bits = 0x3800u;
}
#endif
