# 34 · 内核/固件契约家族审计（kbase uAPI 1.21 × PanVK kbase 后端）

> **对象**：OPPO PHZ110 / MT6989 / Immortalis-G720 MC12 / Android 16 / kbase CSF **uAPI 1.21**。
> **构建树**：`/root/zenithblue/work/mesa`（**只读**，本轮一行未改；读数时树已含 v66 诊断改动）。
> **纪律**：未构建、未操作手机、未改任何只读树、未 git 操作。本轮唯一写入 = 本文件。
> **标注**：【已定论】= 有 file:line 或内核/DDK 源码支撑；【推断】= 由已定论事实演绎；【未验证】= 无直接证据。
> **权威对照物**（服务器上已有，本轮只读）：
> * DDK uAPI 头：`/root/research/23-work/precedent/include/csf/mali_kbase_csf_ioctl.h`（r44p0 时代，含逐字段注释）
> * DDK 头副本：`<tree>/src/panfrost/lib/kmod/mali_base_csf_kernel.h`、`mali_kbase_csf_registers.h`
> * **内核真源码**：`/root/research/tiler-work/ref-r43p0-mali_kbase_csf.c`（r43p0）、`/root/research/csf-work/pub-kbase/csf.c`（ARM 2018-2024 版，含 `init_user_io_pages` 与 CSG SYNC_UPDATE 处理）

---

## 0. 一页结论（先读这段）

1. **CS_EXTRACT_INIT 不写不是违规，也不可能是本次挂起的原因。**【已定论，三条否证见 §3】
   内核自己在 queue 页分配时就把它写成 0（`ref-r43p0-mali_kbase_csf.c:170-185` `init_user_io_pages()`；
   2024 版同源：`pub-kbase/csf.c:166-181`），此后**内核再不改它**（全 ref 只此一处）。
   DDK 寄存器头的语义是 **"Initial extract offset for ring buffer"**（`mali_kbase_csf_registers.h:139-140`），
   即"CS 被（重新）启动时的起始 extract"，不是"当前消费进度回写"。我们从不写 ⇒ 值恒为 0；
   **显式写 0 与现状逐位等价**，不可能把一条停在 CALL 的 CS 推起来。
   v66 实测 `extract` 从未回 0（88096 / 88096 …），也证明本轮**没有发生 CS 重启** ⇒ 该字段没被固件重新读取。

2. **v66 证据把机制改写成：「固件进了被 CALL 的流并在流内停车」，而不是「ring 消费进度回写失败」。**【已定论】
   sq0 的 marks `pre_call/post_call/post_wait` 三跳全推进 + `FINISH_BEFORE_WAIT/FINISH_AFTER_WAIT/CMDBUF_DONE`
   全置位 ⇒ **VT 的 ring entry 连同 CALL 之后的完成写都执行完了**（它不欠 extract）。
   sq1/sq2 的 `post_call` 之后不再推进 ⇒ 它们**停在 callee 内部**；此时 ring 侧 `CS_EXTRACT` 天然停在 CALL
   （提取指针在 ring 地址空间里的位置没动），`extract < insert` 只是**后果**，不是故障。
   ⇒ **「工作已完成但 extract 不前进」这个前提对 sq1/sq2 不成立**（它们的工作就是没完成）。

3. **sq1/sq2 为什么连 FINISH 位都没有 —— 位图本身不完整，但对 COMPUTE 仍给出强定位。**【已定论】
   * `PANVK_KBASE_PROGRESS_VT_*`（bit1..6，`panvk_cmd_buffer.h:64-69`）与
     `PANVK_KBASE_PROGRESS_FRAG_*`（bit1..6，`:71-76`）**全树没有任何发射点**（grep 只命中枚举与名字表）
     ⇒ **sq1(FRAGMENT) 的「只有 bit0」不含位置信息**，不能据此说它"卡在流首"。
   * `PANVK_KBASE_PROGRESS_COMPUTE_ENTER`（bit1）**有发射点**：`cmd_dispatch.c:218-220`，就在**第一次 dispatch 之前**。
     v66 实测 COMPUTE **bit1 = 0** ⇒ COMPUTE 被**证明**卡在"进流之后、第一次 dispatch 之前"的**同步前导**里
     （`emit_barrier_csf()` 的跨子队列 `SYNC_WAIT`，`cmd_buffer.c:600-621`）。

4. **最可能的内核/固件契约错配：三个 subqueue 分属三个 queue group。**【推断，高】
   PanVK 的跨子队列会合**读的是"另一个 subqueue 的 progress-seqno 寄存器"**
   （`cs_progress_seqno_reg()`，`panvk_cmd_buffer.h:427-432`，arch≥12 用 **r116..r121**；
   用在 `cmd_draw.c:4256-4262` 的 VT→FRAG 会合、`cmd_buffer.c:616` 的 barrier 会合），
   而**我们自己**的 wrapper 注释已经写明 kbase 上"CS 寄存器状态不跨 kick 保持"，所以才要在**每条 ring entry**
   重写 subqueue-ctx 寄存器（`gpu_queue.c:855-859`）——**但 r116..r121 没有任何地方重写或初始化**。
   三个 CS 各自一个 group（`panvk_queue.h:71-75` 的注释 + `gpu_queue.c:1381-1389` 每次循环
   `kbase_kmod_csf_group_create(dev->kmod.dev, 1, ...)`）
   ⇒ 对端寄存器槽在**本 CS 的寄存器文件里根本不被对端更新**（组内只有 1 个 CS）⇒ 会合目标值来自残留/外来内容
   ⇒ 一旦目标值高于对端**将来**会 signal 的值，等待方**永久停车**（无 fault、无通知、extract 停在 CALL）。
   sq0(VT) 的流首不依赖别的 subqueue ⇒ 它跑完，与 v66 完全自洽。panthor 的正常模型是**一个 group 三个 CS**。

5. **`error 0x0` 的含义（回答任务问题 2）**：本机在协议上**拿不到** per-CS fault 报告
   （`cs_fault_report_enable` 只在 112 B 布局 = uAPI ≥1.25，`mali_kbase_ioctl.h:458-484`；1.18 的 40 B 布局
   `:433-454` 无此字段），而**内核事件通道我们是在读的**（`kbase_kmod.c:828-873` poll+read），
   它只会推 FATAL / QUEUE_FATAL / TIMEOUT / TILER_HEAP_OOM / QUEUE_ERROR_FAULT（`:681-726`），本轮**一条都没有**
   ⇒ 内核视角"组没出错"。同时 `CS_FAULT/CS_FATAL` 那一族**根本不在 uAPI 契约里**（见 §1.3，v66 实测全 0）
   ⇒ **`error 0x0` = 我们失明，不是"没有错误"**。【已定论】
   而 `csi_handlers` 恰好会把**唯一还可能出现的 tiler-OOM 通知**变成静默（§2.2）⇒ 候选 2。

---

## 1. 契约面逐项核对（uAPI 1.21）

### 1.1 版本阶梯与布局（nr / 尺寸对照，全部【已定论】）

| ioctl | nr | 我们用的布局 | uAPI 1.21 应走 | 判据 |
|---|---|---|---|---|
| `CS_QUEUE_GROUP_CREATE`（112 B，`cs_fault_report_enable` 在此） | 58 | 仅 ≥1.25 | ✗ 不走 | `kbase_kmod.c:538`；`mali_kbase_ioctl.h:458-484` |
| `CS_QUEUE_GROUP_CREATE_1_18`（40 B，`csi_handlers` 在此） | 58 | **1.18 ≤ v < 1.25** | ✓ **本机走这条** | `kbase_kmod.c:571-591`；`:433-454` |
| `CS_QUEUE_GROUP_CREATE_1_6`（无 `csi_handlers`） | 42 | 兜底 | 只在上面两条都失败时 | `kbase_kmod.c:599-628` |
| `CS_TILER_HEAP_INIT`（32 B，带 `buf_desc_va`） | 48 | 无版本门，直接用 | ✓ 正确（`_1_13` 24 B 只适用于 ≤1.13） | DDK 头 `:481-535` 明写 "earlier version **up to 1.13**" |

* 与版本门相关的一点：**40 B 与 112 B 共用 nr 58，内核靠 `_IOC_SIZE` 区分**（DDK 同时保留两个宏就是为此）。
  只有 1.18 调用**失败**时才会落到 1.6；本轮的判据是 logcat 里
  `kbase: created CSF group %u with TILER_OOM CSI handler (1.18 layout, ioctl 58)`（`kbase_kmod.c:588-589`）。
  **请父代理在下一轮 logcat 里确认这一行存在**——它同时证明 `csi_handlers` 已送达内核（见 §2.2）与候选 2 的 A/B 前提。

### 1.2 已核对**正确**的项（不要浪费实验预算）

| 项 | 结论 | 依据 |
|---|---|---|
| `priority = 0` 是不是写错了（注释说 HIGH） | **没写错**：本 DDK 版本 `BASE_QUEUE_GROUP_PRIORITY_HIGH = 0`（MEDIUM=1, LOW=2, REALTIME=3） | `mali_base_csf_kernel.h:190-196`；内核校验 `priority >= BASE_QUEUE_GROUP_PRIORITY_COUNT` 才 EINVAL（`ref-r43p0-...csf.c:1323-1326`） |
| `cs_min = 队列数` | **语义正确**：`@in.cs_min: Minimum number of CSs required`，内核用 `iface_has_enough_streams(kbdev, cs_min)` 校验 | DDK 头 `:298, 348`；内核 `:1327-1331` |
| `tiler_mask=1 / fragment_mask=~0 / compute_mask=~0` + `tiler_max=1 / fragment_max=64 / compute_max=64` | **合法**：内核要求 `*_max <= hweight64(*_mask)`，我们正好等于 | 内核 `:1303-1319` |
| `CS_QUEUE_REGISTER` 的 `priority=0` | **正确**：队列优先级 0 最大（`BASE_QUEUE_MAX_PRIORITY 15`） | `mali_base_csf_kernel.h:119` |
| `CS_QUEUE_BIND` 用 `csi_index=0`、三页 mmap（doorbell/input/output） | **顺序正确**：uAPI 注释 `page 0: doorbell, page 1: input, page 2: output` | `mali_kbase_ioctl.h:392-401` |
| `CS_QUEUE_KICK` 契约 | **正确**：内核只置 `queue->pending=1` 并排队 `pending_submission_worker() -> kbase_csf_scheduler_queue_start()` | 内核 `:899-936, 769-800` |
| doorbell 快速路径先查 `CS_ACTIVE` | **必需且已做**：CS off-slot 时用户 doorbell 会被重定向到 dummy sink page（"avoiding potential segmentation fault ... when a csi is off slot"） | `pub-kbase/csf.c:3419-3430`；我们 `gpu_queue.c:1046-1053` |
| `MEM_JIT_INIT` 之后才 `CS_TILER_HEAP_INIT` | **顺序正确**（heap 分配走 JIT） | `kbase_kmod.c:1245-1270, 1052-1082` |

### 1.3 output page **只承诺两个字段** —— 页 2 寄存器镜像不是契约【已定论】

* uAPI 原文：`page 2: output page (CS_EXTRACT at 0x0, CS_ACTIVE at 0x8)`（`mali_kbase_ioctl.h:395, 400-401`）。
  **没有**任何一行承诺 CS_STATUS / CS_FAULT / CS_HEAP 会镜像到用户页。
* v66 实测：`CS_STATUS_CMD_PTR 0x0`、`CS_STATUS_WAIT 0x0`、`wait_sync_ptr 0`、`CS_FAULT/CS_FATAL/FAULT_INFO 0`、
  `CS_HEAP vt_start/vt_end/frag_end 0`、raw page2 全 0 ⇒ **该镜像在本机不存在**（或偏移完全不同）。
* ⇒ `kbase_log_callee_diag()`（`gpu_queue.c:576-629`）里那一整段"读页 2 CS 寄存器块"的诊断
  **必须标注为"本机无镜像、读数无意义"**，否则会被误读成"无 fault / 未阻塞"。
  真正可用的 in-band 证据只有三样：**input page 的 CS_INSERT**、**output page 的 CS_EXTRACT / CS_ACTIVE**、
  以及**我们自己写进 GPU 内存的面包屑**（marks / progress bitmap / subqueue ctx）。

---

## 2. 通知与事件面（任务问题 2 的正面回答）

### 2.1 本机（uAPI 1.21）到底有哪些"通知"通道【已定论】

| 通道 | 1.21 可用？ | 我们用了？ | 说明 |
|---|---|---|---|
| `cs_fault_report_enable`（CS_FAULT 上报用户态） | **否**（只在 112 B 布局 = ≥1.25） | 仅 1.25 档设 `=1`（`kbase_kmod.c:551`） | 1.18 的 40 B 布局无此字段（`mali_kbase_ioctl.h:433-454`） |
| 内核事件 `read(fd)` -> `base_csf_notification` | 是 | **是**（`kbase_kmod.c:828-873`） | 只会推 FATAL / QUEUE_FATAL / TIMEOUT / TILER_HEAP_OOM / QUEUE_ERROR_FAULT（`:681-726`）；本轮无一条 |
| `KBASE_IOCTL_CS_EVENT_SIGNAL`（nr 44，无载荷） | 是 | **零调用** | **不是"完成/异常通知开关"**：它是 host 侧事件通知（本项目自己的设备测试注释就写作 "Host-notify of an evicted group"，并明确记 UNPROVEN：`/root/zenithblue/device/csf-event-regression.c:18-20`）。内核自己也会在 CSG SYNC_UPDATE 时调 `kbase_csf_event_signal_cpu_only()` 唤醒 fd 等待者（`pub-kbase/csf.c:3400-3416`）⇒ 属 CPU 唤醒件，与 GPU 完成路径无关 |
| KCPU queue（CQS wait / fence） | 是 | **默认关**（`PANVK_KBASE_KCPU_SYNC=1` 才开，`kbase_kmod.c:1365-1371`） | 完成路径因此是"自旋 + 内核通知" |
| CS 寄存器块镜像（页 2） | **否**（无契约，实测全 0） | 读但无意义 | 见 §1.3 |

### 2.2 `csi_handlers` 的真实效果（"设置后是否改走 app handler 而不发通知"）-> **是**【已定论】

* 内核里 `group->csi_handlers` **只有三处**使用：赋值（`:1234`）、未知 flag 校验（`:1332-1335`）、
  以及 **tiler-OOM 分支**（`:1926-1938`）。**没有任何一处影响 fault / 完成通知。**
* tiler-OOM 分支原文（`ref-r43p0-...csf.c:1926-1938`）：
  `if ((group->csi_handlers & BASE_CSF_TILER_OOM_EXCEPTION_FLAG) && (pending_frag_count == 0) &&`
  `    (err == -ENOMEM || err == -EBUSY)) { new_chunk_ptr = 0; /* 交给应用增量渲染 */ }`
  —— `new_chunk_ptr = 0` 之后**不会**走 `report_tiler_oom_error()`（`:1958-1974`）⇒
  **不再有 `BASE_GPU_QUEUE_GROUP_ERROR_TILER_HEAP_OOM` 通知，`error_type` 也就永远是 0**。
* 而"应用 handler"要求**当时正武装**：PanVK 只在每次 draw 的 VT 等待窗口内
  `cs_set_exception_handler(TILER_OOM)`（`cmd_draw.c:4513-4523`），随后**立刻撤销**
  （`:4529-4539`，注释写明"撤销后异常会**留在 pending**"）。
  ⇒ 组创建时**永久**置位 `csi_handlers` 与 PanVK"按 draw 窗口武装"的模型**不匹配**：
  窗口外到达的 tiler-OOM 会被内核转成"NULL chunk / 增量渲染"，而固件此时没有 handler ⇒ 静默停在原地，
  正好长成我们看到的签名（无 fault、无通知、extract 停住、10 s -> DEVICE_LOST）。
* 两个已跑通先例都**没有**这个字段（报告 23 §5.1/§5.2），且报告 14 §6.3 要求的 `csi_handlers=0` 对照组
  **至今未做过**（报告 23 §3.3(c-2) 的原话）。

---

## 3. `CS_EXTRACT_INIT`（Lead 的 a/b 问）—— 不该写、不是原因

**(a) 该不该写、什么时候写、写什么值**【已定论 + 一处未验证】

* 语义：`CS_EXTRACT_INIT` = "**Initial** extract offset for ring buffer"（`mali_kbase_csf_registers.h:139-140`，
  直接抄自 DDK 寄存器头）；uAPI 位置 = input page +0x8（`mali_kbase_ioctl.h:394, 399`，宏名
  `CS_USER_IO_INPUT_CS_EXTRACT_INIT`）。
* **内核在 queue 页分配时写 0**：`init_user_io_pages()`（`ref-r43p0-...csf.c:170-185`；2024 版
  `pub-kbase/csf.c:166-181`）把 `CS_INSERT / CS_EXTRACT_INIT / CS_EXTRACT / CS_ACTIVE` 全部清零，
  且**全 ref 再无第二处触碰 `CS_EXTRACT_INIT`**（grep 只命中这两份同源代码的同一函数）。
  ⇒ 用户态**唯一的写入时机**是"想让 CS 从非 0 位置重新开始"（ring 复用 / 重启）；
  我们从不重启 CS、ring 每 64 KiB 回绕、`extract` 全程连续 ⇒ **写 0 是正确且必要的全部**，不写等价于 0。
* 上游 **panthor / 主线 Mesa / 本树 panthor 路径**里**找不到任何写 `CS_EXTRACT_INIT` 的代码**
  （panthor 的 ring 由内核拥有：`panthor_sched.c::prepare_job_instrs()` 只发 FLUSH / CALL / WAIT / SYNC_ADD），
  因为 panthor 根本不给用户态这个页。**"给上游确切引用"这一点的诚实答案是：主线没有对应代码可引，
  因为该字段只存在于 kbase 的用户页 ABI 里**——这本身就是"它是 kbase 侧可选初始化字段"的证据。
* 【未验证】：固件是否会在**某类重启**（CSG suspend/resume、PROTM 退出）时重新读取 `CS_EXTRACT_INIT` 作为起点。
  即便会，读到的也是 0 = 从头，而 v66 实测 `extract` 从不回 0 ⇒ **本轮没有发生这类重启** ⇒ 与现病无关。

**(b) 若仍要"契约卫生"地写**：见 **候选 3**（写 0 + 回读日志；明确标注**非修因**）。

**(c) sq1/sq2 为何没有 FINISH 位**：见 §0.3。**位图不完整**（VT_* / FRAG_* 无发射点）是主因；
唯一有信息量的 COMPUTE bit1 缺失，把 COMPUTE 定位在"第一次 dispatch 之前的前导同步"里。
另外注意 `FINISH_BEFORE_WAIT`(28) / `AFTER_WAIT`(29) / `CMDBUF_DONE`(30) 由 `finish_cs()` 写在**流的尾部**
（`cmd_buffer.c:148/151/235`）⇒ 没有它们只说明"没走到那条 command buffer 的尾"，sq0 则有 ⇒ VT 从头走到了尾。

---

## 候选 1（最可能）：一个 queue group、三个 CS（`csi_index = subqueue`）

### 依据

1. PanVK 的跨子队列会合**读对端 subqueue 的 progress-seqno 寄存器**：
   `cs_progress_seqno_reg(b, j)` = `cs_reg64(b, PANVK_CS_REG_PROGRESS_SEQNO_START + j*2)`，
   arch≥12 为 **r116..r121**（`panvk_cmd_buffer.h:427-432`、`:367-368`）。使用点：
   * VT->FRAG 瓦片会合：`cmd_draw.c:4256-4262`（FRAG 流读 VT 的 progress register 算出等待值），
     信号方 `cmd_draw.c:4216-4219`（VT 流 `SYNC_ADD64`）。
   * barrier / 依赖会合：`cmd_buffer.c:600-621`（对每个 j 读 `cs_progress_seqno_reg(b, j)` 后 `SYNC_WAIT`）。
   * COMPUTE 批处理：`cmd_dispatch.c:52-54`。
2. 本树自己的注释已承认 **kbase 上 CS 寄存器状态不跨 kick 保持**——所以才要在**每条 ring entry** 重写
   subqueue-ctx 寄存器（r122:123）：`gpu_queue.c:855-861`
   （"kbase executes userspace-owned ring entries, so restore it at each CALL boundary instead of relying on
   the init stream's register state to survive separate kicks"）。**但 r116..r121 全树没有任何初始化或重写点**
   （`grep cs_progress_seqno_reg` 只有"自增自己"与"读别人"两类用法）。
3. 当前配置把三个 subqueue 放进**三个独立 group**，每 group **1 个 CS、`csi_index=0`**：
   `gpu_queue.c:1381-1389` + `:1434-1437`；设计注释在 `panvk_queue.h:71-75`
   （"each PanVK subqueue owns a CSG and uses CSI0. Some Android kbase stacks accept later CSIs into a
   scheduled CSG but never execute them."）。
   ⇒ 组内只有自己一个 CS：**对端寄存器槽在本 CS 的寄存器文件里无人更新** ⇒ 会合值来自残留 / 外来内容。
4. 内核侧契约支持"一个 group 多 CS"：`cs_min` = "Minimum number of **CSs** required"
   （DDK 头 `:298`），内核用 `iface_has_enough_streams()` 校验（`:1327-1331`），
   `bind->in.csi_index >= max_streams`（`groups[0].stream_num`）才拒（`:700-703`）。
   panthor 路径就是**一个 group 三个 queue**（`gpu_queue.c:2291-2327` 的 `qc[3]`）。
5. 与 v66 现象自洽：sq0(VT) 流首不依赖对端 ⇒ 跑完；sq1/sq2 的流首 / 前导就是要等其他 subqueue
   （FRAG：`wait_finish_tiling()` 于 `cmd_draw.c:4527` 被调用；COMPUTE：`cmd_dispatch.c:218` 之前的 barrier 前导）
   ⇒ 永久停车、无 fault、无通知、ring 侧 extract 停在 CALL。

### diff（单文件，抄自 v66 树；`git apply -p1 --recount` 可直接用）

```diff
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ -1346,19 +1346,24 @@
 static void
 kbase_destroy_group(struct panvk_gpu_queue *queue)
 {
    struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
+   /* All subqueues share one group handle (see kbase_create_group). */
+   const uint32_t group = queue->subqueues[0].kbase.group_handle;
 
    for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
       struct panvk_subqueue *subq = &queue->subqueues[i];
 
       if (subq->kbase.user_io)
          kbase_kmod_csf_queue_term(dev->kmod.dev, subq->kbase.ringbuf_dev,
                                    subq->kbase.user_io);
 
       pan_kmod_bo_put(subq->kbase.ringbuf_bo);
       subq->kbase.ringbuf_bo = NULL;
       subq->kbase.user_io = NULL;
+      subq->kbase.group_handle = UINT32_MAX;
+   }
 
-      if (subq->kbase.group_handle != UINT32_MAX) {
-         kbase_kmod_csf_group_destroy(dev->kmod.dev,
-                                      subq->kbase.group_handle);
-         subq->kbase.group_handle = UINT32_MAX;
-      }
-   }
+   if (group != UINT32_MAX)
+      kbase_kmod_csf_group_destroy(dev->kmod.dev, group);
 }
 
 static VkResult
@@ -1371,19 +1376,28 @@
 kbase_create_group(struct panvk_gpu_queue *queue)
 {
    struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
    VkResult result;
+   uint32_t group_handle = UINT32_MAX;
 
    queue->group_handle = UINT32_MAX;
 
    for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
       queue->subqueues[i].kbase.group_handle = UINT32_MAX;
 
+   /* One group, one CS per subqueue.  PanVK's cross-subqueue rendezvous reads
+    * the peer subqueue's progress seqno *register* (cs_progress_seqno_reg(),
+    * panvk_cmd_buffer.h:427-432 = r116..r121 on arch >= 12), and the kbase
+    * wrapper has to restore the subqueue context register on every ring entry
+    * because CS register state is not preserved across separate kicks
+    * (see kbase_subqueue_emit_job()).  Neither can work when the three CSs
+    * live in three different queue groups, so create a single group holding
+    * all of them: cs_min is documented as "Minimum number of CSs required"
+    * (DDK uapi, kbase_csf_ioctl.h) and cs_min = PANVK_SUBQUEUE_COUNT is what
+    * the CSG create ABI expects for a multi-CS group.  panthor does the same
+    * (create_group(): one group, three queues). */
+   if (kbase_kmod_csf_group_create(dev->kmod.dev, PANVK_SUBQUEUE_COUNT,
+                                   &group_handle)) {
+      result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
+                            "Failed to create a kbase queue group");
+      goto err_destroy_group;
+   }
+
    for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
       struct panvk_subqueue *subq = &queue->subqueues[i];
 
-      if (kbase_kmod_csf_group_create(dev->kmod.dev, 1,
-                                      &subq->kbase.group_handle)) {
-         result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
-                               "Failed to create a kbase queue group");
-         goto err_destroy_group;
-      }
+      subq->kbase.group_handle = group_handle;
 
       subq->kbase.ringbuf_bo =
          pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, KBASE_RINGBUF_SIZE,
@@ -1434,9 +1448,9 @@
       subq->kbase.user_io = kbase_kmod_csf_queue_bind(
-         dev->kmod.dev, subq->kbase.group_handle, 0,
+         dev->kmod.dev, subq->kbase.group_handle, i,
          subq->kbase.ringbuf_dev,
          KBASE_RINGBUF_SIZE);
       if (!subq->kbase.user_io) {
          result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                                "Failed to bind a kbase CS queue");
@@ -1444,9 +1458,9 @@
       subq->kbase.insert = 0;
       subq->kbase.emitted_jobs = 0;
-      mesa_logd("kbase: bound subqueue %u to group %u CSI0, ring CPU %p, "
+      mesa_logd("kbase: bound subqueue %u to group %u CSI%u, ring CPU %p, "
                 "ring VA 0x%" PRIx64 ", user_io %p",
-                i, subq->kbase.group_handle, subq->kbase.ringbuf_cpu,
+                i, subq->kbase.group_handle, i, subq->kbase.ringbuf_cpu,
                 subq->kbase.ringbuf_dev,
                 subq->kbase.user_io);
    }
```

应用：`cd /root/zenithblue/work/mesa && git apply -p1 --recount --check <patch>`；失败则说明 v67 改动已挪动上下文，
按 hunk 手工落（四个 hunk 都自包含）。

### 预期可观测差异

1. **成功准入**：`kbase: created CSF group ... (1.18 layout, ioctl 58)` **只出现 1 次**（现在是 3 次），
   紧随 `kbase: bound subqueue 0/1/2 to group N CSI0/CSI1/CSI2`。
2. **主判据**：v66 那族超时签名（`extract` 停在 CALL / sq1、sq2 无 FINISH 位 / `error 0x0`）
   **消失或挂起点显著推后**；若消失，Minecraft 存活时间应从 ~30 s 跳到"跑满不 DEVICE_LOST"。
3. **反判据（很重要）**：若本机 kbase 真的**不执行 CSI>0**（`panvk_queue.h:73-74` 那条历史经验），
   现象会是"VT 仍跑完、FRAG/COMPUTE **一条 ring entry 都不消费**（extract 恒 0 或恒等于初始值）"
   ⇒ 与现在的"extract 停在 CALL"**可区分**，这是一次干净的判定实验。
4. 若 group create 被拒：logcat 直接出现
   `kbase: KBASE_IOCTL_CS_QUEUE_GROUP_CREATE failed: Invalid argument`（`kbase_kmod.c:614-617`）
   ⇒ 本机 `stream_num < 3` 或没有 CSG 有 3 个 CS，立即回到旧布局，不产生歧义。

### 风险

* **中**。若 CSI>0 真的不执行（历史经验来自**别的** Android kbase 栈），会从"偶发挂起"变成"完全不渲染"——
  但完全可逆（一个 diff）且现象**极易判定**（见上 3）。
* 次要：一个 group 现在同时申报 tiler + fragment + compute 端点（`tiler_mask=1 / fragment_mask=~0 / compute_mask=~0`），
  与 panthor 意图一致；但若固件在该机型的 CSG 上不允许一个 group 同时占三类端点，create 会被拒（日志可见）。
* 端点资源申报（wrapper 里每条 entry 的 `REQ_RESOURCE`）在"一个 group"下语义更接近内核期望，
  但**本轮不一起动**（单变量纪律）。

### 最小验证方法

1. 装 v67（候选 1）跑一次，**只取三行**：`created CSF group` 出现次数、
   `bound subqueue ... CSI0/1/2`、超时快照里 sq0/sq1/sq2 的 `extract` / `progress` / `marks`。
2. 无论成败都值得同时打开 `PANVK_DEBUG=kbase_diag`（v66 已有面包屑）：
   * 成功 ⇒ sq1/sq2 应出现 `post_call` / `post_wait` 推进与 FINISH / CMDBUF_DONE 位；
   * 失败 ⇒ 用 `COMPUTE_ENTER`(bit1) 判定 COMPUTE 是否卡在同一位置（同位置 = 不是本因）。
3. **零风险的决定性加测（建议随 v67 一起出）**：把 FRAG/COMPUTE **算出来的等待目标值**写进 subqueue ctx
   （`panvk_cs_subqueue_context.debug`，`panvk_cmd_buffer.h:224-229`），位置：
   `cmd_draw.c:4256-4258`（`vt_sync_point` 算完、`panvk_instr_sync64_wait` 之前）与 `cmd_buffer.c:616` 之前；
   超时快照已有 ctx 打印（`gpu_queue.c:556-573`），再把它和 `kbase_log_queue_syncobjs()`
   （`gpu_queue.c:245-263`）打的 **syncobj 实际 seqno** 对照，即可一眼看出"目标值是否高于对端会 signal 的值"——
   这正是候选 1 的因果判据。（该加测是 3 文件小改，需要时我再出完整 diff。）

---

## 候选 2：把 `csi_handlers` 退回 0（1.18 rung，单变量 A/B）

### 依据

* §2.2 全部（内核源码 `:1926-1938` 唯一效果 + 不发通知 + PanVK handler 只在 draw 窗口内武装
  `cmd_draw.c:4513-4539`）。
* 报告 14 §6.3 要求过对照组、报告 23 §3.3(c-2) 记录该对照组**从未做过**；两个已跑通先例都没有这个字段。
* 现在 `PANVK_KBASE_HEAP_RENEW_INTERVAL` 已在用（v66 renewal 8 次 / interval 32 生效），
  堆压力比报告 14 时代低 ⇒ flag 的收益下降，而"静默停车"的代价上升。
* 与候选 1 **不冲突**：可以分别单跑，也可以先跑候选 1（结构面）再叠加。

### diff（`src/panfrost/lib/kmod/kbase_kmod.c`，只动 1.18 rung；1.25 rung 有意不动）

```diff
--- a/src/panfrost/lib/kmod/kbase_kmod.c
+++ b/src/panfrost/lib/kmod/kbase_kmod.c
@@ -576,10 +576,9 @@
             .cs_min = cs_queue_count,
             .priority = 0, /* BASE_QUEUE_GROUP_PRIORITY_HIGH */
             .tiler_max = 1,
             .fragment_max = 64,
             .compute_max = 64,
-            .csi_handlers = BASE_CSF_TILER_OOM_EXCEPTION_FLAG,
          },
       };
 
@@ -585,9 +584,10 @@
       if (ioctl(dev->fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18, &req18) == 0) {
          *group_handle = req18.out.group_handle;
-         mesa_logi("kbase: created CSF group %u with TILER_OOM CSI handler "
-                   "(1.18 layout, ioctl 58)", *group_handle);
+         mesa_logi("kbase: created CSF group %u (1.18 layout, ioctl 58, "
+                   "csi_handlers 0x%x: kernel keeps tiler OOM fatal)",
+                   *group_handle, req18.in.csi_handlers);
          return 0;
       }
```

（若希望**同时**覆盖 1.25 档，再加删 `kbase_kmod.c:552` 的 `.csi_handlers = ...` 一行即可，
但那会让本机 A/B 的"单变量"变成两处，建议本机只删 1.18 这一处。）

### 预期可观测差异

* 正向：若"静默停车"确由 flag 造成 ⇒ 挂起消失；或**挂起形态改变**为
  `E/MESA: kbase: CSF group N tiler heap OOM notification`（`kbase_kmod.c:707-710`）后 DEVICE_LOST
  ——后者说明"OOM 真的在发生，以前被我们静默转走了"，同样是重大信息。
* 负向：若挂起签名一字不变 ⇒ flag 与本病无关，回到候选 1 / 3。
* 附带可观测：logcat 里那行文案变化（含 `csi_handlers 0x0`），可确认新驱动真的装上了。

### 风险

* **低-中**：重新暴露 tiler-heap OOM 会杀组（报告 14 的老症状）。但那条是**可见的**失败（有通知、有 0xc3），
  比现在的静默停车更可诊断；且 `PANVK_KBASE_HEAP_RENEW_INTERVAL` 与报告 14 的 renew 逻辑仍在。

### 最小验证方法

* 与 v67 同轮 A/B：一次运行只看两个字符串——
  `created CSF group .* csi_handlers 0x`（确认生效）与
  `tiler heap OOM notification` / 超时快照（判定因果）。

---

## 候选 3：`CS_EXTRACT_INIT` 显式写 0 + 输入页回读（**契约卫生与可观测性，非修因**）

### 依据

* §3(a)：字段语义 = "Initial extract offset"（`mali_kbase_csf_registers.h:139-140`），
  内核在页分配时写 0（`ref-r43p0-...csf.c:178-179`；`pub-kbase/csf.c:166-181`），此后无人再写。
* 我们只在 `kbase_subqueue_publish()` 写 `CS_INSERT`（`gpu_queue.c:1035-1036`），从不写 `+0x8`。
  **值上等价于写 0**，所以这**不会**修好任何挂起；它的价值是把"契约面确实被覆盖"与
  "ring 复用 / CS 重启场景下起点可控"变成代码事实，并让 logcat 能自证这两个字段。
* 明确标注：**未验证推断** —— "写它会带来行为变化"没有任何证据支持；不要把它当作 Lead 假设的验证实验。

### diff（`src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c`，紧跟 bind 之后）

```diff
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ -1440,6 +1440,24 @@
       if (!subq->kbase.user_io) {
          result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                                "Failed to bind a kbase CS queue");
          goto err_destroy_group;
       }
 
+      /* uAPI contract: input page +0x0 = CS_INSERT (we own it), +0x8 =
+       * CS_EXTRACT_INIT ("Initial extract offset for ring buffer",
+       * mali_kbase_csf_registers.h:139).  kbase zeroes both when it allocates
+       * the pair of pages (ARM kbase mali_kbase_csf.c::init_user_io_pages()),
+       * so writing 0 here is value-identical to not writing: this is contract
+       * coverage (ring reuse / CS restart start offset) and a readback, NOT a
+       * fix for the current hang. */
+      uint8_t *kbase_input_page = (uint8_t *)subq->kbase.user_io + 4096;
+      *(volatile uint64_t *)(kbase_input_page +
+                             CS_USER_IO_INPUT_CS_EXTRACT_INIT) = 0;
+      kbase_gpu_wmb();
+      mesa_logi("kbase: subqueue %u input page: CS_INSERT %" PRIu64
+                " CS_EXTRACT_INIT %" PRIu64 " (both must be 0 before the "
+                "first kick)", i,
+                *(volatile uint64_t *)(kbase_input_page +
+                                       CS_USER_IO_INPUT_CS_INSERT),
+                *(volatile uint64_t *)(kbase_input_page +
+                                       CS_USER_IO_INPUT_CS_EXTRACT_INIT));
+
       subq->kbase.insert = 0;
       subq->kbase.emitted_jobs = 0;
       mesa_logd("kbase: bound subqueue %u to group %u CSI0, ring CPU %p, "
```

### 预期可观测差异

* logcat 新增 3 行 `kbase: subqueue N input page: CS_INSERT 0 CS_EXTRACT_INIT 0`。
* **不应有任何行为变化**。若挂起消失 ⇒ 说明我这条判断错了（那将是很重要的反例，请记录）。

### 风险

* 极低（只写自己拥有的页 + 打印）。唯一注意点：`kbase_gpu_wmb()` 在 bind 后立刻调用是多余的
  （尚未 kick），保留它只是与 publish 路径一致。

### 最小验证方法

* 与候选 1 / 2 一起出，仅作对照与自证；单独跑一次可确认"字段 = 0"这一事实。

---

## 4. 已排除 / 已修正的可疑点清单（省实验预算）

| 可疑点（任务书原文） | 判定 | 依据 |
|---|---|---|
| `csi_handlers` 只在 1.25 设、1.18/1.21 如何 | **1.18 rung 也设了，本机走的就是这条** | `kbase_kmod.c:582`（1.18）+ `:552`（1.25）；`:571` 版本门；判据 = logcat 的 `1.18 layout, ioctl 58` 行 |
| 设置 `csi_handlers` 后内核是否改走 app handler 而不发通知 | **是（仅 tiler-OOM 一类）** | 内核 `:1926-1938`（不进 `report_tiler_oom_error()`）+ `:1958-1974` |
| 完成 / 异常通知是否根本没 enable | **per-CS fault 上报在 1.21 无法 enable（协议缺失）；内核事件通道已 enable 且在读** | §2.1 |
| `KBASE_IOCTL_CS_EVENT_SIGNAL` 零调用是否违规 | **不违规、且与完成路径无关**（host 事件 / off-slot 唤醒件，本仓设备测试亦记 UNPROVEN） | `device/csf-event-regression.c:18-20`；`pub-kbase/csf.c:3400-3416` |
| KCPU queue 默认关闭是否有问题 | **不是本因**：只在 `PANVK_KBASE_KCPU_SYNC=1` 时用于 CPU 侧等待；关掉只是回退到"自旋 + 内核通知" | `kbase_kmod.c:1365-1371, 901-1041` |
| `CS_EXTRACT_INIT` 从未写是否违规 | **不违规、非本因**（写 0 == 现状；v66 extract 从不回 0 ⇒ 无 CS 重启） | §3(a) |
| queue group 的 priority 是否写错 | **没写错**（HIGH = 0） | `mali_base_csf_kernel.h:190-196` |
| tiler heap 参数 / 布局是否符合内核期望 | **符合**（32 B 当前布局，1.13 变体只适用 ≤1.13） | DDK 头 `:481-535`；`kbase_kmod.c:1063-1071` |
| 页 2 CS 寄存器镜像能否作为"无 fault"证据 | **不能**（uAPI 不承诺；v66 实测全 0） | `mali_kbase_ioctl.h:395`；v66 快照 |

---

## 5. 未验证 / 交给下一轮

1. **【未验证】CS 寄存器文件（r116..r121）在"一个 group 多 CS"下的共享语义**：
   候选 1 的因果链依赖"组内 CS 能看到彼此的 progress-seqno 寄存器"。依据是 PanVK 上游代码本身
   （FRAG 读 VT 的 slot）+ panthor 三 CS 一组的模型，**我没有拿到 CSF 规范原文**。
   候选 1 的实测即为判定实验（见其"预期可观测差异"3 / 4）。
2. **【未验证】固件是否在某些重启路径读 `CS_EXTRACT_INIT`**（§3(a) 末）；本轮 extract 无回绕，观察不到。
3. **【未验证】`REQ_RESOURCE`（wrapper 每条 ring entry，`gpu_queue.c:849-853`）**：
   主线 panthor 的 ring entry 不发它（资源申报走 CSG 接口），我们在 ring 里每 entry 申报。
   v66 diag 已打印 `CS_STATUS_REQ_RESOURCE`，但**该寄存器在页 2 没有镜像（实测 0）** ⇒ 无法用该读数判定，
   需要 `PANVK_DEBUG=kbase_diag` 的面包屑或一次单变量删除实验（报告 31 §7.2 的第 2 条，风险中）。
4. **【未验证】同族"wrapper 抢占流状态"的其余 4 条 `SET_STATE`**
   （`gpu_queue.c:826-830`：SB_SEL_ENDPOINT / SB_MASK_WAIT / SB_SEL_OTHER / SB_SEL_DEFERRED）：
   报告 31 §7.1 已列；本轮注释（`gpu_queue.c:831-844`）自己承认这 4 个也是 stream state，却仍然每条 entry 重写
   ⇒ 与 v65 同一契约理由，建议作为 v65 无效后的下一个单变量。
5. **【未验证】`cs_fault_report_enable` 在 1.21 世代是否真的"不存在"还是"藏在别的 nr"**：
   本轮只证明它不在 1.18 的 40 B 布局里；本机按 40 B 调用成功即证明内核 nr 58 的 1.18 分支存在且尺寸为 40。
   若某轮 logcat 出现 `kbase: 1.18 CS_QUEUE_GROUP_CREATE failed: ...`，这条结论要重开。

---

## 6. 自检（本轮，全程只读）

* 三份 diff 均用 `git apply -p1 --recount --check -`（**只校验、不落盘**）对 v66 树验证通过：
  * 候选 1（本文件 195–291 行）：**CAND1-APPLIES-CLEAN**
  * 候选 2（本文件 349–370 行）：**CAND2-APPLIES-CLEAN**
  * 候选 3（本文件 411–441 行）：**CAND3-APPLIES-CLEAN**
* 取用方式（自行落盘到工作目录，本文件不含 patch 文件）：
  `sed -n '195,291p' /root/research/34-kbase-contract.md > /tmp/c1-one-group-three-cs.patch`
  `sed -n '349,370p' /root/research/34-kbase-contract.md > /tmp/c2-csi-handlers-0.patch`
  `sed -n '411,441p' /root/research/34-kbase-contract.md > /tmp/c3-extract-init-hygiene.patch`
* 本轮**未编译、未构建、未操作手机、未改动 `/root/zenithblue/**` 或任何只读树**；写入仅本文件。
* 本文件：34 727 B / 493 行 / md5 `ea245b8a89f7794610301de71dbc500d`（追加本节前）。
