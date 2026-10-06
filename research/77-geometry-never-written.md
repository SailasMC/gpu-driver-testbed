# 77 — 假设成立且更强：kbase 上 **geometry 区从不被写**，而 descriptor 仍按 64 KiB 告诉固件去读它

日期：2026-10-06 · 只读分析 · 配套：`76b-tiler-geometry-chain.md`（预言被你的 v118 证实 ✓）
**你的 v118 单变量结果 = 决定性**：`geometry 64 KiB→32 KiB`（只改分配、不改打包）⇒ **故障精确移动 0x8000** ⇒ **固件确实按打包寄存器里的"尺寸字段"去读 geometry** ✓✓✓

---

## 1. 你第 1 点：`tiler_heap.desc + 4096`（geometry）在 kbase 上**到底有没有被写** —— **没有**

**全树 grep 的结果（三条命令的并集）**：往 `tiler_heap.desc + 4096` 写的地方**只有一处**，且**在 kbase 上被 `#ifdef` 分支排除**：

```c
/* csf/panvk_vX_gpu_queue.c:2942-2952 —— 逐行核实 */
   uint32_t init_stream_size = cs_root_chunk_size(&b);
#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(dev)) {
      panvk_priv_mem_flush(subq->kbase.init_cs, 0, init_stream_size);   /* ← kbase 走这支 */
      kbase_cache_clean_range(root_cs.cpu, init_stream_size);
   } else
#endif
   {
      panvk_priv_mem_flush(queue->tiler_heap.desc, 4096, init_stream_size);  /* ← 只有非 kbase 才填 geometry */
   }
```
**其它与 geometry 相关的点（全部已核）**：
| 位置 | 作用 |
|---|---|
| `:2884-2885` | geometry 的 cpu/gpu 视图（`desc + 4096`）—— **只是视图，不写内容** |
| `:2950` | **唯一**写 geometry 的语句，**kbase 分支排除** ✗ |
| `:2870-2877` | `subq->kbase.init_cs` 的分配与视图（**init stream 在 kbase 上写进这里**） |
| `:2679` | `init_cs` 的释放 |
| `:2956-2968` | init stream 作为**普通 queue submit** 提交（`cs_root_chunk_size/gpu_addr`）⇒ **kbase 用"提交执行"这条替代路径**，与 panthor 的"嵌进 geometry"不同 |
**⇒ 结论（你第 1 点）**：**kbase 上 geometry 区确实从无任何人写入** ✓✓ 而它**仍是已映射内存**（属于那个 `0x11000` BO）⇒ 固件读它**不会**翻译故障、但**会读到未初始化内容**（garbage 指针）⇒ **固件顺着 garbage 走 ⇒ 越过 BO ⇒ TRANSLATION_FAULT** ✓ 与"READ + CSF LSU"吻合 ✓

---

## 2. 你第 2 点：固件期望那里是什么 + `geom_buf` 的消费者

- **非 kbase 分支**把 **`cs_root_chunk_size(&b)` 字节的 CS init stream** 写进 geometry ⇒ **语义 = "geometry 里放一段固件要用的 CS 流"**（该 BO 的注释也说 *"so we can pass it through a single 64-bit register to the VERTEX_TILER command streams"*）；
- **`geom_buf` 的消费者（已 grep 到唯一一处）**：`csf/panvk_vX_cmd_draw.c:1356` —— *"Load the tiler_heap and geom_buf from the context."* ⇒ 它被装进 **tiler/render descriptor** 交给固件 ✓（`panvk_cmd_buffer.h:279` 是该字段声明）
- ⇒ **⇒ 在 kbase 上，我们一边"把 init stream 改道去 `init_cs` 并当普通提交执行"，一边"仍在 descriptor 里把 geometry 以 64 KiB 的尺寸指给固件"** ⇒ **两者不一致**：**要么 kbase 的固件不需要 geometry（那我们不该把它指过去），要么它需要（那我们漏填了）** ✓✓ **这正是根因所在的分岔**。

---

## 3. 你第 3 点：下一步单变量实验（**两处一起改**，并加一个"消失"实验）

### E1（确认尺寸耦合，128 KiB）
```diff
@@ csf/panvk_vX_gpu_queue.c (init_tiler, :3196-3198)
-   struct panvk_pool_alloc_info alloc_info = { .size = (64 * 1024) + 4096, .alignment = 4096 };
+   struct panvk_pool_alloc_info alloc_info = { .size = (128 * 1024) + 4096, .alignment = 4096 };
@@ csf/panvk_vX_gpu_queue.c (cs_ctx 初始化, :2836-2838)
-   cs_ctx->render.geom_buf = (cs_ctx->render.tiler_heap + 4096) | ((64 * 1024) >> 12);
+   cs_ctx->render.geom_buf = (cs_ctx->render.tiler_heap + 4096) | ((128 * 1024) >> 12);
```
（`oom_fbd`（`:3218`）用同一 `alloc_info` ⇒ **一起改**；`>>12` 的页数必须同步 ⇒ 否则重演 v112 自伤。）
**判据**：故障再移动（应再移 0x10000）⇒ **尺寸耦合二次确认** ✓

### E2（★"消失"实验，根因定位）—— **kbase 分支也把 init stream 填进 geometry**
```diff
@@ csf/panvk_vX_gpu_queue.c:2942-2952
    uint32_t init_stream_size = cs_root_chunk_size(&b);
+   /* v120: the firmware is still handed geom_buf (descriptor build loads it in
+    * panvk_vX_cmd_draw.c:1356) and is told the geometry is 64 KiB, but on kbase
+    * the init stream is diverted to subq->kbase.init_cs and the geometry region
+    * is never written => the firmware reads uninitialised content (and can walk
+    * off the BO).  Fill it on kbase too: strictly additive (the region is
+    * already allocated inside tiler_heap.desc). */
+   assert(init_stream_size + 4096 <= panvk_priv_mem_size(queue->tiler_heap.desc));
+   panvk_priv_mem_flush(queue->tiler_heap.desc, 4096, init_stream_size);
 #ifdef HAVE_PAN_KMOD_KBASE
    if (gpu_queue_uses_kbase(dev)) {
       panvk_priv_mem_flush(subq->kbase.init_cs, 0, init_stream_size);   /* 保留原路径 */
       kbase_cache_clean_range(root_cs.cpu, init_stream_size);
    } else
 #endif
    {
       panvk_priv_mem_flush(queue->tiler_heap.desc, 4096, init_stream_size);
    }
```
（即：把 geometry 的填充**提到分支之外**——kbase 上两条路径都填；语义上**只增加初始化、不改变任何交付方式** ⇒ 风险极低。）
**判据**：
- **故障消失** ⇒ **根因确认：kbase 漏填 geometry** ✓✓✓
- **故障仍在但地址变化** ⇒ 固件读的是别处（但 geometry 未初始化这条**仍应修**）
- **无变化** ⇒ geometry 内容不是触发条件 ⇒ 回到"固件读了 BO 之外"的其它通道（此时 E1 的尺寸耦合仍是唯一线索）

### E3（若想更保守、甚至不用重编推理）
**在分配后立刻把 geometry 区清零**（`panvk_priv_mem_write`/`memset` 4096..end）⇒ 与 E2 的区别是"清零 vs 填合法流"：
- **清零后故障消失** ⇒ 固件只是读了它（不需要合法流）；
- **清零后仍崩** ⇒ 固件**需要合法的 CS 流** ⇒ 必须 E2。

---

## 4. 修复候选（patch 方向）

| 候选 | 内容 | 风险 |
|---|---|---|
| **F1（首选）** | **geometry 的填充移出 `#ifdef`**（E2 的 diff）—— kbase 也填 init stream；若确认 kbase 固件根本不使用 geometry，则反向修：**kbase 上把 `geom_buf` 置 0 或指向 `init_cs`**（二选一，取决于 E2 结果） | 低（只影响一个 BO 的初始化） |
| **F2（必备护栏）** | `assert(init_stream_size + 4096 <= panvk_priv_mem_size(tiler_heap->desc));`（release 改为限流日志 + 截断） | **0** |
| **F3（同源化，你已认领）** | `#define PANVK_TILER_GEOM_SIZE (64*1024)`：分配 `+4096` 与打包 `>>12` **同源** + `static_assert`（页数 < 4096） | 0 |
| **F4** | `oom_fbd` 与 `desc` 尺寸同步（同一 `alloc_info` 的事实要在代码里显式绑定） | 0 |
| **F5（一致性收尾）** | 若 kbase 路线保留"提交 init stream"，则把 geometry 语义写进注释（为什么填/为什么不填），避免下一轮再被当成"漏写" | 0 |

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1/§2 的代码**逐行核实**（`:2832-2838`、`:2870-2885`、`:2942-2952`、`:2956-2968`、`panvk_vX_cmd_draw.c:1356`、`panvk_cmd_buffer.h:279`）。
2. **"固件在 kbase 上确实会去读 geometry"尚未直接证明**——已知的是：(a) 我们把 `geom_buf` 装进 descriptor（`cmd_draw.c:1356`）；(b) 固件按打包的尺寸字段读它（**你的 v118 已用"故障随尺寸移动"证明** ✓）。**E2 就是"它读了什么/需不需要合法内容"的判别**。
3. **一个必须正视的残留矛盾**：v118 的故障移动与"读 geometry 越界"**完全自洽**；但 v115 那次故障地址 `0x5fffe1e000` 落在**一个 64 KiB slab** 的末尾之后，而当时那个 `0x11000` 的 geometry BO 在 `0x5ffffa5000`（**不相邻**）⇒ **那两次可能不是同一个对象**（或那份 BO 表与故障来自不同运行——我们已确认过日志混行风险）⇒ **请在同一局、同 PID 打印里同时给出"geometry BO 的 VA 区间"与"故障地址"**，这一点一确认，全部就闭环了。
4. `init_stream_size` 的**实际上限**仍未读到（`grep -rn "init_stream_size"` 只命中本文件的使用点）；F2 的断言无论上限如何都该加。
5. 所有 diff **未编译、未上机**（按纪律只出提案）。
