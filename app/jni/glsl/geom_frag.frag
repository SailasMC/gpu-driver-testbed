#version 450
// 极轻片元：只写常量色（flat 插值，零额外成本）⇒ 瓶颈留给几何
layout(location = 0) flat in vec3 vColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vColor, 1.0); }
