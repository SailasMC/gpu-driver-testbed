# 64 — 收窄后的表述：按 VT 作业采样 `syncobjs`（最小实现 + 三分判读）

日期：2026-10-06 · 只读分析（出提案，不改码）· 行号基线 = 我读到的当前树
配套：`63-p6-refuted-threshold.md`、`63b-add-not-effective.md`
**已采纳你的三条核对**：宏体无条件发射（`csf/panvk_instr.h:93-115`）✓ · kbase 走专用 CSF 事件 BO（我的 R-A 否证）✓ · `cs_now()` = `wait_mask 0` ⇒ 立即执行 ✓

---

## 0. 先把"还剩什么"钉死

在"**无 skip 分支 + `cs_now()` 立即执行 + 地址算式两侧同源 + 专用事件 BO**"全部成立之后，**剩下的可能只有三类**：

| 类 | 含义 | 现在的位置 |
|---|---|---|
| **A. 写与读不在同一份内存** | 不是算式错，而是**指针值不同**：各自子队列 ctx 的 `.syncobjs` 字段**未必都指向同一个地址** | **`panvk_vX_gpu_queue.c:2744`** `.syncobjs = panvk_priv_mem_dev_addr(queue->syncobjs)` —— **必须确认它在"按子队列建 ctx"的循环里覆盖全部三个子队列**（我两轮都标了"未逐行确认"，现在它是头号嫌疑） |
| **B. 写了但不在"宿主/消费者看到的那份"** | 专用 BO + scope/flag 交互（`PAN_KMOD_BO_FLAG_CSF_EVENT` + `MALI_CS_SYNC_SCOPE_CSG`） | 若 A 排除，转这里（把跨队列信号统一到 `SYSTEM` scope 的单变量实验） |
| **C. 指令确实执行了，但增量被后续同址写覆盖/回退** | 例如同作业内另有一条写把该元素写回旧值 | 需要 A/B 排除后，用 §2 的按作业采样看"是否曾经涨过又回落"（回落 = C 的铁证） |

⇒ **一分钟可做的第一件事（零成本）**：
```bash
grep -n -B12 -A6 "offsetof(struct panvk_cs_subqueue_context, syncobjs)" \
  /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c | head -40
# 看 :2744 那行是否在 `for (subqueue …)` 循环体内、且循环覆盖 0..PANVK_SUBQUEUE_COUNT-1
```

---

## 1. 按作业采样：最小实现

**插入点**：作业发射函数内、`target_seqno` 刚算出来处 —— `panvk_vX_gpu_queue.c:1445`
```c
   uint64_t target_seqno = subq->kbase.emitted_jobs + 1;
```
（该函数末尾在 `:1625` `subq->kbase.emitted_jobs++;`；`queue`/`subqueue`/`subq` 都在作用域内——`:1630` 的既有日志同时用了 `subqueue` 与 `emitted_jobs`。）

**新增函数**（放在既有 `kbase_log_queue_syncobjs()`（`:273-292`）之后，复用它的读法）：

```c
/* v97: 按作业采样跨队列 syncobjs。每秒采样只能告诉我们"冻结了"，
 * 不能告诉我们"从哪个作业起不再增长"；本函数补上后者。
 *
 * 纯宿主读（同一个 priv BO、同一套 invalidate + volatile 读法，与超时路径一致），
 * 不发射任何 GPU 指令 ⇒ 不可能影响卡死本身 ⇒ 可以常开。
 *
 * 输出策略：只在"相比上一个作业没有前进"时打印（外加每 1000 个作业一次心跳），
 * 因此正常运行时几乎无日志，异常时第一行就是我们要的作业号。 */
static void
kbase_log_syncobjs_per_job(struct panvk_gpu_queue *queue, uint32_t subqueue,
                           uint64_t target_seqno)
{
   static uint64_t last[PANVK_SUBQUEUE_COUNT];
   static uint64_t samples;

   struct panvk_cs_sync64 *so = panvk_priv_mem_host_addr(queue->syncobjs);

   if (!so)
      return;

   kbase_cache_invalidate_range(so, sizeof(*so) * PANVK_SUBQUEUE_COUNT);

   const bool stalled = samples && so[0].seqno == last[0];

   if (stalled || (++samples % 1000) == 0) {
      mesa_loge("kbase: syncobjs @ job %" PRIu64 " (subq %u): "
                "s0 %" PRIu64 " (d=%+" PRId64 ") s1 %" PRIu64 " s2 %" PRIu64
                "%s",
                target_seqno, subqueue, so[0].seqno,
                (int64_t)(so[0].seqno - last[0]), so[1].seqno, so[2].seqno,
                stalled ? "  <== s0 has NOT advanced since the previous job" : "");

      for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
         last[i] = so[i].seqno;
   }
}
```

**调用点**（紧跟 `:1445` 之后）：
```c
   if (PANVK_DEBUG(KBASE_DIAG) && subqueue == PANVK_SUBQUEUE_VERTEX_TILER)
      kbase_log_syncobjs_per_job(queue, subqueue, target_seqno);
```
**代价/风险**：每 VT 作业 1 次 cache invalidate（~64 B）+ 3 次读 + 极少数日志；**零 GPU 指令、零发射改动** ⇒ **不可能改变卡死行为**（这一点很关键：它不会像 F-C 那样把"观测"和"行为"混在一起）。
**两个必须写进注释的边界**：
1. 采样发生在**发射时**（GPU 侧进度是"截止到上一个作业"的值）⇒ **"一个作业看起来没涨"可能是滞后，不是丢失** ⇒ 判"停止"要求**连续两个作业不涨**（或与超时快照互证）。
2. 静态变量是进程级的；若一个进程多设备，需要按 `queue` 分桶（单设备场景可忽略，但要写在注释里）。

---

## 2. 采样到之后如何三分（你问的判读表）

记：`E` = 生产侧发射计数（§见 63b §4 的 `cell+60` 插桩，可选）、`T` = 消费侧阈值（同一插桩）、`S` = 宿主读到的 `syncobjs[j]`。

| 观测 | 含义 | 下一步靶心 |
|---|---|---|
| **S 从未涨过（从第 1 个作业起）** | 机制整体不工作（与"长期自增到 6300/62901"矛盾 ⇒ 只会出现在**换了 .so / 换了 BO** 之后） | 先查 A（ctx `.syncobjs` 是否为 0/陈旧） |
| **S 长期正常、从作业 N 起连续不涨** | **本 bug 的现场** ⇒ 取作业 N 的 ckpt/marks/insert/extract 对齐（这就是你要的"定位"） | ① 若 N 的 marks 三个都写了（`:1504/:1547` 都执行）⇒ 流跑到了 add 之后 ⇒ 转 A/B；② 若 marks 只到 pre-call ⇒ 流没跑到 add ⇒ 回到"为什么停在 CALL"（另一族） |
| **S 曾经涨到某值后又回落/变小** | **C**：有另一条写把该元素写回旧值（同址竞争的写） | grep 同元素的所有 store（`cs_store64`/`sync64_add` 之外的写） |
| **E 增长而 S 不涨** | 指令被发射了（`E` 是发射前写的，必然可观测）但**内存没变** | **A（地址/指针）优先**，其次 B（scope/BO 可见性） |
| **E 不增长而 S 不涨** | 发射点根本没被走到（例如 `flush_tiling()` 提前 return、或该 pass 无 tiler 工作） | 查 `flush_tiling()` 的 `if (!render.tiler && !inherits_render_ctx) return;`（`:4247-4248`）与该 pass 的 tiler 状态 |
| **E == T == S** | **没有丢失、也没有多算** ⇒ 消费者为何还等？⇒ 说明消费者等的**不是**这个元素（回到"消费者等的是哪个 j/哪个地址"） | 用 `T` 的高 8 位（生产者 id）核对：若 id ≠ 0，说明消费者等的是别的子队列 |
| **E == T > S** | 生产者按记账发了、消费者按记账等同一个数，但内存里没有 ⇒ **A/B** | 同上 |

> 注意：`E/T` 是 63b §4 的**可选**插桩；**若只想上一版，就先只上 §1 的按作业采样**——它已经能把"从哪个作业起停"钉出来，并区分"C. 回落"与"A/B. 从未增长"。

---

## 3. 与你现有仪器的对齐方式（一次运行就能写完判读）

1. 跑一版含 §1 的 **DIAG** 构建，复现卡死；
2. 从日志里取**第一行** `syncobjs @ job N … <== s0 has NOT advanced`（N = 停滞起点）；
3. 用超时快照里 **subqueue 0 的 job N/N±1 行**（`seqno/ls_copy/target/marks/insert/extract`）对齐：
   - `target == N` 且 `seqno == N`（完成）⇒ 该作业"完成但信号没落地" ⇒ **A/B**；
   - `insert > extract` ⇒ 该作业没被消费完 ⇒ 另一族（流内停滞）。
4. 若同时上了 `E/T`（63b §4），直接看 §2 表格的最后四行。

---

## 4. 建议顺序（把上机预算花在刀刃上）

1. **零成本 grep**（§0 的 `:2744` 是否覆盖三个子队列）——**可能直接就是根因**，且不需要上机。
2. **§1 的按作业采样**（常开、零风险、零 GPU 指令）⇒ 拿到停滞起点作业号 N。
3. 用 N 对齐超时快照 ⇒ 落进 §2 的某一行。
4. 只有走到 **A/B** 那一行时，再考虑 63b 的 `E/T` 插桩；走到 **C** 时转"同址竞争写"的 grep。

---

## 5. 未验证 / 限制（诚实清单）

1. 全部为**代码级**分析；**未做设备实验**，§1 的代码**未编译**（按纪律不落地）。
2. **§0-A 仍未确认**：`.syncobjs` 的写入点是否覆盖三个子队列 ctx——这是本轮**最该先做**的一步（一条 grep），也是"零成本可能直接命中根因"的唯一入口。
3. §1 的插入点依据是 `:1445` 的 `target_seqno` 与 `:1625` 的 `emitted_jobs++`（我读到的是**同一函数内**，`queue`/`subqueue`/`subq` 可见）——**未逐个确认该函数的签名与所有提前 return**（若某条 early-return 在 `:1445` 之前，采样会漏掉那些作业；对"定位停滞起点"影响不大，但要写进注释）。
4. §2 表格里的 `E`（发射计数）依赖 63b §4 那条"发射前的 store 可观测"的推论，**未经硬件手册复核**。
5. 静态变量多设备分桶问题（§1 边界 2）在单设备场景可忽略。
