# 80 — 换靶：**"空洞"很可能是"没被记录的 BO"**（`pan_kmod_bo_alloc` 不进 priv-BO 表）+ 新排序 + CS 流内常量评估

日期：2026-10-06 · 只读分析 · 配套：`79-desc-ring-capacity.md`（H-Desc 的两条定量支撑被你否证 ✓）
**已接受你的三条**：`RENDER_DESC_RINGBUF_SIZE = 512 KiB` 与故障偏移不符 ✗ · 默认路径 `ringbuf->size == RENDER_DESC_RINGBUF_SIZE`（同源）✗ · **该环的 BO 由 `pan_kmod_bo_alloc` 分配 ⇒ 不在 priv-BO 日志里** ✓✓

---

## 0. ★ 由你自己那条事实推出的新结论（本轮最重要）

> **`pan_kmod_bo_alloc` 分配的 BO 不进我们的 priv-BO 日志** ⇒ **那份"完整 BO 表"其实是"priv BO 表"** ⇒ **`0x5fffe00000` 之后那段 ~1.6 MiB 未必是空洞——它可能住着若干"没被记录"的 BO** ✓✓✓

**这直接复活了"地址是某个 BO 的边界"这一族，而且解释力最强**：
- 你观测到 `0x5fffe1e000` = `0x5fffe00000 + 0x1E000`（**120 KiB**）、`0x5fffe26000` = `+0x26000`（**152 KiB**）
  ⇒ 这两个数**恰好可以是"从 `0x5fffe00000` 起算的两个 BO 的尺寸/终点"**：
  **BO_A: `[0x5fffe00000, 0x5fffe1e000)`（120 KiB）**、**BO_B: `[0x5fffe00000, 0x5fffe26000)`（152 KiB）** —— 或者它们是**同两个 BO 的"终点 + 一个固定小偏移"**；
- 而"两个故障地址**固定且确定**、只有真 draw 触发、且不随 utrace 堆尺寸变"——**如果它们是 unlogged BO 的边界，全部自洽** ✓✓（priv-BO 表里看不到 ⇒ 你以为那里是空洞）
- **⇒ 下一步最省、最有价值的一步：把 `pan_kmod_bo_alloc` 的分配也打出来**（va/size/site），再与两个故障地址对位 ✓✓ **这一步能把"算出来的地址"变成"某个 BO 的终点"，后者是可执行的**（见 §3）。

---

## 1. 你第 ★ 点（"地址被编进 CS 指令流"）：真实但**很窄**，逐处评估

**全树里"把计算出来的地址做成 CS 操作数"的地方（我逐处过了一遍）**：

| 站点 | 编进流里的值 | 是否可能是故障源 |
|---|---|---|
| `:1515`、`:2935` `cs_move64_to(heap_ctx_addr, queue->tiler_heap.context.dev_addr)` + `cs_heap_set` | **内核给的 heap context VA** | ✗ 你 v110 实测 `ctx_va=0x6000000000`，与故障地址不同 |
| `cmd_buffer.c:693-700`（消费者等待）`cs_load64_to(sync_addr, ctx, offsetof(syncobjs))` + `cs_add_imm64(…, sizeof(sync64)*j)` | `ctx->syncobjs + j*16` | ✗ `syncobjs` 来自 ctx（复合字面量已正确初始化；`j≤2`）⇒ 偏移只有 0/16/32 |
| `:2836-2838` `geom_buf = tiler_heap + 4096 \| (size>>12)` | 打包值（进 **ctx→descriptor**） | ⚠️ 你 v118–v120 已排除"尺寸/内容"这一支（v120 自洽仍崩） |
| **描述符环游标**（`cmd_draw.c:1325/4736/4847` 的 `calc_render_descs_size` + `cs_render_desc_ringbuf_move_ptr`） | 环指针/游标 | ⚠️ 容量与 BO 同源（512 KiB）⇒ **该支被你否证**；**但"游标 + 尺寸"仍可能越过**（容量对≠不越界，见 §2-③） |
| **命令环换行**（`gpu_queue.c:1403-1408`）`insert += KBASE_RINGBUF_SIZE - offset` | **不是 CS 操作数**（走内核 submit 的 `insert`） | ✗（但它是"环尺寸双常量"那一族，73 号已记） |
| **init stream 作为 queue submit**（`:2790-2793`、`:2956-2968`）`cs_buffer_addr/size = cs_root_chunk_*` | **每次提交给内核的流地址+长度** | **★ 未排除**（见 §2-⑤） |
| checkpoint/marks（诊断版） | `debug.kbase_progress_addr` | ✗ 非诊断版为 0，而故障在 `PANVK_DEBUG=0` 下也发生 |

**⇒ 结论**：你提的这条**机制上成立**（`cs_move64_to` 一个算错的 `base+size` ⇒ 固件确定地访问它），**但可落点只有两处仍未被排除**：**描述符环游标越界**（§2-③）与 **init stream 的地址/长度**（§2-⑤）✓

---

## 2. "设备初始化期创建 / 地址固定 / 每次 draw 消费"三条筛后的**新排序**

| # | 候选 | 为什么排这里 | 一次运行的判据 |
|---|---|---|---|
| **1 ★** | **unlogged BO（`pan_kmod_bo_alloc`）的边界** | **§0**：priv-BO 表看不见它们 ⇒ "空洞"可能是它们 ⇒ 两个故障偏移正好是 BO 尺寸量级 | **打 `pan_kmod_bo_alloc` 的 va/size/site** ⇒ 与 `0x5fffe1e000`/`0x5fffe26000` 对位 |
| **2** | **init stream 的提交地址/长度**（`subq->req_resource.{cs_buffer_addr,cs_buffer_size}`，`:2790-2793`；提交在 `:2956-2968`） | 每次作业提交、地址固定、由 init 期创建；**其 BO 也来自池**（`:2766`）⇒ 若 BO 被搬走/尺寸算错 ⇒ 确定地址 | 打印 `cs_buffer_addr/size` + 该 BO 的区间 ⇒ 是否恰在故障附近 |
| **3** | **描述符环游标越界**（容量对≠不越界） | 容量 512 KiB 与 BO 同源；但 `calc_render_descs_size()` 的**累计值**仍可能超过容量（尤其"chunks not released prematurely"那一带） | `pos + descs_sz` 与 `RENDER_DESC_RINGBUF_SIZE` 比较（E3' 的断言就是它） |
| 4 | `desc_ringbuf.syncobj`（`mempools.rw`，`0x4000`，见你表里的 `0x5ffffa1000`/`0x5ffffa9000`） | 地址**在**表里 ⇒ 不在故障附近 ✗ | 一眼 |
| 5 | `render.oq_chain/ts_chain/ts_done_chain` | 复合字面量已置 0；只有用查询时才填 ⇒ 探针不用 ⇒ 降级 | 若 `tri` 里出现过 `vkCmdWriteTimestamp`/query，再提级 |
| 6 | `tiler_heap.context.dev_addr` | v110 实测 `0x6000000000` ✗ **但请在同局再确认一次**（内核可能按运行给不同 VA） | 打印 `ctx_va` 与故障同局对比 |

---

## 3. 5 分钟实验（按性价比，**全部零/极低改动**）

### E-A（★首选，1~2 行）：把 **`pan_kmod_bo_alloc` 也记录**
在 `pan_kmod_bo_alloc`（或它的唯一封装处）成功后加一行：
```c
   mesa_logi("kmodbo: va=[0x%" PRIx64 ", 0x%" PRIx64 ") size=0x%" PRIx64
             " site=0x%zx", bo->addr.dev, bo->addr.dev + bo->size, bo->size,
             (size_t)((uintptr_t)__builtin_return_address(0) - (uintptr_t)base));
```
（`site` 用你 v115 已经用对的 `dladdr` 写法 ✓）
**判据**：若有一块 BO 的 **`va_end == 0x5fffe1e000`**（或其 `va_start == 0x5fffe00000` 且 size 为 `0x1E000`/`0x26000`）⇒ **owner 落网** ✓✓ —— 之后就能直接查"谁算出了指向该 BO 之后/边界处的地址"。

### E-B（零代码 A/B，5 秒）：**`PANVK_DEBUG=TRACE`** 跑同一局
该开关会走 `:2348-2360` 的**另一支**：`ringbuf->size = PANVK_DESC_TRACEBUF_SIZE`（默认 2 MiB），并且**第二个映射 `[size,2*size)` 故意不映射当 guard**（`:2449-2453` 的注释与 `tracing_enabled ? 1 : ARRAY_SIZE(vm_ops)`）。
**判据**：故障**移动/消失/变形** ⇒ 说明它与"环尺寸+guard 布局"相关 ✓；**完全不变** ⇒ 环这一族整体降级 ⇒ 火力集中到 §2-1/§2-2 ✓

### E-C（1 行）：`init stream` 的地址核对
在 `:2790-2793` 后打印 `subq->req_resource.cs_buffer_addr / cs_buffer_size`（+ 该 BO 区间），与两个故障地址对位。
**判据**：`cs_buffer_addr + cs_buffer_size` 与故障地址相等/相邻 ⇒ **§2-2 命中** ✓（"固件读到流尾之外"是典型的固定确定地址 ✓）

---

## 4. 修复方向（等 E-A/E-C 出结果后再落地）

- 若 owner 是 **unlogged BO** ⇒ 修"计算该 BO 地址/长度的那处算术"，并**把它纳入地址校验器**（78 §4-E2 的来源校验器，届时把 `pan_kmod_bo_alloc` 也登记进去）；
- 若命中 **init stream**（§2-2）⇒ 在提交处加 `assert(stream_size <= BO size)`，并让 `stream_addr/stream_size` 与 BO 尺寸**同源**；
- 若命中 **描述符环游标**（§2-3）⇒ E3' 的断言 + 环尾显式终止块；
- **无论哪一支**：把"guard 区"作为**通用做法**（每个环/流的 BO 之后留一页不映射）⇒ 越界立刻以**确定的地址**暴露，而不是读到别人的内存 ✓

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§0 的推断**只依据"`pan_kmod_bo_alloc` 不进 priv-BO 日志"这一条你给的事实**（我未复核该函数是否确实绕过 `panvk_priv_bo_create` 的打印——**这一点请你顺手确认**，它是 §0 的地基）。
2. **§2 的排序是依据"三条筛"的推理**，不是已找到实例；`0x1E000`/`0x26000` 与"BO 尺寸"的吻合是**量级吻合**（120/152 KiB 都是合理的 BO 尺寸），**尚未对位到具体 BO** ⇒ **E-A 就是它**。
3. §1 的表格**逐处过了一遍**（站点行号来自我此前逐行读过的代码），但 `cs_render_desc_ringbuf_move_ptr` 的实现我**仍未读到**（`genxml/cs_builder.h` grep 无输出）⇒ §2-3 的"仍可能越界"是**语义推断**，E3' 的断言可判。
4. E-B 的判据依赖 `PANVK_DEBUG=TRACE` 确实开启 tracing 并走那一支（`:2348-2360` 我是逐行读的 ✓），但 tracing 还会带来其它差异（tracebuf 分配等）⇒ 它是**便宜的排除项**，不是干净的单变量。
5. 所有改动**未编译、未上机**（按纪律只出提案）。
