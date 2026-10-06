# 65 — 停滞探测器修法（确认）+ **M7' 命中**（WSI acquire/present 的两个同步缺陷）+ bug 3 终局候选表

日期：2026-10-06 · 只读分析 · 行号基线 = 我读到的当前树
配套：`64-perjob-syncobj-sampling.md`（按作业采样）· `61d`（形态学）· `61e`（IR 出局）

---

## 1. 停滞探测器修法：**你的方案正确**，但不必新增计数器

**先确认关键事实**：每子队列的作业计数**已经存在**——`subq->kbase.emitted_jobs`（`:1445` 取 `target_seqno = emitted_jobs + 1`，`:1625` `emitted_jobs++`），且超时快照**已经在打印它**：`kbase_log_subqueue_state()` 的 `jobs %" PRIu64` 字段（`:1100-1102`，打印的就是 `subq->kbase.emitted_jobs`），而超时路径对**全部三个子队列**调用它（`：for i … kbase_log_subqueue_state(queue, i, "timeout snapshot")`）。
⇒ **"卡死时停在哪个作业号"现在就能看到**（subqueue 0 那行的 `jobs`）⇒ 你那个函数内 static 计数器是**冗余**的。

**真正缺的是"这个作业的 +1 落地了没有"**。修法（把你"连续 16 个作业不前进"的**轮询式**判据换成**发射式**判据）：

```c
/* 文件作用域（每个子队列一份），由"每次作业发射"更新，而不是由轮询更新：
 * 卡死后不再有作业 ⇒ 轮询永远等不到，但发射式记录已经留下了最后状态。 */
static struct {
   uint64_t seqno[PANVK_SUBQUEUE_COUNT];      /* 上次采样值 */
   uint64_t advanced_at_job[PANVK_SUBQUEUE_COUNT]; /* 它最后一次变化的 VT 作业号 */
   uint64_t samples;
} kbase_syncobj_track;
```
- **更新点**：§64 的 `kbase_log_syncobjs_per_job()` 里（作业发射时，`target_seqno` 已知）：对每个 j，若 `so[j].seqno != seqno[j]` ⇒ `seqno[j] = so[j].seqno; advanced_at_job[j] = target_seqno;`
- **打印点**：超时快照（与 `kbase_log_queue_syncobjs(queue)` 同一处，`:1890` 附近）新增一行：
```c
   mesa_loge("kbase: syncobj advance trace: s0 %" PRIu64 " @ job %" PRIu64
             " | s1 %" PRIu64 " @ job %" PRIu64
             " | s2 %" PRIu64 " @ job %" PRIu64 " (VT emitted_jobs %" PRIu64 ")",
             so[0].seqno, kbase_syncobj_track.advanced_at_job[0],
             so[1].seqno, kbase_syncobj_track.advanced_at_job[1],
             so[2].seqno, kbase_syncobj_track.advanced_at_job[2],
             subq0->kbase.emitted_jobs);
```
**要打印的字段（最小集，且与现有快照逐项对齐）**：

| 字段 | 来源 | 与现有行的对齐 |
|---|---|---|
| 三个 syncobj 的当前值 | 已有 `kbase_log_queue_syncobjs()` | 直接对账 |
| **每个 syncobj "最后一次变化发生在哪个作业"** | 新增（上面的 `advanced_at_job[]`） | 与 subqueue 0 行的 `jobs`（= 在飞作业 target）相减 ⇒ **差值 = 停滞了多少个作业** |
| 每子队列 `emitted_jobs` | **已有**（`kbase_log_subqueue_state` 的 `jobs`） | 无需新增 |
| 在飞作业的 `seqno/ls_copy/target/marks/insert/extract` | **已有**（subqueue 行 + ENTRY 解码） | 停滞作业的 ckpt/marks 直接读那一行 |
| 呈现侧 `image_index` + 每图呈现计数 | **新增**（见 §2） | 用于 M7' |

**判读**：`advanced_at_job[0] = 623`、subqueue0 `jobs = 625` ⇒ **624、625 两个作业的 +1 都没落地** ⇒ 靶心是"这两个作业为何写不进去"；若 `advanced_at_job[0] = 625`（最后一个作业）⇒ 只是"最后一个还没到" ⇒ 需要再看一次快照才下结论。

---

## 2. ★ M7' 命中：WSI 的 acquire/present 有**两个**实质同步缺陷（file:line 已核）

```c
static VkResult
panvk_android_swapchain_acquire_next_image(..., const VkAcquireNextImageInfoKHR *info,
                                          uint32_t *image_index)      /* panvk_wsi.c:222-231 */
{
    uint32_t idx = chain->next_image;
    chain->next_image = (chain->next_image + 1) % chain->base.image_count;
    *image_index = idx;
    return VK_SUCCESS;                    /* ← 不 signal info->semaphore，也不 signal fence */
}

static VkResult
panvk_android_swapchain_queue_present(..., uint32_t image_index, ...)  /* panvk_wsi.c:260-308 */
{
   ...
   dev->dispatch_table.DeviceWaitIdle(swapchain->device);   /* ← 唯一的栅栏；pWaitSemaphores 完全没用 */
   if (chain->window && chain->images[image_index].ahb) { ANativeWindow_lock … CPU 逐行拷贝 … unlockAndPost }
}
```
**缺陷 A（acquire 侧）**：`info` 参数**从头到尾没被使用** ⇒ `vkAcquireNextImageKHR` 的 `pSemaphore`/`pFence` **既不被 signal 也不被等待**，且**永远返回 `VK_SUCCESS`**（永不 `VK_TIMEOUT`/`VK_SUBOPTIMAL_KHR`）。⇒ 应用的"取到图再渲染"这条同步链**整体失效**；应用以为图空闲了，其实上一帧的 CPU 拷贝可能还在进行。
**缺陷 B（present 侧）**：`pWaitSemaphores` **完全没被消费**⇒ 唯一保证是 `DeviceWaitIdle`。若该 idle 在本 fork 的 kbase 路径上不是可靠的全队列栅栏（本树的等待/排空路径被大改过），或应用在 idle 与拷贝之间由**另一线程**提交了工作 ⇒ **CPU 可能拷到"渲染到一半"的图像**。
**与四个事实的吻合度**：

| 事实 | M7' 解释 | 吻合 |
|---|---|---|
| 轻载探针像素正确 | 探针是自己写的单图、单次 present、无并发提交 ⇒ 两个缺陷都不触发 | ✅ |
| 重载游戏坏 | 帧率低、队列深、拷贝耗时大 ⇒ A/B 的窗口被放大 | ✅ |
| 有时全黑 | 呈现了"只被 clear、渲染还没写进去"的那张图（3 张图轮转 ⇒ 有 1/3 概率撞上） | ✅ |
| 有时面板/重复 | 拷贝到"部分渲染"的图：已写的区域是新的、未写的区域是**上一帧的旧内容** ⇒ 直边分界 + 内容重复（新旧同场景相似） | ✅✅ |

**最小插桩（你提的方案 + 两点补强，一次运行可判）**：
```c
/* 新增到 struct panvk_android_swapchain（panvk_wsi.c:60-80 一带）：
 *   uint64_t present_count[MAX_SWAPCHAIN_IMAGES];  uint64_t acquire_count;  */
   /* acquire_next_image() 里：chain->acquire_count++; */
   /* queue_present() 里：chain->present_count[image_index]++;            */
   /*   并在 DeviceWaitIdle 之前/之后各打一次时间戳，写出 idle 实际耗时：   */
   mesa_loge("kbase: present #%" PRIu64 " image_index %u (per-image %" PRIu64
             "/%" PRIu64 "/%" PRIu64 ") waitsem %u fence %u idle_us %" PRIu64,
             chain->acquire_count, image_index,
             chain->present_count[0], chain->present_count[1], chain->present_count[2],
             pPresentInfo->waitSemaphoreCount, pPresentInfo->pNext ? 1u : 0u, idle_ns / 1000);
```
**判读（全部零代码可读）**：
1. **每图呈现计数严重不均**（例如 image 0 占 90%）⇒ 应用没按轮转取图 ⇒ **典型 stale/黑**根源；
2. **`waitsem == 1` 而我们从不用它** ⇒ 缺陷 B 坐实（配合下面 A/B）；
3. **`idle_us` 极小（<100 µs）而 FPS 很低** ⇒ `DeviceWaitIdle` **没有真的在等**（重载下渲染必然在飞）⇒ **缺陷 B 的可靠性问题坐实**；
4. **A/B 实验（单变量、零代码）**：在 `DeviceWaitIdle` 之后加一次 `usleep(3000)` 再拷贝（或连做两次 idle）——**若面板/黑消失或显著减少 ⇒ M7' 成立**（这是最便宜的决定性实验）。

---

## 3. bug 3「终局候选表」：在全部既有排除之后仍能产生该形态的机制

（已排除：pitch · AFBC · 从未被写 · 管线整体 · M1 fb extent · M2 layer/td · IR 三趟）

| # | 机制 | 落点（file:line） | 轻载探针对 / 重载坏 / 有时全黑 / 有时面板 | 一次运行的判别实验 |
|---|---|---|---|---|
| **M7'** | **acquire/present 同步失效** ⇒ 呈现"未渲染完/已 clear"的图 | `panvk_wsi.c:222-231`（acquire 忽略 `info`、恒 SUCCESS）、`:260-308`（present 忽略 `pWaitSemaphores`、只靠 idle） | ✅/✅/✅/✅（**四格全中**） | §2 的三条日志 + `usleep` A/B |
| **M7''** | **`DeviceWaitIdle` 在 kbase 路径上不可靠**（不等真栅栏） | 同上（present 唯一栅栏）；idle 的等待路径在本 fork 被大改 | ✅/✅/✅/✅ | 打印 `idle` 耗时；`usleep` A/B |
| **M8** | **应用只渲染子矩形/部分图层**（MobileGL 的 framebuffer 仿真、blit rect 算错），其余留在旧内容 | 不在本树（MobileGL 侧）；本树只负责整幅拷贝 | ✅（探针不涉及）/✅/➖/✅ | 相机平移：**面板边界是否随内容移动**；换非 MobileGL 的 VK 应用 |
| **M9** | **present 发生在"本帧提交尚未完成"时**（多线程/多队列提交） | `panvk_wsi.c:272`（idle 只覆盖"此刻已提交"的工作） | ✅/✅/✅/✅ | 打印 present 时刻"在飞提交数"；`usleep` A/B |
| **M10** | **`image_index` 跨 swapchain 世代陈旧/越界**（重建后旧索引） | `panvk_wsi.c:268`（有范围检查，但没有"世代"概念）；`next_image` 在 `:370` 初始化为 0 | ✅/✅/✅/➖ | §2 的 per-image 计数 + 重建 swapchain 时的日志 |
| **M11** | **应用的 acquire/render/present 顺序被打乱**（因为我们恒 SUCCESS、不 signal 信号量） | 同 M7' 缺陷 A | ✅/✅/✅/➖ | 与 `waitsem` 日志共同判读 |
| **M12** | **"面板"本身就是应用的合成**（letterbox/HUD/分屏/画中画） | 不在本树 | ✅/✅/➖/✅ | 相机平移看边界是否锁屏幕；0 成本 |
| **M13** | **CRC / 事务消除**把错误的块判为"未变" | `PANVK_DEBUG=no_crc`（`panvk_instance.h:37`） | ✅/✅/➖/✅ | `PANVK_DEBUG=no_crc` 一局对照（零代码） |
| **M14** | **tiler OOM / IR 路径**（曾排除，但 v99 卡死发生在 256–512 作业 ⇒ 重载） | `61e`；`tiler_oom.counter` | ✅/✅/✅/✅（但 v84d/v91 实测 counter=0） | 面板帧上再读一次 `tiler_oom.counter`（坏帧必须 ≠0 才成立） |
| **M15** | **`AHardwareBuffer_lock` 与 GPU 写入并发**（present 期间 GPU 仍在写同一张 AHB） | `panvk_wsi.c:296`（lock 用 `CPU_READ_OFTEN`，不排除并发写） | ✅/✅/✅/✅ | 与 M7' 同一实验（`usleep` A/B）即覆盖 |

**排序建议**：**M7' / M7'' / M9 / M15 是同一族（present 与渲染的同步）**，四格全中且**与"强随机"（v98 32 万作业不卡、v99 256–512 就卡）完全一致**（竞态）。⇒ **先做 §2 的日志 + `usleep` A/B**；若有效，再考虑把 acquire/present 的同步补全（signal 信号量 + 真正等待），那是**根因级修复**但改动面在 WSI（中风险，单变量上机）。

---

## 4. 未验证 / 限制

1. 全部为**代码级**分析；**未做设备实验**（§1/§2 的代码未编译，按纪律不落地）。
2. **`DeviceWaitIdle` 在本 fork 是否可靠未验证**（M7''）——需要 §2 的 `idle_us` 读数或 A/B 才能定；这是 M7' 族里唯一需要实测才能定的子项。
3. 我**未逐行核对** `queue_present` 是否消费了 `pPresentInfo` 的其它字段（我只确认了它不用 `pWaitSemaphores`、只用 `image_index`）；也**未确认** `AcquireNextImageKHR` 的 `info` 是否在上层 `wsi_common` 里被用到（若上层用了，则缺陷 A 只在 `image_index` 层面成立，需一并核对）。
4. M8/M12 在 MobileGL 侧，我**给不出 file:line**（不在本工作区）。
5. §1 的 `advanced_at_job[]` 依赖"作业发射时更新"这一点：若某条 early-return 在 `:1445` 之前，个别作业不会被记录（对定位停滞起点影响有限，但要在注释里写明）。
