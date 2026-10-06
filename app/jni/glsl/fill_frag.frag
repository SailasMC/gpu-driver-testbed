#version 450
// 填充率 + ALU 阶梯：每像素 pc.steps 次 FMA（2 FLOP/次），且每轮依赖上一轮 + gl_FragCoord
// ⇒ 编译器无法折叠（这是"可放大负载"的关键）
// 接口契约：push constant，set 无关，offset 0，size 4，类型 int，名字 steps
// 口径：GFLOP/s = 像素/秒 × steps × 2 ÷ 1e9      （另报 Mpixel/s = 像素/秒 ÷ 1e6）
layout(push_constant) uniform PC { int steps; } pc;
layout(location = 0) out vec4 outColor;
void main() {
    float a = gl_FragCoord.x * 1e-4 + gl_FragCoord.y * 1e-4;
    for (int i = 0; i < pc.steps; ++i) {
        a = fma(a, 1.000001, 1e-7);      // 1 FMA = 2 FLOP
    }
    outColor = vec4(a, a * 0.5, 1.0 - a, 1.0);
}
