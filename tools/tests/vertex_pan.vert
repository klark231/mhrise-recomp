#version 450
layout(location=4) in vec4 camera;
layout(location=0) out vec2 uv;
void main() {
    uvec4 step = floatBitsToUint(camera);
    ivec2 p = ivec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vec2(p) * 2.0 - 1.0, 0.5, 1);
    float scale = exp2(float(step.y)) / 4.0;
    uv = vec2(p)*scale + vec2(float(step.x)*scale/16.0,0);
}
