#version 450
// 与 fill_frag.frag 等价，但 push constant 用 float（若原生侧 steps 是 float 就用这个）
layout(push_constant) uniform PC { float steps; } pc;
layout(location = 0) out vec4 outColor;
void main() {
    float a = gl_FragCoord.x * 1e-4 + gl_FragCoord.y * 1e-4;
    float n = pc.steps;
    for (int i = 0; float(i) < n; ++i) {
        a = fma(a, 1.000001, 1e-7);
    }
    outColor = vec4(a, a * 0.5, 1.0 - a, 1.0);
}
