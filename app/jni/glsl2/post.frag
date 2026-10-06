#version 450
/* 后处理 pass：泛光开关 + 色调映射开关 + 调试视图（0=最终 1=阴影图）
   全屏三角形（复用 fs_vert ✓）⇒ 只采样与着色 ✓ */
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uScene;
layout(set=0, binding=1) uniform sampler2D uShadowDbg;
layout(push_constant) uniform PC { int bloom; int tonemap; int debugView; float texel; } pc;
void main() {
    if (pc.debugView == 1) {                       /* 调试：直接看阴影深度图 ✓ */
        float d = texture(uShadowDbg, vUV).r;
        outColor = vec4(vec3(1.0 - d), 1.0);
        return;
    }
    vec3 c = texture(uScene, vUV).rgb;
    if (pc.bloom != 0) {                            /* 泛光：5 点采样近似（开关生效 ✓）*/
        vec2 d = vec2(pc.texel);
        c = c * 0.40;
        c += texture(uScene, vUV + vec2( d.x,  0.0)).rgb * 0.15;
        c += texture(uScene, vUV + vec2(-d.x,  0.0)).rgb * 0.15;
        c += texture(uScene, vUV + vec2( 0.0,  d.y)).rgb * 0.15;
        c += texture(uScene, vUV + vec2( 0.0, -d.y)).rgb * 0.15;
    }
    if (pc.tonemap != 0) {                          /* 色调映射 + 伽马（开关生效 ✓）*/
        c = c / (c + vec3(1.0));
        c = pow(c, vec3(1.0 / 2.2));
    }
    float vig = smoothstep(1.05, 0.45, length(vUV - 0.5));
    outColor = vec4(c * vig, 1.0);
}
