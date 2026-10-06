# 57 — F1「续租失败不得清零计数」最小补丁提案 + F3 窗口前提的**否证**

日期：2026-10-06 · 树 `/root/zenithblue/work/mesa`（未提交）· 只读分析，**未改源码、未编译、未碰设备**
缩写：`F` = `src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c` · `D` = `src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c` · `Q` = `src/panfrost/vulkan/csf/panvk_queue.h`

---

## 0. 一句话

- **F1**：不是一个 bug 而是三个叠在一起——**返回值语义混淆**（`VK_SUCCESS` 同时表示"换堆了"和"跳过了"）、**日志误导**（`mesa_logi(... renewal ...)` 打在真正续租**之前**）、**无条件清零**（`:3936-3937`）。最小修法 = **回传 `bool renewed` + 只在 `renewed` 时清零 + 跳过时按 `interval/4` 退避**（退避不是可选项：不清零又不退避 ⇒ **每个提交都做一次全图形排水**）。
- **F3**：**否证**。我此前的窗口前提（"排水只等 CS seqno，而 tiler 是异步的 ⇒ 在飞 tiler 撞上换堆"）**站不住**：每个 ring entry 在写 seqno **之前**执行 `cs_wait_slots(all_mask)`（等**全部** scoreboard 槽），而 `cs_vt_end`（`VERTEX_TILER_COMPLETED`）正是以 `cs_defer_indirect()` 发出的 async op、其 signal 槽 ∈ `all_mask` ⇒ **seqno 前进 ⟹ VT_END 已退休 ⟹ tiler 对该堆的写入已结束**。次生通道（FINISH_FRAGMENT 回传跨代际空闲块）同样被 FRAG 侧的排水覆盖 ⇒ 一并撤回。**保留的只是"重负载"这个观察与 env A/B 的判决价值**（§3.5）。

---

## 1. 边界

| 做的事 | 没做的事 |
|---|---|
| 读 `F`/`D`/`Q` 与上游 `main` 对照，定位 F1 三处、F3 证据链 | **没有改任何源码**；本文件里的代码块都是**提案草案**，未落盘 |
| 设计最小补丁与退避参数、分析代价与副作用 | **没有编译、没有打包、没有操作设备** |
| 用 Lead 给的实测（v82 局 `begin=9832/end=9828`；v81 局 1703/1703）校正 F1 结论 | 未跑 pandecode、未取新 logcat |
| 只写本文件 | 未碰 `/root/mesa`、`/root/MobileGL`；未 `rm`；未 `git checkout/stash/reset` |

---

## 2. 【F1】「续租失败不得清零计数」最小补丁提案

### 2.1 现状（事实 + 行号）

```c
/* F:3274-3288 —— 守卫：两个图形子队列都必须"自上次退休以来发射过新作业" */
static bool kbase_try_destroy_retired_heap(struct panvk_gpu_queue *queue) {
   if (!queue->kbase_retired_heap.ctx) return true;
   if (queue->subqueues[VT].kbase.emitted_jobs   <= queue->kbase_retired_heap.vt_jobs ||
       queue->subqueues[FRAG].kbase.emitted_jobs <= queue->kbase_retired_heap.frag_jobs)
      return false;                                   /* ← 不能销毁 */
   kbase_kmod_csf_tiler_heap_destroy(dev->kmod.dev, queue->kbase_retired_heap.ctx);
   queue->kbase_retired_heap.ctx = 0;
   return true;
}

/* F:3292-3341 —— 续租 */
static VkResult kbase_renew_tiler_heap(struct panvk_gpu_queue *queue) {
   const uint32_t renew_gen = ++queue->kbase_heap_renew_count;
   mesa_loge("kbase: CKPT host heap renew #%u begin (old ctx 0x%"PRIx64")", …);   /* ① begin 无条件打印 */
   if (!kbase_try_destroy_retired_heap(queue))
      return VK_SUCCESS;                              /* ② 跳过：语义与"换堆成功"无法区分，且不打 end */
   … create new heap / rewrite desc / retire old ctx …
   mesa_loge("kbase: CKPT host heap renew #%u end (new ctx 0x%"PRIx64")", …);     /* 只有成功才有 end */
   return VK_SUCCESS;
}

/* F:3906-3938 —— 调用点 */
if (count >= interval || (submit->tiler_work_estimate && renew_work && work >= renew_work)) {
   result = kbase_wait_graphics_targets(queue, submit->kbase_target_seqnos, UINT64_MAX);  /* ③ 全图形排水 */
   if (result != VK_SUCCESS) return result;
   mesa_logi("kbase: tiler heap renewal (uAPI %u.%u, submits %u, renew interval %u)", …); /* ④ 打在续租之前 */
   result = kbase_renew_tiler_heap(queue);
   if (result != VK_SUCCESS) return vk_queue_set_lost(&queue->vk, "…renewal failed");
   queue->kbase_tiler_submit_count = 0;               /* ⑤ 无条件清零（即使②跳过） */
   queue->kbase_tiler_work_count   = 0;
}
```

字段类型（`Q:139-143,151-154`）：`uint32_t kbase_tiler_submit_count` · `uint64_t kbase_tiler_work_count` · `uint32_t kbase_heap_renew_count` · `kbase_retired_heap{uint64_t ctx, vt_jobs, frag_jobs}`。

**实测校正（Lead 提供）**：v82 那局 `renew begin=9832 / renew end=9828` ⇒ **4 次被②静默跳过**；v81 那局 1703/1703 配平。⇒ F1 不是理论隐患，**已经发生**（4/9832 ≈ 0.04%，属"偶发"量级，符合"只在某侧子队列恰好没推进时命中"）。

### 2.2 最小补丁草案（**提案，未落盘**）

**改动 1：让"跳过"可被调用点识别（签名加出参）**

```c
static VkResult
kbase_renew_tiler_heap(struct panvk_gpu_queue *queue, bool *renewed)
{
   …
   *renewed = false;
   mesa_loge("kbase: CKPT host heap renew #%u begin (old ctx 0x%" PRIx64 ")",
             renew_gen, (uint64_t)tiler_heap->context.dev_addr);

   if (!kbase_try_destroy_retired_heap(queue)) {
      /* 退休 ctx 仍不可销毁：不改堆、不改描述符。
       * 这里刻意不打 end —— begin/end 不配平就是"被推迟"的唯一指纹。 */
      mesa_loge("kbase: CKPT host heap renew #%u postponed "
                "(retired ctx 0x%" PRIx64 ", vt %" PRIu64 "/%" PRIu64
                ", frag %" PRIu64 "/%" PRIu64 ")",
                renew_gen, queue->kbase_retired_heap.ctx,
                queue->subqueues[PANVK_SUBQUEUE_VERTEX_TILER].kbase.emitted_jobs,
                queue->kbase_retired_heap.vt_jobs,
                queue->subqueues[PANVK_SUBQUEUE_FRAGMENT].kbase.emitted_jobs,
                queue->kbase_retired_heap.frag_jobs);
      return VK_SUCCESS;
   }
   … create / 写 desc / 记退休快照 …
   *renewed = true;
   mesa_loge("kbase: CKPT host heap renew #%u end (new ctx 0x%" PRIx64 ")",
             renew_gen, (uint64_t)tiler_heap->context.dev_addr);
   return VK_SUCCESS;
}
```

**改动 2：只在真正换堆时清零，并在跳过时退避**

```c
      result = kbase_wait_graphics_targets(queue, submit->kbase_target_seqnos, UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;

      bool renewed = false;
      result = kbase_renew_tiler_heap(queue, &renewed);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(&queue->vk, "kbase: tiler heap renewal failed");

      const uint32_t interval = kbase_tiler_heap_renew_interval();
      if (renewed) {
         queue->kbase_tiler_submit_count = 0;
         queue->kbase_tiler_work_count = 0;
      } else {
         /* 退避：跳过时若把计数留在阈值上，会变成"每个提交都排水重试"；
          * 若清零则回到 F1 原状。折中：只退 backoff 次提交再试。
          * interval=32 ⇒ backoff=8（≈每 8 次图形提交一次尝试，代价可控）。 */
         const uint32_t backoff = MAX2(1u, interval / 4u);
         queue->kbase_tiler_submit_count = interval - MIN2(backoff, interval);
      }

      /* 日志挪到结论之后，并区分两种结果（④ 的误导一并修掉） */
      mesa_logi("kbase: tiler heap renewal (uAPI %u.%u, submits %u, "
                "renew interval %u) -> %s",
                dev->kmod.dev->driver.version.major,
                dev->kmod.dev->driver.version.minor,
                queue->kbase_tiler_submit_count, interval,
                renewed ? "renewed" : "postponed");
```

**改动 3（可选、纯诊断）**：把 `begin/end` 之外再加一个 `uint32_t kbase_tiler_renew_defer` 计数（跳过时 `++`，成功时清 0），日志打印它 —— 用于回答"跳过是否在累积"。不加也不影响正确性。

**改动 2b（同一处的统一 diff 形态，便于 Lead 直接改）**

```diff
@@ panvk_vX_gpu_queue.c :3910-3938 (调用点) @@
       result = kbase_wait_graphics_targets(
          queue, submit->kbase_target_seqnos, UINT64_MAX);
       if (result != VK_SUCCESS)
          return result;
 
+      bool renewed = false;
-      /* P2 diagnostic: ... */
-      mesa_logi("kbase: tiler heap renewal (uAPI %u.%u, submits %u, "
-                "renew interval %u)", ...);
-      result = kbase_renew_tiler_heap(queue);
+      result = kbase_renew_tiler_heap(queue, &renewed);
       if (result != VK_SUCCESS)
          return vk_queue_set_lost(&queue->vk,
                                   "kbase: tiler heap renewal failed");
 
-      queue->kbase_tiler_submit_count = 0;
-      queue->kbase_tiler_work_count = 0;
+      const uint32_t interval = kbase_tiler_heap_renew_interval();
+      if (renewed) {
+         queue->kbase_tiler_submit_count = 0;
+         queue->kbase_tiler_work_count = 0;
+      } else {
+         const uint32_t backoff = MAX2(1u, interval / 4u);
+         queue->kbase_tiler_submit_count = interval - MIN2(backoff, interval);
+      }
+
+      mesa_logi("kbase: tiler heap renewal (uAPI %u.%u, submits %u, "
+                "renew interval %u) -> %s",
+                dev->kmod.dev->driver.version.major,
+                dev->kmod.dev->driver.version.minor,
+                queue->kbase_tiler_submit_count, interval,
+                renewed ? "renewed" : "postponed");
```

```diff
@@ panvk_vX_gpu_queue.c :3292-3307 (函数头与跳过分支) @@
-static VkResult
-kbase_renew_tiler_heap(struct panvk_gpu_queue *queue)
+static VkResult
+kbase_renew_tiler_heap(struct panvk_gpu_queue *queue, bool *renewed)
 {
    ...
+   *renewed = false;
    mesa_loge("kbase: CKPT host heap renew #%u begin (old ctx 0x%" PRIx64 ")",
              renew_gen, (uint64_t)tiler_heap->context.dev_addr);
-   if (!kbase_try_destroy_retired_heap(queue))
+   if (!kbase_try_destroy_retired_heap(queue)) {
+      mesa_loge("kbase: CKPT host heap renew #%u postponed "
+                "(retired ctx 0x%" PRIx64 ", vt %" PRIu64 "/%" PRIu64
+                ", frag %" PRIu64 "/%" PRIu64 ")",
+                renew_gen, queue->kbase_retired_heap.ctx,
+                queue->subqueues[PANVK_SUBQUEUE_VERTEX_TILER].kbase.emitted_jobs,
+                queue->kbase_retired_heap.vt_jobs,
+                queue->subqueues[PANVK_SUBQUEUE_FRAGMENT].kbase.emitted_jobs,
+                queue->kbase_retired_heap.frag_jobs);
       return VK_SUCCESS;
+   }
    ...
+   *renewed = true;
    return VK_SUCCESS;
 }
```

### 2.3 为什么"必须有退避"（代价分析，Lead 直接问的点）

- 若**只在成功时清零、跳过时什么都不做**：`kbase_tiler_submit_count` 停在阈值上 ⇒ **下一次提交立刻再进这个分支** ⇒ 由于③在②之前，**每次提交都会执行 `kbase_wait_graphics_targets(..., UINT64_MAX)`（等待本队列 VT+FRAG 到最新 seqno 的同步排水）**。这会把"偶尔一次排水"变成"每次提交都排水"，提交线程被反复阻塞 —— 比现在的行为**更糟**，且会把 F5（帧时间尖刺）放大到不可用。
- ⇒ 最小可用形态是**"不清零，但把计数压低到 `interval - backoff`"**，即**每 `backoff` 次图形提交才重试一次**。取 `backoff = MAX2(1, interval/4)`：
  - `interval=32`（当前生产默认）⇒ `backoff=8`：重试频率是设计频率的 4 倍，但**永不永久失能**；单次代价仍是一次排水（与正常续租相同量级）。
  - `interval=1`（加压实验）⇒ `backoff=1` ⇒ 每次提交重试（此时本来就每提交续租，无额外退化）。
  - `interval=UINT32_MAX`（`PANVK_KBASE_HEAP_RENEW_INTERVAL=0` 的语义）⇒ `backoff≈1.07e9` ⇒ 实际永不重试（与"禁用续租"一致，不会溢出：`interval - backoff` 仍在 uint32 内，因为 `backoff ≤ interval`）。
- 退避的**判据合理性**：被②拒绝的正常原因就是"某一侧子队列自上次退休后还没发射新作业"；这类原因通常在下一次或几次提交内自然消失（`backoff=8` 足够覆盖"只提交 FRAG 的时段"）。⇒ 退避既避免了永久失能，也不会在健康路径上增加任何排水。

### 2.4 `emitted_jobs` 在 `F:2200` 被重置的**精确影响**（F6 的准确边界）

- 事实：`F:2200`（队列 bind/setup 路径）执行 `subq->kbase.emitted_jobs = 0;`；而守卫用的是 **绝对计数比较** `emitted_jobs <= retired.{vt,frag}_jobs`（`F:3279-3282`）。
- 精确后果：若这次重置发生时 `kbase_retired_heap.ctx != 0` 且快照为 `vt_jobs = N > 0`，则此后 `emitted_jobs` 从 0 重新爬升，在爬过 `N` 之前**守卫恒为假**；在这段区间内本补丁的**退避也只是"每 backoff 次白跑一次排水"**，因为②永远拒绝。若 `N` 很大（例如重置前已发射过数千作业），**退避仍然无法自愈** —— 这正是我报告 56 里 F6 的准确含义：**F1 是"偶发跳过"，F6 是"低频但一旦命中就不可自愈"**，两者同源但后果不同。
- ⇒ 修 F1 的同时**必须**处理 F:2200，二者选一：
  - **(A) 最小**（推荐，与既有 teardown 语义一致）：在重置 `emitted_jobs` 的同一处，把退休槽一并销毁并清空。依据是 `F:3232-3241` 的既有注释："The queue groups are terminated by now, so the retired context (if any) is no longer firmware-visible and can be destroyed with the current one."
    ```c
    /* F:2200 附近（提案） */
    if (queue->kbase_retired_heap.ctx) {                     /* 组已重建 ⇒ 不再 firmware-visible */
       kbase_kmod_csf_tiler_heap_destroy(dev->kmod.dev, queue->kbase_retired_heap.ctx);
       queue->kbase_retired_heap.ctx = 0;
    }
    queue->kbase_tiler_submit_count = 0;
    queue->kbase_tiler_work_count = 0;
    subq->kbase.emitted_jobs = 0;
    ```
    **落地前需确认**：走到 `F:2200` 时队列组**确实**已经终止（该路径是"重新建立组"，其上文有 `kbase_destroy_group`/新建 group 的分支）——否则会退化成"提前 TERM"，正是 `F:3255-3272` 注释里那条 **0xc0 CSG fatal** 的成因。**这是本提案里唯一需要 Lead 逐行确认的前提。**
  - **(B) 更稳但更大**：引入只增不减的 `uint64_t kbase_job_epoch`（在 `F:1625` 与 `emitted_jobs++` 同时 `++`，且**不随组重建清零**），守卫改比 `epoch`。代价：多一个字段 + 守卫改写，但彻底消除"计数器回绕/重置"这一类前提依赖。

### 2.5 为什么这个修法**不会让卡死更糟**

1. **不碰 TERM 的条件**：`kbase_kmod_csf_tiler_heap_destroy()` 仍只在 `kbase_try_destroy_retired_heap()` 返回 true 时调用 ⇒ 不会引入新的"提前 TERM"（0xc0 / CSG fatal 那一族）。
2. **不碰排水时机**：`kbase_wait_graphics_targets()` 仍在同一个分支内、仍在续租之前 ⇒ 图形队列的可见性与现在完全一致。
3. **不改描述符的写法**：`write_desc + clean` 一字不动。
4. 唯一行为变化是**重试节奏**（跳过时从"下个 interval"变成"下个 backoff"）与**日志内容**；退避保证重试频率**不会高于**每 `backoff` 次提交一次 ⇒ 不存在"每提交排水"的最坏路径。
5. F:2200 的 (A) 改动在语义上是把"退休槽"与"队列组生命周期"对齐，与 teardown 路径的既有做法同构；风险点已在 2.4 标出（需确认组已终止）。

### 2.6 可证伪判据（零新代码）

| 判据 | 命令/观察 | 期望 |
|---|---|---|
| 补丁生效 | `grep -c "renew #.*begin"` vs `grep -c "renew #.*end"` | 仍然允许不配平（跳过是合法状态），但**每次不配平都应有一条 `postponed` 行**，且行内含 `vt a/b, frag c/d` 可直接看出是哪一侧没推进 |
| 未退化成"每提交排水" | `grep -c "tiler heap renewal"`（现在这条是每进分支一行） | 频率 ≤ `图形提交数 / backoff`；若接近"每提交一行" ⇒ 退避没生效（写错了） |
| F6 是否命中 | 出现 `postponed` 且 `vt b`/`frag d` 长时间不增长（尤其跨越"组重建"日志） | 命中 ⇒ 需 (A) 或 (B) |
| 不引入卡死 | `grep -nE "CS error|CSG fatal|0xc0"` | 不应新增（与本补丁无因果关系） |

---

## 3. 【F3】窗口前提的确证 / 否证

### 3.0 前提确证过程：「VT 与配对 FRAG 是否会不同批」（Lead 指定的线索）

**先更正一条线索**：`kbase_flush_subqueues` **在本树不存在**（`grep -n "kbase_flush" panvk_vX_gpu_queue.c` = 0 命中）。本树做批量提交/发布的只有两处 `kbase_subqueue_publish()`，且都是"对本次 `touched` 掩码逐个子队列发布"：

| 步骤 | 位置 | 事实 |
|---|---|---|
| 本次提交要发哪些子队列 | `F:3854-3870` | `touched` 只累计 `qsubmit->stream_size != 0` 的子队列 ⇒ 空流不发 |
| 全部发射后**一次性**发布 | `F:3872-3873` | `u_foreach_bit(i, touched) kbase_subqueue_publish(queue, i);` ⇒ **同一次提交内的 VT 与 FRAG 是同批发布的**（中间没有 flush/等 fence） |
| 第二处 publish | `F:2500-2508` | 在 `kbase_submit_init_subqueues()`（队列初始化流，一次性），与渲染路径无关 |
| 目标 seqno 的快照 | `F:3876-3877` | 发布**之后**取 `emitted_jobs` ⇒ 排水的目标覆盖本次提交的全部图形作业 |

**那么"不同批"可达吗？——可达，但只有一条合法路径**：

- `CmdEndRendering`（`D:4866-4870`）：`bool suspending = … & VK_RENDERING_SUSPENDING_BIT;` 且 `if (!suspending) { … get_fb_descs() … }` ⇒ **挂起渲染**时，本次不结束该 render pass 的片元侧工作，把它留给**后续 command buffer / 后续提交**的 `VK_RENDERING_RESUMING_BIT`（`panvk_vX_cmd_buffer.c:1293` 设置 RESUMING）。
- ⇒ 此时**某一次提交可以只有 VT（tiling）流、没有配对的 FRAG 流**。
- 另一种被上游注释点名的情况是**反向的**：clear-only 提交只有 FRAG、没有 VT（"Clear-only fragment submissions don't use the tiler heap"，`F:3891-3892`）。

**但对 F3 而言，这只影响"窗口前提"的第一半，第二半（关键那半）由 §3.2 的证据否掉了**：即使 VT 单独成批，**VT 子队列自己的 seqno 就蕴含 tiler 退休**（wrapper 在写 seqno 前等**全部** scoreboard 槽，而 `cs_vt_end` = `VERTEX_TILER_COMPLETED` 是占用其中一槽的 indirect async op）。⇒ 排水即使只等到 VT，tiler 也已经停了。

**复现命令（只读）**：
```bash
M=/root/zenithblue/work/mesa/src/panfrost ; F=$M/vulkan/csf/panvk_vX_gpu_queue.c
grep -n "kbase_flush" $F                                   # → 0 命中（Lead 线索里的函数不存在）
sed -n '3854,3878p' $F                                     # touched 掩码 + 一次性 publish
sed -n '2496,2510p' $F                                     # 第二处 publish（init 流）
sed -n '4866,4880p' $M/vulkan/csf/panvk_vX_cmd_draw.c       # suspending ⇒ 推迟片元侧
grep -n "RESUMING_BIT" $M/vulkan/csf/panvk_vX_cmd_buffer.c  # 恢复渲染
sed -n '1520,1560p' $F                                     # wrapper: cs_wait_slots(all_mask) → deferred seqno add
grep -n "cs_vt_end" $M/vulkan/csf/panvk_vX_cmd_draw.c $M/genxml/cs_builder.h
```

### 3.1 我原来的说法（报告 56 的 A.4-F3）

> 排水只等到 CS seqno；tiling 是异步的，其完成由 **FRAG** 的 `wait_finish_tiling()` 观测。若某批提交有 VT 而配对 FRAG 不在同批，排水就只等到 VT 的 CS seqno，而它**不证明 tiler 已停** ⇒ 在飞 tiler 撞上换堆。

### 3.2 新证据链（逐条带行号）

1. **seqno 写在"全槽等待"之后**：`F:1520-1595`（ring entry wrapper 尾部）先
   `cs_wait_slots(&b, dev->csf.sb.all_mask);`（注释原文：*"Signal completion once all prior operations retired: an explicit WAIT on all scoreboard slots followed by a SYNC_ADD64 on the cell proper"*），随后才是 deferred `cs_sync64_add(..., 1, seqno_addr, cs_defer(0, SB_ID(DEFERRED_SYNC)))` —— 即 **`cell->seqno` 前进 ⟹ 该 entry 的全部 scoreboard 槽已退休**。
2. **`cs_vt_end` 就是"tiler 完成"那条 op，且占一个被覆盖的槽**：
   - `cs_builder.h:2395-2399`：`cs_vt_end()` = `cs_heap_operation(MALI_CS_HEAP_OPERATION_VERTEX_TILER_COMPLETED, async)`。
   - 调用点 `D:4271` / `D:4292`：`cs_vt_end(b, cs_defer_indirect())` —— 属 **indirect async op**；`F:1397-1406` 的注释明确点名 `cs_vt_end, cs_finish_fragment, RUN_FRAGMENT, compute dispatch` 这四个 indirect async op 的 **wait mask / signal slot 来自寄存器**，且这些槽属于 `all_iters_mask`。
   - `all_iters_mask = BITFIELD_RANGE(PANVK_SB_ITER_START, iter_count)` ⊆ `dev->csf.sb.all_mask`（`panvk_vX_device.c` 中 `all_mask = BITFIELD_MASK(scoreboard_slot_count)`）。
3. ⇒ **VT 子队列的 seqno 前进 ⟹ VT_END（VERTEX_TILER_COMPLETED）已退休 ⟹ tiler 对该堆的写入已结束。**

**结论：F3 的主要窗口不成立（否证）。** 我此前把"CS seqno"与"tiler 退休"当作两件事，是本实现对 wrapper 的设计（等待全槽后才写 seqno）所**排除**的；这也解释了为什么 FRAG 流的 `wait_finish_tiling()`（`D:4304-4322`，等 VT 的 syncobj）能成立——两处依赖的是同一个"槽即完成"的语义。

### 3.3 次生通道的撤回

- 我原来说：`FINISH_FRAGMENT` 回传的 First/Last free heap chunk 来自 **tiler 上下文**，若产自旧堆，可能把旧堆的块并入新堆空闲链。
- 撤回理由：`cs_finish_fragment` 在 **FRAG 流**内（`D:4725-4729`），而 FRAG 流的 seqno 同样在"全槽等待"之后才写 ⇒ **排水等到 FRAG seqno ⟹ FRAG 流内的 FINISH_FRAGMENT 已退休** ⇒ 不存在"换堆后才执行的跨代际块回传"。
- 残留（无证据、仅记录）：固件**内部**如何处理跨代际的块回收，属 kbase 闭源侧，无法从本树证实或否证。

### 3.4 那么"窗口"到底还剩什么？

| 可能的分流 | 是否可达 | 依据 |
|---|---|---|
| 同批提交里 VT 与 FRAG 都在 | 排水覆盖两者 ⇒ **无窗口** | §3.2 |
| VT 与配对 FRAG **不同批**（`VK_RENDERING_SUSPENDING_BIT`/`RESUMING_BIT` 的挂起/恢复渲染可合法造成） | 仍然**无窗口**——因为 VT 自己的 seqno 就蕴含 tiler 退休 | §3.2 第 3 条；挂起/恢复的处理见 `panvk_vX_cmd_buffer.c:1293`（设置 RESUMING）与 `D:4870`（读 SUSPENDING） |
| 某一侧子队列从未推进 ⇒ 续租被跳过 | **可达**，但后果是"堆不回收"而不是"在飞 tiler 撞换堆" | F1/F6（§2） |

⇒ **F3 作为"楔形机制"退役**；`PANVK_KBASE_HEAP_RENEW_INTERVAL=0` 的 A/B 仍然值得做，但它的解释空间已经变小（见 3.5）。

### 3.5 env A/B 的判决价值（以及两种结果各自的下一步）

**先验证实验本身成立**（很重要）：`=0` ⇒ `kbase_tiler_heap_renew_interval()` 返回 `UINT32_MAX`（`F:119-121`）⇒ 续租分支**永不进入**（work 路径因 `tiler_work_estimate` 无生产者而恒为死路，见 P2 注释 `F:3893-3899`）
⇒ **logcat 里 `renew #… begin` 行数应 ≈ 0**。**若仍有 begin 行 ⇒ env 没生效**（`static` 缓存要求重启进程；或包/驱动没换），此时**不要解读画面结果**。
（另外：v83 包的驱动 `.so` 与 v82 相同 ⇒ 严格单变量，这一点已由 Lead 保证。）

| 观察 | 含义 | 下一步 |
|---|---|---|
| **楔形消失** | 换堆**活动本身**有害。但 §3.2 已排除"tiler 在飞"⇒ 剩余嫌疑是 **销毁退休堆 ⇒ chunk VA 被复用** 这条路径（`F:3274-3288` + `kbase_kmod.c:1085-1095` 的 `TERM`）：需要在内核侧或通过 fault 日志验证"被复用的 chunk VA 仍被某个已退休堆的固件引用" | 用一个**极小**实验分离：把 `kbase_try_destroy_retired_heap()` 里的 `TERM` 临时改为"只丢弃引用不 TERM"（泄漏一个堆，换来零销毁）——若楔形消失 ⇒ 直指 TERM/VA 复用 |
| **楔形照旧** | 堆生命周期（含换堆与销毁）与楔形无关 | 立刻转"**多边形链表头 / tiler context / desc ringbuf / 顶点缓冲**"线（报告 56 A.1 的三个替代嫌疑）；注意 v82 帧的地形正确、只有一块黑三角 ⇒ 更像**单个 draw 的多边形链被污染**而不是全局 |
| **楔形变差** | 堆耗尽路径（TILER_OOM → `oom_fbd` / `ir_scratch_fbd_ptr`，`D:4568-4581, 4334`）被反复走 ⇒ **OOM 兜底路径本身是楔形候选**（这是一个尚未被否证的候选，且与"重负载"高度吻合：重负载才耗尽堆） | 优先查 OOM 兜底：`setup_tiler_oom_ctx()` 的字段与 `oom_fbd` 的生命周期、以及 OOM handler 运行时与换堆的交互 |

> 注：把"楔形变差"单独列为**有信息量的结果**——它与"消失"指向完全不同的机制，两者都不是"实验失败"。

---

### 3.6 F3 提案 diff（**结论：不需要**；并说明"强行加"为什么可能把卡死搞得更糟）

既然 §3.2 已把窗口否掉，F3 的"正确修法"是**不修**——现状的"全槽等待后才写 seqno"已经把"tiler 已退休"这件事编码进了排水判据。为了让这个**不修**的结论可被审计，这里逐条评估 Lead 给的两个候选：

**候选 A：切换堆前额外 `cs_wait_slots` 到 FRAG 的 all-iters mask**

- **不可行的部分**：`cs_wait_slots()` 是 **CS 指令**，只能在 **CS 流内部**发出；续租发生在 **host 侧**（`kbase_renew_tiler_heap()` 跑在提交线程里），host 无法"插入"一条 GPU 指令。
- **已经等价存在的部分**：wrapper（`F:1520-1595`）在每个 ring entry 的尾部已经 `cs_wait_slots(&b, dev->csf.sb.all_mask)`——**all_mask ⊇ all_iters_mask**，而且这就是 seqno 前进的前置条件。⇒ 候选 A 想达到的效果**已经冗余地存在**。
- **强行加会更糟（重要）**：若真的在 VT 流里额外插一条"等 FRAG 的 iters"的等待，就会让 **VT 去等 FRAG**，而 FRAG 的推进依赖 VT 的完成（`wait_finish_tiling()`，`D:4304-4322`）⇒ **互相等待**。更一般地，`F:1397-1406` 的注释明确警告：scoreboard 槽 3..15 是 **CSG 共享**、由各子队列的 endpoint work 信号，**让一个 in-flight endpoint op 去等一个没人会信号的槽**正是"第 3~4 个作业就卡死"那一族的成因。⇒ **绝不能在 VT 流里等 FRAG 的迭代槽。**

**候选 B：用 `FINISH_FRAGMENT` 回传的空闲块（first/last heap chunk）做判据**

- 那两个字段（`genxml/v10.xml:649-650`）是 **GPU → 固件** 的 heap operation 参数（`cs_finish_fragment(... completed_top, completed_bottom ...)`，`D:4725-4729`），**host 侧读不到**，所以不能直接做判据。
- 若想用"堆是否已全部归还"做 host 判据，需要**读回** tiler heap 描述符/tiler context 的 `bottom/top`（都在 device memory，且需要一次显式 cache invalidate——本树的 `panvk_priv_mem` 读回路径存在，但这是一条**新的**同步设计，不是最小改动）。
- 而且它解决的是"块是否归还"，**不是**"tiler 是否还在写"——后者才是 F3 的命题，而后者已被 §3.2 否掉。⇒ **投入产出不成立。**

**因此 F3 的提案是：维持现状，并加一条"守望"判据**（纯诊断，零时序改动）：

| 守望项 | 位置 | 判据 |
|---|---|---|
| 排水是否真的覆盖了两侧 | `F:3789-3810` | `kbase_wait_graphics_targets` 只在 `targets[i] != 0` 时等待 ⇒ 在 `F:3910` 的排水之后、`kbase_renew_tiler_heap()` 之前，若想把"跳过了哪一侧"看得更清楚，只需 §2.2 的 `postponed` 日志（**不需要新同步**） |
| "VT seqno ⟹ tiler 退休"是否成立 | wrapper `F:1520-1595` | 若将来有人改动 wrapper 的等待掩码（例如把 `all_mask` 收窄），这条蕴含关系会被破坏 ⇒ 建议在 wrapper 的注释里把它写成**显式不变式**："seqno 前进必须蕴含 cs_vt_end/cs_finish_fragment 的槽已退休；续租依赖这一点"（**纯注释，不动时序**） |

## 4. 未验证 / 限制（诚实清单）

1. 本文件的两个补丁草案**均未落盘、未编译、未上机**；退避参数 `interval/4` 是**基于代价推理的取值**，未做真机频率测量。
2. §2.4 的 (A) 修法有一个**未确认前提**：走到 `F:2200` 时队列组是否**确实**已终止（若否，该改动会退化成提前 TERM ⇒ 0xc0 CSG fatal）。**这是 Lead 落地前必须逐行确认的一项。**
3. §3.2 的否证依赖两条语义：`cs_vt_end` 的 signal 槽 ∈ `sb.all_mask`（由 `F:1397-1406` 的注释与 `all_iters_mask ⊆ all_mask` 推出，**未在硬件上直接观测**）；以及 "VERTEX_TILER_COMPLETED 槽退休 ⟺ tiler 真的写完堆"（这是该 op 的命名与用途，但**属语义推断**，不是实证）。⇒ 否证的把握标为**中等偏高**，不是"已证"。
4. 若 env=0 的实测是"楔形消失"，则 §3.2 的否证**不覆盖**该结果（它只否证"在飞 tiler"这一条机制），应走 §3.5 第一行的 TERM/VA 复用线。
5. 我未核对 v82 局 logcat 里那 4 次 `postponed` 的**具体是哪一侧**未推进（需要 logcat，设备侧归 Lead）；本文件 §2.2 的 `postponed` 日志行正是为下一次实验准备的。
6. 未验证挂起/恢复渲染是否真的在本项目的 Minecraft/Zalith 路径上出现（§3.4 只论证了"即使出现也无窗口"）。

---

## 5. 附：给 Lead 的落地顺序建议（单变量）

1. **先做**（零风险、纯诊断）：§2.2 的**改动 1 + 改动 3**（签名加 `bool *renewed`、加 `postponed` 日志、日志挪到结论后）。这一步不改任何时序，只让"跳过"可见 ⇒ 下一局 logcat 能直接指出是哪一侧没推进。
2. **再做**：改动 2（只在 `renewed` 时清零 + 退避）。观察 §2.6 的四条判据。
3. **最后**：F:2200 的 (A)/(B)（先确认组已终止，再选 A；不确定就选 B）。
4. 全程与 env A/B **分开**跑：env 实验判"换堆是否有害"，本补丁判"续租状态机是否正确"——两者混在一起会互相污染结论。
