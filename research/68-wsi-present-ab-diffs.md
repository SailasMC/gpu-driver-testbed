# 68 — 可上机补丁（两份独立 diff：A 纯观测 / B 加等待）+ idle 钩子定位

日期：2026-10-06 · 只读分析（出提案，不改码）· 行号基线 = 我读到的当前树
配套：`67-idle-12us.md`、`67b-idle-mechanism-and-patch.md`

---

## 0. 两条你要的"写死的行"（先给结论）

### 0.1 从 WSI 拿 `panvk_gpu_queue` 的那一行
```c
/* swapchain 里已有 struct vk_device *dev（VK_FROM_HANDLE(vk_device, dev, swapchain->device)）。
 * vk_device 的队列是链表：struct list_head queues;  (src/vulkan/runtime/vk_device.h:197)
 * panvk 的队列对象就是 vk_queue（csf/panvk_queue.h:165：
 *   VK_DEFINE_HANDLE_CASTS(panvk_gpu_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)）
 * ⇒ 遍历方式照抄 vk_common_DeviceWaitIdle()（vk_device.c:658-668），然后把每个 vk_queue 转过去：*/
static struct panvk_gpu_queue *
panvk_wsi_first_gpu_queue(struct vk_device *vk_dev)
{
   struct vk_queue *q;
   list_for_each_entry(q, &vk_dev->queues, link)   /* ← 成员名请照抄 vk_common_DeviceWaitIdle 的写法 */
      return panvk_gpu_queue_from_vk_queue(q);
   return NULL;
}
```
**为什么安全（四条）**
1. **不持有悬垂指针**：`swapchain->device` 在整个 swapchain 生命周期内都是活的（WSI 持有 device 引用），我们从它派生队列指针、**只在本次调用内使用**，不缓存；
2. **不加锁是安全的**：队列列表在 `vkCreateDevice` 期间建立、之后**不再变更**（`vk_queue_init` 只在创建时调用，见 `panvk_bind_queue.c:44`），`vk_common_DeviceWaitIdle()` 自己也是无锁遍历同一链表 ⇒ 我们与它同构；
3. **只读状态**：`gq->subqueues[i].kbase.emitted_jobs`（`panvk_queue.h:119` 的 `panvk_gpu_queue`）+ 宿主映射的 seqno cell（`panvk_priv_mem_host_addr(gq->syncobjs)`，与超时路径 `kbase_log_queue_syncobjs()` `:275` 同一读法）；
4. **不新增同步原语** ⇒ 不会与提交路径互锁。

**取 cell 的稳健写法（绕开可能为 static 的访问器）**：
```c
struct panvk_cs_sync64 *so = panvk_priv_mem_host_addr(gq->syncobjs);
if (so) {
   kbase_cache_invalidate_range(so, sizeof(*so) * PANVK_SUBQUEUE_COUNT);
   /* so[i].seqno = 子队列 i 的完成计数 */
}
```
（`kbase_subqueue_seqno_cell()`（`:237`）是等价访问器，但**它没有出现在任何头文件里**（我 grep `csf/*.h` 无命中）⇒ 很可能也是 static ⇒ **上面对 `gq->syncobjs` 的直接读法更省事、且不依赖可见性**。）

### 0.2 修复 (b) 要用的现成设施：**完整签名与语义（已核）+ 一个必须先解决的可见性问题**
```c
static VkResult
kbase_subqueue_wait_seqno(struct panvk_gpu_queue *queue, uint32_t subqueue,
                          uint64_t target_seqno, uint32_t rekick_mask,
                          bool allow_ring_drain, uint64_t abs_timeout_ns)   /* csf/panvk_vX_gpu_queue.c:1737-1741 */
```
- **阻塞式**：函数体是 `while (true)` 轮询（读 ring 的 `CS_INSERT`/`CS_EXTRACT`、cell 的 seqno/ls_copy/marks/progress），**只在"全槽等待之后发出的完成写"上接受成功**（源码注释：*"Only accept the completion writes emitted after the all-scoreboard wait"*）⇒ **正是我们要的语义**（语义上蕴含该 entry 的所有异步 op 已退休）。
- **超时**：内部有内置看门狗 `watchdog = start + KBASE_WAIT_TIMEOUT_NS`，实际截止 `deadline = MIN2(abs_timeout_ns, watchdog)` ⇒ **调用方传"绝对时间戳"即可 50 ms 封顶**（`os_time_get_nano() + 50*1000*1000`）。
- **`rekick_mask`**：需要时用它给指定子队列补门铃（超时路径用过）；**present 里建议传 `0`**（不在 present 里踢门铃）。
- **`allow_ring_drain`**：**必须传 `false`**（drain 是危险旋钮，只应出现在排水/续租路径）。
- ⚠️ **可见性**：它**没有头文件声明**（`grep csf/*.h` 无命中）⇒ **是 static** ⇒ **WSI 不能直接调**。所以 Diff B 里必须加一个**薄导出**（下面 §3 给了逐行文本）。

---

## 1. `idle` 钩子的定位（你要的"顺手一并写明"）

- `grep -rn "wait_idle" src/panfrost/vulkan/` ⇒ **零命中**；`panvk_bind_queue.c` 里只设置了 `queue->vk.driver_submit = panvk_bind_queue_submit;`（`:55`）。
- ⇒ **panvk 根本没有注册任何队列 idle 回调** ⇒ `VkQueueWaitIdle`/`VkDeviceWaitIdle` **永远走运行时 common 实现**（`vk_common_QueueWaitIdle`，`vk_queue.c:1465`）。
- ⇒ **(c)「修 DeviceWaitIdle 本身」没有独立钩子可修**：common 实现是"正确"的（你已确认），**唯一可疑的落点是 `vk_sync_signal` 的 kbase 处理**（`csf/panvk_vX_gpu_queue.c:4028-4040`，把 signal 绑到 **`submit->kbase_target_seqnos`**）⇒ **要修就修那里（R1）**，这也是"全进程受益"的唯一入口。
- 结论：**A（观测）→ 若 W1 成立 → 先 B（止血）→ 同时查 R1（根因）**；(c) 不与 A/B 并列，它就是 R1。

---

## 2. Diff A —— 纯观测（**不改任何行为**，可单独上机）

```diff
--- a/src/panfrost/vulkan/panvk_wsi.c
+++ b/src/panfrost/vulkan/panvk_wsi.c
@@ struct panvk_android_swapchain {          /* 结构体内，images[] 附近 */
    ...
+   /* v102 (report 68-A): observation only.  acquire/present counters per image
+    * plus the three subqueue SEQNO cells around DeviceWaitIdle.  No behaviour
+    * change: nothing here waits, retries or reorders anything. */
+   uint64_t v102_acquire_count;
+   uint64_t v102_present_count[8];
 };
@@ panvk_android_swapchain_acquire_next_image()   /* :222-231 */
 {
    uint32_t idx = chain->next_image;
    chain->next_image = (chain->next_image + 1) % chain->base.image_count;
+   chain->v102_acquire_count++;
    *image_index = idx;
    return VK_SUCCESS;
 }
@@ panvk_android_swapchain_queue_present()        /* :261 起 */
 {
    struct panvk_android_swapchain *chain = (struct panvk_android_swapchain *)swapchain;
    if (image_index >= chain->base.image_count)
       return VK_ERROR_OUT_OF_DATE_KHR;
 
    VK_FROM_HANDLE(vk_device, dev, swapchain->device);
+
+   /* ---- v102-A: snapshot the cells before the idle ---------------------- */
+   struct panvk_gpu_queue *v102_gq = panvk_wsi_first_gpu_queue(dev);
+   uint64_t v102_before[PANVK_SUBQUEUE_COUNT] = {0};
+   uint64_t v102_target[PANVK_SUBQUEUE_COUNT] = {0};
+   if (v102_gq) {
+      struct panvk_cs_sync64 *so = panvk_priv_mem_host_addr(v102_gq->syncobjs);
+      if (so) {
+         kbase_cache_invalidate_range(so, sizeof(*so) * PANVK_SUBQUEUE_COUNT);
+         for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
+            v102_before[i] = so[i].seqno;
+            v102_target[i] = v102_gq->subqueues[i].kbase.emitted_jobs;
+         }
+      }
+   }
+   if (image_index < ARRAY_SIZE(chain->v102_present_count))
+      chain->v102_present_count[image_index]++;
 
    int64_t v101_t0 = os_time_get_nano();
    dev->dispatch_table.DeviceWaitIdle(swapchain->device);
    int64_t v101_idle_us = (os_time_get_nano() - v101_t0) / 1000;
+
+   /* ---- v102-A: one line, everything needed to separate W1 from W2 ------ */
+   if (v102_gq) {
+      struct panvk_cs_sync64 *so = panvk_priv_mem_host_addr(v102_gq->syncobjs);
+      uint64_t after[PANVK_SUBQUEUE_COUNT] = {0};
+      if (so) {
+         kbase_cache_invalidate_range(so, sizeof(*so) * PANVK_SUBQUEUE_COUNT);
+         for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
+            after[i] = so[i].seqno;
+      }
+      mesa_loge("kbase: present #%" PRIu64 " img %u (acq %" PRIu64 " pres %" PRIu64
+                "/%" PRIu64 "/%" PRIu64 ") waitsem %u fence %u idle_us %" PRId64
+                " | seqno before->after / target:"
+                " VT %" PRIu64 "->%" PRIu64 "/%" PRIu64
+                " FR %" PRIu64 "->%" PRIu64 "/%" PRIu64
+                " CP %" PRIu64 "->%" PRIu64 "/%" PRIu64,
+                chain->v102_acquire_count, image_index,
+                chain->v102_acquire_count,
+                chain->v102_present_count[0], chain->v102_present_count[1],
+                chain->v102_present_count[2],
+                pPresentInfo ? pPresentInfo->waitSemaphoreCount : 0,
+                (pPresentInfo && pPresentInfo->pFence) ? 1u : 0u,
+                (int64_t)v101_idle_us,
+                v102_before[0], after[0], v102_target[0],
+                v102_before[1], after[1], v102_target[1],
+                v102_before[2], after[2], v102_target[2]);
+   }
```
**要点**
- `image_index`/`pPresentInfo` 的可用性：`queue_present` 的签名是 `(swapchain, image_index, present_id, damage)`（65 号已核）⇒ **没有 `pPresentInfo`**！⇒ 把那两个字段改成 **`0`/去掉**，或改为在 `chain` 上记录"上一次 present 的 waitsem"（若上层 WSI 能拿到）。**这点我无法从签名确定，已在 §5 标为待你替换的占位符**（diff 里我用 `pPresentInfo ? … : 0` 的形式，编译期请按实际签名删掉）。
- `list_for_each_entry` / `panvk_gpu_queue_from_vk_queue` / `panvk_priv_mem_host_addr` / `kbase_cache_invalidate_range` 的 include：`panvk_wsi.c` 已经 include 了 panvk 与 panfrost 内部头（它用了 `VK_FROM_HANDLE`、`DeviceWaitIdle`），若缺 `list.h`/`panvk_queue.h` 补一行 include 即可。
- **零行为改变**：只读 cell、只增计数、只打印 ⇒ 不影响卡死/撕裂，不会污染 B 的归因 ✓
- 建议加一个限流旋钮（`PANVK_WSI_PRESENT_LOG_EVERY`，默认 1=每帧），量大时可调大。

---

## 3. Diff B —— 加等待（**单变量**；A 之后单独上）

**B-1 薄导出（因为 `kbase_subqueue_wait_seqno` 是 static）**
```diff
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ (在 kbase_subqueue_wait_seqno 定义之后)
+/* v102-B: exported for the WSI present path.  Same body/semantics as the
+ * static kbase_subqueue_wait_seqno(); rekick 0 and no ring drain because
+ * present must never disturb the submission state machine. */
+VkResult
+panvk_kbase_wait_subqueue_seqno(struct panvk_gpu_queue *queue, uint32_t subqueue,
+                                uint64_t target_seqno, uint64_t abs_timeout_ns)
+{
+   return kbase_subqueue_wait_seqno(queue, subqueue, target_seqno,
+                                    0 /*rekick_mask*/, false /*allow_ring_drain*/,
+                                    abs_timeout_ns);
+}
```
（同时在 `csf/panvk_queue.h` 里加一行声明；该头已被 `panvk_vX_gpu_queue.c` 与 WSI 侧可见。）

**B-2 present 侧等待（插在 Diff A 的 idle 之后、CPU 拷贝之前）**
```diff
--- a/src/panfrost/vulkan/panvk_wsi.c
+++ b/src/panfrost/vulkan/panvk_wsi.c
@@    dev->dispatch_table.DeviceWaitIdle(swapchain->device);
+
+   /* v102-B: DeviceWaitIdle may be a no-op for the app's in-flight work
+    * (vk_common_QueueWaitIdle submits a signal-only work item; the kbase path
+    * binds that signal to this submit's targets, which are empty).  Wait
+    * explicitly for each subqueue's last submitted job, on the CS completion
+    * word that is only written *after* the wrapper's all-scoreboard wait.
+    * Hard 50 ms cap: on timeout we log and continue, so present can never
+    * become a hang point. */
+   if (v102_gq) {
+      for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
+         const uint64_t target = v102_gq->subqueues[i].kbase.emitted_jobs;
+         if (!target)
+            continue;
+         const uint64_t abs = os_time_get_nano() + 50ull * 1000 * 1000;
+         VkResult wr = panvk_kbase_wait_subqueue_seqno(v102_gq, i, target, abs);
+         if (wr != VK_SUCCESS)
+            mesa_loge("kbase: present wait TIMEOUT subq %u target %" PRIu64
+                      " (idle_us %" PRId64 ") - continuing with the copy",
+                      i, target, (int64_t)v101_idle_us);
+      }
+   }
```
**风险边界（可检验的一句话，与 62b 同一套）**
> **本补丁只增加等待**：等待条件是"CS 完成字（在 wrapper 全槽等待之后写入）达到该子队列最后一个已发射作业号"，与提交路径自己用的判据**完全相同**，且带 50 ms 硬上限 ⇒ **只可能让 present 更晚、绝不可能更早** ⇒ **不可能引入提前放行型数据竞争**；超时即放行，故也不会把 present 变成新的挂死点。

**检验方式**：B 前后各跑一次 A 的日志 ⇒ 正常情况下 `seqno_after` 的行应与 B 之后一致（只是**更晚**出现）；**若 B 让撕裂消失且不影响 FPS**（50 ms 上限内通常几毫秒）⇒ 止血成功。

**⚠️ 与 `emitted_jobs` 语义有关的一条诚实提醒**：`emitted_jobs` 在**作业发射完成后**才自增（`:1625`），所以"正在构建、尚未 publish"的那个作业不在计数内 ⇒ 若应用此刻正在提交，本等待可能**早一个作业**通过。要更严可改为"`emitted_jobs + 1`"，但那会在应用尚未提交新作业时白等到 50 ms 超时 ⇒ **建议先用 `emitted_jobs`**，读 A 的日志再定。

---

## 4. 建议的上机顺序（单变量）

1. **只上 A** → 跑一局 → 读那一行日志：
   - `idle_us` 小 **且** 三行 `after >= target` 全部成立 ⇒ **W2**（GPU 真空闲）⇒ 火力转"呈现索引/应用侧"（32:1 倾斜；A 的计数就是它的证据）；
   - 存在 `after < target` ⇒ **W1 坐实** ⇒ 上 B；
   - **取图计数 ≠ 呈现计数**（或某图呈现数远超其取图数）⇒ **第二条独立根因**（present 拷了没渲染的图）⇒ 与 65 号的 acquire/present 缺陷 A/B 合并处理。
2. **再只上 B**（含 B-1 薄导出）→ 同一局对照。
3. 并行查 **R1**（`panvk_queue_submit_process_signals_kbase()` 的目标绑定）⇒ 修好则 B 可回退、全进程受益。

---

## 5. 未验证 / 限制（诚实清单）

1. 两份 diff **均未编译、未上机**；按纪律我只出提案。
2. **`pPresentInfo` 在 `queue_present` 里不存在**（签名为 `(swapchain, image_index, present_id, damage)`）⇒ Diff A 里那两处 `waitsem/fence` 字段是**占位符**，请按实际签名删除或改为 `chain` 上记录的值。
3. `list_for_each_entry(..., &vk_dev->queues, link)` 的**成员名 `link` 未逐字核实**（`vk_device.h:197` 只证明是 `struct list_head queues`）⇒ 请照抄 `vk_common_DeviceWaitIdle()`（`vk_device.c:658-668`）里的循环写法，那是唯一权威模板。
4. `kbase_subqueue_wait_seqno()` 的**返回值**我只确认了签名与内部 `deadline` 机制，**没读到它的 `return` 语句**（成功/超时各返回什么）⇒ B-2 里用 `wr != VK_SUCCESS` 判超时，若它超时也返回 `VK_SUCCESS`（只打日志），需要改成"超时由 wrapper 自己判定"（例如 wrapper 内比较 `cell->seqno >= target` 后再返回）。
5. `kbase_subqueue_seqno_cell()` 与 `kbase_subqueue_wait_seqno()` 的**链接性**是从"头文件无声明"推断为 static（未看函数前缀的那一行）⇒ 若它们其实有声明，B-1 的薄导出可省。
6. A 的每帧日志在 23 FPS 下约 23 行/秒（可接受）；如需降噪加 `PANVK_WSI_PRESENT_LOG_EVERY`。
