# 63b — 为什么那个 +1 不生效：已排除项、唯一未闭合的口子、以及不依赖 syncobjs 一致性的判别法

日期：2026-10-06 · 只读分析 · 配套：`63-p6-refuted-threshold.md`
**本轮的边界**：你问的三条里，**两条我能给出确定性结论（都否证）**，**第三条（add 是否真被执行到）我未能闭合**——原因与确切命令见 §2，我拒绝猜。

---

## 1. 你假设 #2（操作数在 wait 前被写坏）——**完全否证，不必上机**

| 发射物 | 用到的 scratch | 证据 |
|---|---|---|
| `kbase_checkpoint()` | **14/15（地址）、16（值）** | `csf/panvk_vX_cmd_buffer.c:96-124`：`KBASE_CKPT_ADDR_REG = 14`、`KBASE_CKPT_VALUE_REG = 16`，`STATIC_ASSERT(...+2 <= CS_REG_SCRATCH_COUNT)` 等逐行可查 |
| `cs_vt_end(b, async)` | **一个都不用** | `genxml/cs_builder.h:2395-2398`：`cs_vt_end()` 就是 `cs_heap_operation(b, MALI_CS_HEAP_OPERATION_VERTEX_TILER_COMPLETED, async)` —— **堆操作，无内存操作数**，不可能踩 0..3 |
| `cs_wait_indirect(b)` | 不写（只读 `SB_MASK_WAIT`） | `cs_builder.h:1833-1835` |

⇒ 操作数寄存器 **0..3 在 `cs_load64_to(sync_addr)`/`cs_move64_to(add_val,1)` 之后一路干净** ⇒ **"把 load/move 挪到 wait 之后"这个最小修法不会带来任何变化，建议不要为它花一版上机预算**（这是本轮能替你省下的一次实验）。
⇒ 顺带：检查点占 14..16 这一点，**正是 wrapper 尾部注释强调"寄存器 14..17 是唯一会被 FW 破坏的"的原因**——两者共用同一批寄存器，属已知约束。

---

## 2. 你假设 #1（这条 add 根本没被执行到）——**我没能闭合，给你确切的下一步**

**已确认**：
- `panvk_instr_sync64_add` 在 `src/panfrost/` 全树里**只有 6 个调用点、没有任何定义**（`grep -rn "instr_sync64_add" src/panfrost/` ⇒ 只命中 `csf/panvk_vX_cmd_draw.c`（`:4296`、`:4315`、`:4817`）、`csf/panvk_vX_cmd_buffer.c:788`、`csf/panvk_vX_cmd_dispatch.c:54/74`）。
- `grep -rn "define panvk_instr_\|PANVK_INSTR_DEFINE" src/panfrost/` ⇒ **零命中**。
- `panvk_per_arch(panvk_instr_end_sync64_add)` **存在**（`csf/panvk_vX_instr.c:244` 附近的 `PANVK_INSTR_WORK_TYPE_SYNC64_ADD` 分派里），但那是**收尾**钩子，不是发射主体。
⇒ **发射主体要么在 panfrost 之外的公共运行时、要么是生成了 token-paste 的宏（我的字面 grep 抓不到）、要么在 build 目录的生成头里。** 因此 **"它会不会不 emit""第三个参数 `true` 是什么""有没有 skip 分支"我无法回答**——这三条只有拿到定义体才能定。

**请跑这三条（都只读，几秒）**，把输出给我，我下一轮直接定案：
```bash
M=/root/zenithblue/work/mesa
grep -rn "panvk_instr_sync64_add" $M/src/vulkan/ 2>/dev/null | grep -v "\.bak" | head
grep -rn "instr_sync64_add\|PANVK_INSTR" $M/src/panfrost/vulkan/csf/*.h 2>/dev/null | head -30
grep -rn "instr_sync64_add" $M/build/ 2>/dev/null | head -20      # 生成头/宏展开
```
（`$M/build/android-v4/` 是你们的构建目录；宏若由 genxml/脚本生成，一定在这里留痕。）

---

## 3. 当"发射路径处处正确"时，剩下最合理的一条：**写到了消费者读不到的副本**

两条独立证据指向"**`syncobjs` 的一致性问题**"，而不是"指令没执行"：
1. **`s0 = 6300`（大数）** ⇒ 这套自增**长期工作**；坏的只是"最后一次"。
2. `queue->syncobjs` 的分配**分两支**（`panvk_vX_gpu_queue.c:2989` / `:2996`），其中 `:2996` 用的是 **`dev->mempools.rw`（写回缓存池）**，而**事件对象同类 syncobj 用的是 `rw_nc`**（`csf/panvk_vX_event.c:31`）；`:2862` 还专门做了 `kbase_clean_priv_mem()`。
⇒ 若走 `:2996` 那一支：**GPU 的 `MALI_CS_SYNC_SCOPE_CSG` 自增**落在缓存层，**宿主采样**（v87 走 `kbase_cache_invalidate_range`）与**消费者 CS 的 `sync64_wait`** 是否看到同一份值，取决于该池策略 ⇒ **可以出现"宿主看到冻结、而 GPU 侧已自增"**。
- 这**单独不能解释卡死**（GPU 侧若已自增，消费者就该被满足），**但它直接摧毁 H1/H3 的判据**——**我们全部结论都建立在"宿主读到的 s0 冻结"之上**。
⇒ **建议把它当成"先修观测、再修逻辑"的第一步**（低风险：与 event 一致地改 `rw_nc`，或让采样用与消费者同级的读法）。

---

## 4. 一次运行的判别插桩（绕开 syncobjs 不确定性；只给可观测方案）

**核心思路**：把**生产侧"我发了多少次"**与**消费侧"我在等什么"**都写进**已知可读的 seqno cell**（marks/ckpt/seqno 都从这里读得很稳），**不要依赖 `syncobjs` 的可读性**。

| # | 写什么 | 写哪 | 为什么可行 |
|---|---|---|---|
| **1** | **生产侧发射计数**：在 `flush_tiling()` 里、**add 发射之前**，把"本 CB 已发射的 VT 信号数"（或 `rel(VT)` 的当前值）写进 **`cell+60`（VT 的 cell，32 位，DIAG 门控）** | `flush_tiling()`，`cs_load64_to`/`cs_move64_to` 之后、`cs_wait_indirect` 之前 | **发射前**的 store 是可观测的（"之后不可观测"这条约束只针对跨过异步 op 的 store）。⇒ 一旦卡死，我们能确认**add 确实被发射过 N 次** |
| **2** | **消费侧阈值 + 生产者 id**：`store32((uint32_t)wait_val \| (j<<24), cell+60)`（消费者自己的 cell） | `emit_barrier_insert_waits()`，`BARRIER_BEFORE_QUEUE_WAIT` 之后（`csf/panvk_vX_cmd_buffer.c:704`） | 给出消费者在等的**具体数值**（现在完全缺失） |
| **3** | **宿主把 #1 与 #2 对减** | 采样改动（一条日志） | ① `#1 ≥ #2` 且 `syncobjs` 低 ⇒ **写没到消费者读的那份内存**（§3 的副本/一致性问题）⇒ 靶心转 `syncobjs` 的池与 scope；② `#1 == #2 == syncobjs` ⇒ **add 根本没执行** ⇒ 回到 §2 的宏定义体（`true`/skip 分支） |

**关于你的哨兵方案（`cell+56`/`cell+60`）**：
- **`cell+56` 不能用**——那是**检查点字**（`PANVK_KBASE_CKPT_MAGIC | id`），写它会污染 `kbase_log_subqueue_state()` 的判读（我们整本手册都建立在这个字上）。
- **`cell+60` 可用**（§2 表里唯一的空闲字；`kbase_seqno_stride() = 64`）。
- 但"**add 之后**再写哨兵"在卡死时**不可观测**（项目自己的论证：LS 按序退休 ⇒ 排在异步 op 后的 store 只有在其已执行后才落地）⇒ **哨兵只能证明"发射点被走到"**，不能证明落地。⇒ 所以**优先级：#1+#2 的"发射计数 vs 阈值" > 单点哨兵**。

---

## 5. 最小修复候选（按风险）

| 候选 | 内容 | 风险 | 说明 |
|---|---|---|---|
| **R-A（先做，0 风险）** | 修**观测**：`syncobjs` 改 `rw_nc`（与 `panvk_vX_event.c:31` 一致），或采样走与消费者同级读法 | **0**（不改 GPU 侧语义） | 让 H1/H3 的判据变可信；否则后续所有结论都有系统性风险 |
| **R-B（低-中）** | 加 §4 的 #1+#2 两个 store（DIAG） | 低 | 一次运行即可把"没发射 / 没落地 / 记账多 1"三者分开 |
| **R-C（视 §2 结论）** | 若宏定义体里有"条件 skip emit"分支 ⇒ 修那个条件 | 视情况 | **必须先拿到定义体**（§2 的三条命令） |
| **R-D（中）** | 若 §4 显示"写没到消费者那份内存" ⇒ 统一 `scope` 与池：把跨子队列信号明确放到 SYSTEM scope（与 wrapper 完成 add 的 `MALI_CS_SYNC_SCOPE_SYSTEM` 一致，`gpu_queue.c:1607`） | 中 | scope 语义改动必须单变量上机；注意消费者 `sync64_wait` 的 scope 要同步核对 |

---

## 6. 未验证 / 限制（诚实清单）

1. **`panvk_instr_sync64_add` 的定义体我未找到**（§2）⇒ 你问的"#1 是否真被执行"**我没有答案**，只有查找命令。这是本轮最主要的口子。
2. `queue->syncobjs` 走 `:2989` 还是 `:2996` 分支**未确认**（决定 §3 是否成立）⇒ 需要一条 grep 或一次日志。
3. `MALI_CS_SYNC_SCOPE_CSG` 与消费者 `sync64_wait` 的 scope 是否严格匹配（跨子队列可见性）**未核对**（`sync64_wait` 的 scope 参数在 `emit_barrier_insert_waits` 调用里是 `false`——**那个 `false` 是"是否 async"还是"scope"我未确证**）。
4. §4 的 #1 依赖"发射前的 store 可观测"这一判断；我依据的是项目对"异步 op 之后的 store 不可观测"的论证的反面，**未在硬件手册层面复核**。
5. 全部为代码级分析；本轮**未做设备实验**。
