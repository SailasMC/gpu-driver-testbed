# 67 — `DeviceWaitIdle` 只用 12 µs：先分清"没等"与"本来就空闲"，再修

日期：2026-10-06 · 只读分析 · 行号基线 = 我读到的当前树
配套：`66-shear-reconcile.md`、`66b-shear-probe-delivery.md`、`65-m7prime-and-endgame.md`

---

## 0. 先把 D3 结案（你要求的记账）

`--mode=shear-win` **PASS（exact 2566080/2566080）** ⇒ **写侧 pitch（`win_buf.stride*4 = 9536`）与显示侧 stride（`AImage_getPlaneRowStride`）在真尺寸下一致** ⇒ **D3 出局** ✓（同时 **D1 早已被代码链压到 0**、**D2 的 4× 量级也不成立**）⇒ 与你的结论一致：**几何/布局全部清白，剩下的是"并发与呈现时序"这一族（M7'/M9/D6）**。
⚠️ 唯一保留：探针的 win 路径是**单次、无并发**的（结构上不可能撕裂），所以它证明的是**几何一致**，**不是**"游戏那种深队列下的时序正确"。

---

## 1. 最要紧的一条：**12 µs 本身不能证明"没等"**

`DeviceWaitIdle` 只花 12 µs 有两种解释，**只看耗时分不开**：

| 解释 | 机制 | 是否与现场吻合 |
|---|---|---|
| **W1「对象解析错/实现是空的」** | `dispatch_table.DeviceWaitIdle` 没落到我们的实现（生成表/通用助手/空实现）⇒ 立即返回 | 若成立，任何 present 都不等 GPU ⇒ **深队列下更容易撕裂** ✓ |
| **W2「GPU 本来就空闲」** | 应用（ZL2/MobileGL）在调用 present 前**已经 `vkWaitForFences` 等过自己的提交** ⇒ present 时 GPU 确实已完成 ⇒ idle 立即返回是**正确**的 | 若成立，**撕裂的原因不在 present 的栅栏**，而在**呈现的到底是哪张图**（见 §4 的 32:1 倾斜）✓ |

⇒ **判据只能是"效果"而不是时间**：**idle 返回后，三个子队列的 SEQNO cell 是否已经到达各自 target**。这正是你提的插桩 ✓（§5），我完全同意并给了字段表。

---

## 2. 我们树里 `DeviceWaitIdle` 的实现路径（已核对的部分 + 一处重要发现）

**已核实**：在 `src/panfrost/vulkan/` 全树（`*.c` + `csf/*.c`）里 grep `DeviceWaitIdle`，**只命中 `panvk_wsi.c` 的那一处调用与它的注释**（`:274/:284/:291`）⇒ **panfrost Vulkan 树里没有名为 `DeviceWaitIdle` 的实现**。
**这意味着什么（两种可能，必须再查一步才能定）**：
- **(a) 入口点是生成/宏拼出来的**（Mesa 的 `vk_device_dispatch_table` 常由注册表/宏生成，字面量不会出现在 `.c` 里）⇒ 实现可能仍在树内（例如 `panvk_vX_device.c` 里以宏形式给出），只是我的字面 grep 抓不到；
- **(b) 它根本没有被 panvk 覆盖** ⇒ 落到**通用/默认实现**（Mesa 运行时有 `vk_common_DeviceWaitIdle` 之类的通用助手，其语义是"遍历设备的队列并 `vk_queue_wait_idle`"）⇒ **若我们的 fork 没有把队列挂进 `dev->queues`（或队列的 idle 回调为空），遍历就是空操作 ⇒ 立即返回** ✓✓ **这一条能直接解释 12 µs**。

**下一步（三条只读命令，几秒）**：
```bash
M=/root/zenithblue/work/mesa
grep -rn "DeviceWaitIdle" $M/src/vulkan/runtime/*.c $M/src/vulkan/runtime/*.h | head -20
grep -rn "DeviceWaitIdle\|device_dispatch_table\|vk_device_dispatch_table" $M/src/panfrost/vulkan/*.c | head -20
grep -rn "queue_wait_idle\|wait_idle\|vk_queue_init\|dev->queues\|queue_count" $M/src/panfrost/vulkan/panvk_vX_queue.c | head -30
```
**判读**：若第 3 条显示 panvk 的 `queue_wait_idle` 指向一个真正等 CS SEQNO 的函数，而第 1/2 条显示入口点确实解析到了它 ⇒ 转 **W2**（→ §4 的呈现索引问题）；否则 ⇒ **W1 成立**，走 §3-(c)。

**关于"是否只等 CS SEQNO 而不等 tiler/FRAG"**：这条我**没能确证**（`panvk_vX_queue.c` 里没有 `wait_idle` 字面量命中，说明队列 idle 实现可能在别处或以宏形式存在）。但有一条**语义上的**判断可以先说：CS SEQNO（`cell+0`）由 wrapper 的**完成 add** 推进，而该 add 位于 wrapper 的**全槽等待之后**（`panvk_vX_gpu_queue.c`：`44 → cs_wait_slots(all_mask) → 45 → 42 → add`）⇒ **SEQNO 到达 target 本身就蕴含"该 entry 的所有被等待槽都已退休"**，包括 FRAG/tiler 的异步 op（只要它们的槽 ∈ `all_mask`）。⇒ **等 SEQNO 在语义上是"够强"的**；这反过来说明：**如果 idle 真的在等 SEQNO，它不可能 12 µs 返回**（除了 §1 的 W2）。

---

## 3. 最小修复候选（按风险排序）

| 候选 | 内容 | 风险 | 说明 |
|---|---|---|---|
| **(b) present 侧加"等 SEQNO 到位"的显式栅栏** | 在 `queue_present` 拷贝前，对**每个子队列**用现成设施等到 `cell->seqno >= 该队列最后提交的 target`（`kbase_subqueue_wait_seqno(queue, i, target, …)`，`csf/panvk_vX_gpu_queue.c:1737`；target 取 `subq->kbase.emitted_jobs`） | **低-中** | 与 `DeviceWaitIdle` 想做但可能没做的事**等价**，且**不依赖** dispatch_table 解析是否正确 ⇒ **W1/W2 两种情况下都安全**（W2 下它立即通过，成本可忽略）。这是**我推荐先做的** |
| **(c) 修 `DeviceWaitIdle` 本身** | 查出它是空/通用实现，补成"遍历所有队列 → 等 SEQNO 到位" | **中** | 一次修好全进程受益（所有应用路径）；但**改动面在公共运行时的入口点解析**，需单变量上机 |
| **(a) 等"应用最后提交的 queue fence"** | present 里等应用自己的 fence | **不可行** | `queue_present` **拿不到**应用的 queue/提交句柄（我们的 WSI 签名里没有 wait-semaphore 参数，已在 65 号记录）⇒ 只能走 (b)/(c) |
| **(d) 结构性：改用 Vulkan 公共 WSI 路径** | 让 `wsi_common` 处理 acquire/present 的信号量与 fence（我们当前是自定义 WSI，绕过了它） | **高** | 一次修掉缺陷 A/B（acquire 不 signal、present 不等信号量），但属于 WSI 重写 |

**为什么 (b) 的风险边界可控**：它**只增加等待、不改变任何发射/同步语义** ⇒ 只可能让 present **更晚**发生，不可能提前 ⇒ **不会引入"消费者提前放行"型的数据竞争**（与 62b 的"提前放行"检验同一套逻辑）。

---

## 4. 那 32:1 的图索引倾斜：它可能与 12 µs 是**同一个病**

v101：`image_index=2` 出现 32 次、`image_index=0` 出现 1 次；而我们的 `acquire_next_image` 是**自增轮转**（`panvk_wsi.c:222-231`）⇒ 应用的**取图序列**与**呈现序列**不一致：
- 若应用**忽略**我们返回的 index、固定呈现某一张 ⇒ **present 拷贝的那张图可能正在被渲染/尚未渲染** ⇒ **面板/黑/重复**（M7' 族）✓ 且**与负载相关** ✓；
- 若应用**取图多次只呈现一张**（或取到的图被复用）⇒ 同样效果。
⇒ **这一条不依赖 `DeviceWaitIdle` 是否坏**，也**不依赖** 12 µs 的解释 ⇒ **我建议把它与 §3-(b) 作为"两个独立的最小改动"分别上机**（一次一个变量）。

---

## 5. 插桩：确认你的方案 + 字段表（一次运行）

**同意你的方案**（present 里打印 idle 耗时 + 拷贝前后三个 SEQNO cell），补两点：

```c
   /* present 前 */
   for (i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {   /* 宿主侧读 cell：与快照同法 */
       target[i] = subq[i]->kbase.emitted_jobs;   /* 该队列最后提交的作业号 */
       seqno_before[i] = cell[i]->seqno;
   }
   idle_ns = now();  dev->dispatch_table.DeviceWaitIdle(swapchain->device);  idle_ns = now() - idle_ns;
   for (i…) seqno_after[i] = cell[i]->seqno;
   mesa_loge("kbase: present #%llu image_index=%u waitsem=%u fence=%u idle_us=%llu | "
             "VT %llu/%llu→%llu  FRAG %llu/%llu→%llu  COMP %llu/%llu→%llu",
             present_counter, image_index, pPresentInfo->waitSemaphoreCount,
             pPresentInfo->pFence ? 1 : 0, idle_ns / 1000,
             seqno_before[0], target[0], seqno_after[0],
             seqno_before[1], target[1], seqno_after[1],
             seqno_before[2], target[2], seqno_after[2]);
```

| 观测 | 结论 |
|---|---|
| `idle_us` 12–25 **且** `seqno_after[i] >= target[i]` 全部成立 | **W2**：GPU 早已空闲 ⇒ present 的栅栏没问题 ⇒ 火力全转 **§4（呈现索引/应用侧）** |
| `idle_us` 小 **但** 有 `seqno_after[i] < target[i]` | **W1 坐实**：idle 没等到真东西 ⇒ 上 §3-(b)（然后 (c)） |
| `idle_us` 大（≈帧时间） | idle 确实在工作 ⇒ 也算 W2 的变体（只是它在等） |
| `waitsem=1` 而 `fence=0`（或反之） | 与 65 号的缺陷 A/B 对照，确认应用确实给了信号量而我们忽略 |
| 把 §4 的"每图 acquire/present 计数"一起打 | 只要两个计数**不相等**（或某图呈现数远超取图数）⇒ **呈现在拷一张没被渲染/正在被渲染的图**（独立于 idle 的第二个根因） |

---

## 6. 未验证 / 限制（诚实清单）

1. 全部为**代码级 + 你的实测日志**推断；本轮**未做设备实验**。
2. **`DeviceWaitIdle` 的实际解析目标我没能确定**（§2）：panfrost 树里没有该名字的实现 ⇒ 生成/宏/通用助手三种可能都在；给出三条 grep 是**下一步唯一的入口**，我不猜。
3. **队列 idle 的实现位置没找到**（`panvk_vX_queue.c` 里无 `wait_idle` 命中）⇒ "是否只等 CS SEQNO"我只给了**语义论证**（SEQNO 蕴含全槽退休 ⇒ 语义够强），**未逐行核对**。
4. §1 的 W1/W2 分离**必须靠 §5 的 cell 读数**；`12 µs` 本身不可作为证据。
5. §4 的"应用忽略 index"是**推断**（依据是 32:1 倾斜 vs 我们的自增轮转）；要坐实需要应用侧或"每图取/呈计数"的对照读数。
6. 所有修复候选**未编译、未上机**（按纪律只出提案）。
