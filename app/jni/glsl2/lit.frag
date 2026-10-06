#version 450
/* 光照 pass：动态 PCF（采数由 push constant 决定 ✓ 对应面板"PCF 质量"开关）
   阴影开关也在着色器里生效 ✓；
   **输出线性值，不做色调映射/伽马** —— 那两件事交给 post pass，
   这样"色调映射"与"泛光"开关才是真的 ✓ */
layout(location=0) in vec3 vWorld;
layout(location=1) in vec3 vNormal;
layout(location=2) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2DShadow uShadow;
layout(set=0, binding=1) uniform sampler2D uAlbedo;
layout(push_constant) uniform PC {
    mat4 mvp; mat4 lightVP; int nInst; float t;
    int pcf; int shadows; int shadowRes; int pad;
} pc;
float shadow(vec4 lp) {
    if (pc.shadows == 0) return 1.0;
    vec3 c = lp.xyz / lp.w * 0.5 + 0.5;
    if (c.z > 1.0) return 1.0;
    int n = pc.pcf; if (n < 1) n = 1; if (n > 5) n = 5;
    int h = n / 2;
    float texel = 1.0 / float(max(pc.shadowRes, 512));
    float s = 0.0; int cnt = 0;
    for (int y = -h; y <= h; y++)
        for (int x = -h; x <= h; x++) {
            s += texture(uShadow, vec3(c.xy + vec2(x, y) * texel, c.z - 0.0018));
            cnt++;
        }
    return s / float(cnt);
}
void main() {
    vec3 N = normalize(vNormal), L = normalize(vec3(0.6, 1.0, 0.4));
    vec3 albedo = texture(uAlbedo, vUV).rgb * vec3(0.85, 0.88, 0.95);
    float sh = shadow(pc.lightVP * vec4(vWorld, 1.0));
    float diff = max(dot(N, L), 0.0) * sh;
    vec3 V = normalize(vec3(0.4, 1.2, 0.9) - vWorld), H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 48.0) * sh;
    vec3 col = albedo * (0.12 + 0.88 * diff) + vec3(0.6) * spec;
    outColor = vec4(col, 1.0);          /* 线性 ✓ 色调映射交给 post ✓ */
}
