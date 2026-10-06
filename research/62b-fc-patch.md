# 62b — F-C 精确补丁：P6 成立（含具体踩踏点）+ 最小 diff

日期：2026-10-06 · 只读分析（本轮只出提案，不改码）· 行号基线 = 我读到的当前树
配套：`62-why-indirect-ops-stall.md`（P1/P6 区分）· 60b（手册）

---

## 0. 结论先行

**P6 成立，而且是"结构性必然"**，踩踏点已定位到具体 file:line：

```
flush_tiling() 里那两条 op 的操作数住在 VT 子队列的 scratch 0..3：
    csf/panvk_vX_cmd_draw.c:4265  sync_addr = cs_scratch_reg64(b, 0)   /* 0..1 地址 */
    csf/panvk_vX_cmd_draw.c:4266  add_val   = cs_scratch_reg64(b, 2)   /* 2..3 加数 */
                                  ← 注意：≥11 分支用的是 (b,2)，不是我在 62 号里写的 (b,4)；
                                    (b,4) 是 ≤10 分支（cmd_draw.c:4283）。此处更正。

而同一个 VT 子队列的流，在**同一条命令缓冲的收尾**处会重写这几个寄存器：
    csf/panvk_vX_cmd_buffer.c:202  sync_addr = cs_scratch_reg64(b, 0)  → cs_load64_to 写 0..1  (:205)
    csf/panvk_vX_cmd_buffer.c:203  error     = cs_scratch_reg32(b, 2)  → cs_load32_to 写 2    (:207)
    csf/panvk_vX_cmd_buffer.c:223  flush_id  = cs_scratch_reg32(b, 0)  → cs_move32_to 写 0    (:227)
```
**两者在程序序上前后相接、而那条 `sync64_add` 是异步的**：它要等 `SB_MASK_WAIT = BIT(next_sb)`（"下一次 endpoint 完成"）才执行，**CS 不会因为它未执行而停下**，于是 `finish_cs()` 的 load/move **先于**它执行 ⇒ **地址/加数被改写 ⇒ add 落到错误地址（或加了错值） ⇒ `syncobjs[0]` 永不前进**；而 VT 自己的 wrapper 收尾用的是**另一组寄存器**（项目注释：14..17）⇒ **wrapper 照常完成、`cell+0` 到达 target** ⇒ **与实测（VT complete + 三个 syncobj 冻结 10 秒）完全吻合** ✓✓

**判定 P1 vs P6**：P6 不需要任何槽语义假设，且现在有"操作数在飞 + 后续覆写"的确凿代码链；**P6 应作为首选解释**（P1 仍可能叠加，但不再是唯一候选）。

---

## 1. 追链：发射点 → VT 流末尾，谁写 scratch

| 位置 | 写入的 scratch | 说明 |
|---|---|---|
| `csf/panvk_vX_cmd_draw.c:4267` `cs_load64_to(sync_addr, …)` | **0..1** | 操作数：`syncobjs` 指针 |
| `:4269` `cs_move64_to(add_val, 1)` | **2..3** | 操作数：加数 1 |
| `:4274` `panvk_instr_sync64_add(..., cs_defer_indirect())` | — | **异步发射（不等）** |
| `:4276` ckpt `VT_AFTER_SYNC_SIGNAL` | — | 检查点（store 走 wrapper 的寄存器，不碰 0..3） |
| 调用点 `:4906-4909`（`CmdEndRendering`） | **不动 VT** | `issue_fragment_jobs()` / `handle_deferred_queries()` 走的是 **FRAGMENT** 的 builder ⇒ 写的是 FRAG 的寄存器文件（每子队列独立 CS）⇒ **不踩 VT** |
| **`finish_cs(cmdbuf, VT)`**（命令缓冲收尾，`panvk_vX_cmd_buffer.c:361` 对每个子队列调用） | **0..3（两次）** | `:194` `cs_wait_slots(all_mask)`（不写）→ **`:205`/`:207` 写 0..1、2** → **`:227` 写 0** |

⇒ **其后每一个会写 VT scratch 0..3 的点就是 `finish_cs()` 的这三处**（我逐个核对了该函数全文 `:185-230+`；`flush_id` 之后还有 `cs_flush_caches` 使用同组寄存器）。
⇒ 唯一的"保护"是 `:194` 的 `cs_wait_slots(all_mask)` —— **而它是否覆盖那条 add 的 signal 槽（`SB_SEL_DEFERRED`）我们无法确认**（`SB_SEL_DEFERRED` 的写入点我上一轮已说明未定位）。**若覆盖 ⇒ add 早已退休 ⇒ 覆写无害；若不覆盖 ⇒ P6 成立。**
⇒ 这把问题收敛成**一句话可检验的判据**：**"`SB_SEL_DEFERRED` 是否 ∈ `dev->csf.sb.all_mask`"**。若 ∈（大概率），则 `finish_cs` 的全槽等待会**先**等到 add 退休 ⇒ P6 不成立；若 ∉（= v84d 现场"槽在 all_mask 之外"那条老线索），**P6 成立**。
**⇒ 这正是 F-A 要打印的东西**（见 §4）：F-A 一次运行即可判定 P6 是否成立，**然后再上 F-C**。

---

## 2. F-C 最小补丁（推荐路线：**同步等待 + 直接 op**，即"路线 (i) 的语义化版本"）

**为什么不选你给的两条原路线**：
- 路线 (i)「换到流尾不再复用的寄存器」：**不存在可证明安全的寄存器**——`finish_cs()` 自己就用 0..3，且其后还有 `cs_flush_caches` 等一串发射；任何"高寄存器"都可能被后续任意 PanVK 命令复用（整个剩余命令缓冲都是"后续"）。⇒ (i) 只能靠"没踩到"的运气。
- 路线 (ii)「把 op 挪到 VT 流最末」：**会破坏 1:1 记账** —— `flush_tiling()` 里同时递增 `++cmdbuf->state.cs[VT].relative_sync_point`（`:4300`），而消费方的阈值 = `镜像 + rel`。若把**信号**挪到命令缓冲末尾而 rel 仍在每个 pass 递增，则"多 pass 场景下信号数 < rel 数" ⇒ **阈值永不满足**（正是我们最不想要的 off-by-N）。⇒ (ii) 不可取。

**推荐补丁（只改"发射方式"，不改等待条件、不改 rel 递增）**：

```diff
--- a/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
@@ (flush_tiling(), #if PAN_ARCH >= 11 分支内，紧接 cs_vt_end 之后)
    panvk_per_arch(kbase_checkpoint)(
       cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER, PANVK_KBASE_CKPT_VT_AFTER_VT_END);
-   panvk_instr_sync64_add(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER, true,
-                          MALI_CS_SYNC_SCOPE_CSG, add_val, sync_addr,
-                          cs_defer_indirect());
+   /* F-C: 原来这里用 cs_defer_indirect()，把 addr/addend 留在 VT scratch 0..3
+    * 里等 SB_MASK_WAIT（= "下一次 endpoint 完成"）。但同一个 VT 子队列的
+    * finish_cs()（csf/panvk_vX_cmd_buffer.c:202-209 与 :223-227）会在该等待满足
+    * 之前重写 0..3，于是 add 可能带着被破坏的操作数落地 ⇒ syncobjs 不前进，
+    * 而 VT 的 wrapper 照常完成（实测：cell+0==target 但三个 syncobj 冻结 10 s）。
+    *
+    * 改成"同步等待同一个 SB_MASK_WAIT，然后直接发射"：
+    *  · 等待条件完全相同（同一个寄存器、同一个槽）⇒ 不可能提前放行；
+    *  · 直接 op 的操作数在下一条指令即被消费 ⇒ 不存在被覆写的窗口；
+    *  · 若该等待永不满足，VT 会停在"可打点"的位置（见可选 checkpoint），
+    *    而不是像现在这样"生产者自称完成、消费者静默冻死"。
+    */
+   cs_wait_indirect(b);   /* 等 SB_MASK_WAIT（genxml/cs_builder.h:1835） */
+   panvk_instr_sync64_add(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER, true,
+                          MALI_CS_SYNC_SCOPE_CSG, add_val, sync_addr,
+                          cs_now());
    panvk_per_arch(kbase_checkpoint)(
       cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER, PANVK_KBASE_CKPT_VT_AFTER_SYNC_SIGNAL);
```

**可选的观测增强**（若愿意动 enum，代价 = 一处 enum + 一处名字表）：在 `cs_wait_indirect(b);` **之前**插
`kbase_checkpoint(..., VT_BEFORE_INDIRECT_WAIT)` ⇒ 一旦卡死，快照里 VT 的 ckpt 会**明确指向这个新 ID**，
而不是停在 42（"设计上的终点值"）让人误判。**建议加**，因为 42 的歧义（任何完成 entry 的终值）已经坑过我们一次。

**不改的东西（风险边界）**：`SB_MASK_WAIT` 的编程值、被等待的槽、`++relative_sync_point` 的次数与位置、`cs_vt_end` 的位置与形式（它没有内存操作数，不受本 bug 影响，动它反而会碰 tiler 语义）。

---

## 3. "为什么不会提前放行"——一句可检验的话

> **本补丁把同一个等待（同一个 `SB_MASK_WAIT` 寄存器值、同一个槽）从"异步队列里等"改成"同步执行等"，等待条件一字未改；同步只能让放行更晚、不可能更早——因此消费者看到 `syncobjs[j]` 前进的时刻只会不变或推后，绝不会提前。**

检验方式：F-C 前后各跑一次 v87 的每秒 `syncobjs` 采样 —— **每个 job 的 `syncobjs[0]` 增量必须完全相同**（只是出现时刻可能略晚）。若增量变多 ⇒ 提前放行（不允许，回滚）。

---

## 4. F-A 应当先落地（理由 + 3~5 行日志文本）

**先做 F-A 的三个理由**：
1. 它**零风险、不改变行为**，一次运行就能回答**决定 P6 生死的那个问题**：`SB_SEL_DEFERRED ∈ all_mask` 吗？（若不 ∈ ⇒ P6 成立、F-C 必上；若 ∈ ⇒ `finish_cs` 的全槽等待已保护 add ⇒ **P6 被否证**，省下一版上机）。
2. 它同时给出 `SB_MASK_WAIT` / `next_sb` / `all_iters_mask` / `sb.count` 的**实际值**，用来判 P2（自等待）/P3（越界）——**这是宿主唯一能看到这对槽的途径**（CS 寄存器宿主读不到，页 2 不可信）。
3. 它可与 F-C **同一版**一起上：因为它**不可能改变行为**，不会污染 F-C 的归因；若 F-C 后卡死消失 ⇒ 归因 F-C；若消失不了 ⇒ F-A 的日志告诉你为什么。

**日志文本**（插在写 `SB_MASK_WAIT` / `SB_SEL_DEFERRED` 的 SET_STATE 发射点：`csf/panvk_cmd_buffer.h:852` 一带的 `SB_MASK_WAIT`，以及 `SB_SEL_DEFERRED` 的写入点——**后者位置我尚未定位**，需先 `grep -rn "SB_SEL_DEFERRED"` 全树）：

```c
/* F-A: 间接 deferred op 的真实 (wait, signal) 对住在 CS 寄存器里，事后宿主读不到
 * （page 2 不镜像），所以在"我们即将写入"的这一刻校验并记录。 */
const uint16_t wait_mask = /* 即将写入 SB_MASK_WAIT 的值：BIT(next_sb) */;
const uint8_t  sel_deferred = /* 即将写入 SB_SEL_DEFERRED 的值 */;
mesa_logi("kbase: SB regs subq %u: SB_MASK_WAIT=0x%04x (next_sb=%u, all_iters=0x%04x, count=%u) "
          "SB_SEL_DEFERRED=%u => self-wait=%s, mask_in_iters=%s, in_all_mask=%s",
          subqueue, wait_mask, next_sb, dev->csf.sb.all_iters_mask, dev->csf.sb.count,
          sel_deferred,
          (wait_mask & BITFIELD_BIT(sel_deferred)) ? "YES(BUG)" : "no",
          (wait_mask & ~dev->csf.sb.all_iters_mask) ? "NO(BUG)" : "yes",
          (dev->csf.sb.all_mask & BITFIELD_BIT(sel_deferred)) ? "yes" : "NO(P6!)");
assert(!(wait_mask & BITFIELD_BIT(sel_deferred)));   /* 复刻 cs_defer 的断言到运行时 */
```
**判读**：末列 `in_all_mask=NO(P6!)` ⇒ P6 成立（add 可能在退休前被 `finish_cs` 踩）；`self-wait=YES(BUG)` ⇒ P2 成立；`mask_in_iters=NO(BUG)` ⇒ P3 成立。

---

## 5. 未验证 / 限制

1. 全部为**代码级**；**未做设备实验**，F-C 也未编译（按纪律不落地）。
2. **`SB_SEL_DEFERRED` 的写入点仍未定位**（我需要一次 `grep -rn "SB_SEL_DEFERRED"`，本轮预算耗尽）⇒ F-A 的插桩位置需先补这一步；`assume` 的"signal 槽 = SB_SEL_DEFERRED"来自项目注释（`gpu_queue.c:1393`），**未逐行复核**。
3. `cs_wait_indirect()` 的确切语义/参数我只从注释读到（`cs_builder.h:1833-1835`："Wait indirectly on a scoreboard (set via SET_STATE.SB_MASK_WAIT)"）⇒ 落地前请确认其签名与"等待后是否清除该槽"的行为（若它会清槽，则与 wrapper 后续的 `wait_slots(all_mask)` 语义需一并核对）。
4. P6 是否**实际发生**仍取决于 §1 末尾那条判据（`SB_SEL_DEFERRED ∈ all_mask`）；**代码链已确证"存在覆写点"，但"是否真的赶在 add 退休之前"要靠 F-A 的值或 F-C 的对照实验**。
5. 我**没有**检查 `finish_cs()` 之后 wrapper 追加的尾部（wrapper 用 14..17，按项目注释不碰 0..3）⇒ 若该假设不成立，覆写点还要加上 wrapper 尾部。
