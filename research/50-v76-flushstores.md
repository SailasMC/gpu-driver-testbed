# 50 — v76 = v74 树 + 回移上游 MR!44659（`cs_flush_stores(b)` 于 indirect draw 之前）

任务：实施型子智能体 / task-v76。
状态：**已完成**（撤 v75 → 确定性对照通过 → 单变量回移上游 diff → 编译 exit 0 → 二次重编可复现 → APK 已出并全部校验 → 已切分 8 份分发）。
**本轮未上真机**（任务明令「禁止操作手机」✗），交付物只到 APK + 切分件；v76 的**效果**判读属于下一轮真机的事。

---

## 0 结论速览

| 项 | 结果 |
|---|---|
| 本轮唯一行为变量 | `panvk_vX_cmd_draw.c` 的 `launch_indirect_draw()` 内新增 2 行（空行 + `cs_flush_stores(b);`） |
| 上游 diff 落点 | 我们树 `src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c:3400-3401`（v74 树行号：插入于旧 3399 与旧 3400 之间） |
| 与上游是否逐字一致 | **逐字一致**（新增 2 行与上游 GitLab diff 完全相同；落点上下文与上游 post-image 逐字节相同，见 §3.3） |
| v75 完成写改动是否整体撤掉 | **是**，`panvk_vX_gpu_queue.c` 已回到 v74 原文，与 `panvk_vX_gpu_queue.c.v74-patched` **byte-identical**，无 v75 残留 |
| **撤改动后是否逐位等于 `1df10436`** | **是，逐位相同** ✅ `1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56`（20 055 024 B） |
| 编译 exit 0？ | **是**（对照构建 + v76 build1 + v76 build2 三次 ninja 全部 `NINJA_RC=0`） |
| C2 核实结论 | **在**（`panvk_vX_cmd_draw.c:1234`，`get_tiler_desc()` 内）；报告 48 称其缺失**不成立**；本轮按令**未改** ✗ |
| v76 `.so` sha256 | `fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109`（20 057 992 B） |
| v76 `.apk` sha256 | `1bb9066bbcf24d6f7a0b58fcc9a6490b2e532ec9210a04ef5c172f0356d70e20`（10 220 079 B） |
| 确定性对照 / 可复现 | 对照构建逐位等于 v74 ✅；v76 二次重编 sha 相同 ✅ |
| 切分分发 | `/data/dsh_downloads/v76p8_00..07` 已就绪，8 份重组 sha == 原 APK ✅ |
| 前置报告 48 | ⚠️ **未在磁盘上找到**（见 §7），本轮改以任务给定的 MR 号为准并经 GitLab API 独立验证 |

---

## 1 交付物

| 文件 | sha256 | 字节 |
|---|---|---|
| `/root/final/mgl-panvk-v76.apk` | `1bb9066bbcf24d6f7a0b58fcc9a6490b2e532ec9210a04ef5c172f0356d70e20` | 10 220 079 |
| 新驱动 `/root/final/libvulkan_panfrost_v76.so` | `fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109` | 20 057 992 |
| 构建产物 `build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so` | 同上（一致） | 20 057 992 |
| 改后的 `csf/panvk_vX_cmd_draw.c` | `615f880dd764ea7a33896073bb53a6e1d726f47bd1feaecbb8703e7e5344fb51` | 191 683 |
| 撤回后的 `csf/panvk_vX_gpu_queue.c`（= v74 原文） | `48da562136c2b89f1b044a96fd336aad7d60e7886ba49f7fe06a86ae0098d2db` | 181 132 |
| `/data/dsh_downloads/mgl-panvk-v76.apk` | 同 APK，逐字节一致 | 10 220 079 |

切分件（`split -n 8 -d`）：

| 文件 | 字节 |
|---|---|
| `v76p8_00` … `v76p8_06` | 1 277 510 ×7 |
| `v76p8_07` | 1 277 509 |
| **合计** | 10 220 079 |

`cat v76p8_* \| sha256sum` = `1bb9066bbcf24d6f7a0b58fcc9a6490b2e532ec9210a04ef5c172f0356d70e20` == 原 APK ✅

> 全程**未使用 `rm`** ✗；v74/v75 的 APK **未被改动** ✗（见 §5.4）。

---

## 2 v75 完成写改动的整体撤回（回到 v74 发射序列）

任务要求：v75 的完成写改动**整体撤掉**，回到 v74 的发射序列，且不得丢失 v71/v72/v73 的检查点与 ATOMIC/SEQNO 打印。

### 2.1 撤回动作

```
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf
cp -f panvk_vX_gpu_queue.c panvk_vX_gpu_queue.c.bak-v76-prerevert-1791215266   # 先备份当前 v75 态
cp -f panvk_vX_gpu_queue.c.bak-1791214365        panvk_vX_gpu_queue.c          # .bak-1791214365 == v74-patched
```

先验证备份等价关系（撤回前已核对）：

```
$ diff -q panvk_vX_gpu_queue.c.bak-1791214365 panvk_vX_gpu_queue.c.v74-patched
IDENTICAL
```

撤回后复核：

```
$ diff -q panvk_vX_gpu_queue.c panvk_vX_gpu_queue.c.v74-patched
REVERT_OK identical to v74-patched
$ grep -n "kbase_seqno_store_enabled\|kbase_defer_diag_enabled\|PANVK_KBASE_SEQNO_STORE\|PANVK_KBASE_DEFER_DIAG" panvk_vX_gpu_queue.c
(residue grep rc=1)        # ← 无任何 v75 残留
```

### 2.2 v74 发射序列已恢复（`panvk_vX_gpu_queue.c:1584-1586`）

```c
1584:    cs_move64_to(&b, val64, 1);
1585:    cs_sync64_add(&b, true, MALI_CS_SYNC_SCOPE_SYSTEM, val64, addr64,
1586:                  cs_defer(0, SB_ID(DEFERRED_SYNC)));
```

v75 曾据此分叉出的 `kbase_seqno_store_enabled()` / 绝对 STORE64 + LS wait 分支、以及 `kbase_defer_diag_enabled()` 门控，**全部随撤回消失**（见 §2.1 的 residue grep）。

### 2.3 检查点与打印未丢失（逐项核实）

| 项 | 核实 |
|---|---|
| v71/v72/v73 检查点枚举 | `grep -c PANVK_KBASE_CKPT panvk_vX_gpu_queue.c` = **53** 处 |
| `WRAP_BEFORE_SYNC_ADD` 检查点（v72 wrapper ckpt） | 在 `632`（名字表）+ `1582`（调用点） |
| ATOMIC 打印 | 在 `1165/1169/1171/1172/1188` |
| SEQNO 打印 | 在 `1045`（`kbase: SEQNO cell+0 = … expected target …`） |
| `kbase_log_defer_diag` 调用点 | 在 `1199`（v75 曾把它包进 `if (kbase_defer_diag_enabled())`，撤回后恢复为无条件调用 = v74 原文） |

撤回依据是「与 canonical v74 文件 byte-identical」这一硬事实，故上述项**按构造**不可能丢失。

---

## 3 上游 MR!44659 的原始 diff 与落点

### 3.1 取 diff 的过程（可复现）

```
# 项目 id 核实：176 == mesa/mesa
curl -s "https://gitlab.freedesktop.org/api/v4/projects/176"
#   -> {"id":176,"name":"mesa","path_with_namespace":"mesa/mesa", ...}

# MR!44659 核实
curl -s "https://gitlab.freedesktop.org/api/v4/projects/176/merge_requests/44659"
#   -> {"id":150037,"iid":44659,"project_id":176,
#       "title":"panfrost/panvk: fix an indirect draw race and add missing v11 cases", ...}

# 目标 commit 的 diff
curl -s "https://gitlab.freedesktop.org/api/v4/projects/176/repository/commits/fbb4993c5d7781c4fcbfba018b938ee9a587eb85/diff"
```

commit 元数据：

| 字段 | 值 |
|---|---|
| id | `fbb4993c5d7781c4fcbfba018b938ee9a587eb85` |
| short_id | `fbb4993c` |
| title | `panvk/csf: flush the FAU stores before indirect draws read them` |
| created_at | `2026-09-24T16:30:24.000+00:00` |
| parent | `e04071d70f9321369955ea3ecd38d606d7d6f672` |
| stats | `{additions: 2, deletions: 0, total: 2}` |
| 文件数 | **1** |

commit message 全文：

```
panvk/csf: flush the FAU stores before indirect draws read them

launch_indirect_draw() copies first_vertex/base_instance from the indirect
buffer into the vertex shader FAUs with STORE and then issues RUN_IDVS,
which reads them back from memory. Nothing orders the stores before the
draw, so a draw can pick up the values of the previous iteration. The
attribute patching helper right above flushes its stores for the same
reason.

On Mali-G615 this shows up as geometry drawn at the wrong offsets in
dEQP-VK.draw.*.indirect_draw.*.first_instance.*: a 25-case subset of those
failed in 4-7 out of 8 runs before this change and in 0 out of 16 after.
A test of the same family is listed in panfrost-g925-flakes.txt.

Fixes: a5a0dd3ccc0 ("panvk: Implement multiDrawIndirect for v10+")
Assisted-by: Claude Code (Claude Opus 5.5)
Reviewed-by: Aksel Hjerpbakk <aksel.hjerpbakk@arm.com>
Reviewed-by: Christoph Pillmayer <christoph.pillmayer@arm.com>
Part-of: <https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44659>
```

### 3.2 上游 diff 原文（逐字，`fbb4993c` 唯一一个 hunk）

```
### FILE: src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
@@ -3371,6 +3371,8 @@ launch_indirect_draw(struct panvk_cmd_buffer *cmdbuf,
                        shader_remapped_sysval_offset(
                           vs, sysval_offset(graphics, vs.base_instance)));
          }
+
+         cs_flush_stores(b);
       }
 
       /* NIR expects zero-based instance ID, but even if it did have an
```

（原始 JSON 已存 `/root/v76work/diff.json`，抽出的纯 diff 存 `/root/v76work/upstream_fbb4993c.diff`；
该 commit 的完整上游源文件存 `/root/v76work/upstream_cmd_draw.c`，5040 行。）

### 3.3 我树落点：file:line 与改动前后对照

落点：`/root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c`
函数：`launch_indirect_draw()`（函数起始第 **3314** 行）
块：`if (patch_faus) {`（v74 树第 **3388** 行）

**改动前（v74 树，3388–3402）：**

```
3388:       if (patch_faus) {
3389:          if (shader_uses_sysval(vs, graphics, vs.first_vertex)) {
3390:             cs_store32(b, cs_sr_reg32(b, IDVS, VERTEX_OFFSET), vs_fau_addr,
3391:                        shader_remapped_sysval_offset(
3392:                           vs, sysval_offset(graphics, vs.first_vertex)));
3393:          }
3394: 
3395:          if (shader_uses_sysval(vs, graphics, vs.base_instance)) {
3396:             cs_store32(b, cs_sr_reg32(b, IDVS, INSTANCE_OFFSET), vs_fau_addr,
3397:                        shader_remapped_sysval_offset(
3398:                           vs, sysval_offset(graphics, vs.base_instance)));
3399:          }
3400:       }                       ← 缺 flush：两次 STORE32 之后直接进入 RUN_IDVS
3401: 
3402:       /* NIR expects zero-based instance ID, but even if it did have an
```

**改动后（v76 树，3388–3404）：**

```
3388:       if (patch_faus) {
3389:          if (shader_uses_sysval(vs, graphics, vs.first_vertex)) {
3390:             cs_store32(b, cs_sr_reg32(b, IDVS, VERTEX_OFFSET), vs_fau_addr,
3391:                        shader_remapped_sysval_offset(
3392:                           vs, sysval_offset(graphics, vs.first_vertex)));
3393:          }
3394: 
3395:          if (shader_uses_sysval(vs, graphics, vs.base_instance)) {
3396:             cs_store32(b, cs_sr_reg32(b, IDVS, INSTANCE_OFFSET), vs_fau_addr,
3397:                        shader_remapped_sysval_offset(
3398:                           vs, sysval_offset(graphics, vs.base_instance)));
3399:          }
3400:                                ← 新增（空行）
3401:          cs_flush_stores(b);   ← 新增（上游原文，9 空格缩进）
3402:       }
3403: 
3404:       /* NIR expects zero-based instance ID, but even if it did have an
```

**我们树的等价 unified diff**（`diff -u` 备份 vs 现文件，存 `/root/v76work/our_v76.diff`）：

```
--- panvk_vX_cmd_draw.c.bak-v76-1791215327	2026-10-05 23:48:47
+++ panvk_vX_cmd_draw.c	2026-10-05 23:48:47
@@ -3397,6 +3397,8 @@
                        shader_remapped_sysval_offset(
                           vs, sysval_offset(graphics, vs.base_instance)));
          }
+
+         cs_flush_stores(b);
       }
 
       /* NIR expects zero-based instance ID, but even if it did have an
```

与上游 hunk 结构**逐字相同**，只有 hunk 行号（3397 vs 3371）与 hunk 头的函数名后缀不同（上游带 `launch_indirect_draw(struct panvk_cmd_buffer *cmdbuf,`）。
行号差 = +26，来自我们树在 v71–v74 期间累积的既有改动，与本次改动无关。

**逐字节验收**（我方 3395–3402 vs 上游 3369–3376）：

```
$ diff <(awk 'NR>=3395 && NR<=3402' panvk_vX_cmd_draw.c) \
       <(awk 'NR>=3369 && NR<=3376' /root/v76work/upstream_cmd_draw.c)
BYTE-IDENTICAL to upstream post-image
```

### 3.4 为什么这处改动与本轮「渲染错误」主题对得上（旁证，非论证）

同一个 `launch_indirect_draw()` 循环里，**属性补丁**路径早就有 flush —— 我们树 `panvk_vX_cmd_draw.c:3301-3303`：

```c
3301:             cs_store32(b, attrib_offset, vs_drv_set,
3302:                        pan_size(ATTRIBUTE) * i + (2 * sizeof(uint32_t)));
3303:             cs_flush_stores(b);
```

这正是 commit message 所说的「The attribute patching helper right above flushes its stores for the same reason」。
即：**同一循环内，属性 STORE 有 flush，FAU 的 `first_vertex`/`base_instance` STORE 没有** —— 上游补的就是这个不对称。我们树此前恰好复制了这个缺陷。

`cs_flush_stores()` 的语义（`src/panfrost/genxml/cs_builder.h:989-997`）：

```c
static inline void
cs_flush_stores(struct cs_builder *b)
{
   struct cs_load_store_tracker *ls_tracker = b->cur_ls_tracker;
   assert(ls_tracker != NULL);

   if (ls_tracker->pending_stores)
      cs_wait_slots(b, BITFIELD_BIT(b->conf.ls_sb_slot));
}
```

**关键安全性质**：它是 `if (pending_stores)` 门控的 LS scoreboard wait —— 在没有待决 store 时**什么都不发射**。
⇒ 本改动只可能给「已经发射的 store」补上顺序，**不可能新增任何其它副作用**，也不可能让画面变差。这使 v76 成为一个干净的单变量实验。

### 3.5 本次**未采纳**的其它部分（MR!44659 的另一个 commit）

MR!44659 实际含 **2 个 commit**（`/api/v4/projects/176/merge_requests/44659/commits`），共触及 3 个文件：

| commit | 标题 | 文件 | 采纳？ |
|---|---|---|---|
| `fbb4993c` | panvk/csf: flush the FAU stores before indirect draws read them | `csf/panvk_vX_cmd_draw.c` | ✅ **本轮唯一采纳** |
| `c9c207bf` | panfrost: add the missing v11 cases to the arch switches | `genxml/decode_common.c`、`lib/pan_mod.h` | ❌ **不碰** |

`c9c207bf` 的内容（**本轮完全未改** ✗）：

* `src/panfrost/genxml/decode_common.c` —— 在 `pandecode_cs_binary()` 与 `pandecode_cs_trace()` 的 arch switch 里各加：
  ```c
     case 11:
        pandecode_cs_binary_v11(ctx, bin_gpu_va, size);
        break;
  ```
  ```c
     case 11:
        pandecode_cs_trace_v11(ctx, trace_gpu_va, size, gpu_id);
        break;
  ```
* `src/panfrost/lib/pan_mod.h` —— 加声明与派发分支：
  ```c
  const struct pan_mod_handler *pan_mod_get_handler_v11(uint64_t modifier);
  ```
  ```c
     case 11:
        return pan_mod_get_handler_v11(modifier);
  ```

**不采纳的理由**：这是 v11 的 arch-switch 补全（解码器/修饰符派发），与 indirect draw / `flush_stores` **毫无关系**；
混进来只会破坏本轮「只有一个行为变量」的约束 ✗。它们是独立可回移的候选，留给后续单变量轮次。

---

## 4 C2 存在性核实（**只核实，未改** ✗）

任务给出的矛盾：报告 48 称 `get_tiler_desc()` 缺 `cs_wait_slots(b, dev->csf.sb.all_iters_mask)`（= 我们早前称为 **C2** 的修复，来自 MR!44173），
但我们记得已应用过 C2。

### 4.1 核实结果：**C2 在**（结论：报告 48 的这一条不成立）

落点：`src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c:1234`，**在 `get_tiler_desc()` 内**（该函数定义于第 **1215** 行）。

```
1214: static VkResult
1215: get_tiler_desc(struct panvk_cmd_buffer *cmdbuf)
1216: {
1217:    assert(cmdbuf->state.gfx.render.invalidate_inherited_ctx ||
1218:           !inherits_render_ctx(cmdbuf));
1219: 
1220:    if (cmdbuf->state.gfx.render.tiler)
1221:       return VK_SUCCESS;
1222: 
1223:    struct cs_builder *b =
1224:       panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
1225: 
1226:    {
1227:       /* The tiler heap is shared across render passes; wait for our own
1228:        * prior async tiling work to retire before reprogramming it.
1229:        * (upstream MR !44173 -- "wait for prior tiling work before reusing
1230:        * tiler heap", reported as tile-aligned corruption on Mali-G720) */
1231:    panvk_per_arch(kbase_checkpoint)(
1232:       cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER, PANVK_KBASE_CKPT_ITERS_WAIT_BEFORE);
1233:       struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
1234:       cs_wait_slots(b, dev->csf.sb.all_iters_mask);        ← ★ C2 就在这里
1235:    panvk_per_arch(kbase_checkpoint)(
1236:       cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER, PANVK_KBASE_CKPT_ITERS_WAIT_AFTER);
1237:    }
```

### 4.2 是否「只在部分路径上」？—— 是，但这是**上游的原意**，不是漏贴

C2 位于 `if (cmdbuf->state.gfx.render.tiler) return VK_SUCCESS;`（1220–1221）**之后**，
所以它只在**确实要构建 tiler descriptor** 的那条路径上执行（每个 render pass 第一次走到 `get_tiler_desc()`、即 `render.tiler == 0` 时）。

这与上游 MR!44173 的题意一致 ——「**复用 tiler heap 之前**等待自己先前的异步 tiling 工作退休」：
只有当堆要被（重新）编程时才需要等；已经建好 tiler desc 的早退路径本来就不碰堆，无需等待。
⇒ 该门控是**语义正确**的，不是半途而废的应用。

### 4.3 旁证（两条独立证据链）

1. **我们自己早前的报告**：`/root/research/29-v62-c2-tiler-wait.md` 明确记载 v62 = v61 + C2，
   落点 `src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c:1244-1252`（当轮树行号；随后的 v63–v75 各轮插行使它前移到今天的 1234），
   在 `get_tiler_desc()` 内、`panvk_get_cs_builder()` 之后，与上游 MR!44173 位置/表达式/mask 来源逐字一致。
   即 C2 是在 **v62 就已合入**并一路保留到今天的。
2. **存下来的 C2 之前的上游对照**：`/root/research/vertex-work/ref/A_get_tiler_desc.txt`（12:53 保存）显示的 `get_tiler_desc()` 形状为
   ```
      if (cmdbuf->state.gfx.render.tiler)
         return VK_SUCCESS;

      struct cs_builder *b =
         panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
      struct panvk_physical_device *phys_dev =
   ```
   —— `panvk_get_cs_builder()` 与 `struct panvk_physical_device *phys_dev =` 之间**没有任何 wait 块**（这就是「缺 C2」的样子）。
   而我们今天的树在这两行之间**有** §4.1 的整块（1226–1237）。两者对照是决定性的。

### 4.4 处置

**本轮不改** ✗（保持单变量）。报告 48 若确称其缺失，应属**版本串味或误读**（例如读了 v61 及更早的树、或读了 `vertex-work/ref/` 里的上游对照件而非我们树）。
若后续报告 48 的原文重新出现，建议以本节的 `file:line` + 两条旁证回溯它当时的依据。

---

## 5 构建、确定性对照与校验

### 5.1 构建环境（与历轮一致）

```
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:/root/zenithblue/build/host-tools/bin:$PATH
cd /root/zenithblue && ninja -j2 -C build/android-v4
```

工具链同一性核实：`/opt/android-sdk/ndk/27.3.13750724` 经 `readlink -f` **就是** `/opt/android-ndk-r27c`（同一棵树），
两路径下 `clang` 的 sha256 均为 `a871130d810536f7bb924c8aeaff57c66de27bed9b13d0ccafba25fdcc8bd02d`（r27c / clang 18.0.3）。
两条路径等价，不存在编译器差异风险。

### 5.2 确定性对照（撤改动 ⇒ 必须逐位等于 v74）

撤 v75 后（§2.1）直接重编：

```
$ ninja -j2 -C build/android-v4
NINJA_RC=0
$ sha256sum build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so
1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56
$ stat -c '%s bytes' …/libvulkan_panfrost.so
20055024 bytes
```

目标值（`/root/final/libvulkan_panfrost_v74.so`）：

```
1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56   20 055 024 B
```

⇒ **逐位相同 ✅**（sha256 与字节数双吻合）。此即「撤掉本轮改动后 == v74」的硬证据，也同时证明构建可复现。
日志：`/root/v76_control_build.log`（12 条边：`git_sha1.h` 再生 + 5 个 CSF arch 变体的 `gpu_queue.c` 重编 + 链接）。

### 5.3 v76 构建与二次重编可复现

| 轮次 | 动作 | ninja rc | `.so` sha256 | 字节 |
|---|---|---|---|---|
| build1 | 贴入 `cs_flush_stores(b);` 后 `ninja -j2` | 0 | `fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109` | 20 057 992 |
| build2 | `touch panvk_vX_cmd_draw.c` 强制 5 个 arch 变体真实重编 + 重链 | 0 | `fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109` | 20 057 992 |

build2 的日志确认是**真重编**而非 no-op：`grep -cE '^\[[0-9]+/[0-9]+\] Compiling' /root/v76_build2.log` = **5**
（`libpanvk_v10/v11/v12/v13/v14.a.p/csf_panvk_vX_cmd_draw.c.o` 各一次）。
⇒ **可复现 ✅**。日志：`/root/v76_build1.log`、`/root/v76_build2.log`。

体积对照：v74 = 20 055 024 B → v75 = 20 057 480 B（+2 456）→ v76 = 20 057 992 B（相对 v74 +2 968）。
CSF 变体 v10–v14 共 5 个实例各多出一处内联的 `cs_flush_stores()` 序列（`if (pending_stores) cs_wait_slots(LS)`），量级合理。

### 5.4 打包（由 `/root/pack_v75.sh` sed 派生）

派生命令：

```
sed -e 's/v75/v76/g' -e 's/\\"75\\"/\\"76\\"/' -e 's/6\.15-seqno-store/6.16-flushstores/' \
    /root/pack_v75.sh > /root/pack_v76.sh
```

`diff pack_v75.sh pack_v76.sh` **只有 3 行差异**（其余逐字符相同：build-tools 探测、`android.jar`、`--min-sdk-version 26 --target-sdk-version 34`、zip/zipalign/apksigner 与同一 keystore）：

```diff
6,7c6,7
< W=/root/v75
< OUT=/root/final/mgl-panvk-v75.apk
---
> W=/root/v76
> OUT=/root/final/mgl-panvk-v76.apk
12c12
< sed -e "s/versionCode=\"74\"/versionCode=\"75\"/" -e "s/6.14-defer-diag/6.15-seqno-store/" /root/v74/AndroidManifest.xml > $W/AndroidManifest.xml
---
> sed -e "s/versionCode=\"74\"/versionCode=\"76\"/" -e "s/6.14-defer-diag/6.16-flushstores/" /root/v74/AndroidManifest.xml > $W/AndroidManifest.xml
```

**env 串一致性**：APK 内只有 `AndroidManifest.xml`、`resources.arsc`、`classes.dex`、`lib/arm64-v8a/libMobileGL.so`、`lib/arm64-v8a/libvulkan_freedreno.so`、`META-INF/*`；
打包脚本未设任何环境变量（`grep -in env pack_v75.sh` 只命中 shebang `#!/usr/bin/env bash`），
MobileGL 侧两份输入与 v74/v75 完全同源：

```
classes.dex                           6bd3abde2c53506f1b54bb88a8069c3cc394c2fb08440e4ca475cbf8fd64f4ad  (= v74 = v75)
lib/arm64-v8a/libMobileGL.so          72919c73a7e07630f329bb4ad9605c606a9aac9e2fc6596683ee7b9f9c76848b  (= v74 = v75)
```

⇒ env 串与 v74/v75 **逐字符一致** ✅（唯一差异是版本标记 `6.16-flushstores`）。

打包输出即：

```
versionCode="76"
versionName="6.16-flushstores"
```

### 5.5 强制校验清单

| # | 校验 | 命令 | 结果 |
|---|---|---|---|
| 1 | APK 内驱动 sha == 新 `.so` | `unzip -p <apk> lib/arm64-v8a/libvulkan_freedreno.so \| sha256sum` | `fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109` == 源 `.so` ✅ **MATCH_OK** |
| 2 | APK 内驱动非空 | 同上 `\| wc -c` | 20 057 992 B ✅ **NONEMPTY_OK** |
| 3 | versionCode | `aapt2 dump badging <apk>` | `package: name='com.dsh.plugin.driver.g720' versionCode='76' versionName='6.16-flushstores' platformBuildVersionName='14' platformBuildVersionCode='34' compileSdkVersion='34' compileSdkVersionCodename='14'` ✅ |
| 4 | v74/v75 APK 未被改 | 打包前先存基线，打包后 `sha256sum -c` | `mgl-panvk-v74.apk: OK` / `mgl-panvk-v75.apk: OK` ✅ |
| 5 | 切分 8 份可重组 | `cd /data/dsh_downloads && cat v76p8_* \| sha256sum` | `1bb9066bbcf24d6f7a0b58fcc9a6490b2e532ec9210a04ef5c172f0356d70e20` == 原 APK ✅ |

校验 4 的基线（打包**前**记录，`/root/v76work/pre_v74_v75_hashes.txt`）：

```
74b173cea11587af7e8d0aa38cb8830db6733779233e7f48e16ed2b99f4b0217  mgl-panvk-v74.apk   (10 215 983 B, mtime 2026-10-05 22:56:15)
a108c3356418524290d84917701d31bb61f88f4c13666f48eef3c84337f5f19c  mgl-panvk-v75.apk   (10 220 079 B, mtime 2026-10-05 23:35:01)
```

mtime 亦未变动 ⇒ 确实没被触碰。

### 5.6 切分分发

```
cp -f /root/final/mgl-panvk-v76.apk /data/dsh_downloads/
cd /data/dsh_downloads && split -n 8 -d mgl-panvk-v76.apk v76p8_
```

产出 `v76p8_00` … `v76p8_07`（1 277 510 ×7 + 1 277 509，合计 10 220 079）。
**未用 `rm`** ✗（v75 的 `v75p8_*` 原样保留，未删）。

打印值（`sha256sum /root/final/mgl-panvk-v76.apk | cut -c1-24`）：

```
1bb9066bbcf24d6f7a0b58fc
```

---

## 6 回滚命令

### 6.1 回滚 v76 的源码改动（⇒ 回到 v74 树，预期 sha `1df10436…`）

```sh
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf
cp -f panvk_vX_cmd_draw.c.bak-v76-1791215327 panvk_vX_cmd_draw.c   # 去掉 cs_flush_stores(b);
cp -f panvk_vX_gpu_queue.c.bak-1791214365     panvk_vX_gpu_queue.c # 保持 v74 发射序列
cd /root/zenithblue && export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:/root/zenithblue/build/host-tools/bin:$PATH
ninja -j2 -C build/android-v4
sha256sum build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so
# 期望：1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56
```

（`panvk_vX_cmd_draw.c.bak-v76-1791215327` 与 `panvk_vX_gpu_queue.c.v74-patched` 内容等价，任取其一皆可。）

### 6.2 回滚到 v75（**不推荐**，仅备查）

```sh
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf
cp -f panvk_vX_gpu_queue.c.bak-v76-prerevert-1791215266 panvk_vX_gpu_queue.c  # 恢复 v75 完成写
cp -f panvk_vX_cmd_draw.c.bak-v76-1791215327            panvk_vX_cmd_draw.c  # 去掉 flush
# 重编 ⇒ 期望 f7a0b0b903cd58220ad1def4d3186b530b6b2d76d10d9e18fe2b0fed76c8cec4
```

### 6.3 真机回退到已发布的 v74 APK

`/root/final/mgl-panvk-v74.apk`（`74b173ce…`，versionCode=74）**仍在原位且未被改动**，可直接装回。

> 纪律：本轮全程改前 `cp <f> <f>.bak-$(date +%s)`；未执行 `git checkout/stash/reset` ✗；未执行 `rm` ✗；未操作手机 ✗。

本轮产生的备份：

| 备份 | 内容 |
|---|---|
| `panvk_vX_gpu_queue.c.bak-v76-prerevert-1791215266` | 撤回**前**的 v75 态（181 132 B） |
| `panvk_vX_cmd_draw.c.bak-v76-1791215327` | 贴入 flush **前**的 v74 态（191 653 B） |

---

## 7 ⚠️ 前置报告 48 未找到（如实标注）

任务指定独立研究报告位于 `/root/research/48-Panfrost上游对照与可借鉴修复.md`，并给出兜底 `find /root -name "48-*"`。

核实结果：

```
$ ls /root/research/*.md | tail
… /root/research/47-v74-defer-diag.md
   /root/research/49-v75-seqno-store.md
$ find /root -name "48-*"           -> (无相关结果)
$ find / -xdev -name "*48-*"        -> 仅 /etc/fonts/conf.d/48-spacing.conf、ISO_11548-1.gz 等无关系统文件
```

即报告 **47 与 49 在、48 不在**，`/root/research/` 下无 48 号文件，全盘亦无该标题文件。

**处置**：本轮改以任务提示中给定的 MR 号 `!44659` / commit `fbb4993c` 为准，并**独立经 GitLab API 验证**（§3.1：project 176、MR iid 44659 存在且标题吻合、commit `fbb4993c` 存在且 message 与任务描述一致、stats +2/-0、单文件）。
因此「回移 MR!44659 的 indirect-draw flush」这一条**不依赖报告 48 的原文**，本轮结论不受影响。

**但受影响的是 C2 那条**（§4）：报告 48 称 `get_tiler_desc()` 缺 C2 —— 我们无法核对它的原文与依据（**未验证**它具体引用了哪棵树/哪一行）。
我们只能给出我们**这棵树**的核实结果：**C2 在**（`panvk_vX_cmd_draw.c:1234`），并附两条旁证。
若需要判定报告 48 为何出错，需先找回该文件。

---

## 8 判读表：下一轮真机 A/B 该怎么读

**实验设置**：v76 = v74 树 **+ 唯一一处** `cs_flush_stores(b)`（indirect draw 前）。
对照组用 **v74**（`74b173ce…`，versionCode 74）——它是本轮确定性对照的逐位基线，是干净的对造。
**建议每组跑 ≥8 次**：上游数据是「25 例中 8 次里失败 4–7 次 → 16 次里 0 次」，说明这是一个**概率型竞态**，
单次跑通不足以判定「消失」；至少 8 次全干净才有说服力。

### 8.1 画面错误**消失**（花屏/几何错乱/大片三角板条 不再出现）

**含义**：错误就是 indirect draw 的 FAU 竞态 —— `RUN_IDVS` 读到了上一轮迭代残留的 `first_vertex`/`base_instance`。

**为什么可信**：
- 上游在 Mali-G615 上把同一族症状（`dEQP-VK.draw.*.indirect_draw.*.first_instance.*`，几何画在错误偏移）定位到同一处，并有 4–7/8 → 0/16 的量化。
- 该改动是 `if (pending_stores)` 门控的 LS wait（§3.4），**只补顺序、无其它副作用**，因此「改善」不可能来自巧合的功能改动。
- 我们树里同一循环的属性补丁路径本来就有这个 flush（`3303`），FAU 路径没有 —— 这个不对称本身就是缺陷的指纹。

**下一步**：
1. 做**竞态签名确认**：同一 APK 连跑 ≥8 次，错误率应为 0；同时用 v74 对照跑同样次数应能复现错误（若 v74 对照跑不出错误，说明触发条件未命中，见 §8.3）。
2. 确认没有引入回归：v76 vs v74 在**直接绘制**（非 indirect）路径上的行为应完全一致（该 flush 在无待决 store 时不发射任何指令）。
3. 若稳定，这一条即结案；剩余工作量转向报告 48 列出的**另外两项**「我们缺、上游有」，或转向稳定性/长跑，而不是继续改代码。

### 8.2 画面错误**减轻**（仍错，但频次下降 / 面积变小 / 只剩某类场景）

**含义**：indirect draw 的 FAU 竞态是**其中一个**成因，但**至少还有第二个独立成因**。

**推理要点**：因为 `cs_flush_stores()` 只可能消除 hazard、不可能引入新 hazard（§3.4），所以「减轻」在逻辑上等价于「本次修掉了一部分」，剩下的是另一条链。这是**信息量最大**的结果。

**下一步（按信息量排序）**：
1. **先分清场景**：剩余错误是否**只出现在 indirect draw**（`vkCmdDrawIndirect/IndexedIndirect`、`multiDrawIndirect`、非零 `firstVertex`/`firstInstance`）？
   - 只在 indirect ⇒ 说明 `launch_indirect_draw()` 里还有别的未定序项。下一个嫌疑点是**间接缓冲/绘制参数描述符**那条链：`draw_params_addr`、`cs_add_imm64(b, draw_params, …, draw->indirect.stride)`（`3308`），以及 tiler/IDVS 描述符的重编程 —— 即「读到的不是 FAU 而是 draw params」。
   - 直接绘制也错 ⇒ **indirect 路径被洗清**，搜索应转向 tiler-heap / IDVS 几何描述符 / 顶点属性补丁（注意 `3303` 那处 flush 已在，说明该路径此前被认为敏感）。
2. **判断是竞态还是确定性**：错误率是否随跑次变化？是竞态就继续找缺的顺序；每次都错则是状态/描述符算错，应从描述符内容入手而不是再加 flush。
3. **考虑是否需要比 `cs_flush_stores()` 更强的序**：若怀疑 LS wait 不足以约束到 `RUN_IDVS` 的取指（例如还需要一次 `cs_wait_slots(b, dev->csf.sb.all_iters_mask)` 或 error-barrier 级别的序），可做成下一轮的**单变量**候选 —— 但必须先有「间接读到了过期值」的**实测证据**（例如把 VS 实际读到的 `first_vertex`/`base_instance` 打出来与间接缓冲内容对照），不要凭猜想叠加。

### 8.3 画面错误**无变化**（与 v74 一模一样的错误）

**先别下结论**：这一格有**两种读法**，必须先区分，否则会错误地把上游的正确修复判成无效。

**(a) 良性读法（最可能，先排查这个）**：本用例**根本没有踩到这个 hazard**。
竞态要显形，需要「上一轮迭代写进 FAU 的值 ≠ 本轮的值」，即需要 **`multiDrawIndirect` 且 `firstVertex`/`baseInstance` 逐次变化**。
若测试 App：
- 只用普通 `vkCmdDraw` / 单次 indirect（`drawCount == 1`）；
- 或 `firstVertex`/`baseInstance` 恒为 0；

⇒ 那么缺 flush 就是**不可观测**的，v76 与 v74 必然同图。此时「无变化」**不构成对上游修复的否定**，只说明该修复与本用例无关。
**判定动作**：先确认失败场景的调用序列（是否有 indirect、`drawCount`、`firstVertex`/`firstInstance` 是否变化）。可先在一个**已知会变化**的 indirect 用例上验证 v76 是否生效，再回到原失败用例。

**(b) 非良性读法**：确实走了 indirect 且值在变，但 flush 不够 —— 说明过期值不在 FAU store 路径上，而在
绘制参数/间接缓冲描述符、tiler 描述符，或本平台固件/kbase 对 CS 序的约束比上游 Mali-G615 更宽/更紧。
**判定动作**：做**定向观测**而不是加代码 —— 打印 VS 实际读到的 `first_vertex`/`base_instance` 与间接缓冲内容对照，
看差是否恰好是「上一轮的值」。差距恰好等于上一轮 ⇒ 仍是 FAU/描述符载入竞态，只是序的强度不够；差距是别的形态 ⇒ 换线索。

**共同结论（不管是 a 还是 b）**：
- v76 是**纯顺序补齐、无功能改动**，且在 `pending_stores == 0` 时**不发射任何指令**（§3.4）。
  ⇒ **「无变化」伴随的回归风险为零**，v74（`74b173ce…`）可以继续当基线用，随时回退（§6.3）。
- 因此**不要**因为「无变化」就把 v76 丢掉：它是一个无代价的正确性修复，应**保留在树上**（作为后续轮次的基础），
  只是它**不是**本用例的答案，下一步该去别的候选上做单变量。

### 8.4 反向结果：画面错误**变差**（预期不会发生 —— 若发生，按红旗处理）

按 §3.4，`cs_flush_stores()` 在无待决 store 时不发射指令、有则只加一个 LS 等待，**理论上不可能让画面变差**。
若真出现变差，说明我们对它的语义假设在**本平台**上不成立（例如该 LS wait 与 kbase 的 scoreboard/超时记账发生意外耦合），
这本身是**高价值负结果**，应立即：
1. 用 v74 复跑确认变差可复现（排除偶发）；
2. 保留现场日志（kbase 检查点/ATOMIC/SEQNO 打印仍在，见 §2.3）；
3. **不要**继续叠加改动，先把现象定住再回滚到 v74（§6.1）。

---

## 9 本轮做了什么 / 没做什么

**做了** ✅
- 整体撤回 v75 完成写改动（`panvk_vX_gpu_queue.c` → 与 v74 canonical byte-identical），确认 v71/v72/v73 检查点与 ATOMIC/SEQNO 打印完好。
- 经 GitLab API 取回 MR!44659 / commit `fbb4993c` 的原始 diff，并**逐字**回移到 `csf/panvk_vX_cmd_draw.c`（唯一一处，+2/-0）。
- 确定性对照：撤改动重编**逐位等于** v74（`1df10436…`，20 055 024 B）。
- v76 编译 exit 0，二次重编 sha 相同（可复现）。
- 派生 `pack_v76.sh` 打包出 `mgl-panvk-v76.apk`，5 项强制校验全过。
- 切分 8 份并同源分发到 `/data/dsh_downloads/`，重组校验通过。
- 核实 C2：**在**（`panvk_vX_cmd_draw.c:1234`），附两条旁证。

**没做什么** ✗（按令）
- 未改 v71/v72/v73 检查点、ATOMIC/SEQNO 打印、修复 A、P2/renew32、超时常量、MobileGL、AFBC。
- 未采纳 MR!44659 的另一 commit `c9c207bf`（v11 arch switches：`decode_common.c`、`pan_mod.h`）。
- 未改 C2（保持单变量）。
- 未执行 `git checkout/stash/reset`；未执行 `rm`；未操作手机。
- **未做真机验证** —— v76 的实际画面效果**未验证**，§8 为前瞻性判读表。

**未验证项**（如实标注）
- 报告 48 的原文与其「C2 缺失」依据 —— 文件不在磁盘上，**未验证**。
- v76 在真机上的画面效果 —— **未验证**（本轮禁止操作手机）。
- `c9c207bf` 对我们树 v11 路径的影响 —— 本轮未采纳，**未验证**。
