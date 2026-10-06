# 62 — 卡死最后一环：那两条 `cs_defer_indirect()` op 为什么从不执行

日期：2026-10-06 · 只读分析 · 行号基线 = 我本轮读到的当前树（`vulkan/csf/panvk_vX_cmd_draw.c` 已被改过 ⇒ 以函数名定位）
配套：`60b-hang-playbook.md`（§11.9 H1 判据）· `61e-ir-audit.md`（TILER_OOM 出局）
**任务状态**：共享板上**没有 task-7**（task-1..4 全部 completed，无 pending）⇒ 我无法 claim；本报告即交付物，请 Lead 决定是否补建任务。

---

## 0. 实测约束（本轮全部推理必须同时满足）

```
等待期间每秒采样：s0=6104  s1=12119  s2=6285      ← 三个跨队列 syncobj 10 秒完全冻结
subqueue 0(VT): seqno 125 == target == jobs == 125, CMDBUF_DONE SET, insert == extract
subqueue 1/2 : ckpt 27 (BARRIER_BEFORE_QUEUE_WAIT)，等 syncobjs[j]
tiler_oom.counter = 0；pitch/布局/覆盖已全部排除
```
⇒ 目标：解释 **"VT 的 ring entry 完整跑完（含 wrapper 完成 add 落地）" 与 "`syncobjs[0]` 永不前进" 如何同时成立**。

---

## 1. 问题 1：槽由谁在何时写（时间线，逐行核实）

| 时刻 | 动作 | 证据 |
|---|---|---|
| 迭代边界（每次 `RUN_IDVS` 之后） | `cs_iter_sb_update_start()` 通过 **`cs_next_sb_entry(b, next_sb, MALI_CS_SCOREBOARD_TYPE_ENDPOINT, …_FORMAT_INDEX)`** 向硬件**申请/回读**下一个 endpoint 槽号，并把回读值放进 `scratch_regs[1]`（`sb_mask = cs_extract32(b, scratch_regs, 1)`） | `csf/panvk_cmd_buffer.h:816-837` |
| 同一迭代结束 | `cs_iter_sb_update_end()` 写**两个**寄存器：<br>① `cs_move32_to(sb_mask, 0); cs_bit_set32(sb_mask, sb_mask, next_sb);`<br>② `cs_set_state(b, **SB_MASK_WAIT**, sb_mask)` ⇒ **`SB_MASK_WAIT = BIT(next_sb)`（单槽！）**<br>③ `cs_move32_to(sb_mask, all_iters_mask); cs_bit_clear32(sb_mask, sb_mask, next_sb);`<br>④ `cs_set_state(b, **SB_MASK_STREAM**, sb_mask)`（注释原文："Prevent direct re-use of the current SB to avoid conflict between wait(current),signal(next) (can't wait on an SB we signal)"） | **`csf/panvk_cmd_buffer.h:839-861`（逐行读过）** |
| 发射 deferred op 时 | `cs_defer_indirect()` 返回 `{wait_mask=0xff, signal_slot=0xff, indirect=true}` ⇒ **操作数只是占位**；运行时的 wait/signal 槽**由上面的寄存器决定** | `genxml/cs_builder.h:807-815` |
| `flush_tiling()`（VT 流尾） | 发 `cs_vt_end(cs_defer_indirect())` + `panvk_instr_sync64_add(..., cs_defer_indirect())`（≥11 分支，**只有这一条 add**） | `csf/panvk_vX_cmd_draw.c:4271/4274`（v84 布局；当前树已后移） |

**⇒ 在 `flush_tiling()` 执行的那一刻**：
- `SB_MASK_WAIT` 应当 = **`BIT(next_sb)`**，其中 `next_sb` 是**硬件在最近一次 `cs_next_sb_entry` 里承诺"下一个 endpoint 完成时我会 signal 的那一个槽"**；
- 两条 indirect op 因此**等的就是"下一次 endpoint（tiler）完成"**，而 **signal 槽来自 `SB_SEL_DEFERRED`**（写入点：见 §1.1 的行号漂移说明）。

### 1.1 两处**行号已漂移**（我复核时发现的，请勿按旧行号找）
- 你给的 `csf/panvk_cmd_buffer.h:750-767` 在我读到的树里**是 conditional-rendering 宏**；`cs_iter_sb_update_end()` 实际在 **`:839-861`**（`cs_iter_sb_update_start()` 在 `:816-837`）。
- 你给的 `panvk_vX_cmd_draw.c:4638/4661` 在我读到的树里**是 `run_fragment` 的 layer 循环**（`cs_while(remaining_layers)`），**不是** `SB_SEL_DEFERRED` 的 SET_STATE。⇒ **`SB_SEL_DEFERRED` 的写入点我本轮没能定位**（唯一确认的 SET_STATE 相关点是 `SB_SEL_ENDPOINT`：`genxml/cs_builder.h:2214`，以及 `SB_MASK_WAIT`：`csf/panvk_cmd_buffer.h:849` 附近）。**这是本轮唯一未闭环的一环**，建议下一轮先 `grep -rn "SB_SEL_DEFERRED"` 全树定位。

### 1.2 ★ 结构性缺口（本项目自己的注释承认了，但没有代码保护）
`cs_defer()`（`genxml/cs_builder.h:783-795`）有断言：
```c
   /* The scoreboard slot to signal is incremented before the wait operation,
    * waiting on it would cause an infinite wait. */
   assert(!(wait_mask & BITFIELD_BIT(signal_slot)));
```
⇒ **"等自己 signal 的槽 = 无限等待"这个致命组合，只对直接路径有断言保护**；**间接路径（`cs_defer_indirect`，占位 0xff）在编译期和发射期都看不到真实 (wait, signal) 对** ⇒ **运行时的自等待/无效槽无人拦截**。这正是 v68 注释（`gpu_queue.c:1368-1406`）警告的同一类问题，而**至今没有代码级护栏**。

---

## 2. 问题 2：能让它变成"永不 signal 的槽"的全部路径

| # | 路径 | file:line | 触发条件 | 与实测是否吻合 |
|---|---|---|---|---|
| **P1** | **等一个"未来才会 signal"的槽**：`SB_MASK_WAIT = BIT(next_sb)` 是**硬件承诺的下一个 endpoint 槽**。若该 endpoint 工作在本 job 内**已经全部完成**（tiler 迭代都退休了），则"下一个"属于**未来的 job**；本 job 的 op 要等到**下一个 job** 才有信号 | `csf/panvk_cmd_buffer.h:839-861` + `cs_next_sb_entry` 语义 | `flush_tiling()` 位于 VT 流尾 ⇒ 其后的 endpoint 工作可能为 0 | ✅ **高度吻合**：syncobjs 冻结、而 VT 的 wrapper（等 `all_mask` 的**已发**槽）照样能过 ⇒ 两者同时成立 |
| **P2** | **自等待**：运行时 `SB_MASK_WAIT` 的位 ∩ `SB_SEL_DEFERRED` 的值 ≠ ∅（等价于直接路径被 assert 掉的那个组合） | `cs_builder.h:783-795`（断言在那里，但这路径绕过它） | `SB_SEL_DEFERRED` 恰好等于 `next_sb`（或 `SB_MASK_STREAM` 的清理没生效） | ⚠️ 可能；**需要 P1 的同类证据**，且注意 `SB_MASK_STREAM` 的注释说明作者**已经意识到**这个危险并试图用"排除 next_sb"来避免 |
| **P3** | **槽号越界/不属于本 CSG 的 iter 区**：`next_sb` 落在 `all_iters_mask` 之外（或 endpoint 池槽号超出 `sb.count`），于是**没有任何一个子队列会 signal 它** | `all_iters_mask` 定义见 `panvk_vX_device.c:535-547`；`SB_MASK_WAIT` 写入 `csf/panvk_cmd_buffer.h:852` | endpoint 池与 iter 池的映射在 v12 上不同（`PANVK_SB_ITER_START=3`/`ITER_COUNT=5`） | ⚠️ 无法从现有日志判定（**槽号宿主不可读**）⇒ 需 §4-#2 的发射期日志 |
| **P4** | **漏了一次 `update_end`**：某条路径在发射 indirect op 前**没有**走 `cs_iter_sb_update_end()`，于是寄存器还停在上一次（或 wrapper 头部）的值 ⇒ 等一个陈旧/已消费的槽 | 所有 `cs_iter_sb_update()` 使用点（`csf/panvk_cmd_buffer.h:863-864`、`:950-951` 两个架构分支的宏） | 迭代循环退出路径（如 `layer_count<=1` 分支、提前 return、异常路径）漏包 | ⚠️ 与 v68 注释"在 ring entry 头部重建默认值会给第一个 deferred op 伪造操作数"**同族**（那次是伪造；这次是陈旧） |
| **P5** | **迭代数为 0 / 该 job 没有任何 endpoint 工作** ⇒ 从来没有人 signal 过任何 iter 槽 | `flush_tiling()` 的调用条件（`cmd_draw.c`，v84 布局 `:4906` 附近唯一调用点） | 无 tiler 工作的 CMDBUF（compute-only 或空 pass） | ❌ 与本现场不符（本 job 明显有 tiler 工作：VT 有 `RUN_IDVS`、FRAG 有 `RUN_FRAGMENT`） |
| **P6** | **op 已退休但写错目标**（不是"没执行"）：in-flight 期间操作数寄存器被后续流复用/覆盖 ⇒ `sync64_add` 写到垃圾地址 ⇒ **syncobjs 不前进，而所有槽都正常退休** | `flush_tiling()` 的 `sync_addr = cs_scratch_reg64(b, 0)`/`add_val = cs_scratch_reg64(b, 4)`（≥11 分支）；**项目自己为完成 add 记录过同一危险**（`gpu_queue.c:1572-1580`：寄存器 14..17 是"唯一会被 FW 破坏的"） | `flush_tiling()` 之后**流里还有指令**并复用了 scratch 0..5 | ✅ **同样与全部实测吻合**（VT 完成 + syncobjs 冻结），且**不要求任何槽语义异常** |

**P1 与 P6 是仅有的两条能同时解释"VT 完整 + syncobjs 冻结"的机制**；P2/P3/P4 需要额外假设。**P6 的额外优势**：它是**纯操作数保护问题**，不需要动槽分配（改动面小得多）。

---

## 3. 问题 3：最小修复候选（按风险排序）

> ⚠️ **先更正我自己上一轮的 F2**：**照 ≤10 分支改是行不通的**——`cs_match_iter_sb` 的定义被 `#if PAN_ARCH == 10` 包住（`csf/panvk_cmd_buffer.h` 中该宏前一行即 `#if PAN_ARCH == 10`）⇒ **v12 上根本没有这个宏**，搬过去连编译都过不了。≤10 的范式（`cmd_draw.c:4280-4299`，逐行）是：
> ```c
> cs_load_to(b, cs_scratch_reg_tuple(b, 0, 3), cs_subqueue_ctx_reg(b), BITFIELD_MASK(3),
>            offsetof(struct panvk_cs_subqueue_context, syncobjs));
> cs_move64_to(b, add_val, 1);
> cs_match_iter_sb(b, x, iter_sb, cmp_scratch) {                 /* #if PAN_ARCH == 10 专用 */
>    cs_vt_end(b, cs_defer(SB_WAIT_ITER(x), SB_ID(DEFERRED_SYNC)));
>    panvk_instr_sync64_add(..., cs_defer(SB_WAIT_ITER(x), SB_ID(DEFERRED_SYNC)));
> }
> ```
> 它之所以**不会退化成提前放行**：等待掩码是**编译期常量 `BIT(ITER_START+x)`**（`x` 是匹配到的迭代号），**signal 槽永远是 1**（≠ 等待槽 ⇒ 满足 `cs_defer` 的断言、无自等待），且 op 只在**该迭代的 endpoint 完成槽退休后**才写 `syncobjs[0] += 1` ⇒ 消费者的阈值只可能在生产者信号真正落地后被满足。**但它在 ≥11 上不可用**（宏被 arch 门控；且 ≥11 的槽是硬件 `cs_next_sb_entry` 动态分配的"移动靶"，这正是上游改用间接寄存器的原因）。⇒ **F2 作废，改用 F-1/F-3。**

| 候选 | 内容 | 风险 | 为什么不会提前放行 |
|---|---|---|---|
| **F-A（零风险，先做）** | **给间接路径补护栏**：在写 `SB_MASK_WAIT`/`SB_SEL_DEFERRED` 的 SET_STATE **发射点**加 `assert`/`mesa_loge`：① 掩码 ⊆ `all_iters_mask`；② signal 槽 ∉ 掩码（复刻 `cs_defer` 的断言到运行时可见的那一对）；③ signal 槽 < `sb.count` | **0**（纯断言/日志；发射期值宿主已知） | 不改语义 |
| **F-B（针对 P1，中）** | 让两条 op 的等待**不指向"下一个"槽**：在 `flush_tiling()` 里于发射前**显式再取一次**当前迭代槽并 `cs_set_state(SB_MASK_WAIT, BIT(cur))`（而不是沿用 `update_end` 写的 `next_sb`）；或**在流尾之后不再需要 endpoint 工作时**改用 `SB_MASK_STREAM` 已排除 next_sb 的那套 | 中（动槽语义，必须单变量上机） | `cur` 是"本迭代的完成槽"，其退休即"本迭代 tiler 工作完成"——正是语义上必须满足的条件；不放宽任何等待 |
| **F-C（针对 P6，低-中，**我推荐优先试**）** | **保护 in-flight 操作数**：把 `flush_tiling()` 的这两条 indirect op 的地址/加数放进**流尾不再被复用的**寄存器，或**把这两条 op 挪到 VT 流的最后**（其后不再发射任何会写 scratch 的指令）；可加一条 DEBUG 断言"发射后本流不再有 scratch 写入" | **低-中**（不改槽；只改寄存器分配/发射位置） | 不动等待条件，纯粹让 op 写在它该写的地方 ⇒ 不可能提前放行 |
| **F-D（针对 P2/P3）** | 若 F-A 的日志显示运行时 (`SB_MASK_WAIT` ∩ `SB_SEL_DEFERRED`) ≠ ∅ ⇒ 在 `update_end` 里**额外清掉** signal 槽（已有 `SB_MASK_STREAM` 的做法，扩到 `SB_MASK_WAIT`/`SB_SEL_DEFERRED` 组合） | 中 | 只是消除自等待（自等待本来就永远不该放行） |

---

## 4. 问题 4：一次运行就能判别的插桩（**只给可观测方案**）

约束确认：项目自己的注释（`gpu_queue.c:1572-1587`）已证明"deferred op 之后再插检查点**不可观测**（LS 按序退休 ⇒ store 只有在 op 已执行后才落地，hang 时那个字仍是 42）"⇒ **一切"在 op 之后打点"的方案都无效**。可用的是下面三类：

| # | 插桩 | 观测什么 | 代价/风险 |
|---|---|---|---|
| **1** | **按 job 采样 `syncobjs[0..2]`**（把 v87 的"每秒"改成**每个 VT 作业的 wrapper 序言里读一次**，宿主侧读已映射 BO；日志按 job 号打印） | **P6 vs P1 的分水岭**：若某个 job 之后 `syncobjs` **完全不动**，就锁定"从该 job 起越界/停滞"；并给出**停滞发生的精确 job 号**（可与该 job 的 ckpt/marks 对齐） | ~8 行；**常开**、零 GPU 开销 |
| **2** | **发射期日志**（宿主侧，不碰 GPU）：在写 `SB_MASK_WAIT`/`SB_SEL_DEFERRED` 的 SET_STATE 处打印**我们即将写入的值** + `next_sb` + `all_iters_mask` + `sb.count` | **P2/P3 的直接判据**：能否看到 mask ⊆ all_iters_mask、signal ∉ mask、signal < count。**这是唯一能看到那对槽的办法**（CS 寄存器宿主读不到；页 2 不可信） | ~6 行；DIAG-only；**0 风险** |
| **3** | **消费者阈值**（`cell+60`：低 24 位 = `wait_val` 低 24 位，bit24..25 = 生产者 id，插在 ckpt 27 之后） | 与 #1 对账：H2 已排除，但**仍能量出"差多少"** ⇒ 判断是"生产者少发了一次"还是"消费者多算了一次" | ~3 行；DIAG-only |

**若只能加一个：加 #1**（零成本、常开，直接给出"从哪个 job 起 syncobjs 停滞"）。**#2 是唯一能判定 P2/P3 的手段**，建议与 #1 同时上（都是 DIAG-only/常开，互不干扰）。

---

## 5. 未验证 / 限制（诚实清单）

1. 全部为**代码级**推断；**未做设备实验**。
2. **`SB_SEL_DEFERRED` 的写入点未定位**（你给的行号在当前树里是 `run_fragment` 的 layer 循环）⇒ §1 时间线缺最后一环；下一轮先 `grep -rn "SB_SEL_DEFERRED"` 补上。
3. `cs_next_sb_entry()` 的**硬件语义**（"下一个 endpoint 完成会 signal 哪个槽"、槽是否会轮转/回卷）我**未从 genxml/驱动注释确证**——P1 的成立依赖这条语义，请视为**假设**。
4. **`flush_tiling()` 之后是否还有 VT 流指令**（决定 P6 是否可能）我只知唯一调用点在 `cmd_draw.c` 的 render-pass 收尾处（v84 布局 `:4906` 附近），**未逐个核对其后是否有 scratch 写入**⇒ P6 仍是"高度可疑但未确证"。
5. P4/P5 的"漏 update_end / 迭代为 0"两条**未逐个核对所有 `cs_iter_sb_update()` 使用点**（涉及两个架构分支的宏与异常路径）。
6. **任务板无 task-7**（task-1..4 均 completed，无 pending）⇒ 我无法 claim；本轮交付记为 62 号报告，请 Lead 决定补建任务或直接采纳。
