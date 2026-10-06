# v81 — 上游 MR!44816「tiling area 只对 IR pass 的 spill store 对齐」回移 + 服务器侧收尾

日期：2026-10-06 · 构建树 `/root/zenithblue/work/mesa` · 构建目录 `/root/zenithblue/build/android-v4`
上游基线 commit `5a07217f034`（radv: slightly rework initializing the DCC predicate；author date 2026-09-17，commit date 2026-09-18T16:45:08Z）· 全部改动未提交（同前几版约定）
上游 MR：<https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44816> — 标题 `panvk: only align tiling area to spill store on IR pass`，作者 Ryan Zhang（`kkaiii`，NXP），**state = opened（未合并）**，created 2026-09-30，updated 2026-10-01，`merge_status = can_be_merged`
分工：源码补丁 / 两次编译 / 打包 = Lead；8 路切分与重组校验 + 本报告 + 上游侦查 = teammate `glm`

---

## 0. 一句话

v81 = 把上游**尚未合并**的 MR!44816 回移到我们 fork 的 `panvk_vX_cmd_draw.c`（csf 3 处 + 通用 1 处，共 4 处），
让**主 framebuffer 的 tiling area 不再被 `spill.store` 无谓扩张**，只给「非最终 IR pass」单独算一份 spill 对齐的 layout；
本轮**只做这一件事**，靶心是 pandecode 指纹 `Scissor Max Y 607 → 599`（fb 高 600）。
两套 APK 已完成 8 路切分并**逐字节**校验重组；**真机画面/指纹未验证**（设备侧归 Lead）。

---

## 1. 本轮边界（诚实声明）

| 做的事 | 没做的事 |
|---|---|
| 切分 `v81p8_00..07` / `v81n8_00..07` 并校验重组（sha256 + `cmp` 逐字节） | **没有改任何源码**：源码 = Lead 已落补丁的状态，本会话只读引用 |
| 独立复核 `.so` / 两 APK / APK 内嵌驱动的 sha256、字节数、ZIP 条目表 | **没有重新编译**（不写构建目录）；两次编译日志只做引用与集合比对 |
| 逐行比对「本地补丁 vs 上游 MR 原始 diff」（含 token 级 / 非空白字符计数） | **没有操作手机**：未安装 APK、未拉起 App、未截图、未跑 pandecode 解帧 |
| 上游 MR / issue / commit 检索（GitLab 只读 API，产物落 `/root/v81work/recon/`） | 未执行 `git checkout/stash/reset/fetch`；未碰 `/root/mesa`、`/root/MobileGL` |
| 写本报告 `/root/research/55-v81-tilingarea.md` | 未做 `rm`（本轮无需清理） |
| 附录 A 的候选**只记录、未采纳、未改代码** | 报告不含任何密钥/token；只含路径、哈希与上游 URL |

---

## 2. 补丁逐行（4 处）与上游 diff 的一致性

### 2.1 两处文件与备份

| 文件 | 备份（回滚用） | 本地改动 |
|---|---|---|
| `src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c` | `…/csf/panvk_vX_cmd_draw.c.bak-v81-1791219118`（192083 B） | **+11 / −2** |
| `src/panfrost/vulkan/panvk_vX_cmd_draw.c` | `…/vulkan/panvk_vX_cmd_draw.c.bak-v81-1791219118`（41977 B） | **+0 / −1** |

上游 MR 含 **2 个文件、4 个 hunk**（csf 3 + 通用 1）。逐行内容如下（`diff -u 备份 现状`）。

### 2.2 hunk ①（csf，循环前为 IR pass 单算一份 layout）

```diff
@@ -1857,6 +1857,10 @@
    invalidate_unselected_crc_rts(b, &fbd_info);
 
+   /* Compute a separate tiling area for incremental rendering passes. */
+   struct pan_fb_layout ir_fb_layout = render->fb.layout;
+   GENX(pan_align_fb_tiling_area)(&ir_fb_layout, &render->fb.spill.store);
+
    for (uint32_t ir_pass = 0; ir_pass < PANVK_IR_PASS_COUNT; ir_pass++) {
```

### 2.3 hunk ②（csf，循环内：非最终 pass 用 `ir_fb_layout`）

```diff
@@ -1879,6 +1883,10 @@
       ir_store = &ir_store_without_crc;
 #endif
+      /* Use the spill-aligned fb layout for non-final IR passes. */
+      fbd_info.fb = ir_pass == PANVK_IR_LAST_PASS ? &render->fb.layout
+                                                  : &ir_fb_layout;
+
       for (uint32_t i = 0; i < enabled_layer_count; i++) {
```

### 2.4 hunk ③（csf，循环后：`fb` 也要复位）

```diff
@@ -1920,8 +1928,9 @@
-   /* Incremental-rendering loop might set fbd_info.load/store to spill nodes.
-    * Set it back to the regular nodes here for CRC patching later. */
+   /* Incremental-rendering loop might set fbd_info.fb/load/store to spill
+    * nodes. Set it back to the regular nodes here for CRC patching later. */
+   fbd_info.fb = &render->fb.layout;
    fbd_info.load = &render->fb.load;
    fbd_info.store = &render->fb.store;
```

### 2.5 hunk ④（通用文件：删掉「主 layout 对齐 spill.store」这一行 = 病根）

```diff
@@ -685,7 +685,6 @@
    GENX(pan_align_fb_tiling_area)(&render->fb.layout, &render->fb.store);
-   GENX(pan_align_fb_tiling_area)(&render->fb.layout, &render->fb.spill.store);
 
    /* Try to optimize and remove unnecessary resolves if we can */
```

> 该行是 `panvk_per_arch(cmd_init_render_state)()` 里 `tiling_area_px = render_area` 之后的第二次对齐——
> `pan_align_fb_tiling_area()`（`src/panfrost/lib/pan_fb.c:112`）会按 **AFBC 块尺寸 / AFRC clump 尺寸**把 `tiling_area_px`
> 向上取整（线性格式直接 return，不产生扩张，见 `pan_fb.c:99-108`）。因此只要 `spill.store` 指向的图是 AFBC，
> 主 framebuffer 的 tiling area 就会被撑到块尺寸的整数倍之外 → 描述符 bbox/scissor 越过渲染区 → 「几何被画到 fb 之外 / 楔形」。

### 2.6 一致性校验（与上游原始 diff 对比）

方法（可复现，见 §10）：把上游 `.diff` 用 `patch -p1 --dry-run/真打` 应用到**补丁前**的备份副本上（目录 `/root/v81work/mrcheck/`，不动真实源码），再三方比较。

| 比较 | csf 文件 | 通用文件 |
|---|---|---|
| 本地补丁（现状 ← 备份） | +11 / −2 | +0 / −1 |
| 上游补丁（上游副本 ← 备份） | +12 / −2 | +0 / −1 |
| **本地 ↔ 上游** 差异 | **+2 / −3** | **0 / 0** |
| 行数 | 5133 行 vs 上游 5134 行 | 1053 行 vs 1053 行 |
| 非空白字符数 | 146566 vs **146566** | 31627 vs **31627** |
| token 流（`tr -s '[:space:]' '\n'` 后逐 token diff） | **完全一致** | **完全一致** |

**结论（诚实版）：4 个 hunk 的语义与 token 与上游完全一致；通用文件逐字节一致；csf 的差异只有 hunk ② 的换行折行**：

```
上游                            本地
+      fbd_info.fb = ir_pass == PANVK_IR_LAST_PASS        +      fbd_info.fb = ir_pass == PANVK_IR_LAST_PASS ? &render->fb.layout
+                       ? &render->fb.layout              +                                                  : &ir_fb_layout;
+                       : &ir_fb_layout;
```

即：上游把三目拆成 3 行、本地写成 2 行（多 1 个空格少 1 个换行；本地 192539 B / 上游 192535 B）。
**这不影响 AST、不影响生成码**，但如果将来要与上游做 `git cherry-pick`/逐字节对拍，需要知道这一处会报冲突。
上游 hunk 的行号与本树不同（应用偏移：csf +27 行、通用 −11 行），原因是本树基线落后上游 + 本树自带 DIY 改动。

---

## 3. 确定性校验（两次强制重编）

- 构建命令（Lead 执行；与 `/root/build_v80.sh` 同款）：
  `export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH` + `ninja -j2 -C /root/zenithblue/build/android-v4`
- 两次重编日志：`/root/v81work/v81_buildA.log`（1615 B）、`/root/v81work/v81_buildB.log`（1615 B）
- 两份日志的**编译目标集合完全一致 = 12 个 `panvk_vX_cmd_draw.c.o`**（与接力卡「重编对象数=12」吻合）：

| 类别 | arch | 个数 |
|---|---|---|
| 通用 `panvk_vX_cmd_draw.c` | v6 / v7 / v10 / v11 / v12 / v13 / v14 | 7 |
| CSF `csf/panvk_vX_cmd_draw.c` | v10 / v11 / v12 / v13 / v14 | 5 |
| **合计** | | **12** |

- 日志里其余步骤：7 个静态库链接（`libpanvk_v6/v7/v10/v11/v12/v13/v14.a`）+ 1 个共享库链接（`libvulkan_panfrost.so`）+ 1 个 `src/git_sha1.h` 生成 = `[n/21]` 共 21 步。
  两次日志步骤**顺序不同**（ninja 并行调度不同），**集合相同**。
- **诚实标注**：两份日志**不含 sha256 输出**，「两次产物 sha 相同」这一结论来自 Lead 的执行记录，日志只能佐证「重编对象集合一致」。
  本会话在服务器侧独立复核的是**当前**产物哈希（§4）。

---

## 4. 产物与哈希

### 4.1 驱动 `.so`

| 产物 | 路径 | 字节 | sha256 |
|---|---|---|---|
| 构建树产物 | `/root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so` | 20053408 | `2e38470d8e32486c22e10df29119a118e1f1ac6d8166244deace3cb74f95dde6` |
| 归档副本 | `/root/final/libvulkan_panfrost_v81.so` | 20053408 | 同上（两者同哈希，本会话实测） |
| 上一版（回滚对照） | `/root/final/libvulkan_panfrost_v80.so` | 20052912 | `bb138465b9e1cf67125d1fb720958864cffa605bbd142ecb7327656978068bf1` |

### 4.2 两个 APK

| 产物 | 字节 | sha256 | 内嵌 `lib/arm64-v8a/libvulkan_freedreno.so` |
|---|---|---|---|
| `/root/final/mgl-panvk-v81.apk`（diag） | 10220079 | `0cabf8b3d941ba4999ee16a0d0419d18ea321b7637abf0e941972a901ae7fb02` | 20053408 B · `2e38470d8e32486c22e10df29119a118e1f1ac6d8166244deace3cb74f95dde6` ✅ |
| `/root/final/mgl-panvk-v81-nodiag.apk` | 10220079 | `b0a66fa6ecf94ff047ffd2ea3776179fe3c6010d92f22a84901345ea4455eba2` | 20053408 B · `2e38470d8e32486c22e10df29119a118e1f1ac6d8166244deace3cb74f95dde6` ✅ |

**APK 内嵌驱动 sha 必须 = `2e38470d8e32486c…`：两版都通过**（`unzip -p … | sha256sum`）。

`v81.apk` 的 ZIP 条目（两版条目名/长度完全一致，仅 manifest 与签名不同）：

| 条目 | 字节 | sha256（两版均相同，除 manifest/签名外） |
|---|---|---|
| `AndroidManifest.xml` | 3584（diag）/ 3576（nodiag） | diag `a5d6693ee476234443884f7850103fdbf26e6565e3fa1d6057f64cfb2adcbd49` / nodiag `8fdceca14d2c84c8b54071f9f7a0a8cb322815ade6c123306ff6eabb038f4f0a` |
| `resources.arsc` | 40 | `1fa3cb291285348ec1b33c85e7317d989467707f4791c4e5302f2d312d1e18c8` |
| `lib/arm64-v8a/libMobileGL.so` | 16956584 | `72919c73a7e07630f329bb4ad9605c606a9aac9e2fc6596683ee7b9f9c76848b` |
| `lib/arm64-v8a/libvulkan_freedreno.so` | 20053408 | `2e38470d8e32486c22e10df29119a118e1f1ac6d8166244deace3cb74f95dde6` |
| `classes.dex` | 1328 | `6bd3abde2c53506f1b54bb88a8069c3cc394c2fb08440e4ca475cbf8fd64f4ad` |
| `META-INF/DSHDRIVE.SF` / `.RSA` / `MANIFEST.MF` | 620 / 1337 / 493 | 两版不同（签名块） |

**两版 APK 的差异只在 `AndroidManifest.xml` + 签名块**（`resources.arsc`/`libMobileGL.so`/驱动/`classes.dex` 逐字节相同）；
manifest 差异来自打包脚本 `/root/pack_v81_nodiag.sh` 的两处 sed：`versionName 6.21-tilingarea → 6.21-tilingarea-nodiag` 与
`PANVK_DEBUG=1,kbase_diag → PANVK_DEBUG=0`。**diag 版带 `PANVK_DEBUG=1,kbase_diag`，nodiag 版 `PANVK_DEBUG=0`。**

交叉佐证（同族产物只有一个变量在动）：`mgl-panvk-v79-nodiag.apk` 与 `mgl-panvk-v81-nodiag.apk` 的
`libMobileGL.so`（`72919c73a7e07630…`）与 `classes.dex`（`6bd3abde2c53506f…`）**完全相同** ⇒ v79→v81 之间只有驱动 `.so` 在变。

上一版 APK 备查（回滚对照）：`mgl-panvk-v80.apk` 10215983 B `003f5a655a6a03fae0b5b0bff73c461d806001804c3ef97e8e20f17b3a75a80d`；
`mgl-panvk-v80-nodiag.apk` 10215983 B `94b9419e91bbebe1b49d9b3f5845f7db7892c4a4e1c31256772edba9acd0e920`。

---

## 5. 切分与重组校验（8 路 × 2 套）

命令（`/data/dsh_downloads/`，切分前该目录**无** `v81p8_*`、`v81n8_*` 残留）：

```bash
cd /data/dsh_downloads
split -n 8 -d /root/final/mgl-panvk-v81.apk        v81p8_
split -n 8 -d /root/final/mgl-panvk-v81-nodiag.apk v81n8_
```

### 5.1 diag 版（`v81p8_*`，重组目标 sha256 `0cabf8b3d941ba49…`）

| 分片 | 字节 | sha256 |
|---|---|---|
| `v81p8_00` | 1277510 | `0db90345c3622ae598ccb1a97d7bf17c86a3a7da4036fffc5f1a0038d2437d2e` |
| `v81p8_01` | 1277510 | `08f00976109120b40d9306be3de5c6a6bd7c0b329730b8a511bd09b1b5fc4e7a` |
| `v81p8_02` | 1277510 | `551ecbcfd4bcc5333a484c6609ac139317b770f3514fdd732c24b8a040ce2520` |
| `v81p8_03` | 1277510 | `7103ffe330d07694a028a1fbfdb02831bba174760db2e786926c6d38780096a5` |
| `v81p8_04` | 1277510 | `46793cbfcb59013e573c5166bdafe1e630075c387f37d2ca8acdf9ded65943a6` |
| `v81p8_05` | 1277510 | `1d66296be9f50522eccf856b08382183ebaf27b31693ac70ba526b3e83815939` |
| `v81p8_06` | 1277510 | `7ebf7a517904d9f2081c68219d355316eaf4b44af0fae8606a08f6855f829df6` |
| `v81p8_07` | 1277509 | `b3be334ea3ba7162eb9afa6404ebf990ebb990e9eb9ddc7685e5943d1aea3dbd` |
| **`cat v81p8_* \| sha256sum`** | 10220079 | **`0cabf8b3d941ba4999ee16a0d0419d18ea321b7637abf0e941972a901ae7fb02`** ✅ |

### 5.2 nodiag 版（`v81n8_*`，重组目标 sha256 `b0a66fa6ecf94ff0…`）

| 分片 | 字节 | sha256 |
|---|---|---|
| `v81n8_00` | 1277510 | `72c8871ca9818e75b2d567f3fc5ab9a99e9e9e992427cd652e4c2cca36ddf121` |
| `v81n8_01` | 1277510 | `faec391e19e5894c1a21d4974438b562152ccbde5875c87a4815a7051aa615a5` |
| `v81n8_02` | 1277510 | `d3544eb941bf68ecc2e2d5688180fb3345f56a5d31ba3bbc0f2f545b59a3514b` |
| `v81n8_03` | 1277510 | `ec2a1d9590a54c84fb1fd66d0a94806c43d57105ebdbed517bc25dcba886efdf` |
| `v81n8_04` | 1277510 | `5281471def8f5b33a902aaab88c78296ac993a48695318180ff113aed794207e` |
| `v81n8_05` | 1277510 | `eed28231f7b076351515201f70414f9402ce4d6dac8de02ca5d334658583eb97` |
| `v81n8_06` | 1277510 | `2967d928515e951c05c6cb5091d412b79d94154c7b987d60b8f311762966c657` |
| `v81n8_07` | 1277509 | `4b1c92b6074b565dea848ada40413244560eb39dc714f8c289534b25c2953819` |
| **`cat v81n8_* \| sha256sum`** | 10220079 | **`b0a66fa6ecf94ff047ffd2ea3776179fe3c6010d92f22a84901345ea4455eba2`** ✅ |

> 注：`v81p8_06`（`7ebf7a517904d9f2…`）与 `v81n8_06`（`2967d928515e951c…`）**不相等是正常的**：两版 APK 本身不同，
> 因此 16 个分片里只有两版**共有的那段数据**才会出现相同哈希。

### 5.3 逐字节校验（不止 sha256）

```bash
cat v81p8_* > /root/v81work/rejoin_p.apk && cmp /root/v81work/rejoin_p.apk /root/final/mgl-panvk-v81.apk
cat v81n8_* > /root/v81work/rejoin_n.apk && cmp /root/v81work/rejoin_n.apk /root/final/mgl-panvk-v81-nodiag.apk
```

两套 `cmp` 均**无输出（逐字节相同）**，重组文件各 10220079 B。⇒ 重组 sha256 相等**且**字节完全一致。
（`split -n 8 -d` 对 10220079 B 的切法：前 7 片各 1277510 B，末片 1277509 B；两套相同。）

---

## 6. 上机判读表（真机验证用，设备侧由 Lead 执行）

### 6.1 靶心指纹（本轮唯一的机器可判定指标）

- 场景：**fb 高 600**（渲染区高 600）的同一帧。
- 旧行为（v80 及以前）：主 `tiling_area_px` 被 `spill.store` 的 AFBC/AFRC 块尺寸向上取整 → 越过 600 → pandecode 读 **`Scissor Max Y 607`**。
- 新行为（v81）：主 layout 只按真实 store 对齐，spill 对齐只给非最终 IR pass → 期望 **`Scissor Max Y 599`**。
- **前置闸门（重要）**：上机第一步先采一帧 CS dump 用 pandecode 解，确认 `Scissor Max Y` 真的从 607 变成 599。
  若仍是 607 → **不要判画面**，先查：APK 版本/驱动是否真的是本次产物（`sha256sum` 应为 `2e38470d…`）、是否加载到了旧驱动（`/data/local/tmp` 残留）、
  是否该 pass 走的是另一条（非 IR）路径。
- 本节指纹 607→599 属**预期**；本会话**未解算任何 dump**（未跑 pandecode、未操作设备）→ 标注「未验证」。

### 6.2 四档判读

| 画面（v81 nodiag vs v80 nodiag，同场景同视角） | 含义 | 下一步 |
|---|---|---|
| **楔形消失** | v81 命中：bug 3 的根因就是「主 tiling area 被 spill.store 无谓扩张」 | ① 记录截图 + pandecode（599）作为证据；② 回到 ② 崩溃族 / ③ 卡死（看门狗 −4）；③ 跟踪上游 !44816 是否合并/改版，必要时把我们的真机结果回馈上游 |
| **楔形减轻** | 部分命中：还有第二个叠加因素（块尺寸/裁剪/呈现侧） | 用 pandecode 量 tiling area 与 fb 实际尺寸的**残差**；按附录 A 的 A.2.4/A.2.6/A.2.13/A.2.15 顺序做**单变量**试验（一次只动一个） |
| **楔形不变**（但指纹已 599） | 补丁生效但**不是**目视楔形的直接原因 | 先确认指纹确实变了（否则是「补丁没进 APK / 加载了旧驱动」，先修这个）；然后把注意力移到附录 A 的呈现侧/描述符侧候选（A.2.4 FBD 布局、A.2.15 Android WSI 自研代码） |
| **楔形变差** | 说明 spill 对齐对某条 pass 是**必需**的（或多 pass 共享 tiling area 的假设不成立） | **立即回滚到 v80**（§7），保留变差截图 + 指纹；回滚后不要继续在同一变量上叠加改动 |

补充判读纪律：
- **测画面必须用 nodiag 版**（`PANVK_DEBUG=0`）。diag 版带 `PANVK_DEBUG=1,kbase_diag`，其调试路径/输出会干扰画面与性能，不能作为画面判据。
- 同场景、同视角、同分辨率；v80 与 v81 各出一段录屏或同点截图对照（避免把随机性当成修复）。
- 若同时还要判「崩溃族/卡死」，请分别记录，不要用一次运行同时下两个结论。

---

## 7. 回滚（v81 → v80）

```bash
# 1) 还原源码（这两个 .bak 就是 v81 补丁前的状态）
cd /root/zenithblue/work/mesa/src/panfrost
cp -a vulkan/csf/panvk_vX_cmd_draw.c.bak-v81-1791219118 vulkan/csf/panvk_vX_cmd_draw.c
cp -a vulkan/panvk_vX_cmd_draw.c.bak-v81-1791219118     vulkan/panvk_vX_cmd_draw.c

# 2) 重编（注意必须先导出 NDK 工具链 PATH）
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
ninja -j2 -C /root/zenithblue/build/android-v4

# 3) 期望：回到 v80 的驱动（sha256 + 字节数都应一致）
sha256sum /root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so
#   期望 bb138465b9e1cf67125d1fb720958864cffa605bbd142ecb7327656978068bf1 (20052912 B)

# 4) 打包（用 Lead 自己的打包脚本；v80 版脚本已存在，输出 /root/final/mgl-panvk-v80.apk）
/root/pack_v80.sh /root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so
#   期望重现 v80 APK: 003f5a655a6a03fa… (10215983 B) / nodiag 94b9419e91bbebe1… (10215983 B)
```

若只回滚设备侧（不改源码）：直接把 v80 的驱动/APK 推回设备即可（v80 产物仍在 `/root/final/`，并已在 `/data/dsh_downloads/mgl-panvk-v80*.apk`）。
**注意**：`.bak` 只覆盖 v81 的 4 处改动，不会影响 fork 里其它 DIY 改动（本树改动全部未提交，见 §1）。

---

## 8. 未做 / 未验证（诚实清单）

1. **真机未验证**：本轮没有安装、没有拉起 App、没有截图、没有跑 pandecode → 「楔形是否消失」与「指纹 607→599」**都是未验证的预期**。
2. **两次重编 sha 相同**：由 Lead 执行并报告；`v81_buildA/B.log` 里**没有 sha 行**，日志只能证明两次编译的**对象集合一致**（12 个 `.o`）。本会话独立复核的是**当前**产物哈希。
3. **上游 !44816 仍是 opened（未合并）**：我们跑的是未合并补丁；若上游改版/关闭，需要重新对齐（§2.6 的折行差异也会让逐字节 cherry-pick 冲突）。
4. **附录 A 的候选全部未采纳、未编译、未上机**；「缺失/已含」的判定只到**文本级**（`grep` 指定行/模式），**没有**做编译验证或真机验证。
5. 附录 A.3 的 21 条「基线之后的上游 panvk 提交」中，我只对其中的 **4 条**（`4ecc4966` / `50bda0b8` / `260ed1ba` / `21365fba`）做了**逐行人工确认**为缺失；其余 17 条**未逐条核验**（我的批量文本抽样脚本有假阳性，其结论已作废，见 A.3 说明）。
6. 「基线之后有 862 个上游提交 / 其中 104 个碰 `src/panfrost` / 21 个碰 `src/panfrost/vulkan`」是按 **commit 日期**（`since=基线 commit 日期`）统计的**近似**，不是严格的祖先关系判定；未执行 `git fetch`（写 `.git` 超出本轮写域）。
7. `libMobileGL.so` / `classes.dex` 只验证了 **v79 → v81 之间相同**；未与 MobileGL 上游版本号做核对。
8. **未验证 APK 的签名/可安装性**（没有在设备上安装过）；签名块哈希只做了「两版不同」的记录。
9. 未验证 `/root/final/mgl-panvk-v81*.apk` 的 `versionCode/versionName` 在设备上是否与预期一致（只从打包脚本 sed 内容推断为 `versionCode=81` / `6.21-tilingarea[-nodiag]`）。
10. `rm` 全程未使用；本轮未产生需要清理的临时文件（临时副本都在 `/root/v81work/`）。

---

## 9. 附录 A：上游侦查（除 !44816 之外，只记录、不采纳）

### A.0 方法（只读，可复现，见 §10）

- 用 GitLab 只读 API（project id 176 = `mesa/mesa`）拉：**全部 open MR**（paged，17 页 1662 个，过滤出 pan 相关 102 个）、
  **按关键词搜 open MR / open issue**、**按 label 拉 open issue**（`panvk` 27 个、`panfrost` 122 个）、
  **基线之后碰 `src/panfrost/vulkan` 的提交**（21 个，并取每个的 diff）。
- 原始 JSON 落在 `/root/v81work/recon/`：`open_mrs_pan.json`、`keyword_search.json`、`issue_search2.json`、
  `upstream_panvk_commits_since_base.json`、`upstream_panvk_diffs.json`、`mr_details.json`、`issue_details*.json`、`key_commit_diffs.json`。
- 限制：`/search?scope=blobs`（按代码检索）与 `/labels` 在本实例需要鉴权 → **返回 401，未使用**；因此「哪些文件调用了 `pan_align_fb_tiling_area`」这类问题是用本地 `grep` 回答的。

### A.1 先更正两件事（避免把已含的当成候选）

| 项 | 结论 |
|---|---|
| **!44173**（open MR）`panvk/csf: wait for prior tiling work before reusing tiler heap` | **本树已含**（不是候选）。本树 `csf/panvk_vX_cmd_draw.c` 的 `get_tiler_desc()`（第 1225 行起）里有注释明确写着 `upstream MR !44173 -- "wait for prior tiling work before reusing tiler heap", reported as tile-aligned corruption on Mali-G720`，并实现了 `cs_wait_slots(b, dev->csf.sb.all_iters_mask)` 自等待。**它的上游描述与截图正是我们这类「Mali-G720 上 back-to-back render pass 的 tile 对齐花屏」**，可作为症状对照参考（`!44173` 描述里附了 `f169.png`）。 |
| **基线落后量** | 我们基线 `5a07217f034`（2026-09-18）之后，上游 main 有 **862** 个提交（近似，见 §8.6），其中碰 `src/panfrost` 的 **104** 个、碰 `src/panfrost/vulkan` 的 **21** 个。 |

### A.2 候选清单（按「与几何越界/楔形/拉伸/尖刺」的相关性分层）

> 「风险」= 采纳/试验该候选本身的代价与副作用风险；**全部未采纳**。

**A.2.1 ★ 缺失提交 `4ecc4966`（2026-09-24，已并入 main）— `panvk: Don't downcast CS func address to a 32-bit VA`**

- 改动只有 1 行，且**就落在我们这次改过的同一个函数 `get_fb_descs()` 里**：
  `uint32_t fn_addr = dev->draw_ctx->fns_bo->addr.dev + fn_idx * fn_stride;` → 改成 `uint64_t`。
- 为何可能相关：`fn_addr` 是「给 fb descriptor 打补丁的 CS 函数指针」，随后 `cs_move64_to(b, addr_reg, fn_addr)` 写进寄存器。
  **VA 一旦超过 4 GiB，32 位截断就会让 GPU 跳到错误地址去设置 framebuffer descriptor** → 完全可能出现「几何被画到 fb 之外 / 楔形 / 尖刺 / 描述符错乱」。
  我们这台的堆上限恰好与 issue **#15551**（G720 只有 ~4 GB 私堆）同域，`>4G` 不是理论问题。
- 本树实测：`grep -n "fn_addr" src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c` → 第 2114 行仍是 `uint32_t fn_addr =` ⇒ **缺失**。
- 风险：**低**（纯加宽类型，不改语义）；但需重编 + 真机验证，且若 `fns_bo` 常驻 <4 GiB 则看不到效果。→ **建议列为 v82 第一候选（单变量）。**

**A.2.2 ★ 缺失提交 `c4673eab` + `f333dd6d`（2026-09-24，已并入 main）+ issue #15551 — 堆/VA 放置**

- `panvk: Revisit the heap selection logic`（`panvk_device.h` +56/−13、`panvk_vX_device.c` +44/−47 等 7 文件）与
  `panvk: Don't force non-executable buffers to live in the first 4G`（7 文件）。
- 为何可能相关：地址放置/堆选择错 → 指针越界或别名 → 花屏、尖刺、卡死。**本树自己大改了这条路径**
  （`src/panfrost/lib/kmod/pan_kmod.c` +120、`panvk_vX_device.c` +114、`panvk_device_memory.c` +41、`panvk_priv_bo.c` +5），
  与上游这套修复**正交**，风险可能叠加。issue #15551 明确提出 G720 上「所有分配都进 ~4 GB 私堆」。
- 风险：**中**（涉及 VM/堆语义，动它可能引入卡死），且与自研 kbase 后端强耦合 → 只记录。

**A.2.3 ○ open MR `!42216` — `pan: use exact coords for framebuffer preload`**

- preload shader 用「全屏 varying 插值」取坐标，大尺寸下精度不足会**采到相邻 texel**（off-by-one），修复改用 `frag_coord`；作者 Arm，已 `Reviewed-by`。
- 为何可能相关：部分填充/blit 场景的边缘一像素错位 → 边缘「楔形/错行」外观；症状与我们的边缘异常同族。
- 风险：**低-中**（改 blitter preload 路径；触发面较窄，且原始报告是 OpenCL `test_cl_fill_images`）。

**A.2.4 ▲ open issue `#15460` — `[Panfrost/Valhall] Mali-G615: FBD_POINTER (R40) layout changed to Buffer Descriptor Array`**

- 报告者用 Frida 对 DDK 动态观察，称 v12/v13 的 `Framebuffer Parameters` 结构已不成立：`FBD_POINTER`(R40) 指向的是
  **32 字节 Buffer Descriptor 数组**（Descriptor 0 = Viewport、1 = Scissor、2 = Render Target），且 viewport 的
  scale/translation 变成 **FP32 投影矩阵**而不是整数。
- 为何可能相关：**本机 Mali-G720 正是 arch v12**，而 fb descriptor 就是承载 viewport/scissor/RT 信息的地方。
  若描述符语义与 `v12.xml` 不一致 → 视口/裁剪被错误解释 → 「几何被画到 fb 之外 / 楔形 / 拉伸」。
- 风险：**高**。这是**用户单方面的动态观察**，上游未见确认，issue 至今 opened；照它改等于重写 fb descriptor 布局，可能全线崩溃。
  → **强烈建议只做验证性观察（pandecode/DDK 对拍），不要直接改代码。**

**A.2.5 ○ open MR `!39366` — `pan: Rework PLS enabling in pan_fb_info`**

- 现状 `pls_enabled` 是盲标志；该 MR 引入 `pls_size` 并把 PLS 显式放在 framebuffer 开头、颜色 RT 排其后。
  **关键信息：改动前 PLS 可能与 RT0 别名，两者互相踩内存**（原 MR 描述原文）。
- 为何可能相关：fb 内互相覆盖 → 画面里出现「垃圾几何/色块」。
- 风险：**中**，但 **panvk（Vulkan）路径基本不走 PLS**（本树 `src/panfrost/lib/pan_desc.h:165` 仍是 `bool pls_enabled`）。
  → 列为 **GL 侧候选**，只有在确认走 GL/PLS 时才考虑。

**A.2.6 ○ open issue `#15546` — `Panfrost / Mali-G610: texture coordinate varyings delivered scaled (~100–256×)`**

- 症状：UV varying 到片元阶段被放大数百倍 → 纹理在网格上密铺（"scaled" = **拉伸同族**）。
- 为何可能相关：提供「varying 布局/系数错 ⇒ 画面被缩放」的机制类比，而 panvk 也有 varying/attribute 打包路径。
- 风险：**中**：它是 **panfrost GL** 的 `#version 120` 固定功能 texcoord 路径，**不能直接套到 panvk**；仅作机制线索。

**A.2.7 ○ open MR `!41468` — `panfrost: improve bounds checking for CSF indirect accesses`**

- Valhall 上间接查询会走软件路径逐个发 draw，`INDEX_BUFFER_SIZE` 未按 `draw.start` 偏移设置好；Bifrost 软件路径也要加强索引边界检查。
- 为何可能相关：索引越界 → 顶点数据被随意读取 → **尖刺/拉丝/长条**。若真机表现为「移动时偶发尖刺」值得回头查。
- 风险：**中**（改的是 panfrost GL 的 `glDrawElementsIndirect`；panvk 的间接绘制另有机制，本树已有 FAU 相关处理）。

**A.2.8 ○ open MR `!44910` — `panfrost: fix UB in pan_csf pointer based type-punning`**

- 用 `uint64_t*` 访问两个 `uint32_t` 的 struct 属 UB，新编译器上会失效；改为 union（C11 合法）。
- 为何可能相关：我们用 **NDK r27c clang** 编译；若编译器真在此处做了错误优化 → 寄存器/描述符内容错乱 → 花屏。
- 风险：**低-中**（1 处 union 改写，`Cc: mesa-stable`）。

**A.2.9 ○ open MR `!38889` — `vulkan,panvk: Merge subpasses`**

- 给 runtime render pass 加 subpass 合并（新 `vk_render` 构建器 + `try_merge_subpasses()`）。
- 为何可能相关：它**直接重塑 render pass / IR pass 结构**，与 `get_fb_descs()` 强耦合 —— 我们这轮补丁正是 IR pass 的 fb layout。
- 风险：**中**（大改，本 fork 未合；将来 rebase 时必须重新审 v81 语义）。

**A.2.10 ○ open MR `!44601` — `nir, vulkan: Big input attachment handling rewrite`**

- 把 input attachment 信息从隐式（`GLSL_SAMPLER_DIM_SUBPASS` + 变量）改为显式 image intrinsic。
- 为何可能相关：input attachment 与 IR pass 分组直接相关（我们的补丁就在 IR pass 上）。
- 风险：**低**（当前 bug 与 input attachment 没有证据链）。

**A.2.11 ○ open MR `!44591` — `pan/crc: Remove AFBC superblock size check against effective tile size`**

- 放宽 CRC（事务消除）限制，让 32×8 宽 AFBC RT 也能用 TE；要求超级块内所有 tile 都被消除。
- 为何可能相关：tile 粒度语义；若 CRC 判定错会出现「整块旧帧/块状撕裂」。
- 风险：**低-中**（仅 AFBC+CRC 场景；症状是块状而非楔形）。

**A.2.12 ○ 缺失提交 `50bda0b8`（2026-09-21，已并入 main）+ `98ed8110` + issue #15703 / #16050 — WSI 的 AFBC/modifier**

- `panvk: Enable AFBC for WSI images by default`：把调试开关从 `wsi_afbc`（**opt-in**）翻成 `wsi_no_afbc`（**opt-out**）；
  `98ed8110` 查 modifier 能力；#15703/#16050 都是「modifier 选择失败/不受支持」。
- 为何可能相关：WSI/AHB 走 AFBC 而 modifier/stride 选择错 → 典型「画面被拉伸/错位」。
- 本树实测：仍是 `PANVK_DEBUG_WSI_AFBC`（`panvk_instance.c:56` / `panvk_instance.h:31` / `panvk_physical_device.c:2071,2246`）⇒ **默认不开 AFBC for WSI**，
  即我们在该风险上**偏保守**；但只要有人 `PANVK_DEBUG=wsi_afbc` 就会踩到。
- 风险：**中**（呈现路径，Android AHB 特有）。

**A.2.13 ○ 缺失提交 `842ff466` / `83cda621`（2026-09-28，Android EFR）— 外部格式 resolve**

- `panvk: advertise VK_ANDROID_external_format_resolve on v10+` + `panvk: patch null color attachment and format for efr`（csf `+21`、通用 `+12/−1`）。
- 为何可能相关：我们是 DIY Android 端口，目前**没有**接 EFR/YUV resolve；一旦接 AHardwareBuffer 的 YUV/EFR 路径，格式错会表现为整体拉伸/色彩错位。
- 本树实测：`vk_android.h`/`vk_external_format_to_efr_format`/`vk_get_rendering_attachment_flags` 均不存在于 `panvk_vX_cmd_draw.c` ⇒ 缺失。
- 风险：**低**（当前很可能根本没走 EFR）。

**A.2.14 ○ 缺失提交 `260ed1ba`（2026-10-02，已并入 main）— `panvk: Honor resolve transfer function flags`**

- 增加 `panvk_resolve_attachment.flags` 与 `skip_transfer_function` 判定（sRGB 在 tile 内 resolve 时是否跳过传递函数）。
- 为何可能相关：**色彩**正确性（不是几何）。若把 sRGB 线性/非线性搞错，边缘可能出现色阶带，容易被误读成「楔形」。
- 本树实测：`skip_transfer_function` / `vk_get_rendering_attachment_flags` / `#include "vk_render_pass.h"` **全无** ⇒ 缺失。
- 风险：**低**。

**A.2.15 ★ open issue `#12350` — `panvk: Make support of Android Surfaceflinger Backend`**

- 上游**至今没有** Android SurfaceFlinger 后端的正式支持（issue 2025-01 创建，仍 opened）。
- 为何可能相关：我们在 `panvk_wsi.c`（+498 行）、`panvk_android.c`（+66）、`panvk_instance.c`（+77）、`panvk_physical_device.c`（+956）
  里补的 Android 呈现路径是**自研、非上游**代码。**如果楔形来自呈现/合成侧（stride、modifier、裁剪、buffer 复用），改 GPU tiling area 永远不会好。**
- 风险：**高优先级排查方向**（但这是自研代码，不是上游 MR）→ 建议在 v81 判读为「不变/减轻」时**优先**做一次「只换呈现路径」的对照实验。

**A.2.16 ○ open issue `#14948` — `panfrost,panvk: Call for help on Immortalis G720 with GPU hangs (Cix Sky1 SoC)`**

- 同芯片族（G720）的 GPU hang 汇总，labuled `gpu-hang` + `panvk`。
- 为何可能相关：对我们 bug ③（看门狗 −4）有参考价值（含别人的现场与硬件环境）。
- 风险：**低**（信息性）。

**A.2.17 ○ open issue `#15308` — `panvk: Support for Vulkan features/extensions for the new Minecraft Vulkan render`**

- Minecraft 新 Vulkan 渲染路径需要的特性清单（当前缺 `fillModeNonSolid`）。
- 为何可能相关：目标 App 就是 Minecraft（我们走 Java 版 + MobileGL DirectVulkan，路径不同，但特性需求可能重叠）。
- 风险：**低**（特性缺失 ≠ 几何错误）。

**A.2.18 ○ open issue `#15982`（2026-08-31）— `panvk/csf: is the render desc ringbuf's double mapping required, or would tail-padding do?`**

- CSF render descriptor ringbuf 的双映射是否必需。
- 为何可能相关：ringbuf/描述符被覆盖或回绕错 → fb/tiler 描述符错乱 → 与楔形/尖刺同域（描述符级）。
- 风险：**低-中**（架构讨论，无结论；正文为空）。

**A.2.19 ○ open MR `!41987`（与 !44816 同一作者 Ryan Zhang）— `panvk: stop exposing maintenance9 on valhall v10`**

- 分支名 `bugfix/G310_unbound_vertex_input_random_fail`：读**未赋值顶点属性**时行为随机失败（i.MX95 / G310 实测 dEQP 失败）。
- 为何可能相关：顶点属性读到垃圾 → 顶点位置乱 → **尖刺/拉伸**。
- 风险：**低-中**（v10 专属 + 需要着色器读未绑定属性；游戏通常绑定齐全）。

### A.3 基线之后碰 `src/panfrost/vulkan` 的 21 个上游提交（缺失面盘点）

**核验说明（重要）**：我只对下表打了 ★ 的 4 条做了**逐行人工确认**（结论：缺失）。其余 17 条**未逐条核验** ——
我最初写的批量文本抽样脚本会把「短且通用的新增行」（如 `cs_flush_stores(b);`、`struct panvk_image image = {0};`）误判为「已含」，
该脚本结论**已作废**，故此处只列清单，不下「缺失/已含」结论。逐条核验建议由 Lead 在允许窗口内 `git fetch` 后用 `git log -S`/`git cherry` 做。

| 日期 | 提交 | 标题 | 文件数 | 备注 |
|---|---|---|---|---|
| 10-02 | `78f9b71d` | anv, panvk, …: use new device memory report function | 9 | 基础设施 |
| 10-02 | `21365fba` | panvk: Advertise VK_KHR_maintenance10 | 3 | ★ **缺失**（`grep maintenance10` 无命中） |
| 10-02 | `260ed1ba` | panvk: Honor resolve transfer function flags | 3 | ★ **缺失**（见 A.2.14） |
| 10-02 | `8b2d038e` | panvk: Move to CmdEndRendering2KHR(..) | 2 | API 迁移 |
| 09-30 | `81db71cf` | panvk/csf: give compute jobs the TLS size of the shared allocation | 2 | 计算队列 TLS |
| 09-28 | `83cda621` | panvk: advertise VK_ANDROID_external_format_resolve on v10+ | 2 | Android EFR（A.2.13） |
| 09-28 | `842ff466` | panvk: patch null color attachment and format for efr | 2 | Android EFR（A.2.13） |
| 09-25 | `6409a5b1` | panvk: Advertise VK_EXT_image_compression_control | 3 | AFBC/压缩控制 |
| 09-25 | `e3a315d6` | panvk: Honor VkImageCompressionControlEXT | 2 | 同上 |
| 09-25 | `19f5cf79` | panvk: Report image compression properties | 3 | 同上 |
| 09-25 | `90be04d4` | panvk: Zero images built for device-level queries | 1 | 本树 `panvk_image.c` 3 处里 1 处有 `= {0}`、2 处无 → **未定论** |
| 09-24 | `fbb4993c` | panvk/csf: flush the FAU stores before indirect draws read them | 1 | 本树该文件有 12 处 `cs_flush_stores` → **未定论**（需逐行 diff） |
| 09-24 | `f333dd6d` | panvk: Don't force non-executable buffers to live in the first 4G | 7 | 堆/VA（A.2.2） |
| 09-24 | `4ecc4966` | panvk: Don't downcast CS func address to a 32-bit VA | 1 | ★ **缺失**（A.2.1，最高优先） |
| 09-24 | `c4673eab` | panvk: Revisit the heap selection logic | 7 | 堆/VA（A.2.2） |
| 09-24 | `4487dd7b` | panvk/csf: check malloc return values in queue submit storage init | 1 | 健壮性 |
| 09-24 | `d7d2cd7b` | panvk: validate descriptor counts in shader deserialization | 1 | 健壮性（崩溃族相关） |
| 09-21 | `eef8db80` | panvk: Advertise VK_EXT_swapchain_colorspace | 3 | WSI 特性 |
| 09-21 | `fb385376` | panvk: Advertise VK_KHR_incremental_present | 3 | 局部呈现（Android 上可能与「只重绘一部分」相关） |
| 09-21 | `50bda0b8` | panvk: Enable AFBC for WSI images by default | 3 | ★ **缺失**（A.2.12；本树仍 `wsi_afbc` opt-in） |
| 09-21 | `98ed8110` | panvk: Query modifier capabilities for format property advertisement | 3 | modifier（A.2.12） |

### A.4 明确排除（记录以免重复劳动）

- `!44815 ci/panfrost: Stop forcing sync in tests`、`!44831`(perf 工具安装)、`!44581`(外部总线宽度)、`!43810`(时间戳查询)、
  `!43688`(maxImageArrayLayers)、`!43410`(DST_ALPHA blend)、`!43359`(transform feedback)、`!43156`/`!40343`(cull/clip distance)、
  `!41366`(v15 支持)、`!43648`(atomic_int64) 等：与「几何越界/楔形/拉伸」无直接关系（特性、CI、性能、格式）。
- `#16052`（26.2 回归、Firefox 花屏）只影响 **Midgard（T860）**，与 Valhall/panvk 无关。
- `#13236`（32 位 ARM Alpine 起不来）与 GPU 渲染无关。

---

## 10. 附录 B：复现命令速查（本报告每个数字都可用这些命令重跑）

```bash
# ---- 产物哈希 / 字节数 ----
sha256sum /root/final/mgl-panvk-v81.apk /root/final/mgl-panvk-v81-nodiag.apk \
          /root/final/libvulkan_panfrost_v81.so /root/final/libvulkan_panfrost_v80.so
stat -c '%n %s' /root/final/mgl-panvk-v81.apk /root/final/mgl-panvk-v81-nodiag.apk
sha256sum /root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so

# ---- APK 内嵌驱动（两版都要跑）----
unzip -p /root/final/mgl-panvk-v81.apk        lib/arm64-v8a/libvulkan_freedreno.so | sha256sum
unzip -p /root/final/mgl-panvk-v81-nodiag.apk lib/arm64-v8a/libvulkan_freedreno.so | sha256sum

# ---- ZIP 条目表 / 每条 sha ----
unzip -v /root/final/mgl-panvk-v81.apk
for e in resources.arsc lib/arm64-v8a/libMobileGL.so lib/arm64-v8a/libvulkan_freedreno.so classes.dex AndroidManifest.xml; do
  printf '%s %s\n' "$(unzip -p /root/final/mgl-panvk-v81.apk "$e" | sha256sum | cut -d' ' -f1)" "$e"; done

# ---- 切分与重组 ----
cd /data/dsh_downloads
split -n 8 -d /root/final/mgl-panvk-v81.apk        v81p8_
split -n 8 -d /root/final/mgl-panvk-v81-nodiag.apk v81n8_
stat -c '%n %s' v81p8_* v81n8_ ; sha256sum v81p8_* v81n8_
cat v81p8_* | sha256sum ; cat v81n8_* | sha256sum
cat v81p8_* > /root/v81work/rejoin_p.apk && cmp /root/v81work/rejoin_p.apk /root/final/mgl-panvk-v81.apk && echo P_OK
cat v81n8_* > /root/v81work/rejoin_n.apk && cmp /root/v81work/rejoin_n.apk /root/final/mgl-panvk-v81-nodiag.apk && echo N_OK

# ---- 补丁内容与三方对比（不写真实源码）----
S=/root/zenithblue/work/mesa/src/panfrost ; U=/root/v81work/mrcheck/mesa/src/panfrost
diff -u $S/vulkan/csf/panvk_vX_cmd_draw.c.bak-v81-1791219118 $S/vulkan/csf/panvk_vX_cmd_draw.c
diff -u $S/vulkan/panvk_vX_cmd_draw.c.bak-v81-1791219118     $S/vulkan/panvk_vX_cmd_draw.c
# 重建「上游补丁版」副本（只写 /root/v81work/mrcheck）：
mkdir -p /root/v81work/mrcheck/mesa/src/panfrost/vulkan/csf
cp $S/vulkan/csf/panvk_vX_cmd_draw.c.bak-v81-1791219118 /root/v81work/mrcheck/mesa/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
cp $S/vulkan/panvk_vX_cmd_draw.c.bak-v81-1791219118     /root/v81work/mrcheck/mesa/src/panfrost/vulkan/panvk_vX_cmd_draw.c
cd /root/v81work/mrcheck/mesa && patch -p1 --dry-run < /root/v81work/mr_diff_44816.diff   # 该文件见下行获取
curl -sSL -o /root/v81work/mr_diff_44816.diff https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44816.diff
patch -p1 < /root/v81work/mr_diff_44816.diff
diff -u $U/vulkan/csf/panvk_vX_cmd_draw.c $S/vulkan/csf/panvk_vX_cmd_draw.c   # 只应出现 hunk② 的折行差异
tr -s '[:space:]' '\n' < $U/vulkan/csf/panvk_vX_cmd_draw.c | diff - <(tr -s '[:space:]' '\n' < $S/vulkan/csf/panvk_vX_cmd_draw.c) && echo TOKENS_SAME

# ---- 上游侦查（只读 API）----
A=https://gitlab.freedesktop.org/api/v4/projects/176
curl -sS "$A/merge_requests?state=opened&search=panvk&per_page=100&order_by=updated_at"
curl -sS "$A/issues?state=opened&labels=panvk&per_page=100&order_by=updated_at"
curl -sS "$A/repository/commits?ref_name=main&path=src%2Fpanfrost%2Fvulkan&since=2026-09-18T16:45:08Z&per_page=100"
curl -sS "$A/repository/commits/4ecc49663347572580acddee5229df36bc843b84/diff"
curl -sS -D - -o /dev/null "$A/issues?state=opened&labels=panvk&per_page=1" | grep -i x-total

# ---- 本轮「确定性」日志（只读引用）----
wc -l /root/v81work/v81_buildA.log /root/v81work/v81_buildB.log
grep -c "Compiling C object.*panvk_vX_cmd_draw" /root/v81work/v81_buildA.log   # 期望 12
grep -c "Compiling C object.*panvk_vX_cmd_draw" /root/v81work/v81_buildB.log   # 期望 12
```

---

## 11. 红线自检

| 红线 | 状态 |
|---|---|
| 不修改 `/root/mesa`、`/root/MobileGL` | ✅ 未触碰 |
| 不执行 `git checkout / stash / reset` | ✅ 未执行（也未 `fetch`）；本轮对 fork 只做 `git log/status/diff --stat` 只读查询 |
| 禁用 `rm` | ✅ 未使用（本轮无需清理） |
| 不操作手机（设备侧归 Lead） | ✅ 未调用任何 `android_*` 工具、未装包、未拉起 App |
| 不把密钥/token 写入文件或报告 | ✅ 报告与 `/root/v81work/recon/*` 只含路径、哈希、上游 URL |
| 只写 `/root/research/55-v81-tilingarea.md`、`/data/dsh_downloads/v81*_*`、`/root/v81work/` | ✅ 写入清单：报告 1 个 + 分片 16 个 + `/root/v81work/` 下（`rejoin_p.apk`、`rejoin_n.apk`、`mr44816.diff`、`e_*.txt`、`sha_*.txt`、`man_p.bin`、`man_n.bin`、`mrcheck/`、`recon/`） |
| 不改源码 | ✅ 源码改动全部来自 Lead（§2 的 4 处）；本会话未写任何 `src/` 文件 |
