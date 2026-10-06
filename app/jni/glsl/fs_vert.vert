#version 450
// 全屏三角形：仅 3 个顶点覆盖整个 render target（比 quad 少一次对角重复着色）
// 无顶点输入、无 push constant、无 descriptor ⇒ 光栅化/填充测试的"干净"顶点级
void main() {
    // gl_VertexIndex = 0,1,2  ⇒  (-1,-1) (3,-1) (-1,3)
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
