#version 450
/* 后处理的全屏三角形顶点：**必须输出 vUV** ✓（原来的 fill 用全屏顶点不输出 uv ✗ 配不上 post.frag ✓）*/
layout(location = 0) out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vUV = p;                    /* p ∈ {0,1} ⇒ 插值出 0..1 的 uv ✓ */
}
