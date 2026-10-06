# v82 — 上游 4ecc4966（CS 函数地址 32 位截断）回移 + 自研 kbase tiler heap 续租审计

日期：2026-10-06 · 构建树 `/root/zenithblue/work/mesa` · 构建目录 `/root/zenithblue/build/android-v4`
上游基线 `5a07217f034` · 上游提交 `4ecc4966`（`panvk: Don't downcast CS func address to a 32-bit VA`，2026-09-24 入库）
分工：源码补丁（v82）+ 编译 + 打包 + 切分 = Lead；**v82 分片复核 + 报告 + 续租审计 = teammate `glm`**
本报告分两部分：**A = 自研 kbase tiler heap 续租审计（主线）**，**B = v82 收尾**。

---

## 0. 一句话

- **A（审计）**：续租路径**不是**"稳定可用的优化"，而是当前**唯一**的堆回收手段（常见情形下 GPU 侧 `FRAGMENT_COMPLETED` 根本不发，见 A.4-F3）；它的**代际切换是"改写描述符内容 + 无 GPU 端屏障"**（A.4-F2，代码级确证），而它的**同步代理是 CS seqno、不是异步 tiler 的退休**（A.4-F3，领先假设）。我同时**证伪了一条**：`pan_size(TILER_HEAP)` = **40 B**（≪ 4096），续租**不会**踩掉紧随其后的几何缓冲（A.5）。
- **B（v82）**：`uint32_t fn_addr` → `uint64_t`（csf:2114），与上游 `4ecc4966` 的 diff **逐字一致**；分片重组两套**逐字节**通过（我独立复核，`65111cd1…` / `7f0cfc7a…` 与 Lead 一致）。

---

## A. 自研 kbase tiler heap 续租审计

### A.0 方法与边界

- **只读**：`sed/grep/diff` 读源码与生成头（构建树 `build/android-v4/src/panfrost/genxml/v12_pack.h`）。**未改任何源码、未编译、未操作设备、未碰 `rm`**。
- 分析台账（每条隐患的精确行号与取证命令）：`/root/v82work/audit-notes.md`。
- 起点是交接说明 §9.5 的既有结论「探针像素精确正确 ⇒ 不是驱动整体画错，而是**重负载下的 tiler heap 数据问题**」。我**没有**把它当事实复述，而是做了两件事：① 追问它的**逻辑是否唯一**（A.1）；② 到代码里找出**能造成同类画面、且与"重负载"相关**的具体机制，并逐条给证伪手段（A.4）。

### A.1 对 §9.5 那句推断的评估（加强 / 削弱）

**它比原文更弱，也比原文更具体。** 削弱的部分：`探针像素精确` 只能排除**全局性**错误（整体变换、格式、stride、色彩空间），它**不能**单独把嫌疑锁定到 tiler heap——同样能造成"局部精确、局部垃圾"的还有：tiler context / 多边形链表头指针（**它不在 tiler heap 里**，在描述符分配里，`panvk_cmd_alloc_desc_array`）、单次 draw 的顶点/属性缓冲读取、render descriptor ringbuf 回绕。加强的部分：楔形的**形状**（一个跨越半屏的巨型三角形）恰恰是**多边形链表**被读坏的指纹，而多边形链表的**存储**就是 tiler heap 的 chunk —— 所以"tiler heap"是**形状学上最贴合的**嫌疑，但不是唯一的。

⇒ 结论：§9.5 是**值得优先排查的领先假设**，不是已证结论。**能用现有仪器一刀切开的分界实验见 A.3 之 ①②（`PANVK_KBASE_HEAP_RENEW_INTERVAL=0/1`）**——它直接判定"堆生命周期"是否与楔形相关，零新代码。

### A.2 续租机制（先厘清事实，再谈隐患）

| 事实 | 证据 |
|---|---|
| 子队列上下文里存的是**描述符地址**（`tiler_heap->desc`），**代际切换只改写它的内容** | `csf/panvk_vX_gpu_queue.c:2763-2765`（队列初始化时写一次）· `csf/panvk_vX_cmd_draw.c:1341-1344`（每作业从上下文 load） |
| 几何缓冲与描述符**同一次分配**：描述符在 +0（4 KiB 内），geom_buf 在 **+4096**，大小编码进地址低 12 位 | `panvk_vX_gpu_queue.c:2767-2771`、`:3108-3120`（`size = (64*1024)+4096`） |
| 续租 = ①尝试销毁上次退休的 ctx ②`HEAP_INIT` 新建堆 ③把新堆的 `size/base/bottom/top` 写进**同一块**描述符 + cache clean ④把旧 ctx 记为退休 | `:3274-3288`、`:3292-3341`、`lib/kmod/kbase_kmod.c:1053-1095` |
| 内核 uAPI：`KBASE_IOCTL_CS_TILER_HEAP_INIT` 返回 `gpu_heap_va` + `first_chunk_va`；`TERM` 释放整个堆 ⇒ **TERM 之后 chunk VA 可被复用** | `kbase_kmod.c:1053-1082`、`:1085-1095` |
| 续租前必须**排水**：等本队列 VT+FRAG 到本提交的 seqno | `:3906-3918` → `:3789-3810` → `:1684-1720` |
| 排水判据是 **CS 写入的 seqno cell**（"在 all-scoreboard wait 之后"写入；注释明确「CS_EXTRACT 只报告固件取指位置，**由这些命令发起的异步 GPU 作业可能仍在运行**」） | `:1684-1698` |
| tiling 是**异步**的；它的完成由 **FRAG** 流的 `wait_finish_tiling()` 观测（等 VT 的 syncobj） | `cmd_draw.c:4304-4322`、`:4582-4600` |
| `FINISH_FRAGMENT` 携带 **First/Last Heap Chunk**（实参 = tiler 上下文的 completed_top/bottom）⇒ 固件据此**回收块** | `genxml/v10.xml:649-650`、`genxml/cs_builder.h:1679-1691`、`cmd_draw.c:4725-4729` |
| 三条 heap ops 是 `HEAP_OPERATION`（`VERTEX_TILER_STARTED/COMPLETED`、`FRAGMENT_COMPLETED`），且是**迭代槽的 signal 来源** | `genxml/v10.xml:490,819-822`、`cs_builder.h:2390-2404`、Lead 交接 C1 |
| **上游自己的注释**：FINISH_FRAGMENT 等当前迭代、signal 下一个迭代，**就是为了保证"用过的 heap chunk 不会被过早释放"** | `cmd_draw.c:4700-4710` |
| 常见情形（`td_count == 1`）**只发 `cs_finish_fragment`，不发 `cs_frag_end`/FRAGMENT_COMPLETED** —— 这是**上游自身**结构，不是我们的改动 | `cmd_draw.c:4723-4736` 与上游 `main` 同处 `4658-4671` **逐行相同**；`MAX_LAYERS_PER_TILER_DESC = 8`（PAN_ARCH<14，`panvk_cmd_buffer.h:36-40`） |

### A.3 首要动作（用**已有**仪器，零新代码，先做这个）

> Lead 手上**已经有** v81 带楔形的真机 run。它的 logcat 里几乎必然有续租日志——**事后数一遍就能立刻缩小范围**。

① **决定性 A/B（堆是否与楔形相关）**：`PANVK_KBASE_HEAP_RENEW_INTERVAL=0`
   - 语义：`:119-121` ⇒ `(v <= 0 || v > UINT32_MAX) ? UINT32_MAX : v` ⇒ 间隔 = UINT32_MAX ≒ **实际不再续租**（注释也这么写）。
   - 判读：楔形**消失**（哪怕随后因堆不再回收而出现 OOM/卡死）⇒ 续租路径被牵入；楔形**照旧** ⇒ **堆生命周期被排除**，立刻转 A.4-F5/F6/F7 与"描述符/多边形链表头指针"线。
   - 反向加压：`=1`（每次图形提交都续租）⇒ 楔形更频繁/更早出现 ⇒ 强化。
   - 注意：该 env 被 `static` 缓存（`:119`），**改值必须重启进程**；且它影响的是**间隔**，不影响 F1/F6 的"静默跳过"。
② **数 logcat（回顾已有 run，最便宜）**：
   ```
   adb logcat -d | grep -c "kbase: CKPT host heap renew #.*begin"
   adb logcat -d | grep -c "kbase: CKPT host heap renew #.*end"      # 只有 begin 没有 end ⇒ F1 命中
   adb logcat -d | grep    "kbase: tiler heap renewal (uAPI"
   adb logcat -d | grep -nE "kbase: CS error|fault 0x"               # 低 32 位形态的 fault 地址 ⇒ v82 靶心命中（B.6）
   ```
   `begin` 无 `end` = `kbase_try_destroy_retired_heap()` 返回 false ⇒ 续租被**静默跳过**（F1）。
③ **对拍"续租是否落在 tiling 窗口内"**（F3 的直接证据）：`begin/end` 的时间戳与 FRAG 检查点 `CKPT_FRAG_ENTER / _BEFORE_TILING_WAIT / _AFTER_TILING_WAIT`（`cmd_draw.c:4582-4600`）的先后；检查点字在 seqno cell 偏移 56（`gpu_queue.c:94-97` 有 static_assert），`PANVK_DEBUG=kbase_diag` 的超时快照会打印 `ckpt 0x%08x`。

### A.4 隐患清单（按可信度排序；每条给 文件:行 + 触发 + 症状 + 可证伪性）

#### F1 ★★★ 续租被"静默跳过"却**仍然清零间隔计数器** ⇒ 可长期失效 ⇒ 堆只增不减

- **位置**：`gpu_queue.c:3306-3307`（`if (!kbase_try_destroy_retired_heap(queue)) return VK_SUCCESS;`，**跳过且不打印 end**）+ `:3910-3938`（成功路径**无条件** `kbase_tiler_submit_count = 0; kbase_tiler_work_count = 0;`）。
- **触发**：`kbase_try_destroy_retired_heap`（`:3274-3288`）要求 VT **且** FRAG 两个子队列的 `emitted_jobs` 都**越过**上次退休快照。只要有一侧不前进（只提交 VT 的时段 / 只提交 FRAG 的时段 / 计算为主），**每次**到点都会跳过并把计数清零 ⇒ 下次再从 0 数 32 次 ⇒ 仍跳过 ⇒ **永久失效**。
- **症状**：堆不再回收 → 涨到 `max_chunks = MAX2(phys_dev->csf.tiler.max_chunks, 200)`（`:3131-3132`、`:3312`）→ TILER_OOM 异常路径 → 依 P2 注释「the heap could only grow until kbase terminated the CSG on -ENOMEM」⇒ **设备丢失 / 看门狗 −4**。**属卡死族（bug ③），不是楔形**。
- **可证伪**：**是**，且零新代码 —— logcat 里 `CKPT host heap renew #N begin` 之后没有对应的 `end`（A.3-②）。连续多次 begin-无-end 即确证。
- **可信度**：**高**（代码可读确证；只是尚未在真机日志里确认是否实际发生）。

#### F2 ★★★ 代际切换 = "改写描述符内容 + 无 GPU 端屏障"（代码级确证）

- **位置**：`gpu_queue.c:2763-2765`（ctx 存**描述符地址**，只在队列初始化时写一次）+ `cmd_draw.c:1341-1344`（每作业从 ctx load）+ 续租 `:3321-3333`（改写内容 + `kbase_clean_priv_mem`，**不更新 ctx**，因为地址没变）+ ring 包装每条目重发 `HEAP_SET`（`genxml/v10.xml:550`、`gpu_queue.c:1386-1396` 注释）。
- **含义**：**已经发射/正在运行的作业，在需要增长堆时读到的是新描述符**；切换没有 generation 标记、没有 fence、GPU 完全不可观测。⇒ 方案正确性**完全**依赖"改写 desc 的那一刻没有任何 GPU 单元还在用旧堆"。
- **可证伪**：**代码级已确证**（这是事实陈述，不是猜测）；"是否真有在飞使用者"由 F3 的检查决定。
- **可信度**：**高**（构造性结论）。

#### F3 ★★☆（领先假设）排水代理是 **CS seqno**，而堆的使用者是**异步 tiler**：两者不等价

- **位置**：排水 `:3906-3918` → `kbase_wait_graphics_targets` `:3789-3810`（**只遍历 graphics mask，且 target==0 的子队列直接跳过**）→ `kbase_subqueue_wait_seqno` `:1684-1720`（判据 `cell->seqno >= target_seqno`，且注释自认"异步 GPU 作业可能仍在运行"，只认 all-scoreboard-wait 之后的完成写）；tiling 完成的唯一观测点是 **FRAG** 的 `wait_finish_tiling()`（`cmd_draw.c:4304-4322`）。
- **窗口（严格前提）**：**若某次提交里有 VT(tiling) 作业、而与它配对的 FRAG 作业不在同一次提交**（或该队列该时段没有 FRAG 作业），排水就**只等到 VT 的 CS seqno**——而 VT 的 CS seqno **不证明 tiler 已停**（tiler 停由**后续** FRAG 作业的 `wait_finish_tiling` 观测）⇒ 此刻改写 desc（并在下一次续租 `TERM` 旧堆）⇒ 仍在跑的 tiler 把多边形数据追加进**新堆首块**；而已按新堆启动的作业可能拿到**同一个首块** ⇒ **同块双写、多边形链表交错** ⇒ 片元阶段栅格化出一个跨半屏的垃圾三角形 ＝ **巨型三角楔形**。
- **次生通道（同一根因）**：`cs_finish_fragment` 回传的 First/Last free heap chunk 来自 **tiler 上下文**（`cmd_draw.c:4725-4729`）。若该上下文产自**旧堆**，回报的"空闲块"属于已被 `TERM` 的堆；固件把它们并入新堆空闲链后，后续多边形就写进"别人的块" ⇒ 同类画面。
- **为何"重负载才出"**：帧越重 ⇒ 每作业吃块越多、tiler 在 CS 退休后仍存活的时间窗越长、续租越频繁（env=32）⇒ 命中率显著上升；轻负载帧（vkcube/vkmark）几乎不触发。
- **可证伪（现有仪器，零新代码）**：A.3 的 ①②③ 三条。尤其 `=0` 的 A/B 是**一刀切**的判据。
- **可信度**：**中高**。机制自洽、与"重负载"和"一个巨型三角形"的形状都吻合、且有决定性实验；但我**未能在代码层确证**"VT 作业与配对 FRAG 作业不同批"这种提交模式是否真的出现（见 F4），故标为**待证伪的领先假设**，不是结论。

#### F4 ★★☆（缺口，诚实标注）VT/FRAG 是否总在同一次提交 —— 我未能确证

- F3 的窗口需要"VT 单独成批"。panvk 正常路径把同一 render pass 的 VT 与 FRAG 流**一起**发射（`touched` 掩码来自本次提交实际发射的流，`:3860-3873`），此时排水同时覆盖 FRAG 的 `wait_finish_tiling` ⇒ **窗口不存在**。
- 我**没有**在代码层找到"VT 单独成批"的确证（跨 submit 的 render pass / 只提交 VT 的时段都只是可能性）。⇒ 这正是我把 F3 降级为假设的原因，也是 A.3-③ 要回答的问题。

#### F5 ★★☆ 参数面：env=32 把"每 32 次图形提交"变成生产默认 ⇒ 每次续租都是一次**全图形同步排水**

- **位置**：`:104-125`（默认 128、env 覆盖、static 缓存、0⇒UINT32_MAX）、`:3890-3905`（P2 去掉 `tiler_work_estimate &&` 前置门 ⇒ 计数器真正开始前进）、`:3906-3938`（到点 ⇒ `kbase_wait_graphics_targets(..., UINT64_MAX)` **同步阻塞提交线程** ⇒ 全图形排水 ⇒ 新建/写描述符/退休）。
- **附带发现（注释与代码不一致）**：P2 上方的注释说"Clear-only fragment submissions don't use the tiler heap. Counting those towards renewal forces a graphics drain and heap replacement with no memory to reclaim."，但代码只判断 `touched & graphics_mask`（FRAGMENT 在位），**clear-only 的 FRAG 提交照样计数**（`:3898-3905`）⇒ 续租频率高于注释描述的设计意图。
- **症状**：帧时间尖刺 / 吞吐下降（**不是**楔形）；但它**放大了 F3 的暴露面**（每次排水+切换 = 一次风险窗）。
- **可证伪**：**是** —— `mesa_logi("kbase: tiler heap renewal (uAPI %u.%u, submits %u, renew interval %u)")`（`:3920-3926`）每 32 次提交打印一行，数 logcat 行数即可量化频率。
- **可信度**：**高**（代码可读确证）；但它是**性能/暴露面**问题，不是病根。

#### F6 ★★☆ 计数器被重置 ⇒ 退休槽**永久**追不上（F1 的永久化版本）

- **位置**：`:2200`（队列 bind 后 `subq->kbase.emitted_jobs = 0;`）+ `:3279-3282`（`emitted_jobs <= retired.*` ⇒ false）+ `:3290-3340`（**只保留一个**退休 ctx）。
- **触发**：任何导致 `emitted_jobs` 归零的路径（队列组重建/bind）恰好发生在有退休 ctx 时 ⇒ 快照（例如 500）永远大于当前计数 ⇒ **只要那个退休 ctx 还在，续租永远跳过**。
- **症状**：同 F1（堆耗尽 ⇒ −ENOMEM / 卡死族）。
- **可证伪**：`PANVK_KBASE_HEAP_RENEW_INTERVAL=1` 下仍**长期**看不到 `renew end` ⇒ 命中。

#### F7 ☆（已**证伪**，记录以免再来一遍）描述符会不会踩掉几何缓冲

- 假设：`desc` 起 4 KiB，`geom_buf` 在 +4096（`:2767-2771`），若续租写入超过 4096 B ⇒ 每次续租毁掉几何缓冲 ⇒ 顶点数据被毁 ⇒ 巨型三角形。
- **证伪**：生成头 `v12_pack.h:9933-9940`，`struct MALI_TILER_HEAP { enum type; enum buffer_type; uint64_t size, base, bottom, top; }` = **4+4+8+8+8+8 = 40 B** ⇒ `pan_size(TILER_HEAP)` = 40 B，**不可能**越界；且续租用的是与 `init_tiler`（`:3196-3205`）和 panthor 路径**完全相同**的写模式与字段。⇒ **不是新风险，排除**。

#### 已含的正面证据（说明这一"类"问题在本区域是真实的）

`gpu_queue.c:3255-3272` 的注释**已经**记录并防住了一个同类 bug：过早 `TERM` 会让 kbase 释放（并被自研 VA 分配器复用）chunk，而 CS 的 HEAP_SET 寄存器仍指向它们 ⇒ 固件把"现在住在那里的东西"当成 chunk 链走 ⇒ 野指针 ⇒ **实测到 exception 0xc0 CSG fatal at a wild sideband address**。⇒ 作者已经在这条线上踩过并修过一次；F1/F2/F3/F6 是**同一族**里剩下的口子。

### A.5 我**证伪**掉的假设（本轮净收益）

| 假设 | 结论 | 依据 |
|---|---|---|
| 续租写描述符越界、踩掉紧随其后的几何缓冲 ⇒ 顶点数据被毁 ⇒ 巨型三角形 | **证伪** | `v12_pack.h:9933-9940`：`MALI_TILER_HEAP` = **40 B**，`desc` 前 4 KiB 足够 |
| "`FRAGMENT_COMPLETED` 不发" 是自研改动引入的缺陷 | **证伪（是上游行为）** | `td_count == 1` 只发 `cs_finish_fragment` 的分支与上游 `main` **逐行相同**（`cmd_draw.c:4723-4736` ↔ 上游 `4658-4671`）；`MAX_LAYERS_PER_TILER_DESC = 8`（v12） |
| 子队列上下文存的是 heap ctx（会变）⇒ 续租后悬垂 | **证伪** | `gpu_queue.c:2763-2765` 存的是**描述符地址**（稳定）；`ctx` 换址不需要、也没有更新 ctx ⇒ 无悬垂指针（但正因此造成 F2 的"静默切换"） |
| §9.5「探针像素精确 ⇒ 就是 tiler heap 数据问题」 | **削弱**（不能单独成立） | 见 A.1：同症状还可来自 tiler context/多边形链表头（不在堆里）、顶点缓冲、desc ringbuf |

### A.6 给 Lead 的建议顺序（单变量纪律）

1. **先看已有 v81 run 的 logcat**（A.3-②）：`begin` vs `end` 计数。—— 零成本，可能直接命中 F1。
2. **`PANVK_KBASE_HEAP_RENEW_INTERVAL=0` 跑同一场景**（A.3-①）：楔形在/不在 ⇒ 一刀切开"堆生命周期"这条线。
3. 若 `=0` 楔形消失 ⇒ 按 F2/F3 的方向做**一个**单变量改动（我的看法：把"排水判据"从 CS seqno 升级为"本队列**所有**图形子队列的 seqno **且**本次提交确实包含了配对的 FRAG 作业"，或直接在续租前**等待 FRAG 到最新 seqno 并确认无 tiler 在飞**）；**不要**同时改 F1/F5。
4. 若 `=0` 楔形照旧 ⇒ 立刻把火力转向"**多边形链表头指针 / tiler context / desc ringbuf**"这条线（A.1 的三个替代嫌疑），并复核 F5 的排水频率是否掩盖/改变症状。

---

### A.7 实测否证清单（真机 + 代码级；这部分本身就是结论）

| # | 被否证的对象 | 否证方式 | 证据 | 影响 |
|---|---|---|---|---|
| N1 | **tiling area 被 spill.store 扩张** ⇒ 楔形 | **真机** | v81（MR!44816）上机后楔形**完全没变**（连拍帧 `/sdcard/MG/shots/v81_05.png`，与基准图同型） | 结案：报告 52 指的那条线**不是**病根。指纹（Scissor Max Y）是否 607→599 仍值得单独记录，但它与目视楔形无因果 |
| N2 | **CS 函数地址 32 位截断**（`fn_addr`）⇒ 楔形 | **真机** | v82 上机后楔形**依然在**（`/sdcard/MG/shots82/v82_19.png`，01:13，FPS 17；地形正确 = 秋树/河/云都对，巨大黑三角仍在，边界为长直斜线，左上残影、底部粉色渐变带） | 结案：v82 是**正确的防御性修复**（见 A.8：我们的 VA 模型下 32 位截断是"活的"风险），但**不是**楔形病根 |
| N3 | 续租写描述符越界、踩掉 geom_buf | **代码** | `v12_pack.h:9933-9940`：`MALI_TILER_HEAP` = **40 B** ≪ 4096 | 排除 |
| N4 | "不发 `FRAGMENT_COMPLETED`" 是自研缺陷 | **代码** | `cmd_draw.c:4723-4736` 与上游 `main` `4658-4671` **逐行相同**；`MAX_LAYERS_PER_TILER_DESC = 8`（v12） | 排除（上游行为） |
| N5 | 续租后存在指向已销毁 ctx 的**悬垂指针** | **代码** | `gpu_queue.c:2763-2765`：ctx 存的是**描述符地址**（稳定）；续租只改内容 | 排除（但正因此造成 A.4-F2 的"静默切换"） |
| N6 | §9.5「探针像素精确 ⇒ 就是 tiler heap 数据问题」 | **逻辑** | 见 A.1：只能排除全局错误；多边形链表**头指针**（tiler context，不在堆里）、顶点缓冲、desc ringbuf 同属嫌疑人 | 削弱（不能单独成立） |
| N7 | **A.4-F3 的主要窗口**（"排水只等 CS seqno ⇒ 在飞 tiler 撞上换堆"） | **代码（新证据，见 A.9）** | `gpu_queue.c:1520-1595`：每个 ring entry 在写 seqno **之前**执行 `cs_wait_slots(&b, dev->csf.sb.all_mask)`（等**全部** scoreboard 槽），之后才 deferred `cs_sync64_add` 到 `cell->seqno`；而 `cs_vt_end`（`HEAP_OPERATION(VERTEX_TILER_COMPLETED)`，`cmd_draw.c:4271/4292`）正是以 `cs_defer_indirect()` 发出的 indirect async op，其 signal 槽 ∈ `all_iters_mask` ⊆ `all_mask` | **否证**：seqno 前进 ⟹ 全部槽退休 ⟹ VT_END（"vertex tiler completed"）退休 ⟹ tiler 对该堆的写入已结束。我此前把"CS seqno"与"tiler 退休"对立起来是**错的** |
| N8 | **续租被静默跳过**（A.4-F1） | **真机（logcat）** | v82 那局：`renew begin=9832 / renew end=9828` ⇒ **4 次开了没结束**；v81 那局 1703/1703 配平 | **命中**：F1 从"代码可读确证"升级为**实测发生**。注意 `begin` 由 `kbase_renew_tiler_heap()` 打印、`end` 只在其成功路径打印，所以 4 次即 4 次"被 `try_destroy` 拒绝后提前 return" |

> 说明：N7 与 N8 方向相反、互不矛盾——**换堆的"安全性前提"（tiler 已停）站得住，但"换堆会不会发生"这条状态机有缺陷**。这也把 `PANVK_KBASE_HEAP_RENEW_INTERVAL=0` 的 A/B 变成更强的判决实验：若楔形消失，则相关机制**不是**"在飞 tiler"，应优先怀疑"跳过 ⇒ 堆长期不回收 ⇒ 反复走 TILER_OOM/oom_fbd 路径"（一个**尚未被否证**的新候选）；若楔形照旧，则堆生命周期整条线出局。

### A.8 VA 宽度审计：我们是否依赖「私有缓冲落在前 4G」？（配 c4673eab / f333dd6d / issue #15551）

**结论（一句话）**：**我们代码里"看起来"有 <4G 的私有堆，但在 kbase 上整条路径被 `AUTO_VA` 旁路 ⇒ 设备上根本不存在任何 VA 区间保证，所有 GPU VA 都是 kbase（SAME_VA）给的 48 位值。因此 32 位截断类 bug 在我们的树上是"活的"、且通常一击即中——v82 修的那一处只是这一类里的一个实例，但它恰好是本树唯一现存的实例（A.8.3 的扫描结论）。**

#### A.8.1 两个上游提交讲了什么

- **`f333dd6d`**（2026-09-23，Boris Brezillon，MR!44651）`panvk: Don't force non-executable buffers to live in the first 4G`：
  上游原本把**所有私有缓冲**限制在前 4G（`PANVK_PRIV_VA_HEAP = user_va_start .. MIN(…, 1<<32)`），而**公有**堆从 `1ull << 32` 起（`init_va_heaps()`）。该提交把限制**收窄到"可执行缓冲"**（重命名为 `PANVK_EXEC_VA_HEAP` / `PANVK_NO_EXEC_VA_HEAP`），理由原文：**只有可执行缓冲不能跨 4G 边界，否则会破坏 blend shader 的逻辑**；`panvk_priv_bo_create()` 改为按 `flags & PAN_KMOD_BO_FLAG_EXECUTABLE` 选堆。
- **`c4673eab`** `panvk: Revisit the heap selection logic`（同日同系列）：重构堆选择（`panvk_device.h` +56/−13、`panvk_vX_device.c` +44/−47 等 7 文件）。

#### A.8.2 我们树里对应位置是什么（文件:行 证据）

| 事实 | 证据 |
|---|---|
| **kbase 上一律强制 `AUTO_VA`** | `panvk_vX_device.c:512-519`：`uint32_t vm_flags = PAN_ARCH < 10 ? PAN_KMOD_VM_FLAG_AUTO_VA : 0;` 然后 `if (physical_device->kbase_node_path[0]) vm_flags = PAN_KMOD_VM_FLAG_AUTO_VA;`（注释：kbase 自管 GPU VA，SAME_VA 模型，无法按调用者指定地址映射） |
| 我们的 VA 堆**结构**（旧式：私有 <4G、公有 ≥4G）仍然存在 | `panvk_vX_device.c:562-580`：`const uint64_t low_va_end = 1ull << 32;` → `as.heap` 从 `low_va_end` 起（≥4G）+ `as.priv_heap` = `user_va_start .. low_va_end`（<4G，或 AS 较小时与 `as.heap` 合并） |
| **但它们在 kbase 上完全不走** | `panvk_priv_bo.c:62-70`（`if (!(vm->flags & AUTO_VA)) { … dev->as.priv_heap … }`）、`panvk_device_memory.c:151/237/298`、`panvk_buffer.c:126`、`panvk_image.c:855` 全部带同一 `AUTO_VA` 守卫 ⇒ 设备上 `as.heap`/`as.priv_heap`/`as.fixed_heap` 是**死代码** |
| 上游那两个补丁**无法直接套到我们树上** | 它们改的是 `heaps[PANVK_PRIV_VA_HEAP]` 数组 + `PANVK_*_VA_HEAP` 枚举 + `panvk_va_heap_fallback()` + `PANVK_KMOD_BO_FLAG_EXECUTABLE` 选堆；我们的树里这些标识符**一个都不存在**（`grep PANVK_PRIV_VA_HEAP\|PANVK_PUB_VA_HEAP\|PANVK_EXEC_VA_HEAP\|PANVK_NO_EXEC_VA_HEAP\|PANVK_FIXED*` = **0 命中**），API 已被重构成 `panvk_as_alloc(dev, struct util_vma_heap *, …)`（`panvk_device.h:236`） |

**对 Lead 关键问题的直接回答**：
1. **我们是否依赖"私有缓冲落在前 4G"这个隐含前提？** —— **代码结构上"是"（`as.priv_heap` 确实被初始化成 <4G），但在 kbase 上这条路径不执行，所以设备上"不是"：我们没有任何区间保证。** 换句话说，上游用"把私有缓冲钉在前 4G"来**兜住** 32 位地址假设；我们**既没有那个钉子、也没有那个假设的替代品**。
2. **如果 VA 到了 4G 以上会怎样？** —— 任何把地址收窄到 32 位的代码都会立刻失效；而且由于 SAME_VA，`addr.dev` 在 64 位 Android 上通常就是 CPU 侧映射地址（典型 `0x7f…`，远高于 4G），**不是边界情形，是常态**。⇒ `4ecc4966` 修的正是这一类；我们在 v82 修掉了本树唯一的一处（见 A.8.3）。
3. **"我们只修了其中一处"是否成立？** —— **不成立（好消息）**：按下面 6 类模式系统扫描，**本树没有第二处**。但这条结论的强度受限于"模式可枚举"（见 A.8.4）。

#### A.8.3 系统扫描（把 `4ecc4966` 那一类扫干净）

- 工具：`/root/v82work/scan3.py`（源 `scan_va_narrowing.py` / `scan2.py`），扫描范围 `src/panfrost/{vulkan,lib,genxml}` 共 **162 个 .c/.h**（排除 `*.bak*`、生成头、测试）。
- **仪器先自检**：把 v82 的**补丁前**文件（`.bak-v82-1791220062`）喂给扫描器 ⇒ 必须报出 `uint32_t fn_addr =`（第 2114 行，**多行声明**）。第一次扫描**漏报**了它（RHS 在下一行），修正"续行合并"后 **`INSTRUMENT_VALIDATED: True`**，随后复跑全树 ⇒ 结论可信。**这条自检本身就是方法论要点：只扫单行会漏掉本案原型。**

| 模式 | 命中 | 真阳性 | 说明 |
|---|---|---|---|
| 窄类型变量 ← 64 位地址（含最多 3 行续行） | 51 | **0** | 全是 `unsigned arch = pan_arch(…gpu_id)`、`int ret = …munmap(…addr.host)` 之类的假阳性 |
| `(uint32_t)` / `(uint16_t)` / `(int)` 强转地址 | 1 | **0** | 唯一命中是上游 `genxml/decode_common.c:219` 对**差值**的 `(int)`（pandecode 打印用，无害） |
| `cs_move32_to(b, reg, <地址>)` | 0 | 0 | — |
| 窄类型结构体字段持有地址（名字含 addr/_va/gpa/dev） | 8 | **0** | 全是**计数/偏移/变体**：`crc_header_addr_count`（计数）、`dump_addr_offset`、`tracebuf_addr_offset`（偏移）、`gpu_variant`、`num_varying_attr_descs` 等 |
| 窄宽度位域持有地址（`addr : ≤32`） | 0 | 0 | — |
| 成员名含 addr/_va/gpa ← 64 位地址 | 1 | 0（且不在编译路径） | `jm/panvk_vX_cmd_draw.c:1836`（Job-Manager 路径；v12 走 `csf/`，且属上游代码） |
| 以 `%u/%x/PRIu32` 打印地址 | 0 | 0 | — |
| 函数返回：窄返回类型返回地址 | 13（变更文件内） | **0** | 逐一核对返回类型：`panvk_device_memory.c:554` = `VKAPI_ATTR uint64_t`、`csf/panvk_vX_gpu_queue.c:245` = `static uint64_t`、`panvk_physical_device.c:1178` = `float`（时间因子，假阳性） |

⇒ **本树不存在第二处 `4ecc4966` 同型缺陷**；v82 是这一类在本树的**唯一**实例，现已修复。

#### A.8.4 零成本证实"VA 真的在 4G 以上"（**用 Lead 手上已有的 logcat**）

不需要新代码、不需要新跑一局：

```bash
adb logcat -d | grep -oE "CKPT host heap renew #[0-9]+ begin \(old ctx 0x[0-9a-f]+" | tail -5
adb logcat -d | grep -oE "ring VA 0x[0-9a-f]+" | tail -5
```

- 打印点：`gpu_queue.c:3305`（`begin (old ctx 0x%PRIx64)`）、`:3339`（`end (new ctx 0x%PRIx64)`）、`:2202`（`ring VA 0x%PRIx64`）。
- 判读：若这些值是 `0x7f…` / 明显 **> 0x100000000** ⇒ "我们的 VA 在 4G 以上"被**实测证实**，A.8.2 的结论从"代码推断"升级为"实测"；同时它也是"N2（v82 靶心）为什么会真炸"的直接旁证。
- **注意**：v82 那局有 **9832** 条 `renew begin` ⇒ 这些行一定在 logcat 里，随手可查。

#### A.8.5 未决与限制

- 扫描是**模式枚举**：能覆盖"类型收窄"（变量/字段/位域/参数/返回值/强转）与"32 位 CS 寄存器写地址"，但**不能**覆盖语义型收窄（例如把地址存进一个 `uint64_t` 字段、但**写入描述符的 32 位字段**——那属于 genxml 字段宽度问题，需要逐字段核对 genxml；上游同样如此）。⇒ 结论限定在"上述 6 类模式内为 0"。
- 未验证 `c4673eab` 是否包含**除**堆选择以外、与我们相关的改动（我只核对了它与 `f333dd6d` 的文件清单与堆相关 hunk）。
- A.8.4 的两条 grep **我没有执行**（不碰设备）；它是给 Lead 的一步操作。

## B. v82 收尾（上游 4ecc4966）

### B.1 本轮边界（诚实声明）

| 做的事 | 没做的事 |
|---|---|
| 复核 Lead 的 v82 分片（16 片：字节数 + sha256 + `cmp` **逐字节**） | **没有改源码**（v82 = Lead 已落的一行改动） |
| 独立复核 `.so`/两 APK/APK 内嵌驱动的 sha256 与字节数、ZIP 条目表 | **没有重新编译**（不写构建目录） |
| 逐字对照本地改动与上游 `4ecc4966` 的原始 diff | **没有操作手机**：未安装、未拉起、未截图 |
| 审计自研 kbase 续租代码（只读；A 部分） | 未跑 pandecode、未取 logcat（设备侧归 Lead） |
| 写本报告 + `/root/v82work/audit-notes.md` | 未碰 `/root/mesa`、`/root/MobileGL`；未 `rm`；未 `git checkout/stash/reset` |

### B.2 补丁逐行（1 处，与上游逐字对照）

`src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c:2111-2116`（`get_fb_descs()` 内，`calc_fn_set_fbds_provoking_vertex_idx()` 分支）：

```diff
@@ -2111,7 +2111,7 @@         （本地行号；上游对应 @@ -2070,7 +2070,7 @@）
          uint32_t fn_idx = calc_fn_set_fbds_provoking_vertex_idx(cmdbuf);
          uint32_t fn_stride =
             dev->draw_ctx->fn_set_fbds_provoking_vertex_stride;
-         uint32_t fn_addr =
+         uint64_t fn_addr =
             dev->draw_ctx->fns_bo->addr.dev + fn_idx * fn_stride;
          cs_move64_to(b, addr_reg, fn_addr);
          cs_move32_to(b, length_reg, fn_stride);
```

**与上游 `4ecc4966` 的对照结论：逐字一致**（`−`/`+` 两侧文本、以及全部上下文行完全相同；唯一差异是 hunk 头行号：上游 `2070`、本地 `2111`，因本树基线落后上游 + 自带 DIY 改动）。
- 备份：`…/csf/panvk_vX_cmd_draw.c.bak-v82-1791220062`（192539 B）——注意它 = v81 已补丁状态（即 v82 相对 v81 只多这一行）。
- 语义：`fn_addr` 是"给 fb descriptor 打补丁的 CS 函数指针"，随后 `cs_move64_to(b, addr_reg, fn_addr)` 写入 64 位寄存器。VA 一旦 > 4 GiB，32 位截断会让 GPU 跳到错误地址 ⇒ 描述符/裁剪被写坏 ⇒ 几何越界/花屏/CS 错误。

### B.3 确定性校验（两次重编）

- 日志：`/root/v81work/v82_buildA.log`、`/root/v81work/v82_buildB.log`（各 12 步，集合相同、顺序不同）：
  `[1]` 生成 `src/git_sha1.h` + **5 个编译**（csf `panvk_vX_cmd_draw.c` 的 v10/v11/v12/v13/v14）+ 5 个静态库链接 + 1 个共享库链接 = 12 步。
- **诚实标注**：与 v81 一样，日志**不含 sha 行**；「两次 sha 相同」来自 Lead 的执行记录，日志只能佐证**重编对象集合一致**。本会话独立复核的是**当前**产物哈希（B.4）。
- 只重编 5 个对象是对的：改动在 **csf** 文件里（v81 改的是 csf + 通用两个文件 ⇒ 12 个对象）。

### B.4 产物与哈希

| 产物 | 字节 | sha256 |
|---|---|---|
| `/root/final/libvulkan_panfrost_v82.so` | 20053424 | `bd61a11da408e90c61504bc91da1225e25082dc3d9e0bdb684e238612acd47a2` |
| `/root/final/mgl-panvk-v82.apk`（diag） | 10220079 | `65111cd1ec21329a25f98d58104e67cb7d5dcb3b13701aad8e85698f5f6b5552` |
| `/root/final/mgl-panvk-v82-nodiag.apk` | 10220079 | `7f0cfc7a76cb4e7db807a3d3db4a86143c9acb0ae347947cff016cd975fd6ce2` |
| 上一版对照 `libvulkan_panfrost_v81.so` | 20053408 | `2e38470d8e32486c22e10df29119a118e1f1ac6d8166244deace3cb74f95dde6` |

APK 内嵌 `lib/arm64-v8a/libvulkan_freedreno.so`：两版均 20053424 B、sha256 = `bd61a11da408e90c61504bc91da1225e25082dc3d9e0bdb684e238612acd47a2` ✅（与 `.so` 一致）。

| ZIP 条目 | v82 | v82-nodiag |
|---|---|---|
| `AndroidManifest.xml` | 3580 B · `956b4261e11f878496521cd5fc4223ebb991fae23f260e6372f7a5b37625fc48` | 3572 B · `1f488a52b3713aee47ce6f9b642d8d8a3dac900ac91f4ff3e4777ea17bebe500` |
| `resources.arsc` | 40 B · `1fa3cb291285348ec1b33c85e7317d989467707f4791c4e5302f2d312d1e18c8` | 同左（逐字节相同） |
| `lib/arm64-v8a/libMobileGL.so` | 16956584 B · `72919c73a7e07630f329bb4ad9605c606a9aac9e2fc6596683ee7b9f9c76848b` | 同左 |
| `lib/arm64-v8a/libvulkan_freedreno.so` | 20053424 B · `bd61a11da408e90c61504bc91da1225e25082dc3d9e0bdb684e238612acd47a2` | 同左 |
| `classes.dex` | 1328 B · `6bd3abde2c53506f1b54bb88a8069c3cc394c2fb08440e4ca475cbf8fd64f4ad` | 同左 |
| `META-INF/*`（.SF/.RSA/MANIFEST.MF） | 620/1337/493 B（两版不同 = 签名块） | — |

⇒ 两版 APK 的差异**只在** `AndroidManifest.xml` + 签名块。打包脚本 `/root/pack_v82.sh` / `pack_v82_nodiag.sh`：`versionCode=82`、`versionName` `6.22-noVAtunc`，nodiag 版把 `PANVK_DEBUG=1,kbase_diag` → `PANVK_DEBUG=0`。
交叉佐证：`libMobileGL.so`（`72919c73a7e07630f329bb4ad9605c606a9aac9e2fc6596683ee7b9f9c76848b`）与 `classes.dex`（`6bd3abde2c53506f1b54bb88a8069c3cc394c2fb08440e4ca475cbf8fd64f4ad`）在 v81-nodiag 与 v82-nodiag **逐字节相同** ⇒ v81→v82 只有驱动在变。

### B.5 切分与重组校验（**独立复核 Lead 的 16 片**）

切法同前：前 7 片各 1277510 B、末片 1277509 B（两套相同）。

| 套 | `cat v82p8_*`/`v82n8_*` \| sha256sum | 与 APK 比对 | `cmp` 逐字节 |
|---|---|---|---|
| diag `v82p8_00..07` | `65111cd1ec21329a25f98d58104e67cb7d5dcb3b13701aad8e85698f5f6b5552` | = `mgl-panvk-v82.apk` ✅ | ✅ 相同 |
| nodiag `v82n8_00..07` | `7f0cfc7a76cb4e7db807a3d3db4a86143c9acb0ae347947cff016cd975fd6ce2` | = `mgl-panvk-v82-nodiag.apk` ✅ | ✅ 相同 |

**与 Lead 给的数字一致**（`65111cd1ec21329a…`、`7f0cfc7a76cb4e7d…`）⇒ 采纳 Lead 的更正（v82n8 = `7f0cfc7a…`）。

每片 sha256（复核用）：

| 片 | sha256 | 片 | sha256 |
|---|---|---|---|
| `v82p8_00` | `5e1003c4390b8713e9ea2bcf45147c452048de45274daa37358faeee1758d954` | `v82n8_00` | `ecb8aa7a2b95ef52414711d65f422306c18428d3ccdaad38b4a7b4ca4cf65adc` |
| `v82p8_01` | `faec391e19e5894c1a21d4974438b562152ccbde5875c87a4815a7051aa615a5` | `v82n8_01` | `dc0ff2bd60227ed3c1e3a8acd4d8d642c6544ae68b91877095137ce2eb694fc7` |
| `v82p8_02` | `d3544eb941bf68ecc2e2d5688180fb3345f56a5d31ba3bbc0f2f545b59a3514b` | `v82n8_02` | `f7c554f57736eb575253ea8c0b841c38ff1c7b9aa3946314b4f13b872aa94f32` |
| `v82p8_03` | `ec2a1d9590a54c84fb1fd66d0a94806c43d57105ebdbed517bc25dcba886efdf` | `v82n8_03` | `318c56f5db53311e25cf857755f6e8077a1baa96765b29847a266d75708c3b51` |
| `v82p8_04` | `c050d6b1484bfb00992ea964d8d09a8261a06438f63238de12e0aac3f1040664` | `v82n8_04` | `eb33b969d22ffb87ce641735cdce1c197ee207668ed841c88d971fbe0277dccc` |
| `v82p8_05` | `93d84bce36d202f39dc614cfaf8a4ffaf3da87a407f33c65f693042dce8e3a47` | `v82n8_05` | `9772cf907b480922309ed04c5fef40bc5f896ea0b802c573121c8fbdec9c88c4` |
| `v82p8_06` | `5f73da95cbfaf08feb215f19c4849382714da667cb22720f44337a281a907664` | `v82n8_06` | `7eaf3e956f39677b39ee367633fcac5402cf1869f808ba49aa527215b245afb6` |
| `v82p8_07` | `aac7b933fd18a165ba4e177f01e3e0dda5348b5ea36c0747a8ea63585798b086` | `v82n8_07` | `a9b1c745d60a8828194be7ec76068219da491b6e88a4f7e47b5cbbde1dc0e001` |

> 注：两套之间只有 4 片哈希相同（`…01/02/03` 等），这是**正常**的——两版 APK 的 manifest 长度不同（3580 vs 3572 B），分片边界因此错位，仅重合区段相同。

### B.6 上机判读表（v82，设备侧）

靶心 = `get_fb_descs()` 里那个被截断的 CS 函数地址（B.2）。

| 观察 | 含义 | 下一步 |
|---|---|---|
| 崩溃族（空指针/越界）**减少**、`kbase: CS error`/fault 消失 | v82 命中：`fns_bo` 的 VA 确实 > 4 GiB，旧码把地址截断后 GPU 跳错地址 | 记录 logcat + 截图；把这条并入"已完成修复"清单，回到楔形线（A） |
| 出现过的 CS fault 地址是**低 32 位形态**（高 32 位为 0，而 BO 实际在高位） | 截断的**直接指纹** ⇒ 即使在 v81 也能事后确认 | 用同一判据回看 v81 的 logcat（`grep -nE "kbase: CS error\|fault 0x"`） |
| 楔形**无变化** | 预期之内：v82 靶心是地址截断，**不是**楔形 | 不要因此回滚；按 A.3 做 `PANVK_KBASE_HEAP_RENEW_INTERVAL=0` 的 A/B |
| 楔形**变差**或新一轮 CS fault | 说明截断修复改变了 fn 地址的落点/时序，暴露了别的问题 | 回滚到 v81（B.7），保留 logcat |

纪律同前：**判画面用 nodiag 版**（`PANVK_DEBUG=0`）；同场景同视角；一次只下一个结论。

### B.7 回滚（v82 → v81）

```bash
cd /root/zenithblue/work/mesa/src/panfrost
cp -a vulkan/csf/panvk_vX_cmd_draw.c.bak-v82-1791220062 vulkan/csf/panvk_vX_cmd_draw.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
ninja -j2 -C /root/zenithblue/build/android-v4
sha256sum /root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so
#   期望 = v81：2e38470d8e32486c22e10df29119a118e1f1ac6d8166244deace3cb74f95dde6 (20053408 B)
```
注意备份 `.bak-v82-1791220062` = **v81 已补丁**状态（v82 只多一行）⇒ 回滚它即回到 v81，不会连带回滚 MR!44816。打包用 Lead 自己的 `pack_v81*.sh`（输出 `/root/final/mgl-panvk-v81*.apk`）。

### B.8 未做 / 未验证（诚实清单）

1. **真机未验证**：v82 未安装、未跑画面、未取 logcat；B.6 全部为预期，不是结论。
2. **两次重编 sha 相同**：来自 Lead；A/B 日志无 sha 行（B.3）。
3. **A 部分（续租审计）全部是代码级静态分析**：未编译验证、未真机验证；F1/F5/F6 是**代码可读确证**的缺陷，但"是否在真机上发生/是否就是楔形病根"**未验证**。F3 是**待证伪的领先假设**（其窗口前提见 F4，我未能确证）。
4. **F3 的机制未做硬件层验证**：固件如何使用 `FINISH_FRAGMENT` 回传的空闲块、以及跨代际块归属是否真会混淆，属**内核侧**（kbase 闭源）行为，我只能从 uAPI（`KBASE_IOCTL_CS_TILER_HEAP_INIT/TERM`）与 panfrost 侧代码推断 —— 标注为推断。
5. 本报告**未**量化续租频率与楔形出现率的相关性（需 logcat/真机数据，设备侧归 Lead）。
6. 我未复核 `max_chunks`/`chunk_size`/`target_in_flight` 这些取值对 G720 是否最优（只记录了取值来源：`MAX2(…, 200)`、`65535`、`MIN2(…, UINT16_MAX)`）。
7. 「v81 上机楔形完全没变」这一事实来自 Lead（连拍帧 `/sdcard/MG/shots/v81_05.png`）；我**没有**看过该帧，也未做像素级对照。

### B.9 过程记录（一处需要披露的小事故）

复核 v82 分片时，我的一条 shell 重定向 `cat v82p8_* > /root/v82work_rejoin_p.apk` 因 `||` 兜底顺序问题，先把文件创建在了 **`/root/v82work_rejoin_p.apk`（超出我的写域）**，随后用 `mv`（**未用 `rm`**）移入允许目录 `/root/v82work/rejoin_p.apk`，并在同一目录完成 `cmp`。最终写入清单：`/root/research/56-v82-novatunc.md`、`/root/v82work/{audit-notes.md,rejoin_p.apk,rejoin_n.apk}` —— 无残留、无越域文件。

---

## 附：本次审计用到的关键取证命令（只读）

```bash
M=/root/zenithblue/work/mesa/src/panfrost
F=$M/vulkan/csf/panvk_vX_gpu_queue.c
grep -n "kbase_retired_heap\|kbase_heap_renew\|emitted_jobs" $F
sed -n '3274,3341p' $F          # try_destroy / renew
sed -n '3890,3940p' $F          # P2 + 续租块 + 无条件清零
sed -n '3789,3810p' $F          # 排水只遍历 graphics mask、跳过 0
sed -n '1684,1720p' $F          # seqno cell 判据 + “异步作业可能仍在运行”注释
sed -n '2763,2771p' $F          # ctx 存描述符地址（代际不可见）
sed -n '1337,1344p' $M/vulkan/csf/panvk_vX_cmd_draw.c   # 每作业从 ctx load
sed -n '4700,4736p' $M/vulkan/csf/panvk_vX_cmd_draw.c   # “chunk 不被过早释放”注释 + td_count 分支
grep -n -A18 "TILER_HEAP" /root/zenithblue/build/android-v4/src/panfrost/genxml/v12_pack.h | head -20
grep -n "HEAP_SET\|HEAP_OPERATION\|First Heap Chunk" $M/genxml/v10.xml
```
