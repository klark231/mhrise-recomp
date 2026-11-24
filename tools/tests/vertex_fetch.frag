#version 450
layout(location=0) flat in uvec4 words;
layout(location=0) out uvec4 result;
void main() { result = words; }
