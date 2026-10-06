# v80 — get_cs_cfg 族守卫续修（打包由新会话补齐）+ 对"+0x328 第二处"的更正

日期：2026-10-06 · 构建树 `/root/zenithblue/work/mesa` · 构建目录 `/root/zenithblue/build/android-v4`
上游基线 commit `5a07217f034`（radv: slightly rework initializing the DCC predicate）· 全部改动未提交（同前几版约定）

---

## 0. 一句话

v80 = **v79 之上再加 3 处空/越界守卫**（全在 `src/panfrost/genxml/decode_csf.c`）。
**源码由上一会话的 v80 子智能体于 00:35 写入、00:36 编译完成，但在打包前中断**；
本会话（新模型接管）完成：打包 → 切分 → 校验 → 本报告。
并且**更正接力卡的一条推断**：v79 的 `get_cs_cfg+0x328` 与 v76 的 `+0x320` 是**同一条指令**，不是"同函数第二处"（§3）。

---

## 1. 本轮边界（诚实声明）

| 做的事 | 没做的事 |
|---|---|
| 派生并运行 `/root/pack_v80.sh` · `/root/pack_v80_nodiag.sh` | **没有改任何源码**（源码 = 00:35 状态，原样编译） |
| 切分 `/data/dsh_downloads/v80p8_00..07` · `v80n8_00..07` 并校验重组 | **没有重新编译**（沿用 00:36 产物；仅跑 ninja 复核） |
| 复核确定性（重跑 ninja ⇒ .so 哈希不变） | **没有操作手机**（本会话档位 workspace-write，设备控制面被门挡住） |
| 写本报告 | 未碰 v71/v72/v73 检查点、未碰 MobileGL、未做 git 操作 |

---

## 2. 源码改动（v80 相对 v79，共 3 处）

`decode_csf.c`：

```diff
@@ -1297,6 +1297,14 @@
    uint64_t *cs = pandecode_fetch_gpu_mem(ctx, address, length);
+
+   /* The subqueue is not (fully) mapped: pandecode_fetch_gpu_mem()
+    * returns NULL and qctx->ip would then be dereferenced by the
+    * instruction fetch loop below. */
+   if (!cs) {
+      fprintf(stderr, "CS call to unmapped address %" PRIx64 "\n", address);
+      return false;
+   }
+
    qctx->ip = cs;

@@ -1440,6 +1448,11 @@
       uint32_t *src =
          pandecode_fetch_gpu_mem(ctx, addr, util_last_bit(I.mask) * 4);
+
+      /* Not (fully) mapped: pandecode_fetch_gpu_mem() returns NULL and
+       * src[i] below would be dereferenced. */
+      if (!src)
+         break;
+
       for (uint32_t i = 0; i < 16; i++) {

@@ -2408,6 +2421,23 @@
    if (!instrs)
       return NULL;             /* ← v79 已加的守卫 */
+
+   /* 越界复查：映射可能只覆盖 CS 二进制开头，末尾越界。
+    * __pandecode_fetch_gpu_mem() 只用 assert 检查该边界，而 release 下 assert 被编译掉。 */
+   struct pandecode_mapped_memory *cs_mem =
+      pandecode_find_mapped_gpu_mem_containing(ctx, bin);
+
+   if (!cs_mem || !cs_mem->addr || bin < cs_mem->gpu_va ||
+       (bin - cs_mem->gpu_va) > cs_mem->length ||
+       bin_size > cs_mem->length - (bin - cs_mem->gpu_va) ||
+       ((uintptr_t)instrs & (sizeof(uint64_t) - 1)))
+      return NULL;
+
    cfg = rzalloc(symbols, struct cs_code_cfg);
```

**前两处**正是 53 号报告 §9 点名的"已知残余风险"（`pandecode_fetch_gpu_mem()` 另两个无空检查的调用点）。
`decode.h` **未改**（`decode.h.bak-v80-1791217959` 与当前文件 `diff` 为空）。

### 2.1 机器码验证（守卫确实进码）

`get_cs_cfg` 四个副本长度：v79 `1588 → ` **v80 `1684 B`（+96 B）**；副本入口 `0xee40dc/0xef4df0/0xf05914/0xf166a4` → `0xee40ec/0xef4e60/0xf059e4/0xf167d4`。

copy2 守卫区（`0xef4ed8` 起，逐条）：

```
  ef4ed8: eb0802e8   subs x8, x23, x8          ; 长度下界比较
  ef4edc: 54000142   b.hs 0xef4f04             ; 通过则继续
  ef4ee0: aa1f03e0   mov  x0, xzr              ; ★ 返回 NULL 路径
  ...
  ef4f00: d65f03c0   ret
  ef4f04: 8b1702ca   add  x10, x22, x23
  ef4f10: cb18015a   sub  x26, x10, x24        ; = instrs
  ef4f14: f2400b5f   tst  x26, #0x7            ; ★ 对齐检查 (instrs & 7)
  ef4f18: 54fffe61   b.ne 0xef4ee4             ; → 返回 NULL
  ef4f1c: f9400d29   ldr  x9, [x9, #0x18]      ; cs_mem->length
  ef4f20: eb08013f   cmp  x9, x8
  ef4f24: 54fffe03   b.lo 0xef4ee4             ; → 返回 NULL
  ef4f28: 2a1503ea   mov  w10, w21
  ef4f2c: cb080128   sub  x8, x9, x8           ; length - offset（越界复查）
```

---

## 3. ★ 更正：接力卡"+0x328 ⇒ 同函数还有第二处"是误判

接力卡 §1 bug2 写：*"v79 修后崩溃点从 `+0x320` 挪到 `+0x328` ⇒ 同函数还有第二处"*。**按反汇编核对，这条不成立。**

| | v76 | v79 |
|---|---|---|
| `get_cs_cfg` 长度 | 1568 B | **1588 B**（+20 B，正是 v79 新增 `cbz x8` 守卫，53 号报告附录 B 已载明） |
| copy2 入口 | `0xef6444` | `0xef4df0` |
| 争议偏移 | `+0x320` = `0xef6764` | `+0x328` = `0xef5118` |
| 该处指令 | `f87b7976  ldr x22, [x11, x27, lsl #3]` | **`f87b7976  ldr x22, [x11, x27, lsl #3]`（逐字节相同）** |
| 其后序列 | `d379fec8 lsr x8,x22,#57` / `7100411f cmp w8,#0x10` / `54fffe61 b.ne` | **完全相同** |

⇒ `+0x328` = `+0x320` **同一条指令随函数增长整体位移**，`0x328 − 0x320 = 8`，而函数恰好长了 `20 B`（前段守卫把该指令往下推）。
**"第二处"是把位移读成了新位置。**

**这条更正的推论**：v79 已在 `get_cs_cfg` 入口加了针对 `instrs` 的 `cbz x8 → return NULL`（53 号报告附录 B 实证在码）。
既然 `0xef5118` 处的 `x11` 按 53 号的寄存器追踪就是 `cfg->instrs`，那么：
- 若 v79 真崩在 `0xef5118`，`cfg->instrs` 为 NULL 的来源**必须在守卫之外**（缓存 cfg 路径 / 另外 3 个副本 / 或者真机当时根本没换上 v79 的 .so —— 见坑①）；
- 或者那次"v79 崩溃"的 hs_err 属于**别的二进制**（地址自然不同），"+0x328"是把绝对地址换算成函数内偏移时用错了基址。

⚠️ **在拿到 hs_err 里的【绝对地址】之前，不要再把 `+0x328` 当"第二处空指针"引用。**
判据应改为：**看 `Problematic frame` 的函数名 + 绝对地址**，并与当轮真机 .so 的 sha256 绑定（§6）。

---

## 4. 产物与哈希

| 产物 | 路径 | sha256 | 字节 |
|---|---|---|---|
| 新 `.so` | `/root/final/libvulkan_panfrost_v80.so` | `bb138465b9e1cf67125d1fb720958864cffa605bbd142ecb7327656978068bf1` | 20 052 912 |
| 诊断版 APK | `/root/final/mgl-panvk-v80.apk` | `003f5a655a6a03fae0b5b0bff73c461d806001804c3ef97e8e20f17b3a75a80d` | 10 215 983 |
| 干净版 APK | `/root/final/mgl-panvk-v80-nodiag.apk` | `94b9419e91bbebe1b49d9b3f5845f7db7892c4a4e1c31256772edba9acd0e920` | 10 215 983 |
| 打包脚本（诊断） | `/root/pack_v80.sh` | — | 由 `pack_v79.sh` sed 派生 |
| 打包脚本（干净） | `/root/pack_v80_nodiag.sh` | — | 由 `pack_v79_nodiag.sh` sed 派生 |
| 源码备份 | `decode_csf.c.bak-v80-1791217959` · `decode.h.bak-v80-1791217959` | — | 00:32 |

- manifest：`versionCode 74→80` · `versionName 6.14-defer-diag → 6.20-cscfg2`（干净版 `6.20-cscfg2-nodiag`）
- env（`aapt2 dump xmltree` 实测）：诊断版 `…:PANVK_DEBUG=1,kbase_diag:…` · 干净版 `…:PANVK_DEBUG=0:…` ✅

### 4.1 确定性复现校验

`ninja src/panfrost/vulkan/libvulkan_panfrost.so` 重跑 ⇒ `.so` sha256 **仍是 `bb138465b9e1cf67…`**，
且 `unzip -p mgl-panvk-v80.apk lib/arm64-v8a/libvulkan_freedreno.so | sha256sum` = 同值
⇒ **APK 内驱动与当前源码一一对应**（非空载荷、非旧版）。

---

## 5. 切分与重组校验（8 路）

| 分片 | 字节 |
|---|---|
| `v80p8_00..06` | 1 276 998 各 |
| `v80p8_07` | 1 276 997 |
| `v80n8_00..06` | 1 276 998 各 |
| `v80n8_07` | 1 276 997 |

| 套 | `cat *p8_* \| sha256sum` | APK sha256 | |
|---|---|---|---|
| `v80p8_` | `003f5a655a6a03fae0b5b0bff73c461d806001804c3ef97e8e20f17b3a75a80d` | 同 | ✅ |
| `v80n8_` | `94b9419e91bbebe1b49d9b3f5845f7db7892c4a4e1c31256772edba9acd0e920` | 同 | ✅ |

HTTP 分发 `http://64.81.112.146:18080/` 实测 `v80p8_00` 返回 **200** ✅
取件：`bash 取文件.sh v80p8_ mgl-panvk-v80.apk 003f5a655a6a03fa 8`（干净版把前缀/期望值换成 `v80n8_` / `94b9419e91bbebe1`）

---

## 6. 上机判读表（真机验证用）

**第一步（必须，否则整轮数据作废）**：抄录 hs_err 基线 → 解驱动 → 校验。

```bash
# ① 基线（坑②）：先把清单抄走
ls "<游戏目录>/hs_err_pid"*.log 2>/dev/null | xargs -n1 basename > /sdcard/MG/hs_err-baseline.txt
# ② 换驱动（坑①）：必须解出来放进 /data/local/tmp 并核对 sha256 = bb138465b9e1cf67…
unzip -p /sdcard/MG/mgl-panvk-v80.apk lib/arm64-v8a/libvulkan_freedreno.so \
  > /data/local/tmp/libvulkan_freedreno.so
sha256sum /data/local/tmp/libvulkan_freedreno.so     # ★ 必须 = bb138465b9e1cf67125d1fb720958864cffa605bbd142ecb7327656978068bf1
```

| 观察项 | 崩溃消失（最佳） | 显著变长（次佳） | 仍崩在同族 | 崩在新函数 |
|---|---|---|---|---|
| 看什么 | 无新 `hs_err_pid*.log` | `elapsed` 比 v76 的 68.2 s 明显增大（≥2×） | `Problematic frame` 仍是 `get_cs_cfg`/`panvk_cmd_draw`/`update_tls`/`cmd_dispatch` | 完全不同的函数 |
| 怎么读 | ⇒ 空指针族命中 | ⇒ 命中但非唯一 | ⇒ **记绝对地址**，按 §3 换算前先确认当轮 .so 的 sha256（基址 = `0xef4e60`） | ⇒ 另立案 |

**额外必查（本轮新增，直接回应 §3 的更正）**：
若 hs_err 又指向 `get_cs_cfg`，**必须同时记录**：
1. `Problematic frame` 的**绝对地址**；
2. 当轮 `/data/local/tmp/libvulkan_freedreno.so` 的 **sha256**；
3. 该 .so 里 `get_cs_cfg` 四个副本的入口（`readelf -sW`）。
⇒ 三者齐了才能判定"是不是同一条指令、是不是第二处"。**缺任一，结论按待定处理。**

**判据行（每轮必核）**：必须出现
`Magma (MobileGL Core) (Mali-G720 MC12, Vulkan 1.4.363, Driver 26.2.99)`
出现 `zink` / `Mali-G720-Immortalis MC12` ⇒ 本轮数据作废。

**测画面必须用干净版**（`mgl-panvk-v80-nodiag.apk`，`PANVK_DEBUG=0`）——kbase_diag 自带 42 处 `cs_flush_stores`，会掩盖真实 flush hazard（坑⑧）。

---

## 7. 回滚

```bash
# 源码回滚到 v80 前（= v79 状态）
TS=1791217959
M=/root/zenithblue/work/mesa
cp -f $M/src/panfrost/genxml/decode_csf.c.bak-v80-$TS $M/src/panfrost/genxml/decode_csf.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja src/panfrost/vulkan/libvulkan_panfrost.so
# 期望 sha256 = 50b8a2eab2f2c74bb803566b63486390f3b92295106013495baa8f65af6672a（20 052 344 B，= v79）
```
退回现成 APK：`mgl-panvk-v79.apk`（`08244f58…`）/ `v79-nodiag.apk`（`47e59302…`）。

---

## 8. 未做 / 未验证

- **v80 真机表现未验证**（本会话档位 workspace-write，设备控制面被门挡住；未操作手机）。
- **v79 那轮"崩溃"的实际 hs_err 未取到**：手机 `/sdcard/MG/` 下无 hs_err 副本，hs_err 目录已空（坑②），
  `/sdcard/MG/cap.txt`(27 MB) 内 `grep get_cs_cfg` = 0 命中、无 `SIGSEGV/hs_err/Problematic frame` 字样。
  ⇒ **"+0x328"的原始证据在本会话已不可复查**，这正是 §3 只能按反汇编反驳、不能按现场反驳的原因。
- **当轮真机 `/data/local/tmp/libvulkan_freedreno.so` 的 sha256 未取**（需设备通道）⇒ 无法排除坑①（在测旧版）。
- 3 处守卫里 **`get_cs_cfg` 的越界复查（第 3 处）是否 DCE、运行期是否真的触发：未验证**（机器码在册见 §2.1，但触发条件未知）。
- 53 号报告 §9 的另两个调用点**本轮已覆盖**；`pandecode_fetch_gpu_mem()` 是否还有**其他**无空检查调用点：**未穷举**。
- bug 3（画面楔形，MR!44816 路线）与 bug 4（卡死 / all-slots WAIT 检查点）：**本轮未推进**。
