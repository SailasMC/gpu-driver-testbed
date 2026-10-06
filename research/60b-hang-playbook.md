# 60b — 「卡死定位播放手册」：把超时快照反查到 CS 停在源码哪一步

配套：`/root/research/60-v85-v84d.md`（v85/v84d 记账）· 代码基线 = 当前工作区（未提交）
只读分析；本文件不含任何代码改动，**v86/v87 建议在 §8**（纯提案）

---

## 0. 一句话 + 三步用法

**结论**：这套诊断已经**足够**把"卡死"定位到**具体某一步**——因为树里除了"累加型"的 `stream progress`，还有一个 **v71 引入的"唯一 ID 型"检查点字**（`cell+56`，值 = `0xc0de0000 | id`），而且**驱动自己会把它翻译成名字**打印出来（`kbase: CKPT <reason> subqueue N word 0x… => id 0x… (NAME)`）。**不要用 progress 定位，用 CKPT 字定位**（progress 会进位，见 §5）。

**三步用法**：

1. `grep -n "kbase: CKPT timeout snapshot"` → 拿到 **id + 名字**（这是"停在哪一步"的主判据）。
2. 到 §3 的名字表查到**写入点 file:line**（GPU 侧那一行就是"最后退休的检查点"）。
3. 用 §4 的 `marks` + §6 的 `insert/extract/ENTRY` 交叉验证"是**没派发**、还是**停在 callee 流内**、还是**停在全槽等待之后**"，并用 §7 判读表定档。

---

## 1. 快照有哪些行（v84d = `PANVK_DEBUG=1,kbase_diag` 时才会多打 DIAG 行）

| 日志行（前缀） | 何时打 | 关键内容 |
|---|---|---|
| `kbase: timeout on subqueue N: seqno …` | 等待超时（watchdog） | **第一判据行**：`seqno`、`ls_copy`、`target`、`marks pre/post-call/post-wait`、`stream progress`、`insert`、`extract`、`active`、`error`、`ring[0..3]` |
| `kbase: last job on subqueue N: ring offset …` | 同上 | 最近一个 ring entry 的几何：`ring offset`、`entry`/`padded` 字节、`stream 0x…/size`、`flush`、`extract offset`（`extract % 65536`） |
| `kbase: last ring[0..15] …` | 同上 | 最近 entry 头部 16 个 qword（原始机器码） |
| `kbase: extract line ring[0..7] …` | 同上 | 取指指针所在 64B cacheline 的 8 个 qword |
| `kbase: SEQNO cell+0 = … [cpu-read-uncached]` | 同上 | 二次核读 + **verdict**（含"deferred path 卡了多久"这类结论） |
| `kbase: <reason> subqueue N: seqno … marks … active …` | `kbase_log_subqueue_state()` | 与第一行同类，但 **`target` 字段语义不同**（见 §9 陷阱 2） |
| `kbase: <reason> subqueue N stream progress 0x%x` | 同上 | progress 单独一行 |
| **`kbase: CKPT <reason> subqueue N word 0x%08x => id 0x%x (NAME)`** | 同上（仅 DIAG） | **主判据：唯一 ID + 名字** |
| `kbase: CKPT host tiler heap renewals … submits since renewal …` | 同上 | 续租侧（与报告 58 的 `begin/end/postponed` 对读） |
| `kbase: ENTRY subqueue N <label>: ring offset … 16 raw bytes` + `first instr 0x… opcode 0x… (NAME) wait_mask 0x… => WAIT-CLASS / …` | `kbase_log_entry_bytes()` | **取指指针正在取哪条指令**（opcode 名字 + 是否阻塞型等待） |
| `kbase: ENTRY subqueue N geometry: … +B into the last entry …` | 同上 | 取指指针**距最近 entry 头多少字节** |
| `kbase: csf timeout on subqueue N @ job …: insert … extract … last_job_offset … last_last_job_offset …` | DIAG + `insert>extract` | 两代 entry 的偏移，并触发 **pandecode 反汇编未消费的 entry** |
| `kbase: DIAG wait subqueue N t=…ms: extract … ckpt 0x%08x` | 等待 >1 s 期间每秒 | 卡死过程中的**时间序列**（看 ckpt 是否在变） |

---

## 2. seqno cell 布局（64 B，`kbase_seqno_stride()`）

| 偏移 | 宽度 | 名称 | 写入者 | 语义 |
|---|---|---|---|---|
| 0 | 8 | `seqno` | **ring wrapper 的 deferred `SYNC_ADD64`**（系统域） | 硬件完成计数（每个 entry +1） |
| 8 | 4+4 | `error`/`pad` | CS 状态镜像 | 故障标志（配合 `CS_FAULT`/`CS_FAULT_INFO` = 输出页 +0x80/+0x88） |
| 16 | 8 | `LS_COPY` | wrapper（DIAG） | 全槽等待通过后写入的 `target_seqno` 副本（第二证据） |
| 24 | 8 | `MARK_PRE_CALL` | wrapper（DIAG） | `0x100000000000 \| target_seqno` |
| 32 | 8 | `MARK_POST_CALL` | wrapper（DIAG） | `0x200000000000 \| target_seqno` |
| 40 | 8 | `MARK_POST_WAIT` | wrapper（DIAG） | `0x300000000000 \| target_seqno` |
| 48 | 4 | `stream progress` | 流内 `kbase_mark_progress()`（**`cs_sync32_add` 累加**）+ wrapper 每 entry 清零 | 位标志，**会进位，慎用**（§5） |
| 52 | 4 | last barrier wait mask | 流内 | 最近一次屏障等待掩码（DIAG 行里打印） |
| 56 | 4 | **CHECKPOINT** | 流内 `kbase_checkpoint()` **与** wrapper `kbase_wrapper_checkpoint()` | `0xc0de0000 \| id`，**唯一 ID，幂等** |
| 60 | 4 | （空闲） | — | **v86 可用的第二个字**（§8 建议 1） |

`static_assert`（`panvk_vX_gpu_queue.c:93-95`）：`56 == 48 + 8`（`KBASE_SEQNO_STREAM_PROGRESS_OFFSET + PANVK_KBASE_SEQNO_CKPT_REL_OFFSET`）——即流侧写的是 `progress_addr + 8`，落点与 wrapper 相同。

---

## 3. 检查点全表（名字 ↔ id ↔ 写入点 ↔ 执行顺序）

**机制**（`panvk_cmd_buffer.h:88-127` 原文要点）：progress 故意用 `cs_sync32_add` 累加 ⇒ **不能当位图读**；checkpoint 改成 **STORE 一个唯一常量**（`MAGIC|id`）到**独立**的 32 位字 ⇒ 该字**永远只持有一个 id**，重复发射幂等。解码表 = `kbase_checkpoint_name()`（`panvk_vX_gpu_queue.c:587-635`）。

### 3.1 ring wrapper 侧（id 40..45；**执行顺序 ≠ id 顺序**）

一个 ring entry 的真实执行顺序：

```
[40 WRAP_BEFORE_CALL] → CALL → 进入 callee 流 [1..39 任意] → 返回 →
[41 WRAP_AFTER_CALL] → [44 WRAP_BEFORE_WAIT] → cs_wait_slots(all_mask) →
[45 WRAP_AFTER_WAIT] → (marks POST_WAIT / LS copy) → [42 WRAP_BEFORE_SYNC_ADD] →
deferred SYNC_ADD64 → ERROR_BARRIER → entry 结束
```

| id | 名字 | 写入点 | 含义（"读到它就说明已经退休到这一步"） |
|---|---|---|---|
| 40 | `WRAP_BEFORE_CALL` | `panvk_vX_gpu_queue.c:1483` | wrapper 序言完成（HEAP_SET/清零/marks 已写）；**callee 流一个检查点都还没退休** |
| 41 | `WRAP_AFTER_CALL` | `:1517` | callee 流已跑到尾部返回 |
| 44 | `WRAP_BEFORE_WAIT` | `:1539` | 即将进入 **等全部 scoreboard 槽** |
| 45 | `WRAP_AFTER_WAIT` | `:1544` | 全槽等待**已通过** |
| 42 | `WRAP_BEFORE_SYNC_ADD` | `:1582` | 已到最后可观测点：随后是 deferred 完成写 + error barrier |
| （43/46/47） | **未定义** | — | 故意不定义：deferred SYNC_ADD64 之后插 store 会踩它正在读的操作数寄存器，且不可观测（`panvk_cmd_buffer.h:168-176` 注释 + 报告 45） |

### 3.2 流内（id 1..39，仅 DIAG；写点=`panvk_vX_cmd_buffer.c:105-124` 的 `kbase_checkpoint()`）

| id | 名字 | 写入点（`csf/`） | 含义 |
|---|---|---|---|
| 1 | `STREAM_START` | `panvk_vX_cmd_buffer.c:983` | 上层流开始发射（每子队列） |
| 2/3 | `VT_BEFORE/AFTER_RUN_IDVS` | `panvk_vX_cmd_draw.c:3174/3235`（直接绘制）· `:3437/3509`（间接绘制 `launch_indirect_draw`） | 顶点/图元作业发射前/后；**2 与 3 之间卡 = RUN_IDVS 未完成** |
| 4/5 | `VT_BEFORE/AFTER_FINISH_TILING` | `:4251/4255` | FINISH_TILING 前后（图元链表收尾） |
| 6/7 | `VT_BEFORE/AFTER_VT_END` | `:4270/4273` | `HEAP_OPERATION(VERTEX_TILER_COMPLETED)` 前后 |
| 8 | `VT_AFTER_SYNC_SIGNAL` | `:4278` | VT 迭代槽 signal 之后（FRAG 的 `wait_finish_tiling()` 等的就是它） |
| 9/10 | `ITERS_WAIT_BEFORE/AFTER` | `:1242/1246` | `get_tiler_desc()` 里的**自等待**（上游 !44173 的"等自己上一次 tiling 退休"） |
| 11/12 | `LS_WAIT_BEFORE/AFTER` | `:1938/1942`（FRAGMENT） | FRAG 侧的 LS 等待 |
| 13/14 | `DESC_RINGBUF_WAIT_BEFORE/AFTER` | `:1313/1316` | 等描述符 ringbuf 释放（`simul_use` 路径） |
| 15 | `FRAG_ENTER` | `:4586` | 片元流进入 |
| 16/17 | `FRAG_BEFORE/AFTER_TILING_WAIT` | `:4592/4598` | **`wait_finish_tiling()`**（等 VT 的槽）前/后；**卡在 16 = 一直没等到 tiling 完成** |
| 18/19 | `FRAG_BEFORE/AFTER_RUN` | `:4627/4664` | RUN_FRAGMENT 前/后 |
| 20/21 | `FRAG_BEFORE/AFTER_FINISH` | `:4722/4739` | `FINISH_FRAGMENT`（回传空闲 heap chunk）+ `FRAG_END` 前/后 |
| 22/23 | `FINISH_BEFORE/AFTER_WAIT` | `panvk_vX_cmd_buffer.c:193/196` | 流尾等待前/后 |
| 24 | `CMDBUF_DONE` | `:284` | **该子队列这一批发射真正结束**（流尾） |
| 25/26 | `BARRIER_BEFORE/AFTER_SB_WAIT` | `:744/747` | 屏障：scoreboard 等待前后 |
| 27/28 | `BARRIER_BEFORE/AFTER_QUEUE_WAIT` | `:704/708` | 屏障：队列等待前后 |
| 29/30 | `BARRIER_BEFORE/AFTER_FLUSH` | `:757/765` | 屏障：flush 前后 |
| 31..37 | `COMPUTE_ENTER / BEFORE_ITER / BEFORE_RUN / AFTER_RUN / AFTER_SIGNAL / BEFORE_SYNC_ADD / AFTER_SYNC_ADD` | `panvk_vX_cmd_dispatch.c:230/316/324/356/363/53/58` | 计算作业各步 |
| 38/39 | `COMPUTE_BEFORE/AFTER_WAIT_INDIRECT` | `:88/91` | 计算侧的间接等待前后 |

> 读法：**`ckpt` = "最后退休的那一步"**。卡死 = 下一个应当退休的检查点没来。所以判读时要用 §7 的"那么它没能到达的是哪一步"来对读。

---

## 4. `marks` 三字段：绑定到"哪个 job"的唯一可靠办法

- 三个值都是 `MARK | target_seqno`，其中 `MARK` 在**高位**：`0x100000000000`（bit44，pre）、`0x200000000000`（bit45，post-call）、`0x300000000000`（bit44+45，post-wait）。
- **低 44 位 = 写它的那个 job 的 `target_seqno`** ⇒ **这是把 cell 内容绑定到具体 job 的唯一手段**。
- 判据：
  - `marks=0/0/0` ⇒ wrapper **第一个 store 都没退休**（该 entry 未派发，或卡在它的序言里）。
  - `pre≠0, post=0` ⇒ 卡在 **CALL 内部**（callee 流）或 CALL 之前。
  - `post≠0, post_wait=0` ⇒ CALL 已返回，卡在 **全槽等待之前/之中**。
  - `post_wait≠0` ⇒ **全槽等待已通过** ⇒ 若 `seqno` 仍未达 `target`，则卡在 **deferred 完成写 / error barrier**。
  - **`marks` 低 44 位 ≠ `target`** ⇒ 这些 marks（以及同一 cell 里的 `ckpt`）属于**更早的 job**，不能用来描述当前 job ⇒ 先判断"当前 job 到底有没有开始"（`insert > extract` + `ckpt==0`）。

---

## 5. `stream progress`（`cell+48`）：**累加型 ⇒ 不可按位解读**

- 写入者：`panvk_per_arch(kbase_mark_progress)()`（`panvk_vX_cmd_buffer.c:45-80`），实现是
  `cs_sync32_add(b, true, MALI_CS_SYNC_SCOPE_CSG, value, addr, cs_now())`（`:79`）——**加法，不是或**。
- wrapper 在每个 entry 的 CALL 之前清零（`panvk_vX_gpu_queue.c:1467`），所以它只反映**当前 entry**。
- **进位失效的具体例子**：某 entry 里 `FRAG_ENTER(1<<1)` 发射了 2 次 ⇒ 累加得 `0b100` ⇒ 位图读法会误判成 `FRAG_BEFORE_TILING_WAIT(1<<2)`。任何"同一 marker 在一个 entry 内出现 ≥2 次"的帧（几乎所有真实帧，多 draw/多 pass）都会污染更高位。
- 另一个坑：**VT / FRAG / COMPUTE 复用同一批位号**（bit1..bit6），只有"progress 是**每子队列各一份**"这一点救了它——但同一子队列内的多次发射仍会进位。
- ⇒ **规则**：`progress` 只用于**粗判**（例如"看到 bit1 说明片元流至少进过一次"），**定位必须用 `ckpt`**。若 `progress` 与 `ckpt` 冲突，**信 `ckpt`**。
- 想修这个缺陷**不可能靠 OR**：`cs_sync32_or` 在 `genxml/cs_builder.h` 里**不存在**（grep 0 命中）⇒ 只能改用"STORE 唯一值"（§8 建议 1 就是这条思路的延伸）。

---

## 6. `insert` / `extract` / ring entry 换算

- `KBASE_RINGBUF_SIZE = 64 KiB`（`panvk_vX_gpu_queue.c:57`）；`insert`/`extract` 都是**单调不减、不回环**的字节指针（`extract % 65536` 才是环内偏移）。
- 每个 entry 占 `padded = ALIGN_POT(entry_size, 64)`（cacheline 对齐，`:1601-1606`）；只保留最近两个 entry 的偏移：`last_job_offset`、`last_last_job_offset`（`:1607-1609`）。
- 换算：
  - `Δ = insert - extract`：**待取指字节数**。`Δ == 0` ⇒ 固件已取完 ⇒ 卡的不是取指，而是**退休**（看 `seqno`/`marks`/`ckpt`）。
  - `extract_off = extract % 65536`；**距最近 entry 头** = `(extract_off + 65536 - last_job_offset % 65536) % 65536`（快照的 `ENTRY … geometry` 行直接给了这个"+B into the last entry"）。
  - `extract_off ∈ [last_job_offset, insert)` ⇒ 停在**最近 entry**；否则很可能在**上一个 entry**（`last_last_job_offset`）。
  - `extract & ~63` = 取指指针所在 cacheline（快照用它 dump `extract line ring[0..7]`）。
- **直接看指令**：`ENTRY … first instr 0x… opcode 0x… (名字) wait_mask 0x… => WAIT-CLASS / …`
  - opcode 名字来自 `mali_cs_opcode_as_str()`；`WAIT-CLASS` ⇒ **该取指指令本身在等 scoreboard**（"取到了、但停在等待上"）；
  - 非 WAIT-CLASS + `Δ>0` ⇒ 取指停在**普通指令**上（更可能是它后面的隐式等待/依赖，或固件没继续取）。
  - `delta-from-extract 0 B` 的那条 = **就是取指指针本身**（另一条 `last-job head` 是最近 entry 头，用于对比）。
- `insert > extract` 时快照会自动 `pandecode_cs_binary(ringbuf_dev + last_last_job_offset, insert - last_last_job_offset)` ⇒ **未消费的 entry 会被反汇编**，这是把"机器码"对回"panvk 发射的哪一段"的桥。

---

## 7. 判读表（`ckpt` 为主，marks/ENTRY/insert 为辅）

| # | 观测组合 | 结论（卡在哪一类步骤） | 置信度 | 已排除 |
|---|---|---|---|---|
| J1 | `seqno >= target` | **作业已退休**：不是 GPU 卡死，而是等待者没被唤醒/条件判错（或 target 抓错） | 高 | GPU 侧全部（除 cell 读取路径） |
| J2 | `marks=0/0/0` + `ckpt==0` + `progress==0` + `insert > extract` | **该 entry 从未开始**（未派发/固件没取指）：查调度、`active`、`error`、`CS_ACTIVE`；配合 RESCHED 实验结果 | 高（DIAG 开） | entry 内部任何步骤 |
| J3 | `pre≠0`（低 44 位 = target）+ `ckpt==0` | 卡在 wrapper **序言**（marks 已写、`ckpt` 还没轮到 40）：即两次 `cs_wait_slot(LS)` / 清零 store 这一小段（也可能读到"正好在序言"的瞬时态） | 中 | callee 流（一个检查点都没退休） |
| J4 | `ckpt==40` | wrapper 序言完成，**callee 流未跑到第一个检查点** ⇒ 卡在 **flush / CALL 建立 / 流的前几条指令** | 中高 | callee 流内部（≥1 个检查点都还没退休） |
| J5 | `ckpt ∈ [1..39]` | **精确停在流内该步**（按 §3.2 查写入点）。例如 `16 FRAG_BEFORE_TILING_WAIT` ⇒ 一直在等 tiling（VT 侧 8/VT_AFTER_VT_END 没来）；`2 VT_BEFORE_RUN_IDVS` ⇒ RUN_IDVS 未完成 | **高**（唯一 ID） | 之后的全部步骤；VT/FRAG 交叉卡点也能由此定位（16 卡 ↔ 8 没来） |
| J6 | `ckpt==41` | callee 流已返回；卡在 **CALL 返回 → 全槽等待** 之间（marks 的 LS 写、或即将进入等待） | 中高 | callee 流内部（已跑完） |
| J7 | `ckpt==44` | **卡在 `cs_wait_slots(all_mask)`**：某个 async op 从未 signal（典型的"等一个没人会 signal 的槽"） | **高**（44 是紧邻该 wait 的最后一次写） | 全槽等待之后的一切 |
| J8 | `ckpt==45` | 全槽等待**已通过**；卡在 45→42 之间（POST_WAIT marks / LS copy） | 中高 | 等待本身 |
| J9 | `ckpt==42` + `seqno < target` | **最后可观测点之后**：deferred `SYNC_ADD64`（或 ERROR_BARRIER）没落地 ⇒ 与 `kbase_log_seqno_uncached` 的 "deferred path stalled" verdict 同因 | **高** | 42 之前的一切 |
| J10 | `error != 0`（且 `CS_FAULT` 异常类型 ≠ 0） | **CS 异常**（不是"卡"）：按 `CS error/fault` 行的异常类型与 fault 地址走（与 0xc0 / 野指针族对读） | 高 | 纯挂起 |
| J11 | `active == 0` | 子队列处于**挂起（suspend）**状态 ⇒ 先看是谁 suspend 了它、有没有人 kick（快照的 RESCHED 段就是为此） | 中高 | 正在执行的指令层面 |
| J12 | `DIAG wait …` 行里 `ckpt` **每秒都在变** | 不是死锁：CS 仍在推进（只是慢）⇒ 往性能/长路径方向查，而不是死锁 | 高 | 死锁假设 |

**交叉验证规则（务必同时满足，否则结论降级）**：
- J5 的流内结论要能与 marks 自洽：`ckpt ∈ [1..39]` ⇒ 必须 `pre≠0`（否则是陈旧读数）。
- J7/J8/J9 必须 `post_wait≠0`（45/42）；若 `ckpt==44 但 post_wait≠0` ⇒ 说明 44 是**上一次**尝试留下的、当前 job 已经过了等待 ⇒ 判读降级为"需再采一次快照"。
- 任何结论都要先过 §9 的四个陷阱。

---

## 8. 缺口与 v86/v87 建议（按性价比排序）

| # | 缺什么 | 建议插在哪 | 代价 | 收益 |
|---|---|---|---|---|
| **1** | **callee 流的最后检查点会被 wrapper 覆盖**（41/44/45/42 依次覆盖同一字）⇒ CALL 返回后**再也看不到流跑到哪一步** | **用空闲的 `cell+60`** 作为第二个检查点字：`kbase_checkpoint()` 写 56，wrapper **不碰 60** | 1 条 store/检查点（DIAG-only）+ 解码器加一行 + 快照多打一个字段 | **高**：能同时回答"流跑到哪"与"wrapper 卡在哪"，直接消掉 J6/J7/J8 的"上游不可见"问题 |
| **2** | 快照里**有两个都叫 `target` 的字段**（第一行=等待目标；`subqueue state` 行=`subq->kbase.emitted_jobs`）⇒ 极易读错 | 改名/加标注（纯文案） | 极低 | 中高：避免把"当前已发射数"误当"等待目标" |
| **3** | `stream progress` 累加失效无法用 OR 修（`cs_sync32_or` 不存在） | 若不采用建议 1，可把 progress 改成"STORE 最近一个 marker 的 id"（与 ckpt 同法）到**第二个字**；或干脆**删掉** progress、只留 ckpt | 低-中 | 中：减少误读 |
| **4** | 缺少"**该 entry 是否被固件取指过**"的显式判据（现在靠 `insert>extract` + `INSERT/EXTRACT` 推断） | 已有 `ENTRY … delta-from-extract` 行；无需新增 | 0 | 已满足 |
| **5** | 缺少 **heap 续租与卡死的时序关联**（`begin/end/postponed` 与 `DIAG wait` 的先后） | 无需插桩：用 logcat 时间戳对齐即可（报告 58 已给不变量） | 0 | 中 |

⇒ **只推 1 与 2**：1 是唯一"能改变结论能力"的插桩，2 是零成本防误读。

---

## 9. 四个读法陷阱（都会导致错误结论）

1. **progress ≠ 位图**：累加会进位（§5）⇒ 只看 `ckpt`。
2. **两个 `target`**：第一行 `target` = 等待目标（`kbase_subqueue_wait_seqno` 的局部量）；`kbase_log_subqueue_state` 打的 `target`/`jobs` = **当前 `emitted_jobs`**（可能更大）。两者不同是正常的，不要当成"seqno 落后"。
3. **陈旧 cell**：cell 是**跨 job 复用**的；`marks` 低 44 位才是 job 身份。看到 `ckpt` 但 marks 低 44 位 ≠ target ⇒ 该 `ckpt` 属于更早的 job。
4. **DIAG 开关**：`ckpt`/`marks`/`progress`/`ENTRY` 全部**只在 `PANVK_DEBUG=kbase_diag` 下才写**（`kbase_checkpoint()`/`kbase_mark_progress()` 开头就 `if (!PANVK_DEBUG(KBASE_DIAG)) return;`）。用 nodiag 版（`PANVK_DEBUG=0`）跑出来的快照里这些字段**全 0**，不是"卡在 0 步"——**定位卡死必须用 v84d 这类诊断版**。

---

## 10. 复现命令（只读）

```bash
F=/root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
grep -n "KBASE_SEQNO_" $F | head -14                      # cell 布局
sed -n '587,635p' $F                                      # id → 名字表
grep -n "kbase_checkpoint_name\|=> id 0x%x" $F            # 快照里的解码行
grep -rn "PANVK_KBASE_CKPT_[A-Z_]*)" \
   /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_cmd_*.c   # 全部发射点
sed -n '88,127p' /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_cmd_buffer.h  # 机制原文
grep -n "cs_sync32_or" /root/zenithblue/work/mesa/src/panfrost/genxml/cs_builder.h      # 0 命中
sed -n '405,450p' $F                                      # ENTRY 解码规则
```

---

## 11. 实测校验（v84d 现场，2026-10-06）——含 1 条**机制更正**、3 条**手册纠错**、2 条**新增判读档**

### 11.1 现场原文（Lead 提供，逐字）

```
kbase: CKPT timeout snapshot subqueue 0 word 0xc0de002a => id 0x2a (WRAP_BEFORE_SYNC_ADD)
kbase: CKPT timeout snapshot subqueue 1 word 0xc0de001b => id 0x1b (BARRIER_BEFORE_QUEUE_WAIT)
kbase: CKPT timeout snapshot subqueue 2 word 0xc0de001b => id 0x1b (BARRIER_BEFORE_QUEUE_WAIT)
kbase: timeout on subqueue 1: seqno 124, ls_copy 124, target 125,
        marks pre/post-call/post-wait 0x10000000007d/0x30000007c/0x30000000007c,
        stream progress 0x49, insert 56000, extract 55720, active 0, error 0x0
kbase: ENTRY subqueue 1 extract pointer: ring offset 55720, delta-from-extract 0 B,
        16 raw bytes: 00 00 00 00 7e 7c 00 20 40 c0 b0 f8 5f 00 7c 01
kbase: ENTRY subqueue 1 ... first instr 0x20007c7e00000000 opcode 0x20 (CALL) wait_mask 0x0000 => NORMAL-CLASS
```
环境：`PANVK_DEBUG=1,kbase_diag` 生效；5 秒内 extract/seqno/marks 完全冻结；随后 DEVICE_LOST；hs_err 无原生崩溃。

### 11.2 先答两个"字面问题"

1. **`0x1b` 属 callee 还是 wrapper？——属 callee（流内）。** `0x1b = 27 = PANVK_KBASE_CKPT_BARRIER_BEFORE_QUEUE_WAIT`，位于枚举的 **1..39 段**（`csf/panvk_cmd_buffer.h:155`），写入点 = `csf/panvk_vX_cmd_buffer.c:704`（`emit_barrier_insert_waits()`，函数定义在 `:686`）。`0x2a = 42 = WRAP_BEFORE_SYNC_ADD` 属 wrapper 段（40..45），写入点 `csf/panvk_vX_gpu_queue.c:1582`。
   ⇒ **"CS 没进 callee"被排除**：wrapper 在每个 CALL 之前把该字清零（`:1473`），现在读到 27 ⇒ **是本 job 的 callee 写进去的**。
2. **`post-call = 0x30000007c` 这个值在代码上不可能出现**（数据完整性问题，必须回原始日志核对）：
   `MARK_POST_CALL = 0x200000000000`（`:98`），job 124 的应有值 = **`0x20000000007c`**；转录值 `0x30000007c` 只有 9 位十六进制且首位是 3（= `3<<32|0x7c`），既不是 POST_CALL 也不是 POST_WAIT（后者 = `0x30000000007c`，另两位已给出）。
   ⇒ 三种可能：**(a)** 手抄掉了几位（最可能，正确值 `0x20000000007c`）；**(b)** 打印/解析错位；**(c)** 我的 marks 模型有误。**在拿到原始行之前，下面的判读按 (a) 成立来写**——但请把原始 `kbase: timeout on subqueue 1` 那一行原样贴回，我据此定稿。

### 11.3 ★ 机制更正：消费者等的是 **`syncobjs[j]`**，**不是** wrapper 的 `cell+0`

- **`cs_progress_seqno_reg(b, j)`** 是一个**寄存器**（`cs_reg64(b, PANVK_CS_REG_PROGRESS_SEQNO_START + j*2)`，`csf/panvk_cmd_buffer.h:515-521`），不是内存。消费者用它 + `relative_sync_point` 算出**阈值**。
- 真正的**信号载体是 GPU 内存 `ctx->syncobjs[j]`**：`emit_barrier_insert_waits()`（`panvk_vX_cmd_buffer.c:687-710`）做
  `sync_addr = &ctx->syncobjs[j]`、`wait_val = progress_seqno_reg(j) + relative_sync_point`、`panvk_instr_sync64_wait(GREATER, wait_val, sync_addr)` —— **等的是 `syncobjs[j].seqno` 越过阈值**。
- **谁推进 `syncobjs[0]`（VT）**：
  - `panvk_vX_cmd_draw.c:4274`：`flush_tiling()` 里的 `panvk_instr_sync64_add(VERTEX_TILER, …, cs_defer_indirect())` —— **这才是跨子队列的 VT 进度信号**；
  - 同一函数 `:4271` 的 `cs_vt_end(b, cs_defer_indirect())`；（两者都紧跟 `cs_finish_tiling(b)` `:4252`）
  - `panvk_vX_cmd_buffer.c:788`：屏障里**有条件的**信号 `if (wait_subqueue_mask & BITFIELD_BIT(i))` —— **"没人等我就不发信号"**。
- **wrapper 的 deferred `cs_sync64_add`（`panvk_vX_gpu_queue.c:1585`）推进的是 `cell+0`（宿主可见的完成计数）**，与本条等待**无关**。
  ⇒ **Lead 的链需要改一处**："deferred SYNC_ADD64 执行 → VT progress seqno 前进" **不成立**；正确的表述是：
  **"tiler 完成 → `flush_tiling()` 的两条 indirect 异步 op（`:4271` `cs_vt_end` / `:4274` `sync64_add`）执行 → `syncobjs[0]` 前进 → FRAG/COMPUTE 在 `27` 的等待才可能满足"**。

### 11.4 那么"distress 现场"如何解释？——两条互斥候选，**当前快照分不开**

VT 停在 **42** 有一个强含义：42 写在 **wrapper 全槽等待（44→45）之后**（见 §3.1 执行顺序）⇒ **VT 那个 entry 的所有 scoreboard 槽都已退休**。若 `:4271`/`:4274` 这两条 indirect op 各自占一个槽，那么它们**已经退休**（即很可能**已经把 `syncobjs[0]` 加了 1**）。于是消费者的等待仍未满足，只剩两类解释：

| 候选 | 内容 | 支持/反对 | 如何判 |
|---|---|---|---|
| **P1 生产者信号确实没到** | `syncobjs[0]` 没有前进（例如 42 是**上一次** entry 留下的、当前 VT entry 尚未写 42；或该 entry 的 indirect op 因**寄存器给出的 wait mask 指向没人 signal 的槽**而静默停住） | 机制**存在**：`cs_defer_indirect()` = `{wait_mask=0xff, signal=0xff, indirect=true}`（`genxml/cs_builder.h:808-815`）⇒ 真实 wait/signal 槽**来自寄存器**；本树自己在 `panvk_vX_gpu_queue.c:1397-1406` 就警告过"会让 in-flight endpoint op 等一个没人会 signal 的槽" | 看 **`syncobjs[0].seqno` 当前值**（宿主可直接读，见 §11.6-a） |
| **P2 生产者的条件信号被跳过 / 阈值不可达** | `syncobjs[0]` 前进了，但消费者的阈值 `progress_seqno_reg(0) + relative_sync_point` **高于**它（例如 `emit_barrier_csf` 在 `:777` 因"当时没人等我"而**跳过了一次 +1**，而消费者的 `relative_sync_point` 仍按"应有"计数） | 机制**存在且是我们自己的代码**：信号是**有条件**的（`:777`），而计数器/阈值是**按 deps 记账**的 | 看**消费者的阈值**（需 §11.6-b 插桩）与 `syncobjs[0]` 对比 |

**两条都不支持"互等成环"**：SUBQUEUE 0(VT) 并没有停在 27，而是停在 42（= 已过全槽等待），所以**本现场不存在 A↔B 环**；环只可能在"VT 也停在 27"的情形出现。⇒ ②的答案：**不是死锁环，是"两个消费者等同一个生产者，而该生产者的信号（或其阈值记账）有问题"**。

### 11.5 新增/修正的判读规则

| 编号 | 规则 | 置信度 |
|---|---|---|
| **R1（范围规则，新增）** | **id 1..39 = callee/流内；40..45 = wrapper**。名字表只说"叫什么"，不说"属哪一侧"；**这条决定"CS 是否进过 callee"** | 高（枚举分段是硬事实） |
| **R2（CALL 陷阱，新增）** | `ENTRY` 解出 `CALL / NORMAL-CLASS` **不等于**"不是在等 scoreboard"：**CALL 执行期间 ring 的 `extract` 不前进**（停在 CALL 上），真正的等待在 **callee 内部**。⇒ 取指指针是 CALL 时，**必须用 CKPT 字定位**，`ENTRY` 只用于确认"已进入 CALL" | 高（本现场即此形态） |
| **J13（新增档）** | `extract` 停在 CALL + `ckpt ∈ [1..39]` + `marks.pre == target` ⇒ **停在 callee 流内的该步**（本例 = `27 BARRIER_BEFORE_QUEUE_WAIT`，即**等某个子队列的 `syncobjs[j]`**）；`insert > extract` 与 `active` 可作旁证（Δ=280 B 未取完） | 高 |
| **J14（新增档）** | **两个子队列停在同一个 ckpt** ⇒ 它们在等**同一个生产者**；先去读那个生产者的 `syncobjs`/ckpt（本例：两个消费者都在 27，生产者的候选 = VT(0)） | 中高 |
| **C1（纠错）** | `ckpt == 42` **不是**"卡在 deferred add"的充分条件：42 也是**一个正常完成 entry 的终值**。必须配 `seqno < target` 才能判"卡在 deferred 完成写"（我 §7 的 J9 已带该条件，但 §3.1 的措辞要按此收紧） | 高 |
| **C2（纠错）** | `last barrier wait mask == 0` 在停在 **27** 时是**预期**：写该字（`cell+52`）的 `kbase_record_wait_mask()` 只在 **SB 等待分支**（`panvk_vX_cmd_buffer.c:744-747`）里被调用，而 27 在它**之前**。⇒ 不能用 mask==0 推断"没有屏障等待" | 高 |
| **C3（纠错）** | `WRAP_BEFORE_SYNC_ADD(42)` = "最后一个可观测点"**同时**意味着"该 entry 的全槽等待（44→45）已经通过"⇒ 可以用来**反推**该 entry 的间接异步 op 已退休（VD 侧的重要推论，见 §11.4） | 中高 |

### 11.6 v86 最小插桩方案（**目标：一眼看出"谁没推进"**）

**(a) 零成本、可常开（宿主侧，不需要 GPU 指令）**：把 `syncobjs[0..2].seqno` 打进超时快照。
- `queue->syncobjs` 是宿主已映射的 `panvk_priv_mem`（上下文里存的是它的 dev 地址），`struct panvk_cs_sync64` 每条 16 B（`panvk_cs_sync64{uint64 seqno; uint32 error; uint32 pad;}`，`panvk_cmd_buffer.h:55-59`）。
- 做法：在 `kbase_log_subqueue_state()` 或新增 `kbase_log_syncobjs()` 里，对 i=0..2 读 `panvk_priv_mem_host_addr(queue->syncobjs) + i*stride` 打印。**~8 行、无 GPU 开销、nodiag 版也能用。**
- 立刻能回答 P1 与 P2 的分界：**生产者的计数器到底动没动**。

**(b) 一个 store（DIAG-only），把"消费者的阈值"也打出来**：在 `emit_barrier_insert_waits()`（`panvk_vX_cmd_buffer.c:687-710`）的循环里，紧跟现有 checkpoint 之后存一次
```
store32( (uint32_t)wait_val | (j << 24), <cell+60 或 ctx->debug.wait_target[i]> )
```
- **最省版**：写进本手册 §2 表里**空闲的 `cell+60`**（32 位）：低 24 位放 `wait_val` 低 24 位，bit24..25 放生产者 id `j`；快照侧加一行打印 ⇒ **1 条 store + 1 行打印**，且 `cell+60` 目前无人使用（§2 表已列）。
- **更完整版**：在 `struct panvk_cs_subqueue_context.debug` 加 `uint32_t wait_target[PANVK_SUBQUEUE_COUNT];` + `uint8_t wait_producer[...]`（~16 B），每个 wait 存一条 ⇒ 可同时看到多个等待。代价：结构体 +16 B（`panvk_cmd_buffer.h` 的 ctx 结构）、~6 行发射、~10 行打印。
- 有了 (a)+(b)，快照就能直接给出：**"消费者 1 等生产者 0：阈值 T，当前 S"** ⇒ 若 `S >= T` 则等待条件其实已满足（问题在别处）；若 `S < T` 则是 P1（生产者没推进）。

**(c) 可选、针对本例最有价值**：把 **indirect 异步 op 的真实 wait mask / signal 槽**也采出来。它们的操作数在寄存器里（`SB_MASK_WAIT` / `SB_SEL_DEFERRED`，见 `panvk_vX_gpu_queue.c:1397-1406` 的说明），因此宿主看不到。可在 `flush_tiling()` 的 `:4271`/`:4274` 附近（DIAG-only）把这两个寄存器的值 STORE 到 `cell+60`（或另一个空闲字）⇒ 直接回答"那条 indirect op 在等哪个槽"。
- 前提待确认（我未验证）：这两个状态寄存器能否用现有机制读到 CS 寄存器空间（本树已有 `cs_sr_reg64(b, IDVS, VERTEX_FAU)` 之类的先例，所以**机制存在**，但具体寄存器号需要查 `cs_builder.h`/genxml）。
- 代价：2 条 store（DIAG-only）+ 打印 1 行；**不影响功能路径**。

**优先级**：(a) 先做（零成本，今天就能答 P1/P2）→ (b) 最省版（1 store）→ (c) 仅在 (a)(b) 仍分不开时做。

### 11.7 现场链路定稿（含对"新旧字段"的可用性裁定）

**A. 逐字段裁定**

| 字段 | 值（现场） | 能不能当证据 | 理由（file:line） |
|---|---|---|---|
| `ckpt` subqueue 1/2 | `0xc0de001b` = 27 `BARRIER_BEFORE_QUEUE_WAIT` | ✅ **是**（唯一 ID，callee 段 1..39） | 写入点 `panvk_vX_cmd_buffer.c:704`；wrapper 在 CALL 前清零（`panvk_vX_gpu_queue.c:1473`）⇒ 证明**本 job 的 callee 已进入并跑到该步** |
| `ckpt` subqueue 0 | `0xc0de002a` = 42 `WRAP_BEFORE_SYNC_ADD` | ⚠️ **半可用**（42 是**任何已完成 entry 的终值**，见 C1） | 写入点 `:1582`；**必须**配 subqueue 0 自己的 `marks`/`cell+0` 才能判"当前 entry 到了 42"还是"上一条 entry 的残留" |
| `marks` subqueue 1 | pre=`0x10000000007d`(125)、post-call=`0x300…`(**不合语法**)、post-wait=`0x30000000007c`(124) | ⚠️ post-call 需原始行 | `MARK_POST_CALL=0x200000000000`（`:98`）⇒ job 124 的 post-call **只能**是 `0x20000000007c`（见 §11.2-2） |
| `cell+0` subqueue 1 = 124 vs target 125 | "atomic DID NOT land" | ✅ **但对定因是平凡信息** | verdict 表 `:1028-1045`（`uncached_read + 1 == target_seqno` 分支）；**注意这行是 subqueue 1 自己的完成计数**——它卡在 callee 里，"落后 1"是**必然结果**，**不构成对 VT 的独立证据** |
| `DEFER slot state`（slot id 1 / mask 0x0002） | — | ❌ **不是运行态证据**（见 C5） | `SB_ID(DEFERRED_SYNC)`=1、`SB_MASK(DEFERRED_SYNC)`=0x0002 是**编译期常量**（`panvk_cmd_buffer.h:369/375`）；该行唯一的运行态字段是 `CS_STATUS_SCOREBOARDS`（读 user_io 页 2，`gpu_queue.c:878-880,889-892`）⇒ 按交接 §4 的读法陷阱**不可信** |
| `stream progress 0x49` | — | ❌ **不可用**（见 §11.8-B 的算例） | 累加型（`cs_sync32_add`，`panvk_vX_cmd_buffer.c:79`）+ 位号在 VT/FRAG/COMPUTE 间复用 + 进位 |
| `active 0` / `error 0x0` | — | ❌ **不可用**（C4） | 二者都取自 user_io 输出页（`CS_ACTIVE` / `CS_FAULT*`）⇒ 交接 §4：页 2 **不镜像**内核 CS 寄存器块 ⇒ 全 0 不是证据 |
| `insert 56000 / extract 55720` | Δ=280 B | ✅ 可用 | 取指停在 `CALL`（ENTRY 解码）⇒ **Δ>0 与 "callee 未返回" 自洽**；同时排除 J2（没派发） |
| `ls_copy 124` | 未达 125 | ✅ 与"全槽等待未过"一致（对 subqueue 1） | `:1552` 之后的 DIAG 写；只在全槽等待通过后写 |

**B. 为什么 hang 时 subqueue 0 的字"必然还是 42"——项目自己的论证（`panvk_vX_gpu_queue.c:1572-1587`，逐字要点）**

> 两条例由，都致命：
> **1)** deferred `SYNC_ADD64` 在飞时仍读它的操作数（`addr64` = seqno cell、`val64` = addend 1，寄存器 14..17），而这四个是 ring entry 唯一可被 FW 破坏的寄存器；一个 checkpoint STORE 需要其中两个 ⇒ 在那里插 store 会**踩掉活的完成操作数**。
> **2)** 而且**不可观测**：LS 操作按程序序退休，所以排在 deferred `SYNC_ADD64` 之后的 store **只有在那个 sync add 已经执行之后才会落地**——也就是"作业已经不 hang 了"的那一刻。**在 hang 的情形下，那个字必然仍读 42。**
> 结论（原文）："Did the completion counter get written?" 由 `cell+0` 的硬件 seqno **免费回答**：seqno 到达 target ⇔ 计数器前进。

⇒ 所以 42 是**设计上的终点值**，不是"卡住的位置"；**它必须与同一子队列的 `cell+0` 联合解读**（这也正是 C1 的收紧版）。

**C. 为什么 `progress 0x49` 不能用（算例）**

`0x49 = 0b1001001` ⇒ 置位 bit0、bit3、bit6。若按位读（FRAG 子队列）：bit0=`CMDBUF_START(1)`、bit3=`FRAG_AFTER_TILING_WAIT(1<<3)`、bit6=`FRAG_AFTER_FINISH(1<<6)`。
但该子队列的 `ckpt=27` 说明它**已经进到屏障段**（`emit_barrier_csf` 之后的位置）——如果它确实做过 render pass 的片元工作，`FRAG_ENTER(1<<1)`/`BEFORE_TILING_WAIT(1<<2)` 等**更早**的 marker 不可能没发；而 0x49 里 **bit1/bit2 恰好缺失**。
两种解释都指向同一结论：**这些位已经被"重复发射 + 进位"污染**（例如 `FRAG_ENTER` 发了 2 次 ⇒ 2+2=4 表现为 bit2，或相加进位吃掉低位），**因此不可按位解读**——与项目文档 `panvk_cmd_buffer.h:90-96` 的原文一致（"deliberately accumulates … carries into unrelated bits as soon as some bit is used twice — which is why the PANVK_KBASE_PROGRESS_* word above cannot be read as a bitmap"）。

**D. 最终判读（分层，含置信度）**

1. **确定**（高）：subqueue 1/2 的 CS **已进入各自的 callee 流**，并**停在 `BARRIER_BEFORE_QUEUE_WAIT`（27）**，即**正在等某个生产者子队列 j 的 `syncobjs[j]`**（`panvk_vX_cmd_buffer.c:687-710`：等 `syncobjs[j].seqno > progress_seqno_reg(j) + relative_sync_point`）。
2. **确定**（高）：subqueue 1 自己的完成计数**未落地**（124 vs 125），这与"卡在 callee"互为因果（不是独立线索）。
3. **高度可能**（中高）：两个消费者在等**同一个生产者**（J14），且该生产者最可能是 **VT(0)**——因为只有 VT 的流尾会向 `syncobjs[0]` 加计数（`panvk_vX_cmd_draw.c:4274`），而 subqueue 0 是本现场唯一"看起来已到流尾"的子队列。
4. **未定**（需一条数据）：**subqueue 0 的 `cell+0` 是否到达它的 target**、以及 **subqueue 0 的 marks 属于哪个 job**。
   - 若 subqueue 0 `cell+0 == target`（且 marks 属当前 job）⇒ VT 的 deferred add **已执行** ⇒ 生产者信号已发 ⇒ 问题落在**消费者的阈值**（P2：`relative_sync_point`/`progress_seqno_reg` 记账，或 `emit_barrier_csf:777` 的**条件信号**被跳过）⇒ 这是我们自己 wrapper/记账的 bug。
   - 若 subqueue 0 `cell+0 == target-1`（且 marks 属当前 job）⇒ VT 的 deferred add **确实没执行** ⇒ 回到"VT entry 卡在 42 之前/不能推进"（P1）⇒ 需要 §11.8 的插桩看它卡在哪个槽。
   - 若 subqueue 0 的 marks 仍是**上一条 job** ⇒ 当前 VT entry **根本没开始**（J2 类）⇒ 是调度/派发问题，而不是槽问题。
   ⇒ **请把 `kbase: timeout snapshot subqueue 0: seqno …` 整行（含 marks）贴回**；这是唯一能把 P1/P2/J2 分开的一条数据。

### 11.8 v86 最小插桩（定稿：要加什么、**读不读得到**、代价、排序）

**A. 能读到 / 读不到（先划边界，避免无效插桩）**

| 数据源 | 宿主可读？ | 依据 |
|---|---|---|
| 宿主已映射的 BO：`queue->syncobjs`、各 `subq->context`、`queue->kbase_seqnos`（seqno cell）、ringbuf、`tiler_heap.desc` | ✅ 可读 | 全是 `panvk_priv_mem`；`syncobjs` 的 dev 地址就存在 ctx 里（`gpu_queue.c:2759` 附近） |
| user_io **输入/输出页** 的 `CS_INSERT`/`CS_EXTRACT` | ✅ 可读（现已在用） | `kbase_subqueue_publish()` / 快照 |
| user_io 输出页的 `CS_ACTIVE` / `CS_FAULT*` / `CS_STATUS_SCOREBOARDS` | ❌ **不可信** | 交接 §4：页 2 **不镜像**内核 CS 寄存器块 ⇒ 全 0 无意义。（代码仍读它们：`gpu_queue.c:880,889-892`、`:1670-1677`） |
| CS 寄存器 r0..r47（含 `SB_MASK_WAIT`/`SB_SEL_DEFERRED` 的**当前值**） | ❌ 宿主不可读 | 只能由 GPU 侧 store 导出（本树已有 `cs_sr_reg*` 先例，但需要具体寄存器号） |
| scoreboard 槽的**实时**占用/待决位 | ❌ 不可信 | 唯一来源是上面的 `CS_STATUS_SCOREBOARDS`（页 2） |
| tiler 的 `completed_top/bottom`（FINISH_FRAGMENT 的实参） | ⚠️ 理论上可读但**地址难拿** | 它们在 per-cmdbuf 的 tiler context（`cur_tiler + 40`），队列在超时时并不知道该 cmdbuf 的地址 ⇒ 需要额外记录，性价比低 |

**B. 建议新增的字段（按价值排序）**

| # | 字段（快照新增） | 怎么来 | 改动量 | 开关 | 代价/风险 |
|---|---|---|---|---|---|
| **1** | **`syncobjs[0..2].seqno` 三个值**（跨子队列进度计数） | **宿主直接读**已映射的 `queue->syncobjs` BO（`panvk_priv_mem_host_addr(queue->syncobjs) + i*sizeof(struct panvk_cs_sync64)`，`panvk_cs_sync64{seqno;error;pad;}` `panvk_cmd_buffer.h:55-59`） | `kbase_log_subqueue_state()` 里 +1 个循环 / 新增 `kbase_log_syncobjs()`，**~8 行** | **常开**（纯宿主读，无 GPU 指令） | 零风险、零 GPU 开销 |
| **2** | **每个消费者的"等待阈值 + 生产者 id"** | 在 `emit_barrier_insert_waits()` 循环里（`panvk_vX_cmd_buffer.c:700-706`）紧跟 checkpoint 之后：`store32( (uint32_t)wait_val | (j << 24), cell+60 )`（`cell+60` 目前空闲，见 §2 表；`kbase_seqno_stride()=64`，`gpu_queue.c:231-234`） | 发射 **+2~3 行**；打印 +2 行 | **DIAG-only**（与相邻 checkpoint 同门控） | 极低（每 wait 1 条 store，且只在诊断版）；风险=0（写的是空闲字） |
| **3** | **间接异步 op 的真实 wait mask / signal 槽** | 现读的 `CS_STATUS_SCOREBOARDS` **不可信** ⇒ 应改为**在 `flush_tiling()` 的 `:4271`/`:4274` 附近把 `SB_MASK_WAIT`/`SB_SEL_DEFERRED` 用 `cs_sr_reg*` 导出**到另一空闲字（若 cell 内已无空位，可在 ctx 的 `debug` 区加 4 B） | 发射 +3~4 行；打印 +2 行 | DIAG-only | 中低（需确认这两个状态寄存器的寄存器号/可读性——**我未验证**，落地前需查 `cs_builder.h`/genxml） |
| **4** | **"当前 entry 是否已开始"的显式标记** | 其实**已经有**：marks（低 44 位 = job 号） | 0（改文档即可） | — | 0 |

**C. 如果只能加一个检查点——加在哪里？（排序与理由）**

1. **首选不是一个"检查点"，而是字段 #1（`syncobjs[0..2].seqno`，宿主读，常开）。** 理由：检查点只能记录"流跑到哪"，而本轮的死结是**跨子队列的计数**——`syncobjs[j]` 是**生产者 deferred add 的"效果"**，是唯一能在宿主侧观测"那条 indirect op 到底退休没退休"的量（因为按项目自己的论证，op 之后无法再插可观测的 store）。配合**已经在打印**的每个子队列 `seqno`/`jobs`（`kbase_log_subqueue_state`），P1/P2/J2 立刻可分。
2. 若 #1 显示"生产者计数**已**前进、消费者**仍在**等" ⇒ 加 **字段 #2**（阈值），定位是**阈值记账**问题（最可能是 `emit_barrier_csf:777` 的**条件信号**或 `relative_sync_point` 计数）。
3. 若 #1 显示"生产者计数**没**前进" ⇒ 加 **字段 #3**（把不可信的页 2 读换成 GPU 侧导出），看那条 indirect op 是不是在**等自己的 signal 槽**（`SB_MASK_WAIT` 含 bit1 而 `SB_SEL_DEFERRED=1` —— 这正是 `cs_defer()` 对直接路径会 assert 掉的情形，`cs_builder.h:784-795`，但**间接路径没有这个保护**）。
4. **不要**把任何新字段建在 `CS_ACTIVE`/`CS_FAULT`/`CS_STATUS_*` 之上（页 2 不镜像）；**不要**用 `progress` 做判据（§11.7-C）。

**D. 建议同时做的"零成本收尾"**

- **把 `kbase_log_subqueue_state` 的输出做成"每子队列一行、字段顺序固定"**，并在超时快照里**对三个子队列都打印**（现在对全部子队列都调用，但阅读时容易只看被超时的那一个）——本轮 P1/P2 分不开，直接原因就是只看 subqueue 1 的那行。

### 11.9 定论节（v84d 现场 + 代码）：能定的、不能定的、以及**唯一那条还没测的量**

#### A. 已被**实测或代码**排除的解释（逐条给依据）

| # | 候选解释 | 状态 | 依据 |
|---|---|---|---|
| 1 | **tiler 堆 OOM** | ❌ 排除 | 现场 `tiler_oom.counter 0`、`cond_render_flag 0`；且 `setup_tiler_oom_ctx()` 的处理器只有在该计数非 0 时才说明走过 OOM 路径（`panvk_vX_cmd_draw.c:4568-4581`） |
| 2 | **核内死锁（A↔B 环）** | ❌ 排除 | 消费者(1/2)停在 27；生产者 subqueue 0 **不在** 27，而是完成态（`seqno==target==jobs==125`、`CBMDUB_DONE` SET、`insert==extract`）⇒ 没有环。代码侧另有 `assert(!(deps.dst[i].wait_subqueue_mask & BITFIELD_BIT(i)))`（自等待）与 `:719-721` 的"同一子队列不做两类等待"⇒ 结构上也不倾向成环 |
| 3 | **生产者（VT）未退休** | ❌ 排除 | subqueue 0 `cell+0 = 125 = target`、`CMDBUF_DONE(24)` 已退休、`insert == extract == 56000`、marks 三值均为 `0x…7d`(125) ⇒ VT 那个 ring entry 从 wrapper 视角**完整跑完** |
| 4 | **`relative_sync_point` 的 per-iteration 记账 off-by-one** | ❌ 排除 | `flush_tiling()` 在 `#if PAN_ARCH >= 11` 分支（**v12 走此分支**）里对 VT **只有一条** `sync64_add`（`panvk_vX_cmd_draw.c:4274`）+ **一次** `++relative_sync_point`（`:4300`）⇒ 1:1 自洽；逐迭代 `cs_match_iter_sb` 的发信号只在 `#else`（≤10）分支（`:4280-4297`） |
| 5 | **`cs_progress_seqno_reg` 寄存器区间被覆盖** | ❌ 排除 | `PANVK_CS_REG_PROGRESS_SEQNO_START..END` = 6 个寄存器（v13+ 为 116..121；v11/12 为 84..89），**与 scratch 区（…≤83/≤115）和 `SUBQUEUE_CTX`（90..91 / 122..123）互不重叠**（`panvk_cmd_buffer.h:453-468`）；且写入受 reg-whitelist 机制约束（`:629-630`） |
| 6 | **`DEFER slot state` 行给出的运行态** | ❌ 不可用 | 该行里 `slot id 1`/`mask 0x0002` 是**编译期常量**（`panvk_cmd_buffer.h:369/375`）；唯一运行态字段 `CS_STATUS_SCOREBOARDS` 读 user_io **页 2**，按交接 §4 **不镜像** ⇒ 全 0 非证据 |

#### B. **不能**由现有数据决定的：Lead 的收敛假设**既未被证实也未被否证**——它卡在一个我原以为能排除它的细节上

Lead 的链：`cs_defer_indirect()` 那两条 op（`cmd_draw.c:4271` `cs_vt_end` / `:4274` `sync64_add`）等"间接迭代槽"，槽不被 signal ⇒ op 永挂 ⇒ `syncobjs[0]` 停滞 ⇒ 消费者卡在 27。

**我原想用"cell+0 已前进"否证它**，理由链是：
1. wrapper 的顺序是 **先全槽等待、后完成 add**：`cs_wait_slots(&b, dev->csf.sb.all_mask)`（44→45；**v84 布局** `:1539`/`:1544`，当前树 `:1563`/`:1566` —— 见 §11.12 的行号基线说明）→ … → 42（`:1582`）→ `cs_move64_to(&b, val64, 1); cs_sync64_add(..., cs_defer(0, SB_ID(DEFERRED_SYNC)))`（**v84 布局** `:1582`(42)/`:1585`(add)；当前树 `:1604`/`:1606-1607`）。
2. 现场 `cell+0 == target` ⇒ 那条 add **已发射并执行** ⇒ 全槽等待**已通过** ⇒ 该 entry 的所有（被等待的）槽都已退休。
3. 若那两条 indirect op 占用的槽 ∈ `all_mask`，则它们**也已退休** ⇒ `syncobjs[0]` 应当已前进 ⇒ Lead 的假设被否证。

**但这个推理有一个漏洞**（本轮最重要的发现）：
- `cs_defer_indirect()` 在**发射时**把 `wait_mask` 与 `signal_slot` 都写成占位值 **`0xff`**（`cs_builder.h:806-815`），**真正的槽号来自 CS 寄存器 `SB_MASK_WAIT` / `SB_SEL_DEFERRED`**（`panvk_vX_gpu_queue.c:1397-1406` 的注释逐字点名 `cs_vt_end, cs_finish_fragment, RUN_FRAGMENT, compute dispatch` 这四条 indirect op）。
- wrapper 的全槽等待只覆盖 **`dev->csf.sb.all_mask` = `BITFIELD_MASK(sb.count)`**，即**槽号 < slot_count** 的那些槽。
- ⇒ **若那两条 op 的有效 signal 槽落在 `all_mask` 之外（或为无效值 0xff/超出范围），它们可以一直挂着，而 wrapper 照样完成、`cell+0` 照样前进。**
- 而 `SB_SEL_DEFERRED`/`SB_MASK_WAIT` 是 **CS 寄存器，宿主读不到**；`DEFER slot state` 行想读的那个页 2 字段又不可信（见 A.6）。
- ⇒ **当前数据无法区分**：**H1（indirect op 挂着 ⇒ `syncobjs[0]` 停滞）** vs **H2（op 已退休、`syncobjs[0]` 已前进，但消费者的阈值不可达）**。

**旁证（弱）**：现场 subqueue 1/2 的 `post-call` 字段高 32 位是异常值（`0x00000003` / `0xfff80000`，低 32 位是正确的 job 号 124）—— 这不是本结论的载荷，但它提示**那些 cell 的高字存在写入/读取异常**，值得单独用"原始 8 字节 dump"确认（§11.10-#4）。

#### C. **唯一那条还没测、且能一刀切开的量**：`syncobjs[0..2].seqno`

- 它是**宿主可直接读**的（`queue->syncobjs` 是已映射的 `panvk_priv_mem`；`struct panvk_cs_sync64` 每子队列 16 B）。
- 判据：
  - **`syncobjs[0]` 已前进（≥ 期望值）⇒ H2 成立**（生产者信号到位）⇒ 病根在**消费者阈值/镜像**（`mirror(j) + rel(j)`），与 A.4 的"记账 1:1 自洽"不矛盾——问题可能出在**镜像寄存器**而非 `rel`。
  - **`syncobjs[0]` 停滞 ⇒ H1 成立** ⇒ 那两条 indirect op 确实没退休 ⇒ 需要 §11.10-#3 导出 `SB_SEL_DEFERRED`/`SB_MASK_WAIT` 才能说"等的是哪个槽、为什么没人 signal"。
- **读出它不需要改任何 GPU 指令**（见 §11.10-#1），是今晚就能定案的一步。

#### D. 与"关掉续租 ⇒ 卡死提前到 seqno 71"的一致性

该观察对 H1/H2 **都成立**（续租改变的是 tiler 侧的时序与堆状态，两种机制都能被它改变触发点），因此**不能**用来在 H1/H2 之间做选择——它只说明"续租与 tiler 侧的推进有耦合"，这对两条候选都是同向证据。

### 11.10 v86 最小插桩（按"能否切开 H1/H2"排序）

| 顺序 | 项 | 怎么来 | 改动量 / 开关 | 能切开什么 |
|---|---|---|---|---|
| **1（先做）** | **`syncobjs[0..2].seqno`** | **宿主读已映射 BO**（无需 GPU 指令） | `kbase_log_subqueue_state()` 加 ~8 行；**常开** | **直接切开 H1/H2**（本轮唯一决定性测量） |
| **2** | 消费者的**阈值 + 生产者 id** | `store32((uint32_t)wait_val \| (j<<24), cell+60)`，插在 `panvk_vX_cmd_buffer.c:703`（checkpoint 27 之后）；`cell+60` 空闲 | +2~3 行发射 / +2 行打印；**DIAG-only** | H2 成立时，量出**差多少**（阈值 vs 实际），并定位是镜像还是 `rel` |
| **3** | **间接 op 的有效等待/信号槽**（`SB_MASK_WAIT`/`SB_SEL_DEFERRED`） | 只能用 `cs_sr_reg*` **从 GPU 侧导出**到空闲字（页 2 读不可信；寄存器号待查，我未验证） | +3~4 行；**DIAG-only** | H1 成立时，回答"等的是哪个槽、它 ∈ all_mask 吗" |
| **4** | 各子队列 `cell+32`/`cell+40` 的**原始 8 字节** | 宿主读 cell（现成） | +2 行打印；**常开** | 顺带确认 A 节末尾的**高字异常**（是否真有撕裂/丢写） |

> **如果只能加一项：加 #1。** 它零成本、常开、无风险，而且**当前唯一**能在 H1/H2 之间做判决的量；#2 只在 H2 成立后才有用，#3 只在 H1 成立后才有用。

### 11.11 最小修复候选（按风险排序；**均需先有 §11.10-#1 的读数**）

| 候选 | 内容 | 风险 | 说明 |
|---|---|---|---|
| **F1（零风险，先做）** | §11.10-#1（+#2）**只加观测**，不动语义 | 0 | 在没有 H1/H2 判决前改语义 = 盲改。**推荐今晚只做这个** |
| **F2（针对 H1，中高）** | 把 VT 那条跨子队列信号的**槽依赖从"间接寄存器"改成"显式编译期槽"**：在 `flush_tiling()` 里对 `sync64_add` 用 ≤10 分支同款的 `cs_defer(SB_WAIT_ITER(x), SB_ID(DEFERRED_SYNC))`（`panvk_vX_cmd_draw.c:4280-4297` 的现成范式），使 wait/signal 槽**不再依赖 `SB_SEL_DEFERRED`** | **中高**：直接触碰 tiler 完成时序；本树 C1 已记载"抑制三条 heap ops ⇒ 第 3~4 个作业就卡死"⇒ 该区域极脆弱 | 优点是**从根上消掉"寄存器漂移"这一类**（本树此前已为同类原因移除过 per-entry `SET_STATE`，见 `panvk_vX_gpu_queue.c:1386-1406`）。**必须单独一个变量上机** |
| **F3（针对 H2，中）** | 修**阈值/镜像**：让消费者等"生产者自己发布的绝对完成计数"，而不是 `镜像寄存器 + rel`；最小形态是**把镜像改为在等待点直接从 `syncobjs[j]` 读一次**（`cs_load64_to` 到 scratch）再算目标，绕开"镜像寄存器长期漂移"的可能 | 中 | 语义等价（仍是 GREATER + 相对点），但不再依赖长期维护的镜像寄存器 |
| **F4（禁用）** | 把等待条件从 `GREATER` 改成 `GREATER_OR_EQUAL` | **禁止** | 会**提前一个信号放行** ⇒ 消费者可能读到生产者尚未写完的数据 ⇒ 真实数据竞争（画面缺块/花屏反而可能被"修"成偶发错误） |

### 11.12 行号基线说明（重要，影响所有引用）

- **本手册 §11.9–§11.11 的 `panvk_vX_gpu_queue.c` 行号按 `v84 布局` 给出**（`44`→`:1539`、`45`→`:1544`、`42`→`:1582`、完成 add→`:1585`、全槽等待→`:1540` 段）——因为**现场快照是 v84 的 `.so`（`61d89bc1…`）产生的**，读那份快照就该用那份布局。
- **当前树已被再次修改**：`panvk_vX_gpu_queue.c` mtime `2026-10-06 01:56:31`，与 `.bak-v84-1791221286` 相比有 **56 行**差异（在 v84/v85 之后由 Lead 继续编辑）。同一批代码在当前树里的位置已后移到：`cs_wait_slots` `:1563`、`WRAP_AFTER_WAIT(45)` `:1566`、`WRAP_BEFORE_SYNC_ADD(42)` `:1604`、`cs_move64_to(val64,1)` `:1606`、`cs_sync64_add(..., cs_defer(0, SB_ID(DEFERRED_SYNC)))` `:1607`。
- **执行顺序未变**（已逐行复核当前树）：`… → 44 → cs_wait_slots(all_mask) → 45 → POST_WAIT marks/LS copy → 42 → move val64=1 → deferred SYNC_ADD64 → ERROR_BARRIER`。⇒ §11.9 的**结论与行号无关**；只有"照行号去 grep"时需要选对布局。
- 建议：以后每份报告/手册在顶部注明**行号基线 = 哪个 `.so` sha / 哪个备份**，否则跨版本引用必然错位（本轮就是实例）。
