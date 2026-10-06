# 69 — 正确落点：给 panvk 加 `QueueWaitIdle` 覆写（构建就绪的逐行 diff）

日期：2026-10-06 · 只读分析（出提案）· 行号基线 = 我读到的当前树
配套：`68-wsi-present-ab-diffs.md`（为何 WSI 落点不行）、`67b`（机制）

---

## 0. 你的洞察成立，而且比"只改 present"更省：**完全不动 WSI**

`vk_common_DeviceWaitIdle()`（`src/vulkan/runtime/vk_device.c:658-668`）：
```c
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;
   vk_foreach_queue(queue, device) {
      VkResult result = disp->QueueWaitIdle(vk_queue_to_handle(queue));
```
⇒ **只要 panvk 的 `dispatch_table.QueueWaitIdle` 指向我们的实现，present 里那句 `DeviceWaitIdle` 就自动变成真栅栏**（WSI 一个字都不用改）✓✓
⇒ 你上一轮 WSI 编译失败的原因也清楚了：**`panvk_wsi.c` 是跨架构通用文件**，没有 include `vk_device.h` / `csf/panvk_queue.h`，所以 `vk_foreach_queue`、`panvk_gpu_queue_from_vk_queue`、`PANVK_SUBQUEUE_COUNT` 全都未声明 —— **不是名字错，是落点错** ✓

---

## 1. dispatch table 在哪装配 + 覆写点（已核到行）

**`src/panfrost/vulkan/panvk_vX_device.c`**：
```
:420   struct vk_device_dispatch_table dispatch_table;
:429-438  (PAN_ARCH <= 9 分支：cmd_dispatch 的填充)
:443   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &panvk_per_arch(device_entrypoints), PAN_ARCH > 9);
:445   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &panvk_device_entrypoints, false);
:447   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_device_entrypoints, false);
:451   result = vk_device_init(&device->vk, &physical_device->vk, &dispatch_table, pCreateInfo, pAllocator);
```
⇒ **覆写必须插在 `:449`（最后一次 `from_entrypoints`）与 `:451`（`vk_device_init`）之间** —— 即紧跟在 `wsi_device_entrypoints` 那一块之后。

### Diff 1/3 —— `panvk_vX_device.c`（装配点）
```diff
--- a/src/panfrost/vulkan/panvk_vX_device.c
+++ b/src/panfrost/vulkan/panvk_vX_device.c
@@
    vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                              &wsi_device_entrypoints, false);
 
+   /* v104: panvk has no queue-idle hook, so VkQueueWaitIdle/VkDeviceWaitIdle
+    * fell through to vk_common_QueueWaitIdle(), which submits a signal-only
+    * work item.  On the kbase path that signal is bound to the *empty* target
+    * set of that very submit (csf/panvk_vX_gpu_queue.c:4028-4040), so it is
+    * resolved immediately and the call returns in ~10 us without waiting for
+    * the application's in-flight rendering.  Overriding the per-queue entrypoint
+    * makes it wait on the CS completion word instead.  Must be assigned before
+    * vk_device_init() copies the table. */
+   dispatch_table.QueueWaitIdle = panvk_per_arch(QueueWaitIdle);
+
    result = vk_device_init(&device->vk, &physical_device->vk, &dispatch_table,
                            pCreateInfo, pAllocator);
```

### Diff 2/3 —— `csf/panvk_queue.h`（声明，放在既有 per-arch 队列入口旁）
```diff
--- a/src/panfrost/vulkan/csf/panvk_queue.h
+++ b/src/panfrost/vulkan/csf/panvk_queue.h
@@
 VkResult panvk_per_arch(gpu_queue_check_status)(struct vk_queue *vk_queue);
+VkResult panvk_per_arch(QueueWaitIdle)(struct vk_queue *vk_queue);
 
 #endif
```

### Diff 3/3 —— `csf/panvk_vX_gpu_queue.c`（实现；**与 `kbase_subqueue_wait_seqno()` 同文件 ⇒ 不需要薄导出** ✓）
```diff
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ (kbase_subqueue_wait_seqno() 定义之后，同一文件内)
+/* v104: real VkQueueWaitIdle for the kbase backend.
+ *
+ * Waits for each subqueue's last submitted job on the CS completion word
+ * (cell + 0), which the ring wrapper writes only *after* its all-scoreboard
+ * wait -- i.e. after every asynchronous GPU op of that entry has retired.
+ * That is strictly stronger than "the command stream was fetched".
+ *
+ * Bounded: a single deadline covers all subqueues (default 50 ms, override
+ * with PANVK_QUEUE_WAIT_IDLE_TIMEOUT_US; 0 = spec-faithful wait forever).
+ * On timeout we log (rate-limited) and return VK_SUCCESS: present must never
+ * become a hang point, and a timeout is itself the measurement we want. */
+VkResult
+panvk_per_arch(QueueWaitIdle)(struct vk_queue *vk_queue)
+{
+   struct panvk_gpu_queue *queue = (struct panvk_gpu_queue *)vk_queue;
+
+   const unsigned timeout_us =
+      debug_get_num_option("PANVK_QUEUE_WAIT_IDLE_TIMEOUT_US", 50000);
+   const uint64_t start = os_time_get_nano();
+   const uint64_t deadline =
+      timeout_us ? start + (uint64_t)timeout_us * 1000ull : UINT64_MAX;
+
+   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
+      const uint64_t target = queue->subqueues[i].kbase.emitted_jobs;
+
+      if (!target)
+         continue;
+
+      VkResult r = kbase_subqueue_wait_seqno(queue, i, target,
+                                             0 /* rekick_mask */,
+                                             false /* allow_ring_drain */,
+                                             deadline);
+
+      /* Re-check the word itself rather than trusting the return value: the
+       * static helper's timeout path is documented by its internal `deadline`,
+       * not by a value we have read. */
+      volatile uint64_t *cell =
+         (volatile uint64_t *)kbase_subqueue_seqno_cell(queue, i);
+      if (r != VK_SUCCESS || *cell < target) {
+         static int64_t last_log;
+         const int64_t now = os_time_get_nano();
+         if (now - last_log > 1000ll * 1000 * 1000) {   /* ≤1 行/秒 */
+            last_log = now;
+            mesa_loge("kbase: QueueWaitIdle TIMEOUT after %" PRIu64 " us: "
+                      "subq %u cell %" PRIu64 " < target %" PRIu64
+                      " (emitted_jobs %" PRIu64 ") - returning VK_SUCCESS",
+                      (os_time_get_nano() - start) / 1000, i,
+                      (uint64_t)*cell, target,
+                      queue->subqueues[i].kbase.emitted_jobs);
+         }
+         return VK_SUCCESS;      /* never fail the app / never hang present */
+      }
+   }
+
+   return VK_SUCCESS;
+}
```

---

## 2. 你要的三条确认

**2.1 `panvk_gpu_queue` ← `vk_queue` 的一行式取法（在正确文件里）**
`csf/panvk_queue.h:165`：`VK_DEFINE_HANDLE_CASTS(panvk_gpu_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)`；`:119` `struct panvk_gpu_queue { struct vk_queue vk; … }` ⇒ **`vk_queue` 在偏移 0** ⇒ 两种写法都成立：
```c
struct panvk_gpu_queue *queue = panvk_gpu_queue_from_vk_queue(vk_queue);   /* 生成宏，优先 */
struct panvk_gpu_queue *queue = (struct panvk_gpu_queue *)vk_queue;        /* 偏移 0 ⇒ 等价 */
```
**说明**：这个宏**在 panfrost 树里没有任何调用点**（我 grep `_from_vk_queue(` 零命中）⇒ 我**无法用"既存用法"证明生成名**；上面第二条（普通 cast + 偏移 0 的论证）是我给的**保底写法**，编译不过就换它。**你上轮在 WSI 报"未声明"，正是因为那个文件没 include 这两个头**（`vk_device.h` 提供 `vk_foreach_queue`、`csf/panvk_queue.h` 提供 cast 与 `PANVK_SUBQUEUE_COUNT`）—— 在 `csf/panvk_vX_gpu_queue.c` 里这些**本来就在作用域内**（该文件已经用 `queue->subqueues[...]`、`PANVK_SUBQUEUE_COUNT`）。

**2.2 不需要薄导出** ✓：`kbase_subqueue_wait_seqno()` 是 `csf/panvk_vX_gpu_queue.c:1737` 的 **static** 函数，而我们的覆写实现**放在同一个文件**（Diff 3/3）⇒ 直接可见、不需要任何 export/声明。**唯一新增声明是 `panvk_per_arch(QueueWaitIdle)`（Diff 2/3）**，因为装配点在另一个文件（`panvk_vX_device.c`）。

**2.3 为什么"只可能更晚、不可能更早"（沿用 68 号那句，逐字可检验）**
> **本覆写只把"返回时机"从"立即"推迟到"该子队列最后一个已发射作业的 CS 完成字（在 wrapper 全槽等待之后写入）达到其作业号"；等待条件与提交路径自身使用的判据完全相同，且带可配置上限（默认 50 ms）⇒ 只可能让 `QueueWaitIdle`/`DeviceWaitIdle`（以及经由它们的 present）更晚返回，绝不可能更早 ⇒ 不可能引入提前放行型数据竞争；超时即返回 ⇒ 也不会把 present 变成新的挂死点。**

**检验方式**：本版与上一版各跑一局，对比 68-A 日志里的 `idle_us` —— **它应当从 9–13 µs 变成"通常几毫秒（= 真正被等掉的时间）"**，而 `seqno_after >= target` 应当**恒成立**；撕裂若消失且 FPS 不掉 ⇒ 即命中。

---

## 3. 两条路线的取舍（你问的第 5 点）

| 路线 | Diff 面 | 风险 | 结论 |
|---|---|---|---|
| **A. `QueueWaitIdle` 覆写（本报告）** | 3 个文件、约 45 行；**WSI 零改动** | **低-中**：全进程生效；带超时与限流日志；`PANVK_QUEUE_WAIT_IDLE_TIMEOUT_US=0` 可切回"符合规范但可能长等" | ✅ **推荐**：它是唯一能同时修 (c) 与 present 的落点，且比 B 代码更少 |
| **B. 只改 present（68 号 Diff B）** | 需要**薄导出 + WSI 改动**，而 WSI 看不到 kbase 类型（你已编译证明）⇒ 必须再加一层 device 级导出 | 中-高 | ❌ **已被 A 取代**：代码更多、收益更窄、且要动跨架构文件 |

**建议**：**只上 A**（`PANVK_QUEUE_WAIT_IDLE_TIMEOUT_US` 默认 50000；若要看"规范行为"再跑一局 `=0` 做对照）。B 不建议再做。

---

## 4. 上机判读（配合 68-A 的日志；若你还没上 68-A，A 单独也能看）

| 观测 | 结论 |
|---|---|
| `idle_us` 从 ~10 µs 变成几毫秒，`seqno_after >= target` 恒成立 | **A 生效**：present 有了真栅栏 ⇒ 看撕裂是否消失（同一版对照截图） |
| `idle_us` 仍 ~10 µs | 覆写没生效 ⇒ 检查 ① `dispatch_table.QueueWaitIdle` 是否在 `vk_device_init` **之前**赋值；② `emitted_jobs` 是否为 0（若队列从未提交过，我们 `continue` ⇒ 立即返回 ✓ 属正常） |
| 出现 `QueueWaitIdle TIMEOUT`（≤1 行/秒） | **两种含义**：① target 算错/作业仍在构建（`emitted_jobs` 在发射后才自增，见 §5-4）；② GPU 真的没完成 ⇒ 与卡死族合并分析。**日志限流保证不会把 logcat 打爆** |
| FPS 明显下降 | 说明超时在**频繁触发**（每次白等 50 ms）⇒ 先把 `PANVK_QUEUE_WAIT_IDLE_TIMEOUT_US` 调小（如 5000）再看，不要盲目保留 |

---

## 5. 未验证 / 限制（诚实清单；每条都影响你构建时的判断）

1. 三个 diff **均未编译、未上机**（按纪律只出提案）。
2. **`dispatch_table.QueueWaitIdle` 字段名**由 `vk_device.c:666` 的 `disp->QueueWaitIdle(...)` **用法**确证 ✓；但**它所在的结构体定义我没找到**（`grep QueueWaitIdle vk_device.h/vk_queue.h` 零命中 ⇒ 应在生成头/宏展开里）⇒ 若编译报"无此成员"，请在该生成头里核对字段名（几乎必然就是 `QueueWaitIdle`）。
3. **生成 cast 宏的名字我无法用既存用法证明**（树内零调用）⇒ 保底写法 `(struct panvk_gpu_queue *)vk_queue` 已随 diff 给出（偏移 0 有据：`panvk_queue.h:119` 首成员 `struct vk_queue vk;`）。
4. **`kbase_subqueue_wait_seqno()` 的返回值在超时路径上返回什么，我没读到 `return` 语句** ⇒ Diff 3/3 里我**不依赖它的返回值**：调用后再自查 `cell >= target`（这条判据本身与手册一致）。
5. **`emitted_jobs` 的语义边界**：它在作业**发射完成后**自增（`:1625`）⇒"正在构建、尚未 publish"的作业不在计数内，故本次等待可能**早一个作业**通过（若应用此刻正在提交）。要更严可对 target 用 `+1`，但那样在应用无新作业时会白等到超时 ⇒ 建议先用现写法，看日志再定。
6. `debug_get_num_option("PANVK_QUEUE_WAIT_IDLE_TIMEOUT_US", 50000)` 的选项名是我新起的（Mesa 的 `debug_get_num_option` 语义为"取整型环境变量，缺省用默认值"）；`os_time_get_nano()`/`mesa_loge` 在该文件已有使用（`kbase_subqueue_wait_seqno()` 自己就用 `os_time_get_nano()`）。
7. `PANVK_SUBQUEUE_COUNT` 在 `csf/panvk_vX_gpu_queue.c` 内已在使用 ✓；`kbase_subqueue_seqno_cell()` 与该文件同文件 ⇒ 可见性没问题 ✓。
