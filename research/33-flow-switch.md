# 33 · 流切换 / endpoint 家族 —— 现场证据修正 + 3 个候选修复 + 1 个定位诊断

> **对象**：`/root/zenithblue/work/mesa`（工作树 = v65 wrapper 改动 + v66 诊断，**只读**）。
> **现场**：`/sdcard/MG/cap.txt` 15:12:31（**v66 诊断实跑**，PID 19096）+ 14:48:16（v65，PID 27047）。
> **纪律**：本轮未构建、未操作手机、未改主树、未改 git；本文件是唯一写入。
> **标注**：【已定论】= 有 file:line / 日志原文支撑；【推断】= 由已定论演绎；【未验证】= 无直接证据。

---

## 0. 一页结论（先读这段：前 3 条**推翻**既有前提）

1. 【已定论·**推翻**】**CALL 已经被取走，callee 已经跑了。** v66 现场 sq1/sq2 的
   `stream progress 0x1`（bit0 = `CMDBUF_START`）就是铁证：wrapper 在 CALL **之前**把 progress 字清零
   （`gpu_queue.c:901-909`），而 `CMDBUF_START` 是**被 CALL 的那条流的第一条指令**（`cmd_buffer.c:877-878`
   在 `init_cs_builders()` 里发射）。字被置位 ⇒ 固件确实转移控制进了 callee。
   ⇒ **"extract 精确停在 CALL" 的正确读法是 (b)：CALL 未返回期间 extract 停在 CALL 上**，
   **不是** "CALL 没被执行"。从 v54 到 v66 的每一次 "停在 CALL" 现场都必须按这个新读法重读。
   ⇒ v65（删 wrapper 的 `SB_MASK_STREAM`）无效是**必然**的：它改的是 CALL **之前**的流状态，
   而卡点在 CALL **之后**的 callee 里。

2. 【已定论·**推翻**】**user_io page2 不镜像 CS_KERNEL_OUTPUT_BLOCK。**
   v66 现场三个队列的 `CS_STATUS_CMD_PTR/WAIT/SCOREBOARDS/REQ_RESOURCE/BLOCKED_REASON/CS_HEAP_*`
   **全 0**，而 sq0 明明刚跑完 275 个 job（它的 `REQ_RESOURCE` 至少应是 `0xc`）。
   ⇒ v66 诊断里这一族读数（`page+0x40..0xD4`）是**噪声**，不是证据；
   ⇒ 更严重：`kbase_subqueue_wait_seqno()` 里"读 `output_page+0x80/0x88/0x8c` 判 CS 异常"的那段
   **永远读到 0**（同一块内存）⇒ `error 0x0` / `error_type 0` 从来就不代表"固件没报错"，只代表**我们失明**。
   可信的只有：page0 doorbell、page1 input、page2 **仅 +0x0/+0x4（EXTRACT）与 +0x8（CS_ACTIVE）**
   （`include/drm-uapi/mali_kbase_ioctl.h:392-401` 明文承诺的四个字段）。

3. 【已定论】**`active 0` 不能当"CS 被挂起"的判据。**
   `CS_ACTIVE`（page2+0x8）在 ARM 头里的定义是 *"Initial extract offset when the CS is started"*
   （`src/panfrost/lib/kmod/mali_kbase_csf_registers.h:145`）。我们**从不写 `CS_EXTRACT_INIT`**（见 §3）
   ⇒ 若该定义成立，`active` 恒为 0（= CS 是从 offset 0 被启动的），此时
   `kbase_subqueue_publish()` 的门铃快速路径（`gpu_queue.c:1041-1052`）**永远不触发**。
   两种读法（"active 标志" vs "启动时的 extract"）用候选 1 一次实验即可判定（§4.1）。

4. 【已定论】**卡点在 callee 的"主体"里，而且非常靠前。**
   - sq0（VT）：`extract == insert == 88256`，marks `pre/post/wait = 0x…113/0x…113/0x…113`（= job 275），
     progress `0x70000001`（bit0 + bit28/29/30 = `FINISH_BEFORE_WAIT`/`AFTER_WAIT`/`CMDBUF_DONE`）
     ⇒ **VT 的 job 275 完整跑完**。
   - sq1（FRAG）/sq2（COMPUTE）：`extract == 88096/88080`（分别 = 各自最后一条 ring entry 的 **CALL** 地址：
     entry 起点 22400 + 160/144，与日志里 `extract line ring[0..7]` 第 5 个字就是 `0x20007c7e00000000` 吻合），
     marks `pre_call = 0x…113(275)`、`post_call/post_wait = 0x…112(274)`（= 上一条 job 的陈旧值），
     progress `0x1`（只有 `CMDBUF_START`）⇒ **CALL 未返回，callee 停在极早的位置**。
   - sq2（COMPUTE）的 `COMPUTE_ENTER`（bit1，`cmd_dispatch.c:218`）都没置位 ⇒ 它连**第一个 dispatch 的入口**都没到。

5. **Q1 答案（收尾序列对比）：三条流的收尾序列是同构的，无法解释 FRAG/COMPUTE 独有的失败。**
   `finish_cs()`（`cmd_buffer.c:142-236`）由 `EndCommandBuffer()` 的三子队列循环（`cmd_buffer.c:306-313`）
   **同一份代码**发射：mark(`FINISH_BEFORE_WAIT`) → `WAIT(all=0xffff)` → mark(`FINISH_AFTER_WAIT`) →
   读 error → `FLUSH_CACHE2`(clean)+`WAIT(IMM_FLUSH)` → [debug 寄存器投毒] →
   `instr_end_work(CMDBUF)` → mark(`CMDBUF_DONE`) → `cs_end()`。
   所以"sq1/s2 没有 FINISH 位"**只能**说明它们没走到收尾，不能由"收尾缺指令"解释。
   真正的**不对称在主体**：VT 的 `flush_tiling()`（`cmd_draw.c:4193-4226`，`cs_vt_end` +
   `SYNC_ADD64` 都用 `cs_defer_indirect()`）是**生产者**；FRAG/COMPUTE 是**消费者**，
   它们主体里多出来的东西是"等别的子队列"（§2.3）。

6. **Q2 答案（消费进度回写）：缺的就是 `CS_EXTRACT_INIT` 这一步**（§3，候选 1）。
   `CS_EXTRACT` 是**固件写、我们只读**；`CS_INSERT` 是我们写；两者之间 uAPI 明文规定的第三个字段
   `CS_EXTRACT_INIT`（page1+0x8）**我们全树一次都没写**，而 panthor 在**每个 job** 发布 insert 前
   都会写 `input->extract = output->extract`。

7. **候选**（§4）：① 补写 `CS_EXTRACT_INIT`（低风险、同时判定 `active` 语义）；
   ② 三个子队列合并回**一个 kbase group**（对齐 panthor/panvk 的 CSG 结构 = 端点申报与调度单元）；
   ③ 删掉 ring wrapper 里**剩下 3 条 `SET_STATE`**（把 v65 的单变量实验做完，让 ring entry 与 panthor 逐条一致）。
   外加 §5 的**定位诊断补丁**（FRAG 中间标记 + 等待点标记）——**建议先做诊断**，否则下一次仍然只能推理。

---

## 1. v66 现场逐条解读（15:12:31，PID 19096）

| 项 | sq0 VERTEX_TILER | sq1 FRAGMENT | sq2 COMPUTE |
|---|---|---|---|
| `seqno / target / jobs` | **275 / 275 / 275（已完成）** | 274 / 275 / 275 | 274 / 275 / 275 |
| `insert` | 88256 | 88256 | 88256 |
| `extract` | **88256（== insert）** | 88096 | 88080 |
| `extract − last_job_offset(22400)` | 320（= padded entry 末尾） | **160（= CALL）** | **144（= CALL）** |
| entry 大小 | 288/320 B | 288/320 B | 272/320 B |
| 被 CALL 的 stream | `0x5ff7f16000/2472` | `0x5fec2b0000/2016` | `0x5fea61c000/1168` |
| marks pre/post-call/post-wait | `0x…113 / 0x…113 / 0x…113` | `0x…113 / 0x…112 / 0x…112` | `0x…113 / 0x…112 / 0x…112` |
| `stream progress` | `0x70000001` | **`0x1`** | **`0x1`** |
| `active`(page2+0x8) | 0 | 0 | 0 |
| `error` | 0x0 | 0x0 | 0x0 |

标记编码：`KBASE_SEQNO_MARK_PRE_CALL = 0x100000000000`、`POST_CALL = 0x200000000000`、
`POST_WAIT = 0x300000000000`（`gpu_queue.c:69-71`），低 32 位是 `target_seqno`
（`0x113 = 275`，`0x112 = 274`）。⇒ sq1/sq2 的 `post_*` 是**上一条 job（274）**留下的陈旧值，
即 job 275 的 CALL **没有返回**。

### 1.1 为什么"extract == CALL"≠"CALL 没执行"（本条最重要）

* wrapper 在 CALL 前清零 progress 字：`gpu_queue.c:901-909`（`cs_store32(...KBASE_SEQNO_STREAM_PROGRESS_OFFSET)` 在 `:907`）（`cs_store32(val32=0, addr64,
  KBASE_SEQNO_STREAM_PROGRESS_OFFSET)`，紧跟在 pre-call 标记之后，再 `cs_wait_slot(LS)`）。
* callee 的**第一条**指令就是 progress 标记：`init_cs_builders()` 里
  `cmd_buffer.c:877-878` 发射 `PANVK_KBASE_PROGRESS_CMDBUF_START`（bit0）。
* sq1/sq2 的 progress = `0x1` ⇒ 那条标记指令**retired** ⇒ **固件进过 callee**。
* 反过来 sq0 的 `0x70000001` 证明标记机制本身工作正常（不是"清零后没人写"）。
* ⇒ 结论：**extract 在 CALL 未返回期间停在 CALL 上**（CS 处于 callee 中）。这同时解释了
  §1.3 的 `active 0` 之谜与 v65 无效之谜。
* 【未验证】extract 是"CALL 未发射"还是"CALL 已发射未返回"的镜像，本报告不再依赖区分；
  两种读法都指向 **callee 内部**，而不是 ring entry。

### 1.2 14:48（v65）与 15:12（v66）的差别

| 运行 | sq0(VT) | sq1(FRAG) | sq2(COMPUTE) | entry 大小 |
|---|---|---|---|---|
| 14:48（v65，无 diag） | extract 63848 = 63744+**104**（CALL，未完成） | 63848 = +104 | 63832 = +88 | 152/136 B |
| 15:12（v66，带 diag） | **完成**（extract==insert） | 88096 = +160（CALL） | 88080 = +144（CALL） | 288/272 B |

⇒ v65 那次是**三个队列都卡**；v66 这次是 **VT 完成、FRAG+COMPUTE 卡**。所以"谁卡住"是可变的，
"卡在 callee 里"不变。14:48 那次没有诊断，无法判断 VT 当时是在 callee 里还是真卡住——**不要**再用
"三队列同时停在 CALL ⇒ 涉及 CSG 级共享资源"这条推理（它建立在旧读法上）。

---

## 2. Q1：FRAG/COMPUTE 的收尾到底缺什么？（结论：不缺，卡点在主体）

### 2.1 收尾指令序列逐条对照（同一份代码）

`finish_cs()` `cmd_buffer.c:142-236`，对 `subqueue ∈ {0,1,2}` 完全同构，调用点
`EndCommandBuffer()` → `cmd_buffer.c:306-313`：

| 顺序 | 指令 | 行号 |
|---|---|---|
| 1 | `kbase_mark_progress(FINISH_BEFORE_WAIT)` | `cmd_buffer.c:147-148` |
| 2 | `cs_wait_slots(b, dev->csf.sb.all_mask)` = `WAIT(0xffff)` | `cmd_buffer.c:149` |
| 3 | `kbase_mark_progress(FINISH_AFTER_WAIT)` | `cmd_buffer.c:150-151` |
| 4 | 读 `ctx->syncobjs[i].error`，非 0 则存 `last_error` | `cmd_buffer.c:153-170` |
| 5 | `FLUSH_CACHE2(clean)` + `WAIT(IMM_FLUSH)` | `cmd_buffer.c:176-190` |
| 6 | `instr_end_work(CMDBUF)`（utrace）/ debug 投毒 | `cmd_buffer.c:196-230` |
| 7 | `kbase_mark_progress(CMDBUF_DONE)` | `cmd_buffer.c:233-234` |
| 8 | `cs_end()` | `cmd_buffer.c:236` |

⇒ **收尾没有任何 FRAG/COMPUTE 特有的东西**，也不含 `cs_defer_indirect()`（那是主体里的
`issue_fragment_jobs()` / `flush_tiling()` 用的）。**"sq1/s2 缺 FINISH 位" 的成因只能是它们没执行到
`cmd_buffer.c:147`**，即卡在主体。

### 2.2 我们能看到的标记覆盖不全（**这就是定位不了的根因**）

* **VT 的中间标记（`VT_BEFORE_RUN_IDVS`…`VT_AFTER_SYNC_SIGNAL`，`panvk_cmd_buffer.h:64-69`）和
  FRAG 的全部中间标记（`FRAG_ENTER`…`FRAG_AFTER_FINISH`，`:71-76`）在整个树里零次发射**
  （`grep -rn "PROGRESS_FRAG_ENTER\|PROGRESS_VT_BEFORE_RUN_IDVS" src/panfrost/vulkan/` 只命中枚举定义）。
* 只有 COMPUTE 有真实中间标记：`cmd_dispatch.c:218`(ENTER)、`:302`(BEFORE_ITER)、`:308`(BEFORE_RUN)、
  `:338`(AFTER_RUN)、`:343`(AFTER_SIGNAL)。
* ⇒ sq1(FRAG) 的 `progress=0x1` 只说明"卡在标记 #1 与收尾之间"——**中间 2000 B 的指令全在盲区**。
  sq2(COMPUTE) 稍好：`COMPUTE_ENTER` 未置位 ⇒ 卡在"流头到第一个 dispatch 入口"之间。
* 【已定论】在补标记之前，任何"卡在哪条指令"的说法都是**猜测**。（§5 给出补标记补丁。）

### 2.3 FRAG/COMPUTE 共有、VT 没有的东西（按可能性排序，均【推断】）

**(a) 跨队列内存等待（最可能）**：FRAG 主体很早就会
`wait_finish_tiling()`（`cmd_draw.c:4245-4263`，调用点 `cmd_draw.c:4527`，在 `issue_fragment_jobs()` 里，
紧跟 OOM handler 设置之后），它
```
cs_load64_to(sync_addr, ctx, offsetof(ctx, syncobjs));
cs_add_imm64(vt_sync_point, cs_progress_seqno_reg(b, VT), rel_vt_sync_point);
panvk_instr_sync64_wait(cmdbuf, FRAGMENT, false, MALI_CS_CONDITION_GREATER, vt_sync_point, vt_sync_addr);
```
`cs_progress_seqno_reg(b, VT)` = **本 CS 自己寄存器堆里的 r116:117**
（`panvk_cmd_buffer.h:367` `PANVK_CS_REG_PROGRESS_SEQNO_START = 116`，`:427-432`）。
COMPUTE 走同一套机制的另一入口：`emit_barrier_csf()` → `emit_barrier_insert_waits()`
（`cmd_buffer.c:600-620`），wait 目标 = `progress_seqno[j] + cs_state->relative_sync_point`。
⇒ **VT 是生产者（不等待任何人），FRAG/COMPUTE 是消费者**——与"只有 VT 完成"完全一致。
⇒ 若这两个等待的目标值算错（`progress_seqno` 是**每个 CS 自己的副本**，靠各流末尾
`flush_sync_points()`（`cmd_buffer.c:113-139`）各自累加），等待就永远不满足。

**(b) 间接 deferred 操作数（本任务书指的那一族）**：`cs_defer_indirect()`（`cs_builder.h:806-814`）的
wait mask 取 `SB_MASK_WAIT`、signal slot 取 `SB_SEL_DEFERRED`，两者由
`cs_iter_sb_update_end()`（`panvk_cmd_buffer.h:750-767`）与
`cmd_draw.c:4636-4690`（`cs_set_state(SB_SEL_DEFERRED, next_sb / DEFERRED_FLUSH / DEFERRED_SYNC)`）维护。
FRAG 是三者中**最重**的使用者（`FINISH_FRAGMENT`+`cs_frag_end`+OQ 链的 flush/sync 全在 deferred 组里）。

**(c) 端点工作量与 CSG 结构**：FRAG 要 fragment endpoint、COMPUTE 要 compute endpoint，
VT 要 tiler+idvs。kbase 路径目前**给每个子队列建一个独立 CSG**（§4.2 候选 2），
每个 CSG 各自申报 `tiler_max=1 / fragment_max=64 / compute_max=64`
（`kbase_kmod.c:539-552, 572-585, 599-612`）——这与 panthor/panvk 的"一个 group 三个队列、
一份 `CSG_EP_REQ`"结构不同。

**(d) LS(slot 0) 被卡住 ⇒ 之后任何 `WAIT(LS)` 永久阻塞（【未验证】但要写下来）**：
wrapper 与 callee 都大量 `WAIT(LS)`（wrapper：`:905/908/931/951/955` 标记 LS 等待、`:919-920` IMM_FLUSH；
callee：每个标记的 `cs_flush_stores()`，`cmd_buffer.c:80`）。任何**已发射但永不完成**的
deferred op（例如 wait mask 含 `all_iters_mask` 的那些：`cmd_draw.c:4754/4862/4865`、
`cmd_dispatch.c:460/486`、`cmd_query.c:331/546`、`cmd_meta.c:123/215/218`）都会让 slot 0 一直非 0，
于是 callee **在第一条 touch slot0 的指令处就冻住**——这与"第一条标记 retired 之后就没了"的现象吻合。
【未验证】要点：需要 §5 诊断把"停在 WAIT(LS)"与"停在 SYNC 等待"分开。

---

## 3. Q2：消费进度（ring extract）回写路径全图

| 字段 | 位置 | 谁写 | 我们的实现 | panthor（权威对照） |
|---|---|---|---|---|
| `CS_EXTRACT` (u64) | **page2 + 0x0** | **固件** | 只读：`gpu_queue.c:754`/`:771`（`reserve_ring` 判空间）、`:1108`（等待循环）、`:1119`（`allow_ring_drain` 判据） | 读 `queue->iface.output->extract` |
| `CS_ACTIVE` (u32) | page2 + 0x8 | 固件 | 只读：`gpu_queue.c:1043-1044`（门铃快速路径）、`:1110` | 不读 |
| `CS_INSERT` (u64) | **page1 + 0x0** | **我们** | `gpu_queue.c:1035-1036` | `queue->iface.input->insert = job->ringbuf.end` |
| `CS_EXTRACT_INIT` (u64) | **page1 + 0x8** | **我们（应写）** | **全树 0 次写**（`grep -rn "CS_USER_IO_INPUT_CS_EXTRACT_INIT" src/` 只命中 uAPI 头定义） | **每个 job 前写**：`queue->iface.input->extract = queue->iface.output->extract;`（`panthor_sched.c::queue_run_job()`），且 `cs_slot_prog_locked()` 里也写一次 |

* uAPI 明文（`include/drm-uapi/mali_kbase_ioctl.h:392-401`）：
  `page 1: input page (CS_INSERT at 0x0, CS_EXTRACT_INIT at 0x8)` —— 所以这**不是**可选字段。
* ARM 头定义：`mali_kbase_csf_registers.h:139-140` `CS_EXTRACT_INIT_LO/HI 0x0008/0x000C`
  *"Initial extract offset for ring buffer"*；`:145` `CS_ACTIVE 0x0008`
  *"Initial extract offset when the CS is started"*。
* **为什么这是"缺的那一步"**：CS_EXTRACT_INIT 是固件**（重）启动该 CS 时**的起始 extract。
  我们让它恒为 0 ⇒ 任何 CS restart（CSG resume / 队列 kick / FW 内部 restart）都会**从 offset 0
  重放整圈 ring**（重放旧 entry = 重跑旧 stream，指向可能已回收的内存），而不是从固件真正的位置续跑。
  对**正在运行**的 CS 写它是等值写入（这就是 panthor 可以无条件每次写的原因）。
* **kbase_seqnos 语义（易混）**：我们的"消费进度"其实是**自己的** `struct panvk_cs_sync64` cell
  （`kbase_seqnos` BO，每个子队列一个），由 ring entry 尾部的 `SYNC_ADD64`（`gpu_queue.c:958-959`）
  递增，等待循环判 `cell->seqno >= target`（`:1117`）。它与 `CS_EXTRACT` **完全解耦**：
  ⇒ **"工作做完 ≠ extract 前进"是设计使然**（callee 未返回时 extract 停在 CALL，§1.1），
  反之"extract 前进"也**不**要求 seqno 前进。任何只解释其中一边的假设都不充分。
* 另有一条**死路**要写下来：`CS_EXTRACT` 是固件写的，**用户态没有任何办法写它**（uAPI 只给
  INSERT 与 EXTRACT_INIT）。所以"驱动侧能改的消费进度"只有 `CS_EXTRACT_INIT` 一处。

---

## 4. 候选修复（按可能性排序；未验证项已标注）

### 候选 1 —— 发布 insert 前把固件的 extract 回落进 `CS_EXTRACT_INIT`（panthor 队列 ABI 对齐）

#### 依据
* panthor 是这套 ring ABI 的权威使用者，它**每个 job 都写**：
  `panthor_sched.c::queue_run_job()`（web 取回，见 §6）：
  ```c
  	/* Make sure the ring buffer is updated before the INSERT register. */
  	wmb();
  	queue->iface.input->extract = queue->iface.output->extract;
  	queue->iface.input->insert = job->ringbuf.end;
  ```
  另一处 `cs_slot_prog_locked()` 也一样（"Program a queue slot …"）。
* uAPI 与 ARM 头都明文定义该字段（§3），我们**零次写**。
* 该写入对运行中的 CS 是等值写入（值 = 固件当前 extract），风险低。
* 【推断】它与 `active` 的语义判定共用一次实验（§4.1 的最小验证方法）。

#### diff（已用 `patch -p1 --dry-run --batch --forward` 对**实时工作树**验证通过：0 offset / 0 fuzz / 0 reject；`git apply -p1` 同样可用）
```diff
diff --git a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ -1026,12 +1026,24 @@
    struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
    struct panvk_subqueue *subq = &queue->subqueues[subqueue];
    uint8_t *input_page = (uint8_t *)subq->kbase.user_io + 4096;
+   uint8_t *output_page = (uint8_t *)subq->kbase.user_io + 8192;
 
    /* kbase_subqueue_emit_job() has already cleaned the CPU-written ring
     * cachelines and completed that clean with kbase_gpu_wmb().  Do not issue
     * a second full-system barrier here: it cannot make those writes any more
     * visible and only serializes the CPU submission path. */
 
+   /* [C1] Publish the firmware's own extract position as the restart offset.
+    * CS_EXTRACT_INIT (input page +0x8, see include/drm-uapi/mali_kbase_ioctl.h)
+    * is the offset the FW starts extracting from whenever it (re)starts this
+    * CS; leaving it at 0 makes any restart replay the whole ring from offset 0.
+    * panthor does exactly this before every insert (panthor_sched.c::
+    * queue_run_job() and cs_slot_prog_locked()).  For a CS that is already
+    * running this is a no-op write, which is why the kernel can do it
+    * unconditionally. */
+   *(volatile uint64_t *)(input_page + CS_USER_IO_INPUT_CS_EXTRACT_INIT) =
+      *(volatile uint64_t *)(output_page + CS_USER_IO_OUTPUT_CS_EXTRACT);
+
    *(volatile uint64_t *)(input_page + CS_USER_IO_INPUT_CS_INSERT) =
       subq->kbase.insert;
 
@@ -1039,7 +1051,6 @@
     * CS_ACTIVE again after ringing it; if a suspend raced the write, the
     * scheduler kick below safely resumes the group. */
    kbase_gpu_wmb();
-   uint8_t *output_page = (uint8_t *)subq->kbase.user_io + 8192;
    volatile uint32_t *active =
       (volatile uint32_t *)(output_page + CS_USER_IO_OUTPUT_CS_ACTIVE);
 
```
（唯一结构性动作：把 `output_page` 的声明提前到函数头，删除原第 1042 行的重复声明。）

#### 预期可观测差异
1. **`active` 读数变成非 0**（= 我们写进去的 extract 值）⇒ 证明 `CS_ACTIVE` == `EXTRACT_INIT` 镜像，
   同时**门铃快速路径开始生效**（`publish` 会走 `*active != 0` 分支，不再每次 ioctl kick）。
   若 `active` 仍为 0，则 `CS_ACTIVE` 是"活跃标志"而不是"启动 extract"——这本身也是结论。
2. **不再出现"extract 回跳/重放"**：任何 CS restart 都从固件当前位置续跑（`extract` 单调不减）。
3. 若本次挂起的真实成因是"CS 被 restart 且从 0 重放"，则 timeout 消失、存活时间变长。
   【未验证】成功率无法先验估计；这条主要是**契约修复**。

#### 风险
* 低。唯一需要确认的是"固件是否会在 CS 运行中把 EXTRACT_INIT 当作『立刻跳转』的目标"——
  panthor 的无条件写入是反证（它也跑同样的固件）。
* 若 `CS_EXTRACT` 与 `CS_EXTRACT_INIT` 的**单位/回绕**不同（例如 FW 的 extract 以 32 位回绕而
  init 需要同一编码），写入会不一致；缓解：先只做候选 1 一次 A/B，观察 `active` 与 extract 单调性。
* 该写入在 `publish()` 里，意味着**每次 submit 多一次 GPU 页读**（非缓存一致性页），代价可忽略。

#### 最小验证方法
1. 只上候选 1，跑同一条 v66 场景（PANVK_DEBUG=kbase_diag 保持开启）。
2. 判定表：
   * `active` 由 0 → 非 0 且等于同一时刻的 `extract` ⇒ EXTRACT_INIT 语义成立（并拿到门铃快速路径）。
   * 出现新的 `kbase: CS ring did not reclaim consumed space`（`gpu_queue.c:773`）⇒ extract 语义被改坏，立即回滚。
   * timeout 是否消失/延后（存活秒数、`seqno` 到达 target 的次数）。
3. 若 `active` 非 0 且 timeout 仍在，把该运行作为候选 2/3 的基线（多了一个"CS 可被正确 restart"的前提）。

---

### 候选 2 —— 三个子队列合并回**一个 kbase CSG**（对齐 panvk/panthor 的调度与端点申报单元）

#### 依据
* panthor 路径：`create_group()`（`gpu_queue.c:2283-2336`）**建一个 group、三个 queue**
  （`qc[VT/FRAG/COMPUTE]`，`:2291-2307`；`gc.max_*_cores` 见 `:2309-2323`），
  并把 `compute/fragment_core_mask` 原样申报给内核；内核再把它写成
  `CSG_ALLOW_COMPUTE/FRAGMENT/OTHER` + `CSG_EP_REQ_COMPUTE/FRAGMENT/TILER/PRIORITY`
  （panthor_sched.c `group_bind_locked`，见 §6 摘录：`csg_iface->input->allow_compute = group->compute_core_mask;`
  `endpoint_req = CSG_EP_REQ_COMPUTE(group->max_compute_cores) | CSG_EP_REQ_FRAGMENT(...) | CSG_EP_REQ_TILER(...) | CSG_EP_REQ_PRIORITY(priority);`）。
* kbase 路径：`kbase_create_group()`（`gpu_queue.c:1370-1458`）在**每个子队列循环里**
  各自 `kbase_kmod_csf_group_create(dev, 1, &subq->kbase.group_handle)`（`:1381-1389`），
  并用 `csi_index = 0` 绑定（`:1434-1437`）⇒ **三个独立 CSG，每个只有 1 个 CS**。
  kbase 的 group-create ioctl 本来就有 `cs_min` 字段表达"这个 group 需要几个 CS 槽"
  （`mali_kbase_ioctl.h:412` / `:438` / `:463`），我们固定传 1。
* 后果（【推断】）：端点（endpoint）申报变成 **3 份独立的 `CSG_EP_REQ`**，
  且三个子队列分属不同 CSG ⇒ 端点/共享 scoreboard 条目的分配与 FW 的组调度都与
  PanVK 命令流所假设的"同组三队列"不同。FRAG/COMPUTE 是"要 fragment/compute 端点"的那两个，
  VT 是"要 tiler/idvs"的那个——与"VT 完成、FRAG/COMPUTE 卡住"的现象方向一致。
* 【未验证】(i) 该设备 FW 是否允许一个 CSG 有 3 个 CS（`csif_info->cs_slot_count = groups[0].stream_num`，
  驱动已在 `kbase_kmod.c:466` 读到，建议先把它打到 `mesa_logi` 确认 ≥ 3）；
  (ii) scoreboard 槽到底是 per-CS 还是 per-CSG（这决定合并后 slot 语义是否变化）。

#### diff（已用 `patch -p1 --dry-run` 对实时树验证通过）
```diff
diff --git a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ -1358,12 +1358,14 @@
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
+   /* One CSG holds all three CSs now: terminate the queues above, then
+    * destroy the single group once. */
+   if (queue->group_handle != UINT32_MAX) {
+      kbase_kmod_csf_group_destroy(dev->kmod.dev, queue->group_handle);
+      queue->group_handle = UINT32_MAX;
    }
 }
 
@@ -1378,15 +1380,23 @@
    for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
       queue->subqueues[i].kbase.group_handle = UINT32_MAX;
 
+   /* PanVK's scheduling unit is *one* group holding the three queues: the
+    * panthor path builds exactly that (create_group(), this file) and kbase's
+    * group-create ioctl carries cs_min for the same purpose.  Three CSGs of one
+    * CS each give the FW three independent CSG_EP_REQ endpoint declarations and
+    * three independent scheduling states, which is not the structure the PanVK
+    * command streams are written against. */
+   if (kbase_kmod_csf_group_create(dev->kmod.dev, PANVK_SUBQUEUE_COUNT,
+                                   &queue->group_handle)) {
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
+      subq->kbase.group_handle = queue->group_handle;
 
       subq->kbase.ringbuf_bo =
          pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, KBASE_RINGBUF_SIZE,
@@ -1432,7 +1442,7 @@
       }
 
       subq->kbase.user_io = kbase_kmod_csf_queue_bind(
-         dev->kmod.dev, subq->kbase.group_handle, 0,
+         dev->kmod.dev, subq->kbase.group_handle, i,
          subq->kbase.ringbuf_dev,
          KBASE_RINGBUF_SIZE);
       if (!subq->kbase.user_io) {
```
（`kbase_kmod_csf_group_create()` 内部 `cs_min = cs_queue_count` 已经是参数化的，
`mali_kbase_ioctl.h` 三个 rung 都带该字段；无需改 `kbase_kmod.[ch]` 的签名。）

#### 预期可观测差异
* 启动日志出现 **一条** `kbase: created CSF group …`（现在是三条；注意 `mesa_logi` 当前没被 cap.txt 收，
  建议临时提到 `mesa_logw` 或把 `csif_info` 一并打印）。
* `/sys/kernel/debug` 不可读（无 root），所以**端点分配的差异不可直接观测**；
  可观测代理：`seqno` 首次 timeout 的出现时刻（存活秒数）、三个子队列是否**同时**卡
  （合并后应回到"三队列同组、共同起落"）以及 FRAG/COMPUTE 的 progress 位图是否前进。
* 若当前挂起的机制确实是"某个 CSG 拿不到 fragment/compute 端点"，合并后应消失或显著延后。

#### 风险
* **中**。这是结构改动：
  1) 需要 FW 允许一个 CSG 挂 3 个 CS（先确认 `groups[0].stream_num ≥ 3`）；
  2) 合并后三个 CS 若共享同组 scoreboard 槽，语义会从"各队列私有槽"变成"组内共享槽"
     （这正是 panvk/panthor 假设的，但也是本次唯一无法本地验证的变量）；
  3) `kbase_kmod_csf_queue_bind()` 的 `csi_index` 必须与 `group_handle` 匹配，
     销毁路径必须只 destroy 一次（diff 已处理）。
* 建议**单变量**上机：先只改 `cs_min` 与 bind 索引，不动其它任何东西。

#### 最小验证方法
1. 先只加一行日志确认 `csif_info->cs_slot_count`（`kbase_kmod.c:466`），≥ 3 才继续。
2. 上候选 2，同一场景；判定：
   * 驱动初始化成功且三个子队列都绑定成功（无 `Failed to bind a kbase CS queue`）；
   * 与候选 1/3 的基线对照"首次 timeout 的 seqno/存活秒数"；
   * 若出现 `KBASE_IOCTL_CS_QUEUE_GROUP_CREATE failed`（`kbase_kmod.c:614-615` 那条 mesa_loge）
     ⇒ 内核不接受 `cs_min=3`，立即回滚。

---

### 候选 3 —— 删掉 ring wrapper 里**剩下的 4 条 `SET_STATE`**（做完 v65 的单变量实验：ring entry 与 panthor 逐条一致）

#### 依据
* panthor 的 ring entry（`prepare_job_instrs()`，见 §6）**一条 `SET_STATE` 都不发**：
  `MOV32 flush_id / FLUSH_CACHE2 / MOV48 cs.start / MOV32 cs.size / WAIT(1<<16) / CALL /
   MOV48 sync_addr / MOV48 #1 / WAIT(all) / SYNC_ADD64.nowait / ERROR_BARRIER`。
  我们的 wrapper 在 CALL 之前多发了 4 条（`SB_SEL_ENDPOINT`/`SB_MASK_WAIT`/`SB_SEL_OTHER`/`SB_SEL_DEFERRED`，
  `gpu_queue.c:825-830`；`SB_MASK_STREAM` 已在 v65 删除）。
* PanVK 自己在 **init stream** 里一次性设置这些默认值（`gpu_queue.c:2081-2087`，
  即 `panvk_queue_init_contexts()`），并且它们的**权威维护者是流内的
  `cs_iter_sb_update_end()`（`panvk_cmd_buffer.h:750-767`）与 `cmd_draw.c:4636-4690`**——
  也就是说它们是**流状态**，不是 ring entry 状态。
* 【已定论】CALL 之前的流状态**不可能**影响 CALL 之后才发生的卡点（§1.1），所以候选 3
  **不是**本次卡点的直接解药；它的价值是：(i) 把 v65 的"wrapper 抢占流状态"假说**单变量做完**，
  (ii) 让 kbase ring entry 与 panthor 逐条同构，消除一个长期混淆变量。
* 【推断】删掉是安全的：init stream（在 kbase 上同样经由 wrapper 被 CALL）已经把同样的默认值
  写进 CS 状态，且 panvk 的流代码在每次使用前都会设置它们。

#### diff（已用 `patch -p1 --dry-run` 对实时树验证通过）
```diff
diff --git a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ -817,17 +817,16 @@
 
    cs_builder_init(&b, &conf, ring_buf);
 
-   /* The kbase wrapper executes LS stores, waits, flushes and the final
-    * deferred sync before/around the called PanVK stream.  The normal PanVK
-    * init stream also programs these scoreboard slots, but on kbase that
-    * stream is reached through this wrapper, so the wrapper has to make its
-    * own async slots valid first. */
+   /* These four SET_STATEs are *stream* state, not ring-entry state: the PanVK
+    * CS builder programs them at every iteration boundary
+    * (cs_iter_sb_update_end() in panvk_cmd_buffer.h) and switches SB_SEL_DEFERRED
+    * around its deferred groups (panvk_vX_cmd_draw.c, issue_fragment_jobs()), and
+    * the PanVK init stream (panvk_queue_init_contexts(), this file) sets the same
+    * defaults once per CS.  The panthor kernel's ring entry
+    * (panthor_sched.c::prepare_job_instrs()) emits no SET_STATE at all, and v65
+    * already removed the SB_MASK_STREAM write from here; this removes the rest so
+    * a kbase ring entry is instruction-for-instruction what panthor emits. */
 #if PAN_ARCH >= 11
-   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_ENDPOINT, SB_ITER(0));
-   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_MASK_WAIT, SB_WAIT_ITER(0));
-   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_OTHER, SB_ID(LS));
-   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_DEFERRED,
-                      SB_ID(DEFERRED_SYNC));
    /* SB_MASK_STREAM (and SB_MASK_WAIT / SB_SEL_ENDPOINT / SB_SEL_DEFERRED)
     * is *stream* state, not ring-entry state: the PanVK CS builder programs it
     * at every iteration boundary (cs_iter_sb_update_end() sets it to
```
（注：`gpu_queue.c:831-843` 那段 v65 写的说明性注释仍然有效，可原样保留——diff 只删代码块。
若删掉 4 条后 `MALI_CS_SET_STATE_TYPE_SB_SEL_OTHER` 在本文件再无引用，编译**不会**报错：
`cs_set_state_imm32()` 的 assert 只在使用处，宏定义在 `genxml/cs_builder.h`。）

#### 预期可观测差异
* ring entry 长度（带 v66 诊断时）：VT/FRAG **288 → 272 B**，COMPUTE **272 → 256 B**；
  不带诊断时：VT/FRAG **152 → 136 B**（19→17 条，CALL 从 +104 移到 **+88**），
  COMPUTE **136 → 120 B**（17→15 条，CALL 从 +88 移到 **+72**）。
  ⇒ 下一次 timeout 的 `entry 136/192` / `entry 120/192` 与
  `extract offset = last_job_offset + 88/72` 就是"这条改动真的生效"的**逐字节验证标记**
  （与 v65 用 `152/136` 验证的方式相同）。
* 若卡点真的与 wrapper 抢占流状态有关，timeout 应消失或显著延后；否则签名不变（这是预期结果，
  因为 §1.1 已证明卡点在 CALL 之后）。

#### 风险
* 低—中。若某个 callee **在第一次 `cs_iter_sb_update_end()` 之前**就发射 deferred-indirect 操作，
  它依赖的 `SB_MASK_WAIT`/`SB_SEL_DEFERRED` 就只剩 init stream 的值（= 本次删除的同一组默认值：
  `SB_WAIT_ITER(0) = BIT(3)`、`SB_ID(DEFERRED_SYNC) = 1`）⇒ 语义等价。
  【未验证】若 init stream 因任何原因（例如 `cs_is_empty()` 提前返回）没有在本 CS 上执行过，
  删除后这些寄存器会是**未初始化值**——这是唯一需要留意的退化路径。

#### 最小验证方法
1. 单变量上机，只看 ring entry 长度与 `extract offset`（不需要 timeout 也能验证生效）。
2. 与候选 1/2 分开做，避免多变量混合；建议顺序：**诊断补丁（§5）→ 候选 1 → 候选 3 → 候选 2**。

---

## 5. 定位诊断补丁（**建议先做这一个**；不是修复，是让下一次运行能定位）

**目的**：把"卡在 callee 的第几条指令附近"从推理变成读数。做法：把已有但**从未发射**的
FRAG/VT 标记插到"主体里每一个可能的等待点"前后，并把 COMPUTE 的等待点也标出来。

（已用 `patch -p1 --dry-run --batch --forward` 对**实时工作树**验证通过：两个文件、0 offset / 0 fuzz）
```diff
diff --git a/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c b/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
--- a/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
@@ -4524,7 +4524,13 @@
                             length_reg);
 
    /* Wait for the tiling to be done before submitting the fragment job. */
+   panvk_per_arch(kbase_mark_progress)(
+      cmdbuf, PANVK_SUBQUEUE_FRAGMENT,
+      PANVK_KBASE_PROGRESS_FRAG_BEFORE_TILING_WAIT);
    wait_finish_tiling(cmdbuf);
+   panvk_per_arch(kbase_mark_progress)(
+      cmdbuf, PANVK_SUBQUEUE_FRAGMENT,
+      PANVK_KBASE_PROGRESS_FRAG_AFTER_TILING_WAIT);
 
    /* Disable the oom handler once the vertex/tiler work has finished.
     * We need to disable the handler at this point as the vertex/tiler subqueue
diff --git a/src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c b/src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
--- a/src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
@@ -647,8 +647,13 @@
       struct cs_builder *b = panvk_get_cs_builder(cmdbuf, i);
       struct panvk_cs_state *cs_state = &cmdbuf->state.cs[i];
 
-      if (deps.src[i].wait_sb_mask)
+      if (deps.src[i].wait_sb_mask) {
+         panvk_per_arch(kbase_mark_progress)(
+            cmdbuf, i, PANVK_KBASE_PROGRESS_FRAG_BEFORE_TILING_WAIT);
          cs_wait_slots(b, deps.src[i].wait_sb_mask);
+         panvk_per_arch(kbase_mark_progress)(
+            cmdbuf, i, PANVK_KBASE_PROGRESS_FRAG_AFTER_TILING_WAIT);
+      }
 
       struct panvk_cache_flush_info cache_flush = deps.src[i].cache_flush;
       if (!panvk_cache_flush_is_nop(&cache_flush)) {
```
（第二个 hunk 借用 `FRAG_*` 位只是**临时**做法：`kbase_mark_progress()` 只接受
`enum panvk_kbase_progress_marker`（`cmd_buffer.c:45-47`），正规做法是先在
`panvk_cmd_buffer.h:60-86` 的枚举里加两个中性位，如
`PANVK_KBASE_PROGRESS_BARRIER_BEFORE_SB_WAIT = 1<<7` / `..._AFTER_SB_WAIT = 1<<8`；
`kbase_progress_bit_name()`（`gpu_queue.c:400-425`）也要补名字。**这一步必须与修复分开上机**。）

**为什么这三个点最有信息量**：
* `FRAG_BEFORE/AFTER_TILING_WAIT` 一旦落地，就能把 §2.3(a)（跨队列 SYNC 等待）与
  §2.3(d)（LS slot 卡住 / 更早的等待）分开：若 `BEFORE` 置位而 `AFTER` 没有 ⇒ **就是卡在
  `wait_finish_tiling()` 的 `SYNC_WAIT64` 上**，那么下一步只需查 `progress_seqno[VT]` 与
  `ctx->syncobjs[0]` 的算术（`cmd_draw.c:4245-4263` 与 `cmd_buffer.c:113-139` 的
  `flush_sync_points()` 之间的配合），无需再猜端点。
* barrier 点上的 `BEFORE/AFTER_SB_WAIT` 能判定"`cs_wait_slots()` 永不返回"（= 某个 scoreboard
  槽永不归零，对应 `cmd_draw.c:4754`、`cmd_dispatch.c:460/486` 那些 `cs_defer(all_iters_mask, …)`
  的间接操作数问题）。
* 另外：wrapper 现有的 `post_call`/`post_wait` 两个标记**已经够用**
  （`post_wait` 在 `WAIT(all)`（`gpu_queue.c:947`）之后），无需再改——sq1/sq2 的
  `post_call` 陈旧就已经证明"CALL 没返回"，这一点是已定论。

---

## 6. 外部出处（本轮取回，供复核）

| 出处 | 用途 |
|---|---|
| `https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/panthor/panthor_sched.c` | `prepare_job_instrs()` 的权威 ring entry 序列；`queue_run_job()` 里 `input->extract = output->extract` 与 `input->insert` 的写入次序；`cs_slot_prog_locked()` 的同一写入；`group_bind_locked()` 的 `allow_compute/fragment/other` + `CSG_EP_REQ_*`（`:1450-1457`）；`cs_slot_sync_queue_state_locked()` 用 `insert == extract && status_scoreboards == 0` 判队列 idle（`:1287-1292`） |
| `include/drm-uapi/mali_kbase_ioctl.h:392-401`（本树） | kbase 队列 user_io 三页与四个字段的**唯一**权威定义（含 `CS_EXTRACT_INIT`） |
| `src/panfrost/lib/kmod/mali_kbase_csf_registers.h:139-145, 331-339, 994-996`（本树） | `CS_EXTRACT_INIT`/`CS_ACTIVE` 语义注释、`CS_REQ_EXTRACT_EVENT`、`CSG_REQ_SYNC_UPDATE`（bit28，说明"sync 更新"是独立事件通道） |
| `/root/panvk-mtk/patches/panvk_mtk.patch:172-250` | 先例 B（G610/arch10/uAPI1.18）的 kbase 路径：只删门铃快速路径 + 超时 10s→120s，**未动 ring entry 结构** |
| `/root/research/23-work/precedent/mesa-snapshot/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c:511-620` | 先例 A（wonderkast02/G720）的 wrapper：与 v64 逐字相同（含 5×SET_STATE + REQ_RESOURCE + CALL） |
| `/root/research/31-v65-next-contract-fix.md` §3 | v65 的 `SB_MASK_STREAM` 删除；本报告 §1.1 修正了它的前提 |

## 7. 未验证清单（**必读**）

1. 【未验证】`CS_ACTIVE` 是"启动时的 extract"（ARM 注释）还是"活跃标志"（先例 B 补丁注释）。
   候选 1 的实验可一次判定。
2. 【未验证】scoreboard 槽是 per-CS 还是 per-CSG；`NEXT_SB_ENTRY`(ENDPOINT/INDEX) 返回的
   "endpoint entry" 编号空间与 `all_iters_mask = 0xfff8` 的对应关系（候选 2 的风险点）。
3. 【未验证】kbase 路径"一子队列一 CSG"是否**本来就是**故障的一部分（候选 2 的核心假设）；
   反证：先例 A/B 用的是同一结构（但它们没有本项目这样的 ~32 s 稳态失败）。
4. 【未验证】§2.3(d) 的"LS slot 0 被永久占用 ⇒ 冻在第一条 WAIT(LS)"机制。
5. 【未验证】FRAG 的 `wait_finish_tiling()` 目标值算术在 kbase 上是否与 panthor 一致
   （`flush_sync_points()` 把三个计数器写进**每条**流的寄存器副本，`cmd_buffer.c:113-139`；
   这依赖"三条流执行同一批 cmd buffer"这一前提——在 kbase 上是否恒成立未验证）。
6. 【未验证】extract 在 CALL 未发射 / 已发射未返回两种状态下的镜像行为（本报告不依赖区分）。
7. 【未验证】候选 2 合并 CSG 后设备 FW 是否接受（`groups[0].stream_num ≥ 3`）。

---

## 8. v67 标记诊断收网（Lead 真机读数 → 进位证明 → 3 个修复 + 1 个决策性诊断）

> **输入**（Lead 提供，2026-10-05 16:03 崩；驱动 = v67 = v66 + 本报告 §5 诊断补丁，树内已确认：
> `cmd_draw.c:4527-4533`、`cmd_buffer.c:651-655` 就是那两个借用 `FRAG_*` 位的发射点，
> 三个文件的 md5 与 §5 时不同、`gpu_queue.c` 未变）。
> **本节所有 diff 都已对**当前 v67 树**跑过 `patch -p1 --dry-run --batch --forward`：0 offset / 0 fuzz / 0 reject。**

### 8.0 一页结论

1. **`bit8` = 进位伪影（已证明，非推断）**；`bit4/bit5` 同样**不是** `FRAG_BEFORE_RUN/FRAG_AFTER_RUN`
   —— 全树没有任何指令发射这两个位（§8.1）。
2. 由算术反推，两处卡点的**确切指令**是：
   * **sq0 VT**：`cs_wait_slots(b, dev->csf.sb.all_iters_mask)`，`panvk_vX_cmd_draw.c:1232`（C2，`get_tiler_desc()` 内）；
   * **sq1 FRAG**：`cs_wait_slots(b, deps.src[i].wait_sb_mask)`，`panvk_vX_cmd_buffer.c:653`（`emit_barrier_csf()` 内）。
3. ⇒ 病因族 = **"某个 scoreboard 槽永不归零"**：两条流都停在"等槽"指令上，而不是停在端点指令/流切换上。
4. ⇒ **与 C1/v60 完全自洽**：`cs_vt_end`/`cs_finish_fragment`/`cs_frag_end` 是 `cs_defer_indirect()` 发射的
   （`cmd_draw.c:4216/4653/4657/4663`），**它们的 signal 槽就是迭代槽**；三条全抑制 ⇒ 等待无物可等
   ⇒ **3~4 个作业即死（v60 实测）**。v60 与 v67 不是矛盾，而是同一机制的两个极端。
5. ⇒ **修正版 C1 = 只抑制 TILER_OOM handler 的注册/注销，保留三条 heap ops**（§8.3，diff C）。
6. **修复 A（最高）**：删掉 wrapper 每条 ring entry 头部剩下的 4 条 `SET_STATE`
   —— 它们正是间接 deferred op 的 **wait/signal 操作数来源**，panvk 状态机自己维护，我们不该每 entry 伪造。
7. **修复 B（次高）**：把 C2 的"13 个迭代槽盲等"收窄为 `cs_wait_indirect()`（只等状态机指定的那 1 个槽）。
8. **诊断 D（决策性，建议与 A 或 B 同轮）**：把 barrier 等待的 **mask 值**用 STORE 记进 seqno cell 的空闲字
   （offset 52），一次运行就能指名"卡的是 3..15 还是 0/1"。**在拿到这个字之前，任何"哪个槽卡住"的说法都还是推断。**

### 8.1 `bit8` 的真相：progress 字是**累加**的，会进位

* `kbase_mark_progress()` 用 **`cs_sync32_add()`**（`panvk_vX_cmd_buffer.c:79`）把标记值**加到** progress 字上；
  该字在每条 ring entry 的 CALL 之前被清零一次（`gpu_queue.c:901-909`）。
  ⇒ **同一标记发射 N 次 = 加 N 倍**，累加值超过 2^k 就**进位**到毫不相干的位。
* v67 树里**实际存在**的发射点（`grep -rn "kbase_mark_progress)(" src/panfrost/vulkan/`）：
  | 位 | 值 | 发射点 | 每条流的次数 |
  |---|---|---|---|
  | bit0 `CMDBUF_START` | 1 | `cmd_buffer.c:877` | 1 |
  | bit2 `FRAG_BEFORE_TILING_WAIT`(借用) | 4 | `cmd_buffer.c:651` **和** `cmd_draw.c:4527` | 每个 barrier + 每个 render pass |
  | bit3 `FRAG_AFTER_TILING_WAIT`(借用) | 8 | `cmd_buffer.c:654` **和** `cmd_draw.c:4531` | 同上 |
  | bit28/29 `FINISH_BEFORE/AFTER_WAIT` | 2^28/2^29 | `cmd_buffer.c:147/150` | 1（收尾） |
  | bit30 `CMDBUF_DONE` | 2^30 | `cmd_buffer.c:234` | 1（收尾） |
  | bit1..bit5 `COMPUTE_*` | 2/4/8/16/32 | `cmd_dispatch.c:218/302/308/338/343` | 每次 dispatch |
* **`FRAG_ENTER`/`FRAG_BEFORE_RUN`/`FRAG_AFTER_RUN`/`FRAG_AFTER_FINISH`/全部 `VT_*` 零发射点**：
  它们只出现在枚举（`panvk_cmd_buffer.h:64-76`）与名字表（`gpu_queue.c:416-417`）里。
  ⇒ **Lead 读到的 "bit4 FRAG_BEFORE_RUN / bit5 FRAG_AFTER_RUN" 是把进位位按名字回读了**，树里没有任何指令能置这两位。
* **算术闭合（FRAG）**：值 = `1 + 4·#BEFORE + 8·#AFTER`。现场集合 `{0,2,3,4,5,8}` = **317**，
  `317 = 1 + 12×26 + 4` ⇒ **26 组完整 (BEFORE, AFTER) 对 + 1 个悬空 BEFORE**
  ⇒ **冻结发生在第 27 个 barrier 点的 `cs_wait_slots()` 之内**（BEFORE 已发、AFTER 未发）。
  （`bit8 = 256` 只能由 ≥22 次 bit2/bit3 累加产生；bit4/5/6/7 同理是进位产物。）
* **sq2 的 `9 = 1 + 8` 无法由 barrier 点单独产生**：COMPUTE 自己的 `COMPUTE_BEFORE_ITER`(bit2=4) /
  `COMPUTE_BEFORE_RUN`(bit3=8) 与借用的 FRAG 位**同值别名**（`cmd_dispatch.c:302/308`）
  ⇒ COMPUTE 的位图不可解读，**必须改用"STORE 记录 mask"**（诊断 D）。
* **教训（写进纪律）**：progress 位图只适用于"每个标记每条流至多发射一次"的场景；
  任何"每 barrier/每 draw 发射一次"的标记都必须用 **STORE 计数器/最后 id**，不能用累加位图。

### 8.2 两处卡点的确切指令（file:line）

**sq0 VERTEX_TILER —— `cs_wait_slots(b, dev->csf.sb.all_iters_mask)`，`cmd_draw.c:1232`**
* 依据：progress 只有 bit0 ⇒ 冻结在**流内第一个未打标记的等待**；VT 路径上第一个等待就是 C2 插入的
  `all_iters_mask` 等待（`get_tiler_desc()`，`cmd_draw.c:1231-1232`，由 `cmd_draw.c:2155` 在首个 draw 时进入）。
  `get_tiler_desc()` 之后才有 `cs_run_idvs2`（`cmd_draw.c:3159`）与 render-pass 的标记对（`:4527`），
  所以"只有 bit0"与"卡在 C2 等待"完全一致；`marks post_call = 0x…67`（陈旧）也证明 CALL 未返回。
* 【未验证】VT 流里是否还存在**别的、更早的**未打标记等待（本报告按代码顺序排除了：`flush_tiling()` 的
  `cs_finish_tiling` 不是等待）。诊断 D 只覆盖 barrier 点，C2 点可由"VT 只有 bit0 + 已知 C2 是首个未标记等待"判定。

**sq1 FRAGMENT —— `cs_wait_slots(b, deps.src[i].wait_sb_mask)`，`cmd_buffer.c:653`（`emit_barrier_csf()`）**
* 依据：§8.1 的进位算术（26 对 + 1 悬空 BEFORE ⇒ 冻结在 barrier 点的等待之内）。
* 该 mask 的两个来源：
  * **含迭代槽**：`add_cs_deps()` 的 `deps->src[i].wait_sb_mask |= dev->csf.sb.all_iters_mask;`
    （`cmd_buffer.c:493`）、CRC 路径 `cmd_buffer.c:730`；
  * **只含 0/1**：`SB_MASK(LS) | SB_MASK(DEFERRED_SYNC)`（`cmd_buffer.c:264`、`cmd_event.c:244`、`cmd_query.c:639`）。
* ⇒ **必须用诊断 D 把这个 mask 读出来**，才能决定修哪一边：
  `0xfff8` ⇒ 迭代槽（3..15）= 端点/堆记账槽；`0x0003`/`0x0001` ⇒ LS/DEFERRED_SYNC 槽。

### 8.3 与 22 号 C1 的关系：v60 为什么更糟，修正版 C1 是什么

* C1 的三条 = `cs_vt_end`（`cmd_draw.c:4216`）、`cs_finish_fragment`（`:4653/:4657`）、`cs_frag_end`（`:4663`），
  **全部以 `cs_defer_indirect()` 发射** ⇒ 它们的 **wait mask 取 `SB_MASK_WAIT`、signal 槽取 `SB_SEL_DEFERRED`**
  （`genxml/cs_builder.h:806-814`；`SB_SEL_DEFERRED` 在 `cmd_draw.c:4638` 被临时指到 `next_sb`、`:4661` 还原）。
* 也就是说：**这三条指令就是迭代槽（3..15）的 signal 来源**。v60 把它们全抑制 ⇒ 这些槽没有被 signal 的机会，
  而 C2（`cmd_draw.c:1232`）与若干 barrier（`cmd_buffer.c:493`）**正在等这些槽**
  ⇒ **等待立刻永久阻塞 ⇒ 3~4 个作业死**。v60 的"更糟"因此**不构成对 C1 动机的反证**，
  而是**第一次把"这些槽靠谁清零"这件事暴露出来**。
* **修正版 C1（diff C）**：只抑制 `cs_set_exception_handler(TILER_OOM, …)` 的注册与注销
  （`cmd_draw.c:4523`、`:4545`），**保留三条 heap ops**。
  * 依据：跑通方在 kbase 上不注册 handler（报告 22 §4.3 H3）；未注册时 kbase 的
    `handle_oom_event()` 走 fatal 分支是"注册了但我们的 handler 没接住"的风险项；
  * 【已定论】本次现场 `OOM=0`、`exception 0xc3=0` ⇒ **它不解释本次卡点**，价值是"把 C1 里唯一安全的子集落地"，
    并把 heap 回收诉求完全交给已有的 P2 整堆换新（`PANVK_KBASE_HEAP_RENEW_INTERVAL=32`）；
  * **不要**再做"三条全抑制"的实验（v60 已证伪）。

### 8.4 修复候选（按可能性排序；全部 dry-run 通过）

#### 候选 A（最高）——删掉 ring wrapper 头部剩余的 4 条 `SET_STATE`

##### 依据
* 这 4 条写的正是**间接 deferred op 的操作数**：`cs_defer_indirect()`（`cs_builder.h:806-814`）的
  wait mask 取 `SB_MASK_WAIT`、signal 槽取 `SB_SEL_DEFERRED`；`cs_wait_indirect()`（`cs_builder.h:1835-1841`）
  等的就是 `SB_MASK_WAIT`。
* 这两个寄存器的**权威维护者是流内的状态机**：`cs_iter_sb_update_end()`（`panvk_cmd_buffer.h:750-767`）
  在每个迭代边界把 `SB_MASK_WAIT` 设为 `BIT(next_sb)`（next_sb 来自 `NEXT_SB_ENTRY`），
  `panvk_vX_cmd_draw.c:4638/4661/4680/4688` 在自己的 deferred 组前后切换 `SB_SEL_DEFERRED`。
* 我们在**每条 ring entry 头部**把它们重写成 init 默认值（`SB_MASK_WAIT = BIT(3)`、`SB_SEL_DEFERRED = 1`）
  ⇒ **每个被 CALL 的流的第一批间接 deferred op 用的是我们伪造的操作数**，而不是状态机留下的值
  ⇒ 迭代槽的 signal 记账错位 ⇒ 之后所有"等迭代槽"的等待（C2 与含 `all_iters_mask` 的 barrier）永不满足。
  这正是 v67 现场的两处卡点。
* panthor 的 ring entry（`prepare_job_instrs()`）**一条 `SET_STATE` 都不发**；v65 只删了 5 条里的
  `SB_MASK_STREAM`（**不是**被间接操作数读取的寄存器）⇒ **v65 无效不构成对本候选的反证**。

##### diff（对当前 v67 树 dry-run 通过）
```diff
diff --git a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ -817,17 +817,23 @@
 
    cs_builder_init(&b, &conf, ring_buf);
 
-   /* The kbase wrapper executes LS stores, waits, flushes and the final
-    * deferred sync before/around the called PanVK stream.  The normal PanVK
-    * init stream also programs these scoreboard slots, but on kbase that
-    * stream is reached through this wrapper, so the wrapper has to make its
-    * own async slots valid first. */
+   /* v68: no SET_STATE here.  These four registers are the *operands* of the
+    * indirect deferred operations PanVK issues inside the called stream
+    * (cs_defer_indirect(): wait mask <- SB_MASK_WAIT, signal slot <-
+    * SB_SEL_DEFERRED, see cs_builder.h:806-814 and cs_wait_indirect() at
+    * cs_builder.h:1835), and they are maintained by the PanVK stream itself:
+    * cs_iter_sb_update_end() (panvk_cmd_buffer.h:750-767) rewrites both at every
+    * iteration boundary and panvk_vX_cmd_draw.c:4638/4661 switches
+    * SB_SEL_DEFERRED around its deferred groups.  Re-establishing the
+    * init-stream defaults at the head of every ring entry therefore hands the
+    * first deferred operation of every called stream fabricated operands
+    * (wait BIT(3), signal slot 1) instead of the values the state machine left
+    * behind, which desynchronises the iteration scoreboard accounting - the very
+    * slots the restored C2 wait and the barrier waits then watch.  The panthor
+    * kernel ring entry (panthor_sched.c::prepare_job_instrs()) emits no
+    * SET_STATE at all; v65 already removed the SB_MASK_STREAM write, this
+    * removes the remaining four. */
 #if PAN_ARCH >= 11
-   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_ENDPOINT, SB_ITER(0));
-   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_MASK_WAIT, SB_WAIT_ITER(0));
-   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_OTHER, SB_ID(LS));
-   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_DEFERRED,
-                      SB_ID(DEFERRED_SYNC));
    /* SB_MASK_STREAM (and SB_MASK_WAIT / SB_SEL_ENDPOINT / SB_SEL_DEFERRED)
     * is *stream* state, not ring-entry state: the PanVK CS builder programs it
     * at every iteration boundary (cs_iter_sb_update_end() sets it to
```

##### 预期可观测差异（**与本次读数可区分**）
1. **生效判据（不需要 timeout）**：ring entry 变短 —— 无 diag 时 VT/FRAG `152 → 136 B`（CALL 由 +104 移到 +88）、
   COMPUTE `136 → 120 B`（CALL +88 → +72）；带 v66/v67 诊断时 `288 → 272` / `272 → 256 B`。
   日志里的 `entry …/192 bytes` 与 `extract offset = last_job_offset + 88/72` 直接自证。
2. **若机制成立**：VT 的 `post_call` 推进到当前 target（不再是陈旧值），FRAG 不再冻结在 barrier 等待；
   timeout 消失或显著延后（对照 v67：~40 s / 103 个作业）。
3. **若签名完全不变**（VT 仍只有 bit0、FRAG 仍停在 barrier 等待）⇒ "wrapper 伪造间接操作数"被**否证**，
   应转向诊断 D 指定的那个槽去查 signal 来源。

##### 风险
* 低—中。唯一退化路径：某个 callee 在**第一次 `cs_iter_sb_update_end()` 之前**就发射 deferred-indirect 操作，
  且 **init stream 没有被执行过**（那时这两个寄存器是 CS 初值 0）——此时 wait 掩码为 0（不等）、
  signal 槽为 0（记到 LS）。判据：init stream 由 `panvk_queue_init_contexts()`（`gpu_queue.c:2081-2087`）
  在队列创建时提交，正常路径必然执行。

##### 回滚
`cp` 回备份（`gpu_queue.c.bak-v66-*` 任一份 = 本候选未应用时的内容），或 `git apply -R -p1 < A.patch`。

#### 候选 B（次高）——把 C2 的"13 槽盲等"收窄为"等状态机指定的那一个槽"

##### 依据
* v67 现场：**VT 冻结在 `cmd_draw.c:1232` 的 `all_iters_mask` 等待上**（§8.2）。
* C2（上游 MR !44173）的诉求是"复用本 per-queue tiler heap 前，等本子队列先前的 tiling 工作退休"；
  而 panvk 的状态机**已经**把"本子队列上一次迭代用过的那一个槽"维护在 `SB_MASK_WAIT` 里
  （`cs_iter_sb_update_end()`，`panvk_cmd_buffer.h:758-760`）——`all_iters_mask` 把"1 个槽"放大成"13 个槽"，
  **任何单槽不归零都会冻死整条队列**。
* `cs_wait_indirect()` 是现成 API（`cs_builder.h:1835-1841`，与 `SB_MASK_WAIT` 配套，正是为这种场景设计的）。

##### diff（对当前 v67 树 dry-run 通过；已加 `#if PAN_ARCH >= 11` 守卫，arch10 分支保持不变）
```diff
diff --git a/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c b/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
--- a/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
@@ -1228,8 +1228,20 @@
        * prior async tiling work to retire before reprogramming it.
        * (upstream MR !44173 -- "wait for prior tiling work before reusing
        * tiler heap", reported as tile-aligned corruption on Mali-G720) */
+#if PAN_ARCH >= 11
+      /* v68: wait for the *one* iteration slot the PanVK state machine says this
+       * subqueue must wait for (SB_MASK_WAIT, maintained by
+       * cs_iter_sb_update_end(), panvk_cmd_buffer.h:758-760) instead of
+       * blanket-waiting for all thirteen iteration slots.  all_iters_mask turns a
+       * single non-retiring slot into a frozen queue: the v67 snapshot shows the
+       * vertex/tiler queue frozen on exactly this wait while the fragment queue is
+       * frozen on a barrier wait, i.e. every queue that touches an iteration slot
+       * stops. */
+      cs_wait_indirect(b);
+#else
       struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
       cs_wait_slots(b, dev->csf.sb.all_iters_mask);
+#endif
    }
 
    struct panvk_physical_device *phys_dev =
```

##### 预期可观测差异（**与本次读数可区分**）
1. **若 C2 是放大器而非病因**：VT 解冻（progress 出现 bit1+ 或 `post_call` 推进到当前 target），
   而 **FRAG 仍冻结**在 barrier 等待 ⇒ 直接把病因定位到 FRAG 侧的槽（配合诊断 D 读 mask）。
2. **若病因就是"某一迭代槽卡住"**：VT 与 FRAG 可能都不再冻结（timeout 消失），
   但**风险变成静默的 heap 竞态**（等得太少）⇒ 必须同时观察报告 22 的顶点错乱是否回归。
3. **若 VT 仍在同一条等待上冻结** ⇒ 说明 `SB_MASK_WAIT` 此刻指向的槽本身就不归零
   （即 `cs_wait_indirect()` 也等同一个槽）⇒ 直接读诊断 D 的 mask 即可确认。

##### 风险
* 中。`get_tiler_desc()` 若在**迭代块之外**被调用，`SB_MASK_WAIT` 可能持有过期值（等一个已归零的槽 = 不等待，
  失去 C2 的保护）。缓解：先只上候选 B 一次，同时盯"tile 对齐错乱是否回归"（报告 22 的症状）。

##### 回滚
`git apply -R -p1 < B.patch`（该 diff 只动 `cmd_draw.c:1231-1232` 一处）。

#### 候选 C（= 修正版 C1）——只抑制 TILER_OOM handler 的注册/注销

##### 依据
§8.3。**【已定论】本次 OOM=0 ⇒ 它不解释本次卡点**；它是对"22 号 C1"的可安全落地子集（去掉唯一风险项、
保留三条 heap ops 作为槽的 signal 来源）。

##### diff（对当前 v67 树 dry-run 通过）
```diff
diff --git a/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c b/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
--- a/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
@@ -1228,8 +1228,15 @@
        * prior async tiling work to retire before reprogramming it.
        * (upstream MR !44173 -- "wait for prior tiling work before reusing
        * tiler heap", reported as tile-aligned corruption on Mali-G720) */
-      struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
-      cs_wait_slots(b, dev->csf.sb.all_iters_mask);
+      /* v68: wait for the *one* iteration slot the PanVK state machine says
+       * this subqueue must wait for (SB_MASK_WAIT, set by
+       * cs_iter_sb_update_end(), panvk_cmd_buffer.h:758-760) instead of
+       * blanket-waiting for all thirteen iteration slots.  all_iters_mask turns
+       * a single non-retiring slot into a frozen queue: the v67 snapshot shows
+       * the vertex/tiler queue frozen on exactly this wait while the fragment
+       * queue is frozen on a barrier wait, i.e. every queue that touches an
+       * iteration slot stops. */
+      cs_wait_indirect(b);
    }
 
    struct panvk_physical_device *phys_dev =
@@ -4518,10 +4525,23 @@
    uint32_t handler_idx = calc_tiler_oom_handler_idx(cmdbuf);
    uint64_t handler_addr = dev->tiler_oom.handlers_bo->addr.dev +
                            handler_idx * dev->tiler_oom.handler_stride;
-   cs_move64_to(b, addr_reg, handler_addr);
-   cs_move32_to(b, length_reg, dev->tiler_oom.handler_stride);
-   cs_set_exception_handler(b, MALI_CS_EXCEPTION_TYPE_TILER_OOM, addr_reg,
-                            length_reg);
+   /* v68 (corrected C1): register the tiler-OOM handler only where the kernel
+    * has a recoverable path for it.  On kbase BASE_CSF_TILER_OOM_EXCEPTION_FLAG
+    * is what we ask for at group creation; where that request did not take, a
+    * registered handler makes kbase take its fatal branch.  The *heap*
+    * operations (cs_vt_end / cs_finish_fragment / cs_frag_end) are deliberately
+    * NOT suppressed: v67 shows every queue that waits on an iteration scoreboard
+    * slot freezing, and those three instructions are the only signal sources of
+    * those slots (they are issued with cs_defer_indirect(), so their signal slot
+    * comes from SB_SEL_DEFERRED).  Suppressing them - v60 - leaves the waits
+    * nothing to retire and kills the queue after three or four jobs. */
+   if (!to_panvk_physical_device(cmdbuf->vk.base.device->physical)
+           ->kbase_node_path[0]) {
+      cs_move64_to(b, addr_reg, handler_addr);
+      cs_move32_to(b, length_reg, dev->tiler_oom.handler_stride);
+      cs_set_exception_handler(b, MALI_CS_EXCEPTION_TYPE_TILER_OOM, addr_reg,
+                               length_reg);
+   }
 
    /* Wait for the tiling to be done before submitting the fragment job. */
    panvk_per_arch(kbase_mark_progress)(
@@ -4540,10 +4560,13 @@
     * By disabling the handler here, any exception will be left pending until a
     * new hander is registered, at which point the correct state has been set
     * up. */
-   cs_move64_to(b, addr_reg, 0);
-   cs_move32_to(b, length_reg, 0);
-   cs_set_exception_handler(b, MALI_CS_EXCEPTION_TYPE_TILER_OOM, addr_reg,
-                            length_reg);
+   if (!to_panvk_physical_device(cmdbuf->vk.base.device->physical)
+           ->kbase_node_path[0]) {
+      cs_move64_to(b, addr_reg, 0);
+      cs_move32_to(b, length_reg, 0);
+      cs_set_exception_handler(b, MALI_CS_EXCEPTION_TYPE_TILER_OOM, addr_reg,
+                               length_reg);
+   }
 
    /* Applications tend to forget to describe subpass dependencies, especially
     * when it comes to write -> read dependencies on attachments. The
```

##### 预期可观测差异（**与本次读数可区分**）
* **不应**改变 v67 的卡点签名（VT 只有 bit0 / FRAG 停在 barrier）；它改变的是：
  * 若发生 TILER_OOM：kbase 走 fatal 分支（组被杀）而不是调用我们的 handler —— **这是回归风险，不是修复**；
  * 因此**它的价值只在"把 C1 从待办里删掉"**。若 Lead 想保留 OOM 恢复能力，可以不上本候选。

##### 风险
* 中。若真发生 TILER_OOM 且没有 handler，kbase 会终止队列组（"CSF group N tiler heap OOM"）。
  本次现场 OOM=0，但长时间运行（>120 s）时无法保证。

##### 回滚
`git apply -R -p1 < C.patch`。

### 8.5 诊断 D（决策性；建议与候选 A 或 B 同轮上机）

**目的**：把"卡的是哪个槽"从推断变成读数。做法：在 barrier 等待之前，把 **mask 值**用 **STORE**（不是 ADD，
因此不会进位）写进 seqno cell 的空闲字 **offset 52**（cell stride = `ALIGN_POT(16,64) = 64`，
0..51 已被 sync64/LS copy/三个 mark/progress 位图占用），并在 timeout 快照里打印它。

```diff
diff --git a/src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c b/src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
--- a/src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
@@ -598,6 +598,41 @@
    }
 }
 
+/* v68 diagnostics: record the scoreboard mask of the wait we are about to
+ * execute into a free word of the seqno cell (offset 52; the cell stride is
+ * ALIGN_POT(16,64) = 64 bytes and 0..51 are taken by the sync64, the LS copy,
+ * the three marks and the progress bitmap).  This is a plain STORE, not the
+ * accumulating SYNC_ADD32 that kbase_mark_progress() uses: the progress bitmap
+ * carries (the v67 reading of "bit4/bit5/bit8" is really 26 repeated (bit2+bit3)
+ * marker pairs plus one dangling BEFORE, see report 33 §8), while this word must
+ * read back exactly the mask that never retired. */
+static void
+panvk_per_arch(kbase_record_wait_mask)(
+   struct panvk_cmd_buffer *cmdbuf, enum panvk_subqueue_id subqueue,
+   uint32_t wait_mask)
+{
+   if (!PANVK_DEBUG(KBASE_DIAG))
+      return;
+
+   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
+   struct panvk_physical_device *phys_dev =
+      to_panvk_physical_device(dev->vk.physical);
+
+   if (!phys_dev->kbase_node_path[0])
+      return;
+
+   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, subqueue);
+   struct cs_index addr = cs_scratch_reg64(b, 14);
+   struct cs_index value = cs_scratch_reg32(b, 16);
+
+   cs_load64_to(b, addr, cs_subqueue_ctx_reg(b),
+                offsetof(struct panvk_cs_subqueue_context,
+                         debug.kbase_progress_addr));
+   cs_move32_to(b, value, wait_mask);
+   cs_store32(b, value, addr, 52);
+   cs_flush_stores(b);
+}
+
 static void
 emit_barrier_insert_waits(struct cs_builder *b, struct panvk_cmd_buffer *cmdbuf,
                           struct panvk_cs_deps *deps, enum panvk_subqueue_id i,
@@ -650,6 +685,8 @@
       if (deps.src[i].wait_sb_mask) {
          panvk_per_arch(kbase_mark_progress)(
             cmdbuf, i, PANVK_KBASE_PROGRESS_FRAG_BEFORE_TILING_WAIT);
+         panvk_per_arch(kbase_record_wait_mask)(
+            cmdbuf, i, deps.src[i].wait_sb_mask);
          cs_wait_slots(b, deps.src[i].wait_sb_mask);
          panvk_per_arch(kbase_mark_progress)(
             cmdbuf, i, PANVK_KBASE_PROGRESS_FRAG_AFTER_TILING_WAIT);
diff --git a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ -546,6 +546,19 @@
                             : "retired everything up to the CALL and never "
                               "returned from it")));
 
+   /* v68: the mask of the last barrier wait this stream executed.  Only a
+    * frozen (stuck) slot leaves a non-zero gap here, and the mask names it:
+    * 0xfff8 = iteration slots 3..15, 0x0003 = LS + DEFERRED_SYNC. */
+   {
+      const volatile uint8_t *cell_bytes =
+         (const volatile uint8_t *)kbase_subqueue_seqno_cell(queue, subqueue);
+
+      kbase_cache_invalidate_range((const void *)cell_bytes,
+                                   kbase_seqno_stride());
+      mesa_loge("kbase: DIAG %s subqueue %u last barrier wait mask 0x%08x",
+                reason, subqueue, *(volatile uint32_t *)(cell_bytes + 52));
+   }
+
    /* 2. The decisive question: did firmware execute anything inside the stream
     *    the ring entry CALLs? */
    kbase_log_callee_progress(subqueue, reason, stream_progress);
```

**读数判定表**（`kbase: DIAG … last barrier wait mask 0x…`）：

| mask | 含义 | 下一步 |
|---|---|---|
| `0xfff8` | 迭代槽 3..15（端点/堆记账槽） | 走候选 A（伪造操作数）→ 再看 signal 来源；必要时查 `cs_vt_end`/`cs_finish_fragment` 的完成 |
| `0x0003` | `LS(0) + DEFERRED_SYNC(1)` | 查 LS 槽：谁能持有它不清（wrapper 的 `FLUSH_CACHE2`→`WAIT(0)` 配对、各 `cs_flush_stores()`） |
| `0x0001` | 只等 LS | 同上，范围更小 |
| `0x0000` / 未打印 | 该流**没走到** barrier 等待 | 说明 FRAG 的冻结不在此处（回到 §8.2 重新定位） |

**注意**：本诊断只覆盖 barrier 点。VT 侧的 C2 卡点无需新标记（它是"首个未标记等待"，已在 §8.2 定案）。

### 8.6 本节未验证清单

1. 【未验证】barrier 等待 mask 的真实值（**必须**用诊断 D 取值，不能从位图推断）。
2. 【未验证】"wrapper 伪造间接 deferred 操作数"是否真的发生（候选 A 的机制）；若候选 A 后签名不变即被否证。
3. 【未验证】`SB_MASK_WAIT` 在 `get_tiler_desc()` 执行点是否**恰好**是"上一次迭代的槽"（候选 B 的前提）。
4. 【未验证】sq2 COMPUTE 的 9 = 1+8 的真实构成（其自有位与借用位同值别名）。
5. 【未验证】v65 的 `SB_MASK_STREAM` 删除与本次机制的关系（本报告认为无关：该寄存器不是间接操作数的来源）。
