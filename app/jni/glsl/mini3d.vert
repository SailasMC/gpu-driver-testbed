#version 450
/* v9.14 · 全屏三角形：最不可能失败的顶点着色器 ——
 * 不依赖顶点缓冲、不依赖矩阵、不依赖 push constant，顶点位置由 gl_VertexIndex 算出 ✓
 * 真正的 3D 立方体在片元着色器里用解析求交画出来（见 mini3d.frag）*/
layout(location = 0) out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    vUV = p * 0.5 + 0.5;
    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);   /* ★ Vulkan 的 NDC **y 向下**（与 GL 相反）⇒ 取反，否则画面上下颠倒 ✓ */
}
