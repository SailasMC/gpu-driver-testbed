# v77 — panvk_cmd_draw() 空指针保护（单一变量）

日期：2026-10-05 · 构建树 `/root/zenithblue/work/mesa` · 构建目录 `/root/zenithblue/build/android-v4`

## 0. 一句话

病根（已在真机 `hs_err_pid5255.log` 钉死，本轮不再论证）：
`panvk_cmd_draw()` 在**检查之前**就解引用了 `vs->spd`；当某个 draw 在没有顶点着色器
的状态下被录制 ⇒ `panvk_shader_hw_variant(NULL)` 返回 NULL ⇒ `vs->spd` 段错误。
本轮加一个 `if (!vs) return;`，**单一变量**，不动任何其它检查点。

## 1. 上游对照（先看上游，不凭猜）

取上游现行源码：

```
curl -s "https://gitlab.freedesktop.org/api/v4/projects/176/repository/files/\
src%2Fpanfrost%2Fvulkan%2Fcsf%2Fpanvk_vX_cmd_draw.c/raw?ref=main"
```

- HTTP exit 0，拿到 `189361` 字节，文件头 Copyright 已含 `2026 NXP` ⇒ 确为现行 main。
- 上游 `panvk_cmd_draw()` 在 **up:3460–3507**，逐字如下（关键段）：

```c
3460: panvk_cmd_draw(struct panvk_cmd_buffer *cmdbuf, struct panvk_draw_info draw)
3461: {
3462:    const struct panvk_shader_variant *vs =
3463:       panvk_shader_hw_variant(cmdbuf->state.gfx.vs.shader);
3464:    VkResult result;
3465: 
3466:    /* If there's no vertex shader, we can skip the draw. */
3467:    if (!panvk_priv_mem_check_alloc(vs->spd))
3468:       return;
```

**结论：上游亦无此保护。** 上游 `main` 与我们的树在这一段**完全相同**（注释、缩进、
`vs->spd` 的写法都一致），即这是一个**上游也存在的真实缺陷**（我们的树只是碰上了）。

补充证据：上游该文件里唯一一处 `!vs` 出现在 `427: if (!vs_desc_dirty(cmdbuf))`，
那是 `vs_desc_dirty` 的一部分，**与 `vs` 空指针无关**（`grep -n "!vs\|vs == NULL\|if (vs)"` 仅此一命中）。
另注：上游/我们的 `prepare_draw()` 里用的是 `ASSERTED bool idvs = vs->info.vs.idvs;`
后面才跟 `assert(vs);`——**同样是"先解引用后断言"**，同一类写法（见 §3）。

⇒ 因为上游没有等价保护，本轮采用**最小改动**（任务书第 1 条第二种分支）。

## 2. 我们的改动（file:line + 前后对照）

文件：`src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c`
改动前该文件 sha256：`615f880dd764ea7a33896073bb53a6e1d726f47bd1feaecbb8703e7e5344fb51`
改动后该文件 sha256：`ae9884fab0932f3685fc2636897199f7cf6918d962acd27798dd443dec0775e1`
备份：`panvk_vX_cmd_draw.c.bak-v77-1791215898`（改前 `cp`，纪律要求）

**改动位置：3496–3499（净增 3 行）**

```diff
@@ -3493,6 +3493,9 @@ panvk_cmd_draw(struct panvk_cmd_buffer *cmdbuf, struct panvk_draw_info draw)
    VkResult result;
 
    /* If there's no vertex shader, we can skip the draw. */
+   if (!vs)
+      return;
+
    if (!panvk_priv_mem_check_alloc(vs->spd))
       return;
```

**改动后 3489–3501 逐行：**

```c
3489 panvk_cmd_draw(struct panvk_cmd_buffer *cmdbuf, struct panvk_draw_info draw)
3490 {
3491    const struct panvk_shader_variant *vs =
3492       panvk_shader_hw_variant(cmdbuf->state.gfx.vs.shader);
3493    VkResult result;
3494 
3495    /* If there's no vertex shader, we can skip the draw. */
3496    if (!vs)
3497       return;
3498 
3499    if (!panvk_priv_mem_check_alloc(vs->spd))
3500       return;
```

风格一致性：`if (!x) return;` 与 `panvk_shader.h` 中 `panvk_shader_only_variant()` /
`panvk_shader_hw_variant()` 自身的写法一致（两函数都已 `if (!shader) return NULL;`）。

### 2.1 代码生成核对（不是推理，是反汇编实测）

v76 崩溃点（真机 pc `0x79c8b945c0`，模块基址 + `0xb8a5c0`）反汇编：

```
b8a5b0: ldr  x8, [x0, #0x2288]     ; cmdbuf->state.gfx.vs.shader
b8a5b4: add  x9, x8, #0x120
b8a5b8: cmp  x8, #0x0
b8a5bc: csel x9, xzr, x9, eq       ; 空则置 0
b8a5c0: ldr  x9, [x9, #0x4e0]      ; ★ 无条件读 ptr+0x4e0（= ->hw）⇒ SIGSEGV
```

v77 `panvk_cmd_draw` 现位于 `0xa53948`，函数入口即出现守卫：

```
a5396c: ldr  x8, [x0, #0x2288]     ; cmdbuf->state.gfx.vs.shader
a53970: str  x0, [sp, #0x58]
a53974: cbz  x8, 0xa56b48          ; ★ x8 == NULL ⇒ 直接跳到函数收尾（return）
a53978: ldr  x9, [x8, #0x600]      ; 只有非空才读
a5397c: cmp  x9, #0x8
a53980: b.lo 0xa56b48              ; 原 panvk_priv_mem_check_alloc(vs->spd) 仍在其后
```

⇒ 守卫**真的进了机器码**，且位置正确（在 `vs->spd` 访问之前）。`cbz` 目标
`0xa56b48` 是函数收尾块（与 `b.lo` 同一个返回目标）⇒ 是干净的 `return;`。
注意 `panvk_cmd_draw` 基址由 `0xb8a07c` 变为 `0xa53948`：`.text` 缩减 5080 B 导致
全模块符号整体位移，这是**源码改动的真实后果**，不是构建漂移（见 §4 对照）。

## 3. 同类风险点（只列表，本轮**不改**）

同型写法定义：`panvk_shader_hw_variant()` / `panvk_shader_only_variant()` 返回 NULL
（入参 shader 为 NULL 时），调用方**在空检查之前就解引用**返回值。

`panvk_shader_hw_variant`（CSF 活树，排除 `.bak-*`）共 8 处调用，全部在
`csf/panvk_vX_cmd_draw.c`：

| # | file:line | 首次解引用 | 是否受 v77 守卫覆盖 | 风险判断 |
|---|-----------|-----------|--------------------|---------|
| 1 | `csf/panvk_vX_cmd_draw.c:3492` | 3496（v77 新增 `if (!vs)`） | — 本体 | ✅ **本轮已修** |
| 2 | `csf/panvk_vX_cmd_draw.c:3320` | 3330 `vs->first_vertex` | 是（`launch_indirect_draw` 仅由 3536 调用，在守卫之下） | 🟡 低（现状受保护，非独立入口） |
| 3 | `csf/panvk_vX_cmd_draw.c:2866` | 2868 `vs->info.vs.idvs`，2870 才 `assert(vs)` | 否（`prepare_draw` 是独立函数） | 🟠 **中高**：同一"先解引用后断言"缺陷；当前唯一调用点 3529 在守卫之下，但**一旦将来新增调用者即崩**。注意 `assert()` 在 release 下被编译掉，守卫形同虚设 |
| 4 | `csf/panvk_vX_cmd_draw.c:2783` | 2789 `vs->info.vs` | 否（`set_tiler_idvs_flags`，调用点 2950 在 `prepare_draw` 之内 ⇒ 被守卫间接覆盖） | 🟡 低 |
| 5 | `csf/panvk_vX_cmd_draw.c:465` | 470 `vs->info.varyings.formats` | 否（`emit_varying_descs`，调用点 **531**，路径不经过 `panvk_cmd_draw`） | 🔴 **高**：独立入口，当前无任何空检查 |
| 6 | `csf/panvk_vX_cmd_draw.c:755` | 762 `vs->…` | 否（`update_tls`，调用点 **2895 与 4978**；4978 路径不经过 `panvk_cmd_draw`） | 🔴 **高**：独立入口（4978），当前无任何空检查 |
| 7 | `csf/panvk_vX_cmd_draw.c:1038` | 1041 `vs->info.vs.writes_point_size` | 否（`MESA_PRIM_POINTS` 分支，函数内被 `prepare_draw` 调用 ⇒ 间接覆盖）；且 `#if PAN_ARCH < 13` | 🟡 低 |
| 8 | `csf/panvk_vX_cmd_draw.c:4960` | **4965** `cmdbuf->state.gfx.vs.shader->desc_info`（**连 `panvk_shader_hw_variant` 都绕过，直接解引用 shader 本身**） | 否（独立入口，4978 调 `update_tls`） | 🔴 **高**：这是 #6 的上游触发者；**比 #1 更早、更直接** |

**同型（"检查之前先解引用"）的 compute 版对照 —— 完全同构，且同样未修：**

| # | file:line | 说明 | 风险判断 |
|---|-----------|------|---------|
| 9 | `csf/panvk_vX_cmd_dispatch.c:372` | `cmd_dispatch()`：`cs = panvk_shader_only_variant(cmdbuf->state.compute.shader);` 紧接 `if (!panvk_priv_mem_check_alloc(cs->spd))` —— 与 #1 是**逐字同构**的写法（注释也一样："If there's no compute shader, we can skip the dispatch."） | 🔴 **高**：与 #1 同一个病，只是走 compute 队列 |
| 10 | `csf/panvk_vX_cmd_dispatch.c:451` | `CmdDispatchBase()` 取 `shader = panvk_shader_only_variant(...)` 后传给 `cmd_dispatch()`（该 `shader` 局部量在函数内未被解引用 ⇒ 本身无害），真正危险的是 372 | 🟡 低（症状点，根在 #9） |

**只读结论（本轮不改）：** 修好 #1 只堵住了"经 `panvk_cmd_draw` 的 draw"这一条路。
若要真正消除同类风险，**至少**还需处理 #5、#6、#8（图形侧独立入口）与 #9（compute 侧同构）。
本轮严守"单一变量"，一律不动。

## 4. 确定性对照（撤改动 ⇒ 必须逐位等于 v76）

`export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH`
后 `ninja src/panfrost/vulkan/libvulkan_panfrost.so`。

| 步骤 | 产物 sha256 | 字节数 | 结果 |
|------|------------|--------|------|
| **v76 基线**（`/root/final/libvulkan_panfrost_v76.so`，任务书给定） | `fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109` | 20 057 992 | 基准 |
| v77 改后编译 #1 | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185` | 20 053 456 | v77 产物 |
| **撤改动**（`cp` 回 `.bak-v77-*`，源码 sha 回到 `615f880d…`）重编 | **`fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109`** | **20 057 992** | ✅ **逐位等于 v76** |
| 贴回改动重编 #2 | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185` | 20 053 456 | ✅ 与 #1 逐位一致（可复现） |

⇒ **确定性双向成立**：撤改动 ⇒ 回到 v76 逐位同一；贴回 ⇒ 两次编译逐位同一。
因此 sha256 的差异 **100% 由本轮 3 行改动引起**，不含任何构建漂移。

编译 exit code：`0`（四次 ninja 均 `NINJA_EXIT=0`，无 warning/error 输出）。

### 4.1 为什么 `.so` 比 v76 小 4536 字节（已核实，非异常）

用脚本解析 ELF section header 对比：

| section | v76 | v77 | delta |
|---|---|---|---|
| `.text` | 7 542 688 | 7 537 608 | **−5 080** |
| `.relro_padding` | 3 912 | 360 | **+3 552** |
| `.symtab` | 879 456 | 879 552 | +96 |
| `.strtab` | 738 376 | 738 382 | +6 |
| `.eh_frame` | 550 444 | 550 836 | +392 |
| `.eh_frame_hdr` | 80 396 | 80 436 | +40 |

净 −4 536 B。`.relro_padding` 是链接器为 RELRO 段做页对齐产生的填充，属
**非语义字节**；`.text` 缩减 5 080 B 是新增的"提前 return"改变了
`panvk_cmd_draw` 基本块布局与后续代码布局（LTO/内联边界随之改变）所致。
配合 §4 的两个对照（撤改动 ⇒ 逐位回 v76；贴回 ⇒ 两次一致），可判定这是
**源码改动的确定性后果**，不是构建环境漂移。

## 5. 交付物与校验

### 5.1 新 `.so`
- 路径：`/root/final/libvulkan_panfrost_v77.so`
- sha256：`cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185`
- 字节数：`20053456`

### 5.2 APK
- 路径：`/root/final/mgl-panvk-v77.apk`
- sha256：`49506ddc28fd543d41a763c673ecebcb604f1e62a676849a80c16d4746d67b5d`
- 字节数：`10215983`
- 打包脚本：`/root/pack_v77.sh`（由 `/root/pack_v76.sh` 派生，`diff` 仅 3 行变化：
  `W`/`OUT` 的 v76→v77、`versionCode 74→77`、`6.14-defer-diag→6.17-nullvs`。
  **env 串以外的每一行与 v76 逐字符一致**）
- 校验（全部通过）：

| 校验项 | 结果 |
|---|---|
| `unzip -p <apk> lib/arm64-v8a/libvulkan_freedreno.so \| sha256sum` | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185` == 新 `.so` ✅ |
| 非空 | `20053456` 字节 ✅ |
| `aapt2 dump badging` | `versionCode='77' versionName='6.17-nullvs'` ✅ |
| v74/v75/v76 APK 未被改动 | v74 `74b173ce…`、v75 `a108c335…`、v76 `1bb9066b…` 与改动前记录逐字符一致 ✅ |

### 5.3 切分分发
```
cp -f /root/final/mgl-panvk-v77.apk /data/dsh_downloads/
cd /data/dsh_downloads && split -n 8 -d mgl-panvk-v77.apk v77p8_
```
（全程**未使用 `rm`**）

| part | 字节数 |
|---|---|
| `v77p8_00` … `v77p8_06` | 1 276 998 各 |
| `v77p8_07` | 1 276 997 |

8 份合计 `10215983` = APK 大小 ✅；`/data/dsh_downloads/mgl-panvk-v77.apk` sha256 同为 `49506ddc…` ✅
**24 字符前缀：`49506ddc28fd543d41a763c6`**

## 6. 回滚命令

只回滚源码（最常用）：

```bash
cp -f /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c.bak-v77-1791215898 \
      /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja src/panfrost/vulkan/libvulkan_panfrost.so
# 期望 sha256 = fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109 (20057992 B)
```

直接退回 v76 的 APK（不重编）：用现成的 `/root/final/mgl-panvk-v76.apk`
（sha256 `1bb9066bbcf24d6f7a0b58fcc9a6490b2e532ec9210a04ef5c172f0356d70e20`）。

## 7. 判读表（真机验证用）

装 `mgl-panvk-v77.apk`，跑与 v76 相同的复现场景（同一 App、同一路径、同一时长），
对照 v76 的 `hs_err_pid5255.log`（`SIGSEGV at panvk_cmd_draw+0x34`，`elapsed time 27.5 秒`）：

| 观察项 | 崩溃消失（最佳） | 显著变长（次佳） | 仍 ~27 秒崩 | 画面变干净 |
|---|---|---|---|---|
| 看什么 | 无新 `hs_err_pid*.log`；`panvk_cmd_draw+0x34` 不再出现；**无 10 秒看门狗** | 仍可能出现 `hs_err`，但 `elapsed time` 从 ~27 s 明显增大（≥2×，或换到**别的**函数名/别的 `+0xNN`） | `panvk_cmd_draw+0x34` 再次逐字出现，`elapsed` 仍 ~27 s | 崩溃前那段"半成品"画面是否消失 |
| 怎么读 | ⇒ **病根命中**，`vs==NULL` 确实是主因；可继续排 §3 的 #5/#6/#8/#9 | ⇒ `vs==NULL` 是真因之一但**不是唯一**；崩点被推到 §3 的后续风险点（尤其 #8→#6，或 compute 侧 #9）。下一步按新 `hs_err` 的 `Problematic frame` 定位 | ⇒ **推翻**"`vs==NULL` 是主因"的推断（回 §3/上游重新取证据，勿再猜） | 独立信号：即使仍崩，只要"半成品"消失，说明守卫确实拦掉了一部分非法 draw |
| 额外 | `logcat` 里不应再有 `panvk_priv_mem_check_alloc` 相关断言 | 记录**新的** `Problematic frame` 全文 | 与 v76 日志做 `diff` | 截图对比 |

**注意判读纪律：** 本轮的确定性交付只保证"`vs==NULL` 不再段错误"，
**不保证**崩溃整体消失——§3 列出的 #5/#6/#8/#9 是**尚未修复**的同类入口。
若 v77 仍崩于**同一** `panvk_cmd_draw+0x34`，那才是对病根推断的反证。

## 8. 本轮**未做**的事（明确边界）

- 未改：v71/v72/v73 检查点、ATOMIC/SEQNO 打印、MR!44659 的 flush（v76）、修复 A、
  C1/C2/B、P2/renew32、超时常量、MobileGL、AFBC —— 一律未碰。
- 未做任何 `git checkout/stash/reset`；未使用 `rm`；未操作手机。
- §3 的同类风险点**只列表未修**（任务书要求）。
- 未验证：真机运行结果（判读表 §7 待真机填充"未验证"）。

## 9. 待确认 / 未验证

- 真机 v77 实际表现：**未验证**（本子智能体不操作手机）。
- §3 中标注 🟡/🔴 的"是否可达"是基于**静态调用图**的判断，未在真机触发：
  标 🟡 的项依据"唯一调用点在守卫之下"，标 🔴 的项依据"存在不经过
  `panvk_cmd_draw` 的独立调用点"——均为静态结论，未经运行时证实，属**未验证**。
- 上游是否已有针对本缺陷的**开放 MR / issue**：本轮只取了 `main` 的文件内容，
  未检索 issue tracker，**未验证**。
