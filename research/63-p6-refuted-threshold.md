# 63 — v96 之后：P6 被否证（含原因），写/等地址同源，靶心回到"阈值 vs 实际"

日期：2026-10-06 · 只读分析 · 行号基线 = 我本轮读到的当前树
配套：`62b-fc-patch.md`（F-C 提案）· `60b-hang-playbook.md`（手册 §11.9 H1/H2）

---

## 0. 结论先行

| 你的问题 | 结论 | 证据 |
|---|---|---|
| #1 操作数是否在 wait 前被写坏？ | **否** | 检查点用的是 **scratch 14/16**，**不碰 0..3**（见 §1）；`cs_vt_end` 无内存操作数 ⇒ 不可能踩 |
| #1 那 P6 为何被实测否证？ | **P6 是"存在覆写点"但"不在退休前"**——我 62b §1 末尾自己标了那条判据，现在由实测关闭 | v96：同步执行+等待通过+VT 完整，`syncobjs[0]` 仍不动 |
| #2 写/等是否同地址？ | **是（j=0 时同址）** | 两侧算式逐项对比见 §2；`.syncobjs` 只有一个写入点 `gpu_queue.c:2744` |
| #3 "skip an ADD" 是什么意思？ | **是"跳过地址偏移的 ADD"，不是"跳过自增"** | 紧邻的 `STATIC_ASSERT(PANVK_SUBQUEUE_VERTEX_TILER == 0)` + 消费侧的 `cs_add_imm64(addr, addr, j*16)` 对照 |
| 那还剩什么？ | **H3：阈值比实际多 1（记账），生产者完全健康** | 见 §4；新证据：`s0=6300`（大数）说明这套自增**长期是工作的**，坏的是"某一次边界" |

---

## 1. #1 详细：检查点用什么寄存器（这条同时解释了为什么 F-C 没用）

`panvk_per_arch(kbase_checkpoint)()`（`csf/panvk_vX_cmd_buffer.c:96-124`）**逐行**：

```c
   enum {
      KBASE_CKPT_ADDR_REG = 14,      /* 14..15 */
      KBASE_CKPT_VALUE_REG = 16,
   };
   ...
   cs_load64_to(b, addr,  cs_subqueue_ctx_reg(b),
                offsetof(struct panvk_cs_subqueue_context, debug.kbase_progress_addr));
   cs_move32_to(b, value, (uint32_t)PANVK_KBASE_CKPT_MAGIC | (uint32_t)ckpt);
   cs_store32(b, value, addr, PANVK_KBASE_SEQNO_CKPT_REL_OFFSET);   /* cell+56 */
   cs_flush_stores(b);
```
⇒ 用 **14/15（地址）与 16（值）**，**与操作数寄存器 0..3 完全不相交** ⇒ 我上一轮怀疑的"checkpoint 踩操作数"**不成立**；`cs_vt_end()` 只带 async-op 结构（无内存操作数）⇒ 也不踩。
⇒ **F-C 的失败因此不是操作数问题**；P6 关闭（与你的实测一致）。**注意**：这条也顺势解释了"为什么 wrapper 的完成 add 要强调寄存器 14..17 是唯一会被 FW 破坏的"——**检查点恰恰占用了 14..16**，wrapper 尾部那段的注释与检查点共用同一批寄存器，属于**已知且已被项目讨论过**的约束。

---

## 2. #2 详细：两侧地址算式逐项对比

**消费侧**（`emit_barrier_insert_waits()`，`csf/panvk_vX_cmd_buffer.c:686-710`）：
```c
   u_foreach_bit(j, deps->dst[i].wait_subqueue_mask) {          /* j = 生产者子队列 */
      cs_load64_to(b, sync_addr, cs_subqueue_ctx_reg(b),         /* 本子队列自己的 ctx */
                   offsetof(..., syncobjs));
      cs_add_imm64(b, sync_addr, sync_addr, sizeof(struct panvk_cs_sync64) * j);   /* ← 偏移 ADD */
      cs_add_imm64(b, wait_val, cs_progress_seqno_reg(b, j), cs_state->relative_sync_point);
      kbase_checkpoint(..., BARRIER_BEFORE_QUEUE_WAIT);
      panvk_instr_sync64_wait(..., MALI_CS_CONDITION_GREATER, wait_val, sync_addr);
```
**生产侧**（`flush_tiling()`，`csf/panvk_vX_cmd_draw.c:4256-4296`）：
```c
   /* We're relying on PANVK_SUBQUEUE_VERTEX_TILER being the first queue to
    * skip an ADD operation on the syncobjs pointer. */
   STATIC_ASSERT(PANVK_SUBQUEUE_VERTEX_TILER == 0);
   struct cs_index sync_addr = cs_scratch_reg64(b, 0);
   struct cs_index add_val   = cs_scratch_reg64(b, 2);
   cs_load64_to(b, sync_addr, cs_subqueue_ctx_reg(b), offsetof(..., syncobjs));   /* 无偏移 */
   cs_move64_to(b, add_val, 1);
   ...
   panvk_instr_sync64_add(..., add_val, sync_addr, cs_now());
```
⇒ **"skip an ADD" 的完整含义 = 当 j = VT = 0 时，`sizeof(sync64)*j == 0`，所以那条"地址偏移 ADD"可以省掉** —— 它是**地址算术的优化**，与"是否自增"无关 ✓（`STATIC_ASSERT` 就在这句话下面一行，是同一个优化的守护）。
⇒ **j=0 时两侧地址完全一致**（都等于 `ctx->syncobjs` 基址）⇒ **"写的地方 ≠ 等的地方"在这条链上不成立**。
⇒ 唯一的残余风险在**指针来源**：两侧都从 `cs_subqueue_ctx_reg(b)`（**各自子队列的 ctx**）读 `.syncobjs`。该字段的写入点我 grep 到**唯一一处**：`panvk_vX_gpu_queue.c:2744` `.syncobjs = panvk_priv_mem_dev_addr(queue->syncobjs)`（应是在按子队列建 ctx 的循环里，**我未逐行确认它覆盖全部三个子队列** ⇒ 请顺手核一眼）。

### 2.1 ★ 顺带发现的一条**新的**可疑点（与"宿主看到冻结"直接相关）
`queue->syncobjs` 的分配分两支（`panvk_vX_gpu_queue.c:2989-2998`）：
```c
      queue->syncobjs = (struct panvk_priv_mem){ ... };                        /* :2989 一支 */
   else
      queue->syncobjs = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);   /* :2996 另一支：rw（写回缓存池！） */
```
并在 `:2855` 初始化、`:2862` `kbase_clean_priv_mem()`。
**对比**：事件对象的同一类 syncobj 用的是 **`dev->mempools.rw_nc`**（`csf/panvk_vX_event.c:31`）。
⇒ **若本设备走的是 `mempools.rw`（缓存、写回）那一支**，则：GPU 的 **CSG 作用域**自增落在缓存里，**宿主读**（v87 采样走 `kbase_cache_invalidate_range`）与**消费者 CS 的 `sync64_wait`** 是否都能及时看到该值，就取决于该池的一致性策略 ⇒ **可以出现"宿主读到冻结、而 GPU 侧其实已自增"或反之**。
- ⚠️ 这**不能单独解释卡死**（若 GPU 侧自增成功，消费者就该被满足）；但它**能解释"宿主采样值不可信"这一半**，从而**污染 H1/H2 的判据**（我们用 s0 冻结推 H1！）。
- ⇒ **建议**：把 `syncobjs` 明确改成 `rw_nc`（与 event 一致），或在采样前做一次与消费者同级的读回方式；这属于**低风险的一致性修正**，且能让后续所有判据变干净。

---

## 3. #3 详细：还有没有"整条 add 没被发射"的路径？

- `flush_tiling()` 只有一个提前返回：`if (!cmdbuf->state.gfx.render.tiler && !inherits_render_ctx(cmdbuf)) return;`（`:4247-4248`）——此时 **`++relative_sync_point` 也不会执行**（在函数末尾 `:4300`）⇒ 生产/记账同步跳过，**不会造成 off-by-one** ✓。
- `panvk_instr_sync64_add()`（`csf/panvk_vX_cmd_buffer.c` 内定义，调用点共 6 处：`:788`（屏障）、`cmd_dispatch.c:54/74`、`cmd_draw.c:4296/4315/4817`）**没有按子队列"跳过自增"的分支**；它只是把 `add_val`/`sync_addr` 交给 `cs_sync64_add(...)` 发射 ⇒ **没有"VT 特判"**。
⇒ **"被优化掉/跳过"这条路基本可以排除**（除非 `cs_sync64_add` 自身对 `cs_now()` 有特殊处理，我未逐行核对 `cs_builder.h` 里 `cs_sync64_add` 的实现——**这是我这轮唯一没闭合的小口子**）。

---

## 4. ★ 新的头号假设 H3：阈值比实际**多 1**（生产者健康）

**为什么现在必须认真回到 H2/记账**：
1. v96 实测：VT **同步执行 + 等待通过 + 完整完成**，而 `syncobjs[0]` 不动 ⇒ "生产者这一侧的物理动作"没问题。
2. **`s0 = 6300`**：这套自增**长期是工作的**（6300 次成功），坏的只是"这一个 job 的这一次"⇒ 更像**边界/记账**问题，而不是"机制坏了"。
3. 记账的两个齿轮**不在同一个地方推进**：
   - 生产侧：`flush_tiling()` 里 `++cmdbuf->state.cs[VT].relative_sync_point`（每个 render pass 一次）；
   - 镜像侧：`flush_sync_points()`（`csf/panvk_vX_cmd_buffer.c:154-183`）在每个子队列流末尾把 `镜像(j) += rel(j)` 然后 `rel = 0`。
   ⇒ **若某次 `flush_tiling()` 发生在"rel 已被清零"之后的下一个 CB**（**挂起/恢复的 render pass**：注意 `flush_tiling` 的调用点带 `|| inherits_render_ctx(cmdbuf)`，`CmdEndRendering` 里还有 `suspended` 分支——**跨命令缓冲的 render pass 是被显式支持的**），就会出现：
   **本 CB 的消费者阈值 = 镜像 + rel(含本次信号) ，而该信号的 `++rel` 却记在另一个 CB** ⇒ **阈值比生产者的实际自增多 1** ⇒ 消费者永远等待、生产者一切正常、宿主采样冻结 ⇒ **与 v96 全部证据一致** ✓✓✓
4. **可证伪的预言**（H3 成立则必然看到）：
   `wait 阈值 == syncobjs[0] + 1`，且 **`syncobjs[0]` 的值恰好等于"上一个 CB 结束时的镜像值"**。

---

## 5. 一次运行的判别插桩（只给可观测方案）

| # | 插桩 | 位置 | 读出来判什么 | 代价 |
|---|---|---|---|---|
| **1（首选）** | **把消费者的阈值与生产者 id 写进 cell+60**：`store32((uint32_t)wait_val \| (j << 24), <cell+60>)` | `emit_barrier_insert_waits()` 里、`BARRIER_BEFORE_QUEUE_WAIT` 检查点之后（`csf/panvk_vX_cmd_buffer.c:704` 附近） | 与宿主读到的 `syncobjs[j]` 直接相减 ⇒ **H3 一眼可判（差 1 = H3；差 >1 = 生产侧真丢）** | +2~3 行发射、+2 行打印；DIAG-only；写的是空闲字 60 |
| **2** | **按 job 采样 `syncobjs[0..2]`**（把 v87 的"每秒"改成 **VT wrapper 序言每 job 一次**） | `gpu_queue.c` wrapper 序言 | 给出**最后一次自增发生在哪个 job**，与 timeout job 号对齐 | ~8 行；常开；零 GPU 开销 |
| **3** | 你的哨兵方案（add 前后写 cell+56/60） | `flush_tiling()` | ⚠️ **只能证明"发射点被走到"，不能证明 add 落地**（项目自己的注释：deferred op 之后的 store 只有在其已执行后才落地）⇒ 信息量低于 #1/#2，**不建议作为主判据** | ~4 行 |
| **4** | 采样前**换一致性读法/把 `syncobjs` 池改 `rw_nc`** | §2.1 | 让"宿主看到的冻结"变得可信 ⇒ 否则 #1/#2 的读数都要打问号 | 一行池名或一次读回方式改动 |

**若只能加一个：加 #1**（它直接给出"阈值 − 实际"这个差值，这是唯一能一刀切开"生产者丢"与"记账多算"的量）。

---

## 6. 未验证 / 限制（诚实清单）

1. 全部为**代码级**推断；**未做设备实验**。
2. **未逐行核对 `cs_sync64_add()` 在 `cs_builder.h` 里对 `cs_now()` 的处理**（§3 末尾的口子）；若它对 `cs_now()` 有特殊语义（例如把 wait_mask 0 解释成"立即"以外的含义），F-C 的结论需重估。
3. **未逐行确认 `gpu_queue.c:2744` 覆盖全部三个子队列 ctx**（若某子队列的 `.syncobjs` 指向别处 ⇒ §2 的"同址"结论只对 VT 成立；**这值得一条 grep/一眼**）。
4. §2.1 的池一致性问题：**未确认本设备走的是 `:2989` 还是 `:2996` 那一支**（决定了"宿主冻结"是否可信）；也未核对 `MALI_CS_SYNC_SCOPE_CSG` 与 `rw` 池缓存策略的相互作用。
5. H3 的"跨 CB 挂起/恢复"触发条件**未在代码里确证**（我只确认了 `inherits_render_ctx()` 与 `suspended` 分支存在、以及 `flush_sync_points()` 的两处调用点 `:352`/`:1192`）⇒ H3 仍是**假设**，但它是目前**唯一同时满足 v96 全部证据**的记账型解释。
6. 你已测的 M1 证伪我已收到（`att=2376x1080 ra={0,0..2375,1079} fb=2376x1080`）⇒ bug 3 回到 **M2（layer/td）/ M5**；`PANVK_IR_PASS_COUNT==3` 的 IR 空间假设已在 `61e` 排除，若下一轮要打 M2，建议先加"数面板 + 打 `layer_count`/`td_count`/`viewMask`"那条（61d §2-#2）。
