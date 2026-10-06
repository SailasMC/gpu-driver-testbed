# 82 — E-α 为何静默（先分清"没执行"vs"走了另一支"）+ 四点证据包 + E-β/E-γ

日期：2026-10-06 · 只读分析（出提案）· 配套：`81-alias-region-1mib.md`（定量命中，你已认）
**已收到**：v124 两次（`PANVK_DEBUG=1,kbase_diag` 与 `PANVK_DEBUG=0`）**E-α 都无输出**，故障仍是 `0x5fffe1e000` ✗；heap 日志能出 ⇒ `HAVE_PAN_KMOD_KBASE` 是定义的 ✓

---

## 0. 先解释"静默"：三种可能，**其中一种必须先排除**

你插的是 `else` 分支（alias）。它静默**恰好也符合**下面两种"根本没进那个 else"的情况：

| # | 可能 | 判据（本次补丁就能分辨） |
|---|---|---|
| **A** | **`init_render_desc_ringbuf()` 根本没被调用**（probe 的队列创建走了别的路径） | **P2**（函数入口打印）—— 若 P2 不出 ⇒ **这条成立**，81 号的"别名边界"解释**在 tri 上不适用**（要重新找那个 1 MiB 区域的来源） |
| **B** | 进了函数，但 `tracing_enabled == true` ⇒ 走 `if` 而不是 `else` | **P3**（在 `tracing_enabled` 判定之前打印它的值）—— 若 P3 出且为 1 ⇒ 这条成立（**注意**：那时 `ringbuf->size = PANVK_DESC_TRACEBUF_SIZE`（默认 2 MiB）⇒ 别名区会是 `2×2 MiB=4 MiB`，与"1 MiB 间距"不符 ⇒ 与你的观测矛盾，所以 B 概率较低） |
| **C** | 进了函数也进了 kbase 块，但**插的位置不在实际编译路径**上（例如插到了 `#else`、`.bak` 副本、或 `tracing_enabled` 之外的另一层） | **P2+P3+P4** 全出而 P1 不出 ⇒ 只可能是"alias_create 没被调用" ⇒ 回到 A/B 的分辨 |

**⇒ 所以本次不要只打 alias，而是打"四个点"，让证据链自证**（这就是你要的"必然执行"的观测点：**P1 在 `kbase_kmod_alias_create` 内部**，只要它被调用就一定出 ✓）。

---

## 1. 四点证据包（P1–P4，全部一行，零行为改变）

### P1（★你点名要的落点）：`lib/kmod/kbase_kmod.c:1098` 的 `kbase_kmod_alias_create()` **函数体内**
**插入点**（函数体最前面，`const uint64_t page_size = 4096;` 之前）：
```c
uint64_t
kbase_kmod_alias_create(struct pan_kmod_dev *dev, uint64_t bo_va,
                        uint64_t size, uint32_t nents)
{
   mesa_logi("kbase: ALIAS enter: bo_va 0x%" PRIx64 " size 0x%" PRIx64
             " nents %u => total 0x%" PRIx64,
             bo_va, size, nents, size * nents);          /* ← P1a: 一旦被调用必出 */
   const uint64_t page_size = 4096;
   …
   for (unsigned attempt = 0; attempt < 16; attempt++) {
      …
      void *ptr = mmap(NULL, total, PROT_READ, MAP_SHARED, dev->fd, req.out.gpu_va);
      if (ptr == MAP_FAILED) { … }
      uint64_t va = (uintptr_t)ptr;
+     mesa_logi("kbase: ALIAS attempt %u: cookie 0x%" PRIx64 " mmap_va 0x%" PRIx64
+               " => [0x%" PRIx64 ", 0x%" PRIx64 ") crosses4G %d",
+               attempt, (uint64_t)req.out.gpu_va, va, va, va + total,
+               (va >> 32) != ((va + total - 1) >> 32));   /* ← P1b: 真正拿到的那段区间 */
      if ((va >> 32) == ((va + total - 1) >> 32))
         return va;
```
**判据（决定性）**：
- **P1b 的 `mmap_va == 0x5fffd1e000`**（或 `mmap_va + total == 0x5fffe1e000`）⇒ **81 号定量命中坐实，根因锁定** ✓✓✓
- **P1a/P1b 都不出** ⇒ **`kbase_kmod_alias_create` 从未被调用** ⇒ 别名支**在 tri 上不适用** ⇒ 那个 1 MiB 区域另有来源 ⇒ 走 P2/P3 定位（下面）✓
- P1b 出的 `mmap_va` 是**低地址**（如 `0x7f…`/`0x1…`）⇒ 别名区与本故障无关 ⇒ 回到 80 号的 E-A（unlogged BO）✓

### P2：`init_render_desc_ringbuf()` **函数入口**（证明"这个函数在 tri 里到底跑没跑"）
**锚点**：该函数的第一行（`grep -n "init_render_desc_ringbuf" csf/panvk_vX_gpu_queue.c` 定位；函数体开头通常是 `struct panvk_gpu_queue *queue` 的类型检查/comments）：
```c
   mesa_logi("kbase: init_render_desc_ringbuf ENTER (queue %p, bo_mmap=%d)",
             (void *)queue, PANVK_DEBUG(TRACE));      /* ← P2: 函数被调用即出 */
```

### P3：kbase 块内、`tracing_enabled` 判定**之前**（分辨 A/B）
**锚点**：`if (tracing_enabled) {` 在 kbase 块里的那一处（`:2388` 一带；**注意**文件里 `tracing_enabled` 出现多次，请用上下文确认是 kbase 块内那一个）：
```c
      mesa_logi("kbase: ring kbase path: tracing_enabled=%d ringbuf->size=0x%" PRIx64
                " auto_va=0x%" PRIx64,
                tracing_enabled, (uint64_t)ringbuf->size, (uint64_t)map_op.va.start);
```
**判据**：`tracing_enabled=1` ⇒ 走 `if`（别名不建）⇒ 与"1 MiB"矛盾 ⇒ 需回头核 `PANVK_DEBUG` 的实际位（TRACE=1<<2）✓；`tracing_enabled=0` 而 P1 不出 ⇒ **alias_create 未被调用**（更怪，需查是否 `gpu_queue_uses_kbase()` 为假，或这里被别的 `#ifdef` 包住）✓

### P4（可选，最保险）：`gpu_queue_uses_kbase(dev)` 的返回值
```c
      mesa_logi("kbase: ring path uses_kbase=%d", gpu_queue_uses_kbase(dev));
```

**插入前请对每个锚点 `grep -c` 确认唯一**（我无法再读文件，只能按此前逐行看到的形态给锚点）✓

---

## 2. E-β：打印 `cs_render_desc_ringbuf_move_ptr()` 的 **`wrap_around` 实参**

**锚点**：`csf/panvk_vX_cmd_draw.c:4847` 那一行（`cs_render_desc_ringbuf_move_ptr(b, calc_render_descs_size(cmdbuf), …)`）；在它**之前**插：
```c
   {
      const uint32_t descs_sz = calc_render_descs_size(cmdbuf);
      const bool wrap = /* ← 与下面调用实参同一个表达式 */;
      mesa_logi("kbase: desc ring move_ptr: size 0x%x wrap_around %d "
                "(RINGBUF 0x%x, pos+size <= 2*RINGBUF ? %d)",
                descs_sz, wrap, RENDER_DESC_RINGBUF_SIZE,
                (ctx_pos + descs_sz) <= 2u * RENDER_DESC_RINGBUF_SIZE);
      cs_render_desc_ringbuf_move_ptr(b, descs_sz, wrap);
   }
```
（`ctx_pos` 用 `cmdbuf->state.gfx.render` 里**真实字段名**；若形状不便，退化为**只打 `wrap` 实参**也足够 —— 判据就是"**是否真的会出现 `false`**" ✓）
**并且**：全树把 `cs_render_desc_ringbuf_move_ptr(` 的**所有调用点**都打一遍（`:4847` 之外可能还有），因为 81 §3 的"洞"正是"**某个调用点传了 false 且 pos 已接近 2×**"✓

---

## 3. E-γ（单变量）：`nents: 2 → 3`

**锚点**：`csf/panvk_vX_gpu_queue.c` 的
```c
         ringbuf->addr.dev = kbase_kmod_alias_create(
            dev->kmod.dev, map_op.va.start, ringbuf->size, 2);
```
**diff**：
```diff
-         ringbuf->addr.dev = kbase_kmod_alias_create(
-            dev->kmod.dev, map_op.va.start, ringbuf->size, 2);
+         /* v125 (report 82, E-γ): 单变量 —— 别名区 2× → 3×。
+          * 若故障地址消失/改变 ⇒ 81 号的"别名区边界"解释成立 ✓
+          * （nents 上限 = ARRAY_SIZE(ai) = 4 ✓） */
+         ringbuf->addr.dev = kbase_kmod_alias_create(
+            dev->kmod.dev, map_op.va.start, ringbuf->size, 3);
```
**判据**：**故障地址改变（尤其变成 `+0x80000`，即 2.5 MiB 侧的边界）或消失** ⇒ **别名边界确认** ✓✓；**完全不变** ⇒ 该 1 MiB 区域与别名无关 ⇒ 回到 80 号 E-A（unlogged BO）✓

**建议组合**：**P1+P2+P3（一次重编，纯观测）** → 若 P1b 命中 ⇒ 直接上 **F1/F2**（81 §5）；若 P1 不出而 P2 出 ⇒ 用 **E-γ** 判"那个 1 MiB 区域是否別名"；**E-β 与 P 包可同版**（都在观测范畴，互不干扰）✓

---

## 4. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**（按纪律只出提案）；**所有锚点行号来自我此前逐行读过的内容**，但**我无法再复核当前树**（预算用尽）⇒ **每个锚点请先 `grep -c` 确认唯一性**，尤其 `tracing_enabled`/`ringbuf->addr.dev = kbase_kmod_alias_create` 这类可能多处出现的形态 ✓
2. **P1 是"必然执行"的观测点**这个判断成立的前提是"别名路径被走到"——它**不**能证明"`init_render_desc_ringbuf` 被调用过" ⇒ **这正是 P2 存在的理由**（两者必须一起上）✓
3. §0 的 A/B/C 三分**是我对"静默"的解释**（**其中 B 与"1 MiB"观测矛盾**，我倾向 A 或 C）⇒ P2/P3/P4 就是分辨它们的 ✓
4. E-β 的 `ctx_pos` 字段名、以及"是否还有其它调用点"**我都未核实** ⇒ 补丁里我已给出"退化为只打 `wrap`"的兜底 ✓
5. E-γ 的 `nents: 3` 会**多要 512 KiB 别名 VA**（并把回绕镜像的可用范围扩大）⇒ 语义上应当无害（回绕算术仍以 `RENDER_DESC_RINGBUF_SIZE` 为模），但**它是行为改动**，请与其他观测补丁**分开版本**（避免再次叠加污染，v118→v119 的教训）✓
