#version 450
/* v9.16 · 跑分风格场景：兵马俑大厅（安兔兔 GPU 测试的招牌场景风格 —— 同风格自建，非其资产）
 *   场景：大厅（抛光地面 + 侧墙 + 顶灯）+ 4×6 = 24 尊程序化"陶俑"（基座+身体胶囊+头球+肩箱）
 *   光照：两盏暖色顶灯 + 定向主光；**逐雕像解析遮挡的阴影**
 *   地面：**反射**（命中地面就再追一条反射光线 ⇒ 抛光大理石观感）
 *   镜头：**沿大厅纵深飞行**（像跑分那样穿过场景，不是围着转）
 *   后处理：体积雾感 + 暗角 + sRGB
 *   诊断保留：管线没跑起来时整屏是清屏色(R=13) ⇒ 日志像素统计仍是全 13 ✓ */
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 oCol;
layout(push_constant) uniform PC { mat4 mvp; float t; float _pad[3]; int set[8]; } pc;   /* v9.28 */

#define NI 24          /* 雕像数（4 列 × 6 行，常量边界 ⇒ 驱动友好 ✓）*/
#define NSH 8          /* 阴影抖动采样数 */

const float HALF_W = 5.2, HALF_L = 7.4, CEIL = 5.0;
const vec3  TERRA = vec3(0.64, 0.37, 0.24);      /* 陶土色 */
const vec3  LDIR  = normalize(vec3(0.35, 0.86, 0.36));

float iSphere(vec3 ro, vec3 rd, vec3 c, float r) {
    vec3 oc = ro - c; float b = dot(oc, rd), k = dot(oc, oc) - r * r, h = b * b - k;
    if (h < 0.0) return -1.0; h = sqrt(h);
    float t = -b - h; return (t > 0.0) ? t : ((-b + h > 0.0) ? -b + h : -1.0);
}
float iBox(vec3 ro, vec3 rd, vec3 c, vec3 hs, mat2 ry) {
    vec3 o = ro - c; o.xz = ry * o.xz; vec3 d = rd; d.xz = ry * d.xz;
    vec3 inv = 1.0 / d;
    vec3 t0 = (-hs - o) * inv, t1 = (hs - o) * inv;
    vec3 tn = min(t0, t1), tf = max(t0, t1);
    float a = max(max(tn.x, tn.y), tn.z), b = min(min(tf.x, tf.y), tf.z);
    if (a > b || b < 0.0) return -1.0;
    return (a > 0.0) ? a : b;
}
float iCapsule(vec3 ro, vec3 rd, vec3 a, vec3 b, float r) {   /* 竖直胶囊：两球 + 无限柱 */
    float t = min(iSphere(ro, rd, a, r), iSphere(ro, rd, b, r));
    float tbest = t > 0.0 ? t : 1e9;
    vec2 o = ro.xz - a.xz, d = rd.xz;
    float A = dot(d, d), B = dot(o, d), C = dot(o, o) - r * r;
    if (A > 1e-9) {
        float disc = B * B - A * C;
        if (disc > 0.0) {
            float tt = (-B - sqrt(disc)) / A;
            if (tt > 1e-3) {
                float y = ro.y + rd.y * tt;
                if (y >= min(a.y, b.y) && y <= max(a.y, b.y)) tbest = min(tbest, tt);
            }
        }
    }
    return (tbest < 1e8) ? tbest : -1.0;
}

/* 第 i 尊雕像：基座箱 + 身体胶囊 + 头球；返回最近交点距离，并给出法线与颜色 */
float statue(int i, vec3 ro, vec3 rd, out vec3 nrm, out vec3 alb) {
    float fi = float(i);
    int col = i % 4, row = i / 4;
    float jx = fract(fi * 0.37) - 0.5, jz = fract(fi * 0.71) - 0.5;
    vec3 b = vec3((float(col) - 1.5) * 2.4 + jx * 0.7, 0.0, -5.6 + float(row) * 2.25 + jz * 0.6);
    float sc = 0.92 + 0.22 * fract(fi * 0.53);
    float a = fi * 1.7;
    mat2 ry = mat2(cos(a), -sin(a), sin(a), cos(a));
    vec3 shade = TERRA * (0.85 + 0.3 * fract(fi * 0.29));
    alb = shade;
    float best = 1e9; nrm = vec3(0.0, 1.0, 0.0);

    float t = iBox(ro, rd, b + vec3(0, 0.14, 0), vec3(0.42, 0.14, 0.42), ry);
    if (t > 1e-3 && t < best) {
        best = t; vec3 p = ro + rd * t, o = p - b - vec3(0, 0.14, 0); o.xz = ry * o.xz;
        vec3 an = abs(o / vec3(0.42, 0.14, 0.42));
        nrm = (an.x >= an.y && an.x >= an.z) ? vec3(sign(o.x), 0, 0)
             : (an.y >= an.z) ? vec3(0, sign(o.y), 0) : vec3(0, 0, sign(o.z));
        nrm.xz = mat2(ry[0][0], ry[0][1], ry[1][0], ry[1][1]) * nrm.xz;
    }
    t = iCapsule(ro, rd, b + vec3(0, 0.62 * sc, 0), b + vec3(0, 1.18 * sc, 0), 0.30);
    if (t > 1e-3 && t < best) {
        best = t; vec3 p = ro + rd * t;
        nrm = normalize(vec3(p.x - b.x, 0.0, p.z - b.z));
    }
    t = iSphere(ro, rd, b + vec3(0, 1.42 * sc, 0), 0.21);
    if (t > 1e-3 && t < best) { best = t; nrm = normalize(ro + rd * t - (b + vec3(0, 1.42 * sc, 0))); }
    t = iBox(ro, rd, b + vec3(0, 1.05 * sc, 0), vec3(0.44, 0.11, 0.24), ry);
    if (t > 1e-3 && t < best) {
        best = t; vec3 p = ro + rd * t, o = p - b - vec3(0, 1.05 * sc, 0); o.xz = ry * o.xz;
        vec3 an = abs(o / vec3(0.44, 0.11, 0.24));
        nrm = (an.x >= an.y && an.x >= an.z) ? vec3(sign(o.x), 0, 0)
             : (an.y >= an.z) ? vec3(0, sign(o.y), 0) : vec3(0, 0, sign(o.z));
        nrm.xz = mat2(ry[0][0], ry[0][1], ry[1][0], ry[1][1]) * nrm.xz;
    }
    return (best < 1e8) ? best : -1.0;
}

/* v9.25: procedural normal map + parallax occlusion mapping */
float surfH(vec2 uv) {
    vec2 g = abs(fract(uv) - 0.5);
    float seam = smoothstep(0.40, 0.50, max(g.x, g.y));
    float n = fract(sin(dot(floor(uv), vec2(12.9898, 78.233))) * 43758.5453);
    float n2 = fract(sin(dot(floor(uv * 3.0), vec2(39.346, 11.135))) * 24634.6345);
    return -0.55 * seam + 0.10 * n + 0.05 * n2;
}
vec2 surfN(vec2 uv, float scale) {
    float e = 0.004;
    float hx = surfH(uv + vec2(e, 0.0)) - surfH(uv - vec2(e, 0.0));
    float hy = surfH(uv + vec2(0.0, e)) - surfH(uv - vec2(0.0, e));
    return vec2(-hx, -hy) * scale;
}
vec2 surfParallax(vec2 uv, vec2 dUV) {
    float h = 0.0;
    for (int i = 0; i < 10; i++) h = surfH(uv - dUV * h * 0.18);
    return uv - dUV * h * 0.18;
}

/* 场景求交：地面 / 天花板 / 侧墙 / 24 尊雕像 */
float traceScene(vec3 ro, vec3 rd, out vec3 nrm, out vec3 alb) {
    float best = 1e9; nrm = vec3(0, 1, 0); alb = vec3(0.5);
    if (rd.y < -1e-6) {                                   /* 抛光地面（带方砖）*/
        float t = -ro.y / rd.y;
        if (t > 1e-3) {
            best = t;
            vec3 p = ro + rd * t;
            vec2 fuv = p.xz * 0.25;
            vec2 dUV = rd.xz / max(abs(rd.y), 0.35) * 0.25;
            fuv = surfParallax(fuv, dUV);
            vec2 fn = surfN(fuv, 30.0);
            nrm = normalize(vec3(fn.x, 1.0, fn.y));
            float c = mod(floor(p.x * 0.5) + floor(p.z * 0.5), 2.0);
            alb = mix(vec3(0.30, 0.30, 0.33), vec3(0.72, 0.70, 0.68), c) * (0.82 + 0.36 * surfH(fuv));
        }
    }
    if (rd.y > 1e-6) {                                    /* 天花板 */
        float t = (CEIL - ro.y) / rd.y;
        if (t > 1e-3 && t < best) { best = t; nrm = vec3(0, -1, 0); alb = vec3(0.22, 0.20, 0.19); }
    }
    for (int k = 0; k < 2; k++) {                          /* 两侧墙 */
        float sx = (k == 0) ? -HALF_W : HALF_W;
        if ((k == 0 && rd.x > 1e-6) || (k == 1 && rd.x < -1e-6)) {
            float t = (sx - ro.x) / rd.x;
            if (t > 1e-3 && t < best) {
                best = t; nrm = vec3((k == 0) ? 1.0 : -1.0, 0, 0);
                alb = vec3(0.34, 0.16, 0.14) * (0.8 + 0.4 * fract((ro.z + rd.z * t) * 0.3));
            }
        }
    }
    int ns = pc.set[7] / 4;
    if (ns < 6) ns = 6;
    if (ns > NI) ns = NI;
    for (int i = 0; i < NI; i++) {
        if (i >= ns) break;
        vec3 n, a;
        float t = statue(i, ro, rd, n, a);
        if (t > 1e-3 && t < best) { best = t; nrm = n; alb = a; }
    }
    return (best < 1e8) ? best : -1.0;
}

/* 阴影：对 24 尊雕像逐个体做遮挡测试（解析 ⇒ 边缘正确）*/
float shadowAt(vec3 p, vec3 n, float t0) {
    float acc = 0.0;
    int p_pcf = pc.set[1];
    int p_res = pc.set[2];
    int p_load = pc.set[6];
    int nsh = 1;
    if (p_pcf >= 5) nsh = 8; else if (p_pcf >= 3) nsh = 4;
    if (p_load > 1) nsh = nsh * p_load;
    if (nsh > NSH) nsh = NSH;
    float soft = 0.090;
    if (p_res >= 4096) soft = 0.020; else if (p_res >= 2048) soft = 0.045;
    for (int s = 0; s < NSH; s++) {
        if (s >= nsh) break;
        float j = (float(s) + 0.5) / float(nsh) - 0.5;
        vec3 l = normalize(LDIR + vec3(j * soft * 8.0, 0.0, j * soft * 5.6));
        vec3 ro = p + n * 2e-3;
        float hit = 0.0;
        for (int i = 0; i < NI; i++) {
            vec3 nn, aa;
            float t = statue(i, ro, l, nn, aa);
            if (t > 1e-3) hit = 1.0;
        }
        acc += 1.0 - hit;
    }
    return acc / float(NSH);
}

vec3 skyColor(vec3 rd) {
    float h = clamp(rd.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.20, 0.18, 0.16), vec3(0.55, 0.50, 0.46), h);
}

void main() {
    /* ---- 镜头：沿大厅纵深飞行（跑分式穿场）---- */
    float ct = 0.0;
    if (pc.set[5] != 0) ct = pc.t;
    float zpos = -HALF_L + mod(ct * 1.6, 2.0 * HALF_L);      /* 从 -7.4 飞到 +7.4 再循环 */
    vec3 ro = vec3(0.45 * sin(ct * 0.7), 1.55 + 0.10 * sin(ct * 1.3), zpos);
    vec3 tg = ro + vec3(0.0, -0.10, 1.0);
    vec3 fw = normalize(tg - ro), up = vec3(0, 1, 0);
    vec3 rt = normalize(cross(fw, up)), uu = cross(rt, fw);
    float fov = 1.05;
    vec3 rd = normalize(fw + (vUV * 2.0 - 1.0).x * rt * fov + (vUV * 2.0 - 1.0).y * uu * fov);

    vec3 n, alb;
    float t = traceScene(ro, rd, n, alb);
    vec3 col;
    if (t < 0.0) {
        col = skyColor(rd);
    } else {
        vec3 p = ro + rd * t;
        float sh = 1.0;
        if (pc.set[0] != 0) sh = shadowAt(p, n, t);
        float d = max(0.0, dot(n, LDIR));
        /* 两盏暖色顶灯（简单的距离衰减 + 高光）*/
        vec3 lamp = vec3(0.0);
        for (int k = 0; k < 2; k++) {
            vec3 lp = vec3((k == 0) ? -2.6 : 2.6, CEIL - 0.25, -2.0 + 6.0 * float(k));
            vec3 lv = lp - p; float dl = length(lv); lv /= max(dl, 1e-4);
            float dif = max(0.0, dot(n, lv));
            lamp += vec3(1.00, 0.72, 0.42) * dif * (2.4 / (1.0 + dl * dl * 0.16));
        }
        vec3 h = normalize(LDIR - rd);
        float spec = pow(max(0.0, dot(n, h)), 48.0) * 0.35;
        col = alb * (0.16 + 0.85 * d * sh) + alb * lamp * sh + vec3(spec);
        if (pc.set[3] != 0) col += vec3(0.16, 0.12, 0.08) * (lamp + vec3(d * sh * 0.35));
        /* 地面反射（再追一条）⇒ 抛光大理石 */
        if (n.y > 0.9 && t < 12.0) {
            vec3 rr = reflect(rd, n);
            vec3 n2, a2;
            float t2 = traceScene(p + n * 2e-3, rr, n2, a2);
            vec3 rc = (t2 < 0.0) ? skyColor(rr) : a2 * (0.25 + 0.75 * max(0.0, dot(n2, LDIR)));
            col = mix(col, rc, 0.35 * exp(-0.10 * t));
        }
        /* 体积雾感 + 灯的辉光 */
        float fog = 1.0 - exp(-0.035 * t);
        vec3 fogc = vec3(0.30, 0.24, 0.20);
        for (int k = 0; k < 2; k++) {
            vec3 lp = vec3((k == 0) ? -2.6 : 2.6, CEIL - 0.25, -2.0 + 6.0 * float(k));
            fogc += vec3(0.55, 0.36, 0.18) * (1.2 / (1.0 + dot(lp - p, lp - p) * 0.5));
        }
        col = mix(col, fogc, fog);
    }
    /* 暗角 + sRGB */
    vec2 q = vUV - 0.5;
    col *= 1.0 - 0.45 * dot(q, q);
    if (pc.set[4] != 0) col = pow(clamp(col, 0.0, 1.0), vec3(0.4545));
    oCol = vec4(col, 1.0);
}
