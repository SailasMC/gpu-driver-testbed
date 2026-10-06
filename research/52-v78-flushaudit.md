# 52 — v78：系统性【刷新/可见性】覆盖审计（我们树 vs 上游现行源码）

任务：实施型子智能体 / task-v78
构建树 `/root/zenithblue/work/mesa` · 构建目录 `/root/zenithblue/build/android-v4`
日期：2026-10-06

---

## 0 结论速览

| 项 | 结果 |
|---|---|
| 上游基准 ref | `main` @ `1887da10a6e716862fa144d364a6e31705b33573`（2026-10-05T14:41:26Z，经 GitLab API project 176 取回） |
| 比较范围 | **整棵 `src/panfrost/vulkan`：98 个 `.c/.h`，与上游 98 个文件名逐一相同** |
| `cs_flush_stores(` 双方计数 | 我们 **31** / 上游 **28** |
| **`cs_flush_stores` 缺口（上游有、我们没有）** | **0 处** ✅ |
| `cs_wait_slot(SB_ID(LS))` 双方计数 | 我们 **13** / 上游 **7**；**缺口 0 处** ✅ |
| `cs_wait_slots(` 双方计数 | 我们 **6** / 上游 **4**；**缺口 0 处** ✅ |
| 「上游有、我们没有」的**全部**排序原语行（`cs_store*` / `cs_flush_stores` / `cs_wait_slot*`） | 98 文件中 **96 个文件为 0**；另 2 个文件各缺 **1 行 `cs_store32(`** —— 那是**我们自己主动删掉的** TLS-size store（**不是** flush、**不是** wait）。⇒ **缺 flush = 0，缺 wait = 0** ✅ |
| 我们多的（上游没有的） | 我们多 **22 行**排序原语，**全部**是本地 `kbase_*` 诊断脚手架 |
| **本轮补齐的缺口** | **无（0 处）** —— 审计未发现任何有上游出处的缺口，按任务令「只补有明确上游出处的 ✗ 不自己发明」⇒ **本轮源码零改动** |
| 直接绘制路径（第 4 条重点）的「写了没 flush 就被用」窗口 | **不存在**（见 §6：直接绘制走 IDVS **状态寄存器** + `cs_update_vt_ctx()`，不走 FAU **内存 store**，flush 在该路径上不是所需的排序原语）。上游同样如此 ⇒ 按令 **只报告不动手** ✗ |
| **撤掉本轮改动后是否逐位等于 v77 `cf65d1e1`** | **是** ✅（本轮改动为空集；强制真重编 15 个对象 + 重链后逐位相同，见 §8） |
| 编译 exit 0？ | **是**（buildB / buildC 两次 `NINJA_EXIT=0`） |
| 二次重编可复现？ | **是**（两次强制重编 sha 相同） |
| 新 `.so` sha256 | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185`（**20 053 456 B**） |
| v78 APK sha256 | `289a0e425d21c0c78bdf5836809991005865fe28c683814470759ef27ab5312b`（**10 215 983 B**） |
| 24 字符前缀 | `289a0e425d21c0c78bdf5836` |
| 切分分发 | `/data/dsh_downloads/v78p8_00..07` 已就绪，重组 sha == 原 APK ✅ |
| ⚠️ **关键提醒** | **v78 的驱动与 v77 逐位相同** ⇒ 装 v78 对「画面错误」而言**预期像素级等价于 v77**，它**不是**一个新的实验变量，而是审计结论 + 构建可复现性背书（见 §11 判读表） |

---

## 1 上游基准的取法（可复现）

```sh
# ref 与 commit
curl -s "https://gitlab.freedesktop.org/api/v4/projects/176/repository/commits?ref_name=main&per_page=1"
#   -> 1887da10a6e716862fa144d364a6e31705b33573  (2026-10-05T14:41:26Z)

# 整棵目录（一次取全，避免逐文件遗漏）
curl -s -o vulkan-up.tar.gz \
  "https://gitlab.freedesktop.org/api/v4/projects/176/repository/archive.tar.gz?sha=main&path=src/panfrost/vulkan"
#   98 个 .c/.h，311804 B
```

任务点名的 4 份文件另有单独 `raw` 取回，并**校验了两种取法与 tar 包内容一致**
（`csf/panvk_vX_cmd_draw.c`：raw 与 tar 内 sha256 均为 `cda2d966e5327d82…`，均 189 361 B）。

> ⚠️ 陷阱（本轮实际踩到并已排除）：`src/panfrost/vulkan/panvk_vX_cmd_draw.c`（**非 CSF**，42 741 B）
> 与 `src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c`（**CSF**，189 361 B）是**两个不同文件**。
> 首次提取函数清单时误用了前者，导致行号/函数全错；本报告全部结论均基于 **`csf/` 路径**。

---

## 2 `cs_flush_stores(` 双方完整清单对照

读取方式：对 98 个文件逐一扫描，并用「最近的上层函数定义」归属每个出现点。

| # | ours file:line | function | upstream file:line | function | status |
|---|---|---|---|---|---|
| 1 | `csf/panvk_cmd_buffer.h:908` | `cs_iter_sb_update_start` | `csf/panvk_cmd_buffer.h:787` | `cs_iter_sb_update_start` | ✅ 一致 |
| 2 | `csf/panvk_vX_cmd_buffer.c:80` | `panvk_per_arch(kbase_mark_progress)` | `—` | `panvk_per_arch(kbase_mark_progress)` | 🔵 我们多（本地 kbase 脚手架） |
| 3 | `csf/panvk_vX_cmd_buffer.c:123` | `panvk_per_arch(kbase_checkpoint)` | `—` | `panvk_per_arch(kbase_checkpoint)` | 🔵 我们多（本地 kbase 脚手架） |
| 4 | `csf/panvk_vX_cmd_buffer.c:215` | `finish_cs` | `csf/panvk_vX_cmd_buffer.c:125` | `finish_cs` | ✅ 一致 |
| 5 | `csf/panvk_vX_cmd_buffer.c:330` | `finish_queries` | `csf/panvk_vX_cmd_buffer.c:236` | `finish_queries` | ✅ 一致 |
| 6 | `csf/panvk_vX_cmd_buffer.c:682` | `panvk_per_arch(kbase_record_wait_mask)` | `—` | `panvk_per_arch(kbase_record_wait_mask)` | 🔵 我们多（本地 kbase 脚手架） |
| 7 | `csf/panvk_vX_cmd_buffer.c:1250` | `panvk_per_arch(CmdExecuteCommands)` | `csf/panvk_vX_cmd_buffer.c:1080` | `panvk_per_arch(CmdExecuteCommands)` | ✅ 一致 |
| 8 | `csf/panvk_vX_cmd_buffer.c:1438` | `panvk_per_arch(cmd_invalidate_crc)` | `csf/panvk_vX_cmd_buffer.c:1268` | `panvk_per_arch(cmd_invalidate_crc)` | ✅ 一致 |
| 9 | `csf/panvk_vX_cmd_dispatch.c:301` | `panvk_per_arch(cmd_dispatch_shader)` | `csf/panvk_vX_cmd_dispatch.c:288` | `panvk_per_arch(cmd_dispatch_shader)` | ✅ 一致 |
| 10 | `csf/panvk_vX_cmd_dispatch.c:435` | `cmd_dispatch` | `csf/panvk_vX_cmd_dispatch.c:402` | `cmd_dispatch` | ✅ 一致 |
| 11 | `csf/panvk_vX_cmd_draw.c:144` | `generate_fn_set_fbds_provoking_vertex` | `csf/panvk_vX_cmd_draw.c:145` | `generate_fn_set_fbds_provoking_vertex` | ✅ 一致 |
| 12 | `csf/panvk_vX_cmd_draw.c:1155` | `cs_render_desc_ringbuf_move_ptr` | `csf/panvk_vX_cmd_draw.c:1156` | `cs_render_desc_ringbuf_move_ptr` | ✅ 一致 |
| 13 | `csf/panvk_vX_cmd_draw.c:1438` | `get_tiler_desc` | `csf/panvk_vX_cmd_draw.c:1421` | `get_tiler_desc` | ✅ 一致 |
| 14 | `csf/panvk_vX_cmd_draw.c:1642` | `mark_crc_valid_after_fragment` | `csf/panvk_vX_cmd_draw.c:1625` | `mark_crc_valid_after_fragment` | ✅ 一致 |
| 15 | `csf/panvk_vX_cmd_draw.c:2016` | `get_fb_descs` | `csf/panvk_vX_cmd_draw.c:1995` | `get_fb_descs` | ✅ 一致 |
| 16 | `csf/panvk_vX_cmd_draw.c:2029` | `get_fb_descs` | `csf/panvk_vX_cmd_draw.c:2008` | `get_fb_descs` | ✅ 一致 |
| 17 | `csf/panvk_vX_cmd_draw.c:2071` | `get_fb_descs` | `csf/panvk_vX_cmd_draw.c:2050` | `get_fb_descs` | ✅ 一致 |
| 18 | `csf/panvk_vX_cmd_draw.c:3066` | `update_prims_generated_query` | `csf/panvk_vX_cmd_draw.c:3045` | `update_prims_generated_query` | ✅ 一致 |
| 19 | `csf/panvk_vX_cmd_draw.c:3303` | `patch_vs_attribs` | `csf/panvk_vX_cmd_draw.c:3278` | `patch_vs_attribs` | ✅ 一致 |
| 20 | `csf/panvk_vX_cmd_draw.c:3401` | `launch_indirect_draw` | `csf/panvk_vX_cmd_draw.c:3376` | `launch_indirect_draw` | ✅ 一致 |
| 21 | `csf/panvk_vX_cmd_draw.c:4416` | `setup_tiler_oom_ctx` | `csf/panvk_vX_cmd_draw.c:4394` | `setup_tiler_oom_ctx` | ✅ 一致 |
| 22 | `csf/panvk_vX_cmd_draw.c:4838` | `handle_deferred_queries` | `csf/panvk_vX_cmd_draw.c:4794` | `handle_deferred_queries` | ✅ 一致 |
| 23 | `csf/panvk_vX_cmd_precomp.c:73` | `panvk_per_arch(dispatch_precomp)` | `csf/panvk_vX_cmd_precomp.c:75` | `panvk_per_arch(dispatch_precomp)` | ✅ 一致 |
| 24 | `csf/panvk_vX_cmd_precomp.c:140` | `panvk_per_arch(dispatch_precomp)` | `csf/panvk_vX_cmd_precomp.c:142` | `panvk_per_arch(dispatch_precomp)` | ✅ 一致 |
| 25 | `csf/panvk_vX_cmd_query.c:142` | `panvk_cmd_reset_queries` | `csf/panvk_vX_cmd_query.c:142` | `panvk_cmd_reset_queries` | ✅ 一致 |
| 26 | `csf/panvk_vX_cmd_query.c:190` | `panvk_cmd_reset_queries` | `csf/panvk_vX_cmd_query.c:190` | `panvk_cmd_reset_queries` | ✅ 一致 |
| 27 | `csf/panvk_vX_cmd_query.c:294` | `panvk_cmd_begin_occlusion_query` | `csf/panvk_vX_cmd_query.c:294` | `panvk_cmd_begin_occlusion_query` | ✅ 一致 |
| 28 | `csf/panvk_vX_cmd_query.c:378` | `panvk_cmd_reset_timestamp_queries` | `csf/panvk_vX_cmd_query.c:378` | `panvk_cmd_reset_timestamp_queries` | ✅ 一致 |
| 29 | `csf/panvk_vX_cmd_query.c:389` | `panvk_cmd_reset_timestamp_queries` | `csf/panvk_vX_cmd_query.c:389` | `panvk_cmd_reset_timestamp_queries` | ✅ 一致 |
| 30 | `csf/panvk_vX_cmd_query.c:686` | `panvk_cmd_begin_prims_generated_query` | `csf/panvk_vX_cmd_query.c:686` | `panvk_cmd_begin_prims_generated_query` | ✅ 一致 |
| 31 | `csf/panvk_vX_exception_handler.c:168` | `invalidate_crc_addrs_from_oom_ctx` | `csf/panvk_vX_exception_handler.c:168` | `invalidate_crc_addrs_from_oom_ctx` | ✅ 一致 |

**逐项判读**

| 项 | 数 |
|---|---|
| 双方**都在**（✅） | 23 个 (file,function) 组合，共 28 处 |
| **上游有、我们没有（🔴 缺口）** | **0** |
| 我们多（🔵） | 3 处，全部在 `csf/panvk_vX_cmd_buffer.c`：`80`（`kbase_mark_progress`）、`123`（`kbase_checkpoint`）、`682`（`kbase_record_wait_mask`）—— **本地诊断脚手架，上游根本没有这三个函数**（见 §5.3 的 grep 证据） |

**这 28 处一致项包括任务最关心的那一处**：`launch_indirect_draw` 的 FAU flush（v76 回移的 MR!44659）。
我们 `csf/panvk_vX_cmd_draw.c:3401` ↔ 上游 `csf/panvk_vX_cmd_draw.c:3376`，
上下文 **8 行逐字节相同**（我们 3395–3402 vs 上游 3370–3377，`diff` 空）：

```
3395:          if (shader_uses_sysval(vs, graphics, vs.base_instance)) {
3396:             cs_store32(b, cs_sr_reg32(b, IDVS, INSTANCE_OFFSET), vs_fau_addr,
3397:                        shader_remapped_sysval_offset(
3398:                           vs, sysval_offset(graphics, vs.base_instance)));
3399:          }
3400: 
3401:          cs_flush_stores(b);        <- 我们（= 上游 3376，逐字一致）
3402:       }
```

---

## 3 `cs_wait_slot(SB_ID(LS))` 与 `cs_wait_slots(` 双方完整清单对照

### 3.1 `cs_wait_slot(… SB_ID(LS) …)`

| # | ours file:line | function | upstream file:line | function | status |
|---|---|---|---|---|---|
| 1 | `csf/panvk_vX_cmd_draw.c:1921` | `get_fb_descs` | `csf/panvk_vX_cmd_draw.c:1902` | `get_fb_descs` | ✅ 一致 |
| 2 | `csf/panvk_vX_cmd_query.c:362` | `panvk_cmd_reset_timestamp_queries` | `csf/panvk_vX_cmd_query.c:362` | `panvk_cmd_reset_timestamp_queries` | ✅ 一致 |
| 3 | `csf/panvk_vX_cmd_query.c:543` | `panvk_cs_write_timestamp` | `csf/panvk_vX_cmd_query.c:543` | `panvk_cs_write_timestamp` | ✅ 一致 |
| 4 | `csf/panvk_vX_cmd_query.c:611` | `panvk_copy_timestamp_query_results` | `csf/panvk_vX_cmd_query.c:611` | `panvk_copy_timestamp_query_results` | ✅ 一致 |
| 5 | `csf/panvk_vX_exception_handler.c:265` | `generate_tiler_oom_handler` | `csf/panvk_vX_exception_handler.c:265` | `generate_tiler_oom_handler` | ✅ 一致 |
| 6 | `csf/panvk_vX_exception_handler.c:346` | `generate_tiler_oom_handler` | `csf/panvk_vX_exception_handler.c:346` | `generate_tiler_oom_handler` | ✅ 一致 |
| 7 | `csf/panvk_vX_gpu_queue.c:1297` | `kbase_wrapper_checkpoint` | `—` | `kbase_wrapper_checkpoint` | 🔵 我们多（本地 kbase 脚手架） |
| 8 | `csf/panvk_vX_gpu_queue.c:1465` | `kbase_subqueue_emit_job` | `—` | `kbase_subqueue_emit_job` | 🔵 我们多（本地 kbase 脚手架） |
| 9 | `csf/panvk_vX_gpu_queue.c:1474` | `kbase_subqueue_emit_job` | `—` | `kbase_subqueue_emit_job` | 🔵 我们多（本地 kbase 脚手架） |
| 10 | `csf/panvk_vX_gpu_queue.c:1506` | `kbase_subqueue_emit_job` | `—` | `kbase_subqueue_emit_job` | 🔵 我们多（本地 kbase 脚手架） |
| 11 | `csf/panvk_vX_gpu_queue.c:1549` | `kbase_subqueue_emit_job` | `—` | `kbase_subqueue_emit_job` | 🔵 我们多（本地 kbase 脚手架） |
| 12 | `csf/panvk_vX_gpu_queue.c:1553` | `kbase_subqueue_emit_job` | `—` | `kbase_subqueue_emit_job` | 🔵 我们多（本地 kbase 脚手架） |
| 13 | `csf/panvk_vX_utrace.c:71` | `cmd_copy_data` | `csf/panvk_vX_utrace.c:71` | `cmd_copy_data` | ✅ 一致 |

**判读：缺口 0。** 我们多的 6 处全部在 `csf/panvk_vX_gpu_queue.c` 的
`kbase_subqueue_emit_job`（1465/1474/1506/1549/1553）与 `kbase_wrapper_checkpoint`（1297）——
同样是本地 kbase 脚手架（上游无此二函数，§5.3）。

### 3.2 `cs_wait_slots(`

| # | ours file:line | function | upstream file:line | function | status |
|---|---|---|---|---|---|
| 1 | `csf/panvk_vX_cmd_buffer.c:194` | `finish_cs` | `csf/panvk_vX_cmd_buffer.c:108` | `finish_cs` | ✅ 一致 |
| 2 | `csf/panvk_vX_cmd_buffer.c:745` | `emit_barrier_csf` | `csf/panvk_vX_cmd_buffer.c:606` | `emit_barrier_csf` | ✅ 一致 |
| 3 | `csf/panvk_vX_cmd_draw.c:1234` | `get_tiler_desc` | `—` | `get_tiler_desc` | 🔵 我们多（本地 kbase 脚手架） |
| 4 | `csf/panvk_vX_cmd_draw.c:1631` | `mark_crc_valid_after_fragment` | `csf/panvk_vX_cmd_draw.c:1614` | `mark_crc_valid_after_fragment` | ✅ 一致 |
| 5 | `csf/panvk_vX_exception_handler.c:288` | `generate_tiler_oom_handler` | `csf/panvk_vX_exception_handler.c:288` | `generate_tiler_oom_handler` | ✅ 一致 |
| 6 | `csf/panvk_vX_gpu_queue.c:1541` | `kbase_subqueue_emit_job` | `—` | `kbase_subqueue_emit_job` | 🔵 我们多（本地 kbase 脚手架） |

**判读：缺口 0。** 我们多的 2 处：
- `csf/panvk_vX_cmd_draw.c:1234`（`get_tiler_desc`）—— 这是 **C2**，来自上游 **MR!44173**
  《panvk/csf: wait for prior tiling work before reusing tiler heap》。**该 MR 至今仍为 `opened`
  （未合入 main）**，所以它在「上游现行源码」里当然看不到 —— 这**不是缺口**，而是我们**领先**上游的一处已回移修复。
  （本轮经 API 独立确认：`!44173` state=`opened`，`+8/-0`，单文件 `csf/panvk_vX_cmd_draw.c`。）
- `csf/panvk_vX_gpu_queue.c:1541`（`kbase_subqueue_emit_job`）—— 本地脚手架。

---

## 4 「缺口 = 0」的三条独立证明

单一方法可能因函数改名/重构而漏判，故本轮用**三条互相独立**的证据链交叉验证。

### 4.1 证明一：按 (文件, 函数) 归属后逐点配对（§2、§3 的表）

结果：`cs_flush_stores` 缺口 0、`cs_wait_slot(LS)` 缺口 0、`cs_wait_slots` 缺口 0。

### 4.2 证明二：**与函数名无关**的「排序原语行」逐文件比对

对每个文件抽出所有匹配
`cs_(store32|store64|store_to|store32_to|store64_to|flush_stores|wait_slot|wait_slots)(`
的**整行文本**，排序后 `comm -23`（上游有、我们没有）：

```
文件                                             上游行数  缺  多
csf/panvk_vX_cmd_buffer.c                            13    0   6
csf/panvk_vX_cmd_dispatch.c                           8    1   0   <<<< 缺 1 行
      缺的那行是： cs_store32(
csf/panvk_vX_cmd_draw.c                              35    0   1
csf/panvk_vX_cmd_precomp.c                            7    1   0   <<<< 缺 1 行
      缺的那行是： cs_store32(
csf/panvk_vX_gpu_queue.c                          0    0  15
-----
文件总数 98 ；零缺失的文件 96 ；上游排序原语行合计 100
上游有、我们没有：2 行 —— 两行都是 cs_store32(，不是 flush、不是 wait
```

那 2 行是**我们主动删除**的 TLS-size 32 位 store（见 §5.2 的 diff），
删除后 `cs_flush_stores(b)` **仍然保留在两侧**（diff 中 `cs_flush_stores` 一行没动）。
⇒ 这是「我们少了一个 store」，**不是**「我们少了一个 flush」。**缺口仍为 0。**

### 4.3 证明三：按函数的「排序原语**序列**」比对

把每个函数体内的 `STORE / FLUSH / WAITLS / WAITSLOTS` 压成有序 token 串，两侧比对：

```
函数内含排序原语且两树皆有：  序列完全相同 30 个 ；不同 3 个
  ### DIFF csf/panvk_vX_cmd_dispatch.c :: cmd_dispatch
      ours = STORE-FLUSH                  up = STORE-STORE-FLUSH
  ### DIFF csf/panvk_vX_cmd_draw.c :: get_tiler_desc
      ours = WAITSLOTS-FLUSH              up = FLUSH
  ### DIFF csf/panvk_vX_cmd_precomp.c :: panvk_per_arch(dispatch_precomp)
      ours = STORE-FLUSH-STORE-STORE-STORE-FLUSH
      up   = STORE-STORE-FLUSH-STORE-STORE-STORE-FLUSH
```

**3 处差异全部是「我们比上游多了排序」或「我们比上游少了一个 store，而 flush 仍在」**，
没有一处是「上游有 flush/wait 而我们没有」。⇒ **缺口 0。**

---

## 5 我们对上游的**全部**差异（21+6+8 hunk 全量归类）

既然审计结论是「缺口 0」，就有义务说明**我们的差异到底在哪**。对 3 个任务点名文件做完整 `diff -u`，
把全部 35 个 hunk 归类如下（`.diff` 已存 `/root/v78work/diff_ours_vs_up_*.diff`）：

| 类别 | 处数 | 说明 |
|---|---|---|
| A. 本地 kbase 诊断脚手架 | 约 28 | `kbase_checkpoint` / `kbase_mark_progress` / `kbase_record_wait_mask` 的调用点与定义；**上游无这些函数** |
| B. Android/MobileGL 适配 | 4 | 去掉 `#include "vk_android.h"`；去掉两处 `vk_android_*_efr_*` EFR 补丁；`panthor_kmod_get_csif_props(dev->kmod.dev)` → `panvk_get_csif_props(dev)` |
| C. v77 的空指针守卫 | 1 | `if (!vs) return;` —— **我们加、上游没有**（报告 51） |
| D. 我们自己的语义修复 | 2 | `uint64_t fn_addr` → `uint32_t fn_addr`（`get_fb_descs`）；`cmd_dispatch`/`dispatch_precomp` 删掉 TLS-size 的 32 位 load/store |
| E. 上游 API 漂移（我们没跟） | 1 | `CmdEndRendering2KHR(…, VkRenderingEndInfoKHR*)`（上游） vs `CmdEndRendering(VkCommandBuffer)`（我们） |
| **F. 刷新/可见性缺口** | **0** | — |

### 5.1 与本地 kbase 脚手架的关系（**重要**）

`kbase_checkpoint()` / `kbase_mark_progress()` 内部**各自发射一次 `cs_flush_stores(b)`**
（`csf/panvk_vX_cmd_buffer.c:80` 与 `:123`）。全树共 **42 个 `kbase_checkpoint` 调用点**
（`cmd_draw.c` 22、`cmd_buffer.c` 11、`cmd_dispatch.c` 9）。

但它们**都受同一个门控**：`if (!PANVK_DEBUG(KBASE_DIAG)) return;`，
而 `kbase_diag` 是 `PANVK_DEBUG` 环境变量解析出的调试位（`panvk_instance.c:62`），**默认关闭**。

⇒ **正常运行（未设 `PANVK_DEBUG=kbase_diag`）时，这 42 处一处都不发射**，不构成对「刷新覆盖审计」的干扰，
也不构成画面差异。

> ⚠️ **反向提醒（对本轮判读直接相关）**：若真机复现画面错误时**开了** `PANVK_DEBUG=kbase_diag`，
> 则每个 draw 会被注入大量额外的 `cs_flush_stores` —— 那会把我们要找的「缺 flush」hazard **掩盖掉**。
> 若 v76/v77 的对照实验是在 `kbase_diag` 打开的状态下做的，则该实验**不能**用来判定 flush 假设。
> **建议**：画面错误的 A/B 一律在 **不设 `PANVK_DEBUG`（或明确不含 `kbase_diag`）** 下进行。

### 5.2 我们主动删除的 TLS-size store（= §4.2 里那 2 行）

`csf/panvk_vX_cmd_dispatch.c` 与 `csf/panvk_vX_cmd_precomp.c` 同一处改动：

```diff
-   /* Copy the global TLS pointer and size to the per-job TSD. */
+   /* Copy the global TLS pointer to the per-job TSD. */
    if (cs->info.tls_size) {
       cs_move64_to(b, cs_scratch_reg64(b, 0), cmdbuf->state.tls.desc.gpu);
-      cs_load32_to(b, cs_scratch_reg32(b, 4), cs_scratch_reg64(b, 0), 0);
       cs_load64_to(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 0), 8);
       cs_move64_to(b, cs_scratch_reg64(b, 0), tsd);
-      cs_store32(b, cs_scratch_reg32(b, 4), cs_scratch_reg64(b, 0), 0);
       cs_store64(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 0), 8);
       cs_flush_stores(b);        <- 两侧都在，未动
    }
```

这是**我们既有的有意改动**（任务书点名的「P2/renew32」一类），**本轮未碰** ✗。
它减少一个 store，**不减少任何 flush**。

### 5.3 「上游没有这些函数」的 grep 证据

```
$ grep -n "kbase_subqueue_emit_job\|kbase_wrapper_checkpoint\|kbase_mark_progress\|kbase_checkpoint\|kbase_record_wait_mask" \
      <upstream>/csf/panvk_vX_gpu_queue.c <upstream>/csf/panvk_vX_cmd_buffer.c
(无输出)
```

⇒ 上面所有标 🔵 的「我们多」项，都落在**上游根本不存在**的函数里，属于本地脚手架，不影响对账。

---

## 6 第 4 条重点：直接绘制（非 indirect）路径的 `first_vertex` / `base_instance` / `draw_count`

### 6.1 结论

**直接绘制路径不存在「写了 FAU 内存但没 flush 就被 GPU 读」的窗口。**
原因不是「忘了 flush」，而是**该路径根本不通过 FAU 内存传递这些参数**。

### 6.2 证据：两条路径的参数传递机制不同（我们树，逐行）

**直接绘制 `launch_draw()`（我们 `csf/panvk_vX_cmd_draw.c:3106`）—— 走 IDVS **状态寄存器**：**

```c
3117:    cs_update_vt_ctx(b) {
3118:       cs_move32_to(b, cs_sr_reg32(b, IDVS, GLOBAL_ATTRIBUTE_OFFSET), 0);
3119:       cs_move32_to(b, cs_sr_reg32(b, IDVS, INDEX_COUNT), draw->vertex.count);
3120:       cs_move32_to(b, cs_sr_reg32(b, IDVS, INSTANCE_COUNT),
3121:                    draw->instance.count);
3122:       cs_move32_to(b, cs_sr_reg32(b, IDVS, INDEX_OFFSET), draw->index.offset);
3123:       cs_move32_to(b, cs_sr_reg32(b, IDVS, VERTEX_OFFSET), draw->vertex.base);   <- first_vertex
3129:       cs_move32_to(b, cs_sr_reg32(b, IDVS, INSTANCE_OFFSET), 0);                 <- base_instance 恒 0
3130:    }
```

`cs_move32_to(cs_sr_reg32(...))` 写的是**状态寄存器**（`cs_sr_reg32`），不是内存；
`cs_update_vt_ctx()` 正是把状态寄存器写入**成组提交**给 IDVS 的包装。
⇒ 该路径的排序原语是 `cs_update_vt_ctx()`，**`cs_flush_stores()` 在这里不是所需的机制**
（它只管 LS scoreboard 上的内存 store）。上游写法与我们**逐字相同**（本区域 `diff` 无 hunk）。

**间接绘制 `launch_indirect_draw()`（我们 `:3313`）—— 才需要 FAU 内存 store：**

```c
3390:          cs_store32(b, cs_sr_reg32(b, IDVS, VERTEX_OFFSET), vs_fau_addr, …);   <- 内存 store
3396:          cs_store32(b, cs_sr_reg32(b, IDVS, INSTANCE_OFFSET), vs_fau_addr, …); <- 内存 store
3401:          cs_flush_stores(b);      <- MR!44659 补的就是这一处（v76 已回移，两侧一致）
```

⇒ 这解释了为什么「同型窗口」只存在于 indirect 路径：**只有 indirect 路径把
`first_vertex`/`base_instance` 放进 FAU 内存**（因为值来自间接缓冲，运行时才知道），
直接绘制的值是编译期已知的立即数，直接写状态寄存器即可。

### 6.3 `draw_count`

两条路径里 `draw_count` 都只作为 **CS 暂存寄存器**的立即数 / 循环计数器使用，**不落内存**：

| 落点 | 代码 | 性质 |
|---|---|---|
| 我们 `:3018`（`update_prims_generated_query`） | `cs_move32_to(b, max_draw_count, draw->indirect.draw_count);` | 立即数 → 暂存寄存器 |
| 我们 `:3244`（`patch_vs_attribs`） | `cs_move32_to(b, max_draw_count, draw->indirect.draw_count);` | 同上 |
| 我们 `:3363`（`launch_indirect_draw`） | `cs_move32_to(b, max_draw_count, draw->indirect.draw_count);` | 同上 |
| 我们 `:3365` | `cs_load32_to(b, draw_count, draw_params_addr, 0);` | **读**间接缓冲（LOAD，非 store） |

⇒ 无 store→read hazard，无需 flush。

### 6.4 处置

按任务令第 4 条：「若发现且上游有对应处理 ⇒ 一并按上游补；**若上游也没有 ⇒ 只报告不动手** ✗」。
本轮结论是**连窗口都不存在**（比「上游也没有」更强），故 **不动手** ✗。

---

## 7 未处理的项（及原因）

| # | 项 | 为什么不处理 |
|---|---|---|
| 1 | **`cs_flush_stores` / `cs_wait_slot(LS)` / `cs_wait_slots` 缺口** | **审计结果为 0 处**，无对象可补 |
| 2 | 直接绘制 `first_vertex`/`base_instance`/`draw_count` 的 flush 窗口 | **窗口不存在**（§6）；且上游写法逐字相同 ⇒ 按令只报告 |
| 3 | 本地 kbase 脚手架（42 个 checkpoint 调用点及其 flush） | 任务明令**不改** v71/v72/v73 检查点、ATOMIC/SEQNO 打印 ✗ |
| 4 | v77 的 `if (!vs) return;` | 任务明令**不改** ✗ |
| 5 | 报告 51 §3 列出的 10 处空指针风险点（#5/#6/#8/#9 等） | 任务明令**另一轮处理** ✗ |
| 6 | `cmd_dispatch` / `dispatch_precomp` 的 TLS-size store 删除 | 任务明令**不改** P2/renew32 一类 ✗ |
| 7 | 上游 API 漂移（`CmdEndRendering2KHR` / EFR / `vk_android.h`） | 与刷新/可见性**无关**；且属 Android 适配层，改动面大 ✗ |
| 8 | 上游 **open** MR（未合入 main） | 「上游现行源码」= main。已逐一筛查 open panvk MR（§7.1），**无一是 panvk 刷新/可见性修复** |

### 7.1 上游 open MR 筛查结果（为避免漏掉「未合入但已有的上游处理」）

对 project 176 的 open MR 做了 `panvk` 与 `panfrost` 两轮检索（各 40 / 60 条），逐一判读：

| MR | 标题 | 判读 |
|---|---|---|
| **!44173** | panvk/csf: wait for prior tiling work before reusing tiler heap | = 我们的 **C2**，**已在树内**（`cmd_draw.c:1234`）；state=`opened` ⇒ 上游 main 看不到，**不是缺口** |
| !44816 | panvk: only align tiling area to spill store on IR pass | 修「tiling area 被不必要扩张」（pandecode 里 Scissor Max Y 607 应为 599）。**是画面类线索，但不是 flush/可见性修复**；`+12/-2`（csf）+ `+0/-1` ⇒ **本轮不采纳**，列为下一轮单变量候选 |
| !41468 | panfrost: improve bounds checking for CSF indirect accesses | 改的是 **gallium**（`pan_csf.c` / `pan_helpers.c`），**不是 panvk**，与我们驱动无关 |
| !39852 | panvk: remove post run sync_adds | 方向相反（**删** sync_add），7 文件大改，非可见性补齐；风险高 ⇒ 不采纳 |

⇒ **不存在「上游已有处理而我们没跟」的刷新/可见性改动。**

---

## 8 构建与确定性对照

### 8.1 构建环境（与历轮一致）

```sh
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue && ninja -j2 -C build/android-v4
```

工具链核实：`clang` = `/opt/android-ndk-r27c/.../clang`，`Android (12470979, +pgo, +bolt, +lto, +mlgo, based on r522817e) clang version 18.0.3`（NDK r27c）。

### 8.2 为什么必须做三次构建（本轮实际踩到的坑）

| 轮次 | 动作 | ninja 结果 | 编译对象数 | `.so` sha256 | 判定 |
|---|---|---|---|---|---|
| buildA | 直接 `ninja` | `NINJA_EXIT=0` | **0 Compiling**（只跑了 `git_sha1.h` 生成） | `cf65d1e1…` | ⚠️ **无效**：ninja 认为全部最新，**没有真正重编**，拿到的只是 v77 留下的旧产物，**不能**用作可复现性证据 |
| **buildB** | `touch` 3 个 csf 源文件后 `ninja -j2` | `NINJA_EXIT=0` | **15 Compiling + 1 Linking target** | `cf65d1e1…` | ✅ **有效**：真实重编 5 个 arch 变体 × 3 文件 + 重链 |
| **buildC** | 再次 `touch` 后 `ninja -j2` | `NINJA_EXIT=0` | **15 Compiling + 1 Linking target** | `cf65d1e1…` | ✅ **有效**：**二次重编可复现** |

> 记录此坑的原因：若只跑 buildA，会得到「与 v77 相同」的**假象**，而实际上根本没有编译任何东西。
> 本报告的确定性结论只以 **buildB / buildC** 两次**强制真重编**为准。日志：`/root/v78work/v78_buildA.log`、`v78_buildB.log`、`v78_buildC.log`。

### 8.3 确定性对照结论

| 项 | 值 |
|---|---|
| 目标（v77 驱动） | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185` / **20 053 456 B** |
| 本轮改动集 | **空集**（审计未发现任何有上游出处的缺口 ⇒ 未改任何源文件） |
| 「**撤掉本轮改动**」重编（buildB） | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185` / 20 053 456 B |
| **是否逐位等于 `cf65d1e1`** | **是，逐位相同** ✅ |
| 「**再贴回**」重编（buildC） | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185` / 20 053 456 B |
| 二次重编可复现 | **是** ✅ |

因为本轮改动为空集，「撤掉本轮改动」与「再贴回」在物理上是**同一棵树**；
两次强制真重编得到同一 sha，同时证明了 **(a) 与 v77 逐位一致**、**(b) 构建可复现**。

**源码状态核对**（改前 / 改后，必须同为 v77 态）：

```
$ sha256sum src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c     # 构建前
ae9884fab0932f3685fc2636897199f7cf6918d962acd27798dd443dec0775e1
$ sha256sum src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c     # 构建后（本轮未改内容）
ae9884fab0932f3685fc2636897199f7cf6918d962acd27798dd443dec0775e1
```

与报告 51 记载的 v77 改动后 sha **一致** ⇒ 树确实停在 v77 态，未被我本轮意外改动
（本轮只 `touch` 过 mtime，内容 `cp -p` 备份前后一致）。

---

## 9 交付物与强制校验

### 9.1 产物

| 文件 | sha256 | 字节 |
|---|---|---|
| `/root/final/libvulkan_panfrost_v78.so` | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185` | 20 053 456 |
| `/root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so` | 同上 | 20 053 456 |
| `/root/final/mgl-panvk-v78.apk` | `289a0e425d21c0c78bdf5836809991005865fe28c683814470759ef27ab5312b` | 10 215 983 |
| `/data/dsh_downloads/mgl-panvk-v78.apk` | 同 APK，逐字节一致 | 10 215 983 |
| 打包脚本 `/root/pack_v78.sh` | 由 `pack_v77.sh` 派生 | 23 行 |

### 9.2 打包脚本派生（与 v77 的差异**只有 3 行**）

```sh
sed -e 's/v77/v78/g' -e 's/\\"77\\"/\\"78\\"/' -e 's/6\.17-nullvs/6.18-flushaudit/' \
    /root/pack_v77.sh > /root/pack_v78.sh
```

```diff
6,7c6,7
< W=/root/v77
< OUT=/root/final/mgl-panvk-v77.apk
---
> W=/root/v78
> OUT=/root/final/mgl-panvk-v78.apk
12c12
< sed -e "s/versionCode=\"74\"/versionCode=\"77\"/" -e "s/6.14-defer-diag/6.17-nullvs/" /root/v74/AndroidManifest.xml > $W/AndroidManifest.xml
---
> sed -e "s/versionCode=\"74\"/versionCode=\"78\"/" -e "s/6.14-defer-diag/6.18-flushaudit/" /root/v74/AndroidManifest.xml > $W/AndroidManifest.xml
```

**env 串一致性**：剥掉版本相关行后，两脚本**逐字符相同**（build-tools 探测、`android.jar`、
`--min-sdk-version 26 --target-sdk-version 34`、zip/zipalign/apksigner 参数与同一 keystore）。
打包脚本本身不设任何环境变量。

### 9.3 强制校验清单（全部通过）

| # | 校验 | 结果 |
|---|---|---|
| 1 | `unzip -p <apk> lib/arm64-v8a/libvulkan_freedreno.so \| sha256sum` == 新 `.so` | `cf65d1e1…` == `cf65d1e1…` ✅ **MATCH_OK** |
| 2 | 同上非空 | **20 053 456 B** ✅ **NONEMPTY_OK** |
| 3 | `aapt2 dump badging` | `package: name='com.dsh.plugin.driver.g720' versionCode='78' versionName='6.18-flushaudit' …` ✅ |
| 4 | v75/v76/v77 APK **未被改动** | `mgl-panvk-v75.apk: OK` / `mgl-panvk-v76.apk: OK` / `mgl-panvk-v77.apk: OK` ✅（`sha256sum -c`，基线于打包**前**记录） |
| 5 | v78 APK 内容 vs v77（逐条目 sha） | 仅 `AndroidManifest.xml` 与 3 个签名文件不同；**`libvulkan_freedreno.so` / `libMobileGL.so` / `classes.dex` / `resources.arsc` 四者 sha 完全相同** ✅ |

校验 4 的基线（`/root/v78work/pre_v75_v76_v77.txt`）：

```
a108c3356418524290d84917701d31bb61f88f4c13666f48eef3c84337f5f19c  mgl-panvk-v75.apk   (10 220 079 B, 23:35:01)
1bb9066bbcf24d6f7a0b58fcc9a6490b2e532ec9210a04ef5c172f0356d70e20  mgl-panvk-v76.apk   (10 220 079 B, 23:50:06)
49506ddc28fd543d41a763c673ecebcb604f1e62a676849a80c16d4746d67b5d  mgl-panvk-v77.apk   (10 215 983 B, 23:59:51)
```

### 9.4 切分分发

```sh
cp -f /root/final/mgl-panvk-v78.apk /data/dsh_downloads/
cd /data/dsh_downloads && split -n 8 -d mgl-panvk-v78.apk v78p8_
```

| part | 字节 |
|---|---|
| `v78p8_00` … `v78p8_06` | 1 276 998 各 |
| `v78p8_07` | 1 276 997 |
| **合计** | **10 215 983** = APK 大小 ✅ |

`cat v78p8_* | sha256sum` = `289a0e425d21c0c78bdf5836809991005865fe28c683814470759ef27ab5312b` == 原 APK ✅
`/data/dsh_downloads/mgl-panvk-v78.apk` sha256 同为 `289a0e42…` ✅
**24 字符前缀：`289a0e425d21c0c78bdf5836`**

> 本轮**未使用 `rm`** ✗；v77 的 `v77p8_*` 原样保留（`/data/dsh_downloads` 现 73 个条目）。

---

## 10 回滚命令

**本轮源码零改动 ⇒ 无需回滚源码。** 若要把树显式复位到 v77 态（幂等，仅作保险）：

```sh
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf
cp -f panvk_vX_cmd_draw.c.bak-v78-1791216583    panvk_vX_cmd_draw.c
cp -f panvk_vX_cmd_buffer.c.bak-v78-1791216583  panvk_vX_cmd_buffer.c
cp -f panvk_vX_cmd_dispatch.c.bak-v78-1791216583 panvk_vX_cmd_dispatch.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue && ninja -j2 -C build/android-v4
sha256sum build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so
# 期望（且必然）：cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185  / 20 053 456 B
```

（三个 `.bak-v78-1791216583` 与现文件**逐字节相同**，因为它们是在未改内容的情况下按纪律备份的。）

**真机回退到 v77 APK**（不重编，最常用）：
`/root/final/mgl-panvk-v77.apk`（`49506ddc…`，versionCode 77，`6.17-nullvs`）仍在原位且未被改动。
更早的 v76（`1bb9066b…`）/ v75（`a108c335…`）/ v74（`74b173ce…`）同样可用。

本轮产生的备份：

| 备份 | 内容 |
|---|---|
| `panvk_vX_cmd_draw.c.bak-v78-1791216583` | 191 710 B（= 现文件） |
| `panvk_vX_cmd_buffer.c.bak-v78-1791216583` | 54 926 B（= 现文件） |
| `panvk_vX_cmd_dispatch.c.bak-v78-1791216583` | 18 964 B（= 现文件） |

---

## 11 判读表：v78 该看什么

> **前提**：v78 的驱动与 v77 **逐位相同**，唯一差别是 APK 的 `versionCode`/`versionName` 标记。
> 因此下面的判读表不是「新实验」的判读，而是「**对照实验**」：
> 理论上 v78 必须与 v77 表现**完全一致**。任何不一致，都是关于**我们自己模型**的重要反证。

| 观察 | 说明什么 | 下一步 |
|---|---|---|
| **楔形/拉伸/尖刺 与 v77 完全一样**（预期） | ✅ 与「驱动逐位相同」一致；**确认 v78 确实是零改动重切**，也确认逐位可复现的 `.so` 在真机上行为等同 ⇒ 本轮审计的**结论可信**：画面错误**不是**「上游 FLUSH 缺口」造成的。刷新/可见性这条线在**上游现行源码范围内已经走完**。 | 把「缺 flush」假设**结案**，转向非-flush 成因：优先 §7.1 的 **MR!44816**（tiling area 被不必要扩张，`Scissor Max Y 607 vs 599` —— 这正是**几何被画到 fb 之外/被拉伸**那类症状的指纹），做成下一轮**单变量**回移。 |
| **楔形【消失】或【显著减少】** | 🔴 **反证**：同一份机器码不可能因为重打包而改变渲染。出现此结果只有三种可能：**(a)** 两次运行的复现条件本来就不同（App 路径/时长/温度/负载）；**(b)** 有环境变量差异（**尤其检查 `PANVK_DEBUG` 是否含 `kbase_diag`**，见 §5.1 的反向提醒）；**(c)** 复现本身是概率型的，单次结果无统计意义。 | 不要当成修复。**先做 ≥8 次重复 + 固定环境变量**，并核对 `/root/final` 两份 APK 内 `libvulkan_freedreno.so` 的 sha 是否真的一致。 |
| **楔形【变差】** | 🔴 同上，逻辑上不可能源于本次重切。 | 记录现场（kbase 检查点/ATOMIC/SEQNO 打印仍在），回退 v77 APK（§10），不要叠加改动。 |
| **FPS / 崩溃行为与 v77 一致**（预期） | ✅ 与逐位相同一致。注意 v77 已修 `vs==NULL`，`hs_err` 计数应**不再增长**（报告 51 的结论）。 | 继续按报告 51 §3 处理剩余 10 处空指针风险点（**另一轮**）。 |

**一句话**：v78 的价值是**否证**——它把「上游还漏了 flush」这个假设**从证据上关掉**，
并给出一个逐位可复现的基线；它**不会**修好楔形。要找楔形，方向应转向
**非-flush 的几何/描述符路径**（首选 MR!44816 的 tiling-area 扩张）。

---

## 12 本轮做了什么 / 没做什么

**做了** ✅
- 取上游**现行** `main`（`1887da10…`）整棵 `src/panfrost/vulkan`（98 文件），与我们的 98 文件一一对齐。
- 完成三类排序原语的**全量对账**：`cs_flush_stores`（31 vs 28）、`cs_wait_slot(SB_ID(LS))`（13 vs 7）、`cs_wait_slots`（6 vs 4）。
- 用**三条独立方法**证明「上游有、我们没有」的刷新/可见性缺口 **= 0**：
  按 (文件,函数) 配对、与函数名无关的排序原语行比对、按函数的排序原语**序列**比对。
- 对 3 个点名文件做完整 `diff -u`（21/8/6 hunk），把我们对上游的**全部**差异归类（§5），确认无一是可见性缺口。
- 完成第 4 条重点：证明**直接绘制路径不存在**「写了 FAU 内存没 flush 就被用」的窗口（机制上走状态寄存器 + `cs_update_vt_ctx()`）。
- 首次发现并量化了 **kbase 诊断脚手架自带 flush** 这一**实验混杂因素**（42 个 checkpoint 调用点，受 `PANVK_DEBUG=kbase_diag` 门控，默认关闭），并给出对历轮 A/B 有效性的提醒。
- 三次构建；其中两次为**强制真重编**（15 对象 + 重链），两次 sha 相同且**逐位等于 v77 `cf65d1e1…`**。
- 派生 `pack_v78.sh` 打包、5 项强制校验全过、切分 8 份并同源分发。
- 筛查上游 open panvk/panfrost MR，确认**不存在**「上游已有处理而我们没跟」的刷新/可见性改动；并把 **MR!44816** 作为下一轮单变量候选记录下来。

**没做什么** ✗（按令）
- **未改动任何源文件**（审计结论：无缺口可补；第 4 条：窗口不存在 ⇒ 只报告不动手）。
- 未改：v71/v72/v73 检查点、ATOMIC/SEQNO 打印、v76 的 flush、v77 的 `if (!vs) return;`、
  修复 A、C1/C2/B、P2/renew32、超时常量、MobileGL、AFBC、报告 51 列出的 10 处空指针风险点。
- 未采纳任何 open MR（!44816 / !41468 / !39852）。
- 未执行 `git checkout/stash/reset`；未执行 `rm`；未操作手机。

---

## 13 未验证项（如实标注）

| 项 | 状态 |
|---|---|
| v78 在真机上的画面表现 | **未验证**（本子智能体不操作手机；且驱动与 v77 逐位相同 ⇒ 预期等价，见 §11） |
| v77 在真机上的画面/崩溃表现 | **未验证**（报告 51 亦未验证；本轮只做静态审计与构建） |
| 历轮 v76/v77 的 A/B 是否在 `PANVK_DEBUG=kbase_diag` 下进行 | **未验证** —— 这一点**决定**了那些实验能否用来判定 flush 假设（§5.1）。**建议优先核实。** |
| MR!44816 对我们树的确切效果 | **未验证**（本轮未采纳、未回移、未真机） |
| 上游 `main` 之外的分支/后续 commit 是否含相关修复 | **未验证**（本轮基准固定为 `main` @ `1887da10`） |
| 我们树 `cmd_dispatch`/`dispatch_precomp` 删除 TLS-size store 的**动机与后果** | **未验证**（属既有改动，任务明令不改，本轮只记录其存在与位置） |

---

## 14 纪律与可复现清单

| 纪律 | 执行 |
|---|---|
| 改前 `cp <f> <f>.bak-$(date +%s)` | ✅ 3 个点名文件各备份为 `.bak-v78-1791216583`（内容与现文件相同，因本轮未改内容） |
| 禁 `git checkout/stash/reset` | ✅ 未执行 |
| 禁 `rm` | ✅ 未执行（切分不覆盖旧件，v77 的 `v77p8_*` 保留） |
| 禁操作手机 | ✅ 未操作 |
| 不确定处标「未验证」 | ✅ 见 §13 |

**本轮产生的可复现材料**（均在 `/root/v78work/`）：

| 文件 | 内容 |
|---|---|
| `up/tree/mesa-main-1887da10…-src-panfrost-vulkan/` | 上游现行整棵 `src/panfrost/vulkan`（98 文件） |
| `up/vulkan-up.tar.gz` | 上述 tar 包原件（可校验来源） |
| `gapproof.sh` / `gapproof.txt` | 与函数名无关的缺口证明（§4.2） |
| `seqcmp2.py` | 按函数的排序原语序列比对（§4.3） |
| `cmp.py` / `funcmap2.py` / `mktable.py` | 函数归属 + 清单/表格生成器 |
| `inv_flush.txt` / `inv_waitls.txt` / `inv_waitslots.txt` | 三份原始清单输出 |
| `table_flush.md` / `table_waitls.md` / `table_waitslots.md` | §2/§3 的完整对照表 |
| `diff_ours_vs_up_panvk_vX_cmd_{draw,buffer,dispatch}.c.diff` | 3 个点名文件的完整 diff |
| `v78_buildA/B/C.log` | 三次构建日志（含 buildA 的「0 编译」无效记录） |
| `pre_v75_v76_v77.txt` | 打包前的 v75/v76/v77 APK sha 基线 |

---

## 15 给下一轮的一句话

**「缺 flush」这一假设在本轮被证据关闭**：在与上游现行 `main` 的逐点对账中，
`cs_flush_stores` / `cs_wait_slot(LS)` / `cs_wait_slots` 的缺口都是 **0**，
直接绘制路径在**机制上**不存在同型窗口，上游 open MR 里也没有对应的可见性修复。
楔形/拉伸/尖刺因此更可能来自**非-flush 的几何/描述符/tiling 路径** ——
下一轮建议的单变量首选是 **MR!44816《panvk: only align tiling area to spill store on IR pass》**
（症状指纹：tiling area 被不必要扩张，pandecode 里 `Scissor Maximum Y 607` 而 fb 只有 600 高）。
