#version 450
#ifdef UINT_TO_FLOAT_REFERENCE
layout(location=4) in uvec4 attribute_value;
#else
layout(location=4) in vec4 attribute_value;
#endif
layout(location=0) flat out uvec4 words;
void main() {
    ivec2 p = ivec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vec2(p)*2.0-1.0, 0, 1);
    words = floatBitsToUint(vec4(attribute_value));
}
