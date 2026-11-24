#version 450
layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(location = 0) out vec4 color;
void main() {
#ifdef FILTER_SAMPLE
    color = textureLod(source_image, vec2(0.5), 0.0);
#else
    color = texelFetch(source_image, ivec2(2), 0);
#endif
}
