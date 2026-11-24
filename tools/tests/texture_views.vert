#version 450
void main() {
    ivec2 p = ivec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vec2(p) * 2.0 - 1.0, 0, 1);
}
