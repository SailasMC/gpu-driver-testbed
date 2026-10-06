# 67b — 12 µs 的机制定位 + 可落地补丁（R1/R2）+ 最省字段插桩

日期：2026-10-06 · 只读分析（出提案）· 行号基线 = 我读到的当前树
配套：`67-idle-12us.md`（D3 结案、W1/W2 判据）

---

## 1. 为什么 `DeviceWaitIdle` 12 µs 就返回 —— **头号机制已定位到 file:line**

你的链完全正确（`vk_common_DeviceWaitIdle` → 逐队列 `QueueWaitIdle` → `vk_common_QueueWaitIdle` 提交一个"只 signal 一个 sync"的空提交再等它）。我核到**我们树里对 `vk_sync_signal` 的处理分两条路**：

| 路径 | 位置 | 行为 |
|---|---|---|
| **kbase 路径** | `csf/panvk_vX_gpu_queue.c:4028-4040`（`panvk_queue_submit_process_signals_kbase`）<br>`panvk_kbase_sync_set_pending(signal->sync, submit->queue, kbase_wait_sync_targets, **submit->kbase_target_seqnos**);` | **不是立即 signal**，而是把 sync 标为 **pending**，绑定到**本次 submit 的目标 seqno** 上，之后由等待目标达成的机制去 signal |
| 非 kbase 路径 | 同文件 `:4095+`（`panvk_queue_submit_process_signals`） | 先 `drmSyncobjTimelineWait(queue->syncobj_handle, …)`，再 `drmSyncobjTransfer` ⇒ 内核 timeline syncobj（正确） |

⇒ **关键**：kbase 路径把 sync 绑到 **`submit->kbase_target_seqnos`**。而 `vk_common_QueueWaitIdle()` 产生的是一个 **"只 signal、没有任何渲染 work"的空提交**：
- 若该空提交的 `kbase_target_seqnos` 是 **0/空**，pending 条件**立即成立** ⇒ sync 立刻被 signal ⇒ `vk_sync_wait()` 立刻返回 ⇒ **12 µs** ✓✓✓
- 更严重的后果：**那笔 sync 与"应用此前在飞的渲染 work"之间没有建立顺序** ⇒ **`DeviceWaitIdle` 从不等待应用真正提交的渲染** ✗✗ —— 这正是我们观测到的现实现象。

**⇒ 这就是头号机制**，且它是**全进程性**的（任何依赖 `DeviceWaitIdle`/`QueueWaitIdle` 的同步都失效）。

**还差三处才能写出 R1 的精确 diff（都是只读、几秒）**：
```bash
M=/root/zenithblue/work/mesa/src/panfrost/vulkan
grep -rn "kbase_target_seqnos" $M/csf/*.c $M/csf/*.h | head -20          # ① 谁填充、何时填
grep -rn -A25 "panvk_kbase_sync_set_pending" $M/*.c $M/csf/*.c | head -60 # ② pending 如何被解除
grep -rn "kbase_wait_sync_targets" $M/csf/*.c | head -10                  # ③ 目标判定（是否空即通过）
```
**判读**：若 ② 显示"targets 为 0/空 ⇒ 立即 signal"或 ③ 显示"集合为空即视为达成" ⇒ **头号机制坐实**，R1 就是"把 signal 绑到**该队列当前已提交的最后目标**（`subq->kbase.emitted_jobs`）而不是本次 submit 的目标"。

---

## 2. 从 WSI 拿 `panvk_gpu_queue`：**一行式取法（已核实）**

`csf/panvk_queue.h:165`：
```c
VK_DEFINE_HANDLE_CASTS(panvk_gpu_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)
```
⇒ **`panvk_gpu_queue` 就是把 `vk_queue` 作为首成员 `vk` 的那个对象** ⇒ 任何 `VkQueue`/`struct vk_queue *` 都能一行转过去：
```c
struct panvk_gpu_queue *gq = panvk_gpu_queue_from_vk_queue(vk_queue);   /* 宏由 VK_DEFINE_HANDLE_CASTS 生成 */
```
（`VK_DEFINE_HANDLE_CASTS` 生成的是 `panvk_gpu_queue_from_vk_queue()` 与 `panvk_gpu_queue_to_vk_queue()`；若你的 Mesa 版本生成的名字不同，用 `vk_queue` → `container_of` 亦可，但**优先用生成的 cast 宏**，它带类型校验。）
**枚举设备的队列**：照抄 `vk_common_DeviceWaitIdle()` 的循环（`src/vulkan/runtime/vk_device.c:658-668`）——它已经在遍历 `dev` 的队列列表，用同一集合即可保证"遍历到的队列 = 应用用的队列"（**若两者不同，本身就是 #2 的答案**；插桩 §4 会打印队列数）。

---

## 3. 最小修复候选（完整可用 diff）

### R2（**我现在就能给完整 diff**；风险低-中；推荐先上，因为它不依赖 #1 的三处待查）
在 `panvk_wsi.c` 的 `queue_present()` 里，**在 `DeviceWaitIdle` 之后、CPU 拷贝之前**，用我们自己的 cell 做一次带超时的显式等待：

```diff
--- a/src/panfrost/vulkan/panvk_wsi.c
+++ b/src/panfrost/vulkan/panvk_wsi.c
@@ (queue_present 内，替换/补强现有 DeviceWaitIdle)
-   dev->dispatch_table.DeviceWaitIdle(swapchain->device);
+   dev->dispatch_table.DeviceWaitIdle(swapchain->device);
+
+   /* R2: DeviceWaitIdle 可能不等真东西（vk_common_QueueWaitIdle 的空提交只
+    * signal 一个 sync；kbase 路径把它绑到本次 submit 的目标上，空提交 ⇒ 立即
+    * 达成 ⇒ 12 µs 返回）。这里用我们自己的 SEQNO cell 做一次显式等待：
+    * 目标是每个子队列"最后提交的作业号 + 1"，与包装器完成 add 的判据一致。 */
+   for (uint32_t qi = 0; qi < dev->queue_count; qi++) {
+      struct panvk_gpu_queue *gq =
+         panvk_gpu_queue_from_vk_queue(dev->queues[qi]);
+      if (!gq)
+         continue;
+      for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
+         const uint64_t target = gq->subqueues[i].kbase.emitted_jobs + 1;
+         volatile uint64_t *cell =
+            (volatile uint64_t *)kbase_subqueue_seqno_cell(gq, i);
+         uint64_t t0 = /* now */;
+         while (*cell < target) {
+            if (/* now - t0 */ > 50ull * 1000 * 1000)
+               break;                      /* 超时：只打日志，绝不阻塞 present */
+            usleep(50);
+         }
+         if (*cell < target)
+            mesa_loge("kbase: present wait TIMEOUT subq %u: cell %" PRIu64
+                      " < target %" PRIu64 " (emitted_jobs %" PRIu64 ")",
+                      i, *cell, target, gq->subqueues[i].kbase.emitted_jobs);
+      }
+   }
```
**要点/风险边界**
- 语义 = `cell+0`（包装器完成 add）到达 target ⇒ **蕴含该 entry 全槽退休**（`44 → wait → 45 → 42 → add`）⇒ **只可能更晚、不可能提前** ⇒ 不引入提前放行。
- **带 50 ms 超时 + 只打日志**（`break` 后继续拷贝）：**绝不会把 present 挂死**（这是我们最怕的副作用）；超时日志本身又是有用的证据。
- **需要你确认的两个符号**：`dev->queues[]`/`dev->queue_count` 的真实字段名（照 `vk_common_DeviceWaitIdle` 的写法即可）、`subqueues[i].kbase.emitted_jobs` 是否可从 WSI 直接读（`panvk_queue.h` 里 `struct panvk_gpu_queue` 的 `subqueues` 数组 ✓，`:119`）；`kbase_subqueue_seqno_cell()` 在 `csf/panvk_vX_gpu_queue.c:237`（**非 static** 才可从 WSI 调；若它是 static，需要加一个薄封装导出）。
- 那个 `/* now */` 用 `clock_gettime(CLOCK_MONOTONIC)` 或现成的 `os_time_get_nano()`；`usleep` 用 `os_time_sleep(50 * 1000)`（Mesa 惯用法）。

### R1（**根因修复**；需先跑 §1 的三条 grep 才能写死 diff；风险中）
位置：`csf/panvk_vX_gpu_queue.c:4028-4040` 的 `panvk_queue_submit_process_signals_kbase()`。
改法（一句话精确描述，待 §1 确认后即成 diff）：
> `panvk_kbase_sync_set_pending(..., kbase_wait_sync_targets, submit->kbase_target_seqnos)` 里的目标**不能只用本次 submit 的目标**；对"无渲染 work 的纯 signal 提交"（`vk_common_QueueWaitIdle` 产生的就是这种），必须绑定到**该队列当前已提交的最后目标**（每个子队列 `subq->kbase.emitted_jobs + 1`，即 `:1498` 的同一算法），否则 sync 会在没有任何在飞工作时被立即 signal。

⇒ **R1 修好则全进程受益**（所有 `DeviceWaitIdle`/`QueueWaitIdle`/fence 语义恢复），且 R2 就变成冗余保险。

### R3（**排除**）
`ANativeWindow_lock` **没有**会等 GPU 的变体（它锁的是 CPU 可访问的窗口缓冲）；能等 GPU 的只有 sync/cell 两条路 ⇒ **R3 不成立**，别在它上面花时间。

**推荐顺序**：**先上 §4 的插桩（一次运行拿到判据）→ 若 `idle` 后 cell 落后 target ⇒ 先上 R2（低风险、立刻止血）→ 并行跑 §1 的三条 grep 拿 R1 的精确 diff → R1 上机（若有效，R2 可回退）。**

---

## 4. 插桩：确认你的方案 + 最省字段版（与 R2 的轮询共用同一份数据）

**同意**（idle 耗时 + 拷贝前后三个子队列的 cell 与 target）。最省字段 = **每个子队列一组三元组**：

```c
#define KLOG_SUBQ(i) \
   mesa_logi("  subq%u: cell %" PRIu64 " / target %" PRIu64 " / emitted %" PRIu64, \
             (i), (uint64_t)*cell[i], (uint64_t)(gq->subqueues[i].kbase.emitted_jobs + 1), \
             (uint64_t)gq->subqueues[i].kbase.emitted_jobs)
   /* present 里：idle 前打一次（SEQNO before）→ DeviceWaitIdle → idle 后打一次
    * （SEQNO after）→ R2 的等待循环 → 拷贝 → 打第三次（SEQNO copied） */
```
**字段就这三个（cell / target / emitted_jobs）× 三个子队列 × 三个时点**，另外加 `idle_us` 与 `queue_count`（§2 的 #2 判据：遍历到的队列数是否 ≥1、是否包含 app 用的那个）。
**判读矩阵（与 67 号 §5 一致，这里补上"三次打点"的用法）**：

| idle 后 | R2 等待后 | 结论 |
|---|---|---|
| `cell >= target` 全部成立 | 立即通过 | **W2**：GPU 早已空闲 ⇒ 火力转"呈现索引"（32:1 倾斜） |
| `cell < target` | 变成 `>= target`（等到了） | **W1 坐实 + R2 有效**（idle 没等，我们的等待补上了） |
| `cell < target` | 50 ms 后仍 `< target` | 两种可能：① 该队列根本没有在飞工作（target 算错/`emitted_jobs` 语义不符）；② GPU 真卡住（此时与卡死族合并分析） |
| `queue_count == 0` 或遍历不到 app 的队列 | — | **#2 成立**：`DeviceWaitIdle` 遍历的队列集合不含应用实际用的队列 ⇒ R1 的修法要连带修队列注册 |

---

## 5. 未验证 / 限制（诚实清单）

1. 全部为**代码级**分析；R1/R2 **未编译、未上机**。
2. **R1 的精确 diff 还差三处**（§1 的三条 grep）：`kbase_target_seqnos` 的填充点、`panvk_kbase_sync_set_pending()` 的 pending→signaled 判定、`kbase_wait_sync_targets` 的语义。**我不在没有这三处的情况下写死 diff**（否则就是猜）。
3. R2 的 diff 里有**两个待确认符号**：`dev->queues[]`/`dev->queue_count` 的真实拼写（照 `vk_common_DeviceWaitIdle` 抄即可）、`kbase_subqueue_seqno_cell()` 的链接性（若为 static 需加薄封装）。
4. §4 的"target = `emitted_jobs + 1`"沿用 `:1498` 的现有算法；**其正确性依赖 `emitted_jobs` 的语义**（`:1625` 在作业发射后自增）——若在飞作业尚未自增，target 会少 1（表现为"idle 后 cell 已 ≥ target 但仍可能未完成"）⇒ 这一点请与 R2 的超时日志一起看。
5. 我**没有**核对 `vk_common_QueueWaitIdle` 在**没有渲染 work 的空提交**下 `signal_count` 与 `kbase_target_seqnos` 的实际取值（这是 §1 三处待查的第①条）——头号机制成立与否，取决于那一条。
