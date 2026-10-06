#version 450
// 几何吞吐：N 个小三角形全部由 gl_VertexIndex 生成 ⇒ **没有顶点缓冲**，瓶颈落在图元装配/顶点
// 接口契约：push constant offset 0，size 4，类型 int，名字 triCount（顶点数 = triCount*3）
// 口径：三角形/秒 = draw 次数 × triCount ÷ 墙钟；另报 顶点/秒 与 每三角形像素数
layout(push_constant) uniform PC { int triCount; } pc;
layout(location = 0) flat out vec3 vColor;
void main() {
    int tri  = gl_VertexIndex / 3;
    int v    = gl_VertexIndex % 3;
    int side = int(ceil(sqrt(float(pc.triCount))));      // 摆成 side×side 网格
    int cx   = tri % side, cy = tri / side;
    float s  = 1.6 / float(side);                        // 每个三角形很小 ⇒ 测装配不测填充
    float ox = (float(cx) + 0.5) / float(side) * 2.0 - 1.0;
    float oy = (float(cy) + 0.5) / float(side) * 2.0 - 1.0;
    vec2 off[3] = vec2[3](vec2(0.0, -s), vec2(-s, s), vec2(s, s));
    gl_Position = vec4(ox + off[v].x, oy + off[v].y, 0.0, 1.0);
    vColor = vec3(float(cx & 3) * 0.25, float(cy & 3) * 0.25, 0.5);
}
