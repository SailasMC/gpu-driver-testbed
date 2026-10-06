# 75 — capture 路径逐行核实：耗尽**已正确处理**（我 74 号的"残缺地址"假设被否证）+ 捕获目标算式 + 让它"消失"的实验

日期：2026-10-06 · 只读分析 · 配套：`74-utrace-clone-heap.md`（对象与尺寸链）
**已收到你的 E-a/E-b 定案**：故障地址随 `PANVK_UTRACE_CLONE_MEM_SIZE` **1:1 平移**（`0xa00000→0x100000` 时精确左移 `0x100000`）✓；`PANVK_DEBUG=0` 下**仍崩** ⇒ **与诊断构建无关的真实故障** ✓

---

## 1. 捕获路径逐行核实（你要的第 1 点）

### 1.1 分配侧（运行时）`src/panfrost/vulkan/panvk_utrace.c:26-57`
```c
void *panvk_utrace_create_buffer(struct u_trace_context *utctx, uint64_t size_B)
{
   const uint64_t alignment = 0x40;                       /* 兼作写 CSF 命令 ⇒ cacheline 对齐 */
   simple_mtx_lock(&dev->utrace.copy_buf_heap_lock);
   const uint64_t addr_dev =
      util_vma_heap_alloc(&dev->utrace.copy_buf_heap, size_B, alignment);
   simple_mtx_unlock(&dev->utrace.copy_buf_heap_lock);

   if (!addr_dev) {                                       /* ← 耗尽**有检查** */
      mesa_loge("Couldn't allocate utrace buffer (size = 0x%" PRIx64 ")."
                "Provide larger PANVK_UTRACE_CLONE_MEM_SIZE (current = 0x%" PRIx64 ")",
                size_B, dev->utrace.copy_buf_heap_bo->bo->size);
      return NULL;                                        /* ← **跳过**，不是用残缺地址 */
   }
   …
   void *addr_host = heap_bo->addr.host + addr_dev - heap_bo->addr.dev;
   *container = (struct panvk_utrace_buf){ .host = addr_host, .dev = addr_dev, .size = size_B };
   memset(addr_host, 0, size_B);                          /* ← 宿主清零 */
   return container;
}
```
**⇒ 自我更正（第二次）**：我 74 号 §3 提的"**耗尽后用残缺/溢出地址**"**不成立** —— 代码 `if (!addr_dev) return NULL;` **正确跳过**，并且 `:63/:91-94/:123-126` 明确处理 NULL（"clone alloc failed" ⇒ 跳过该 capture）✓
**⇒ 剩下唯一能"非零但越界"的通道**：**`util_vma_heap_alloc` 返回了一个非零 VA，但它落在 BO 的真实映射之外** —— 即 `util_vma_heap_init(&heap, bo->addr.dev, bo->bo->size)` 里的 **`bo->bo->size` 大于该 BO 的实际映射尺寸**（**又是一处"双尺寸"**）⇒ 堆会把 BO 之外的 VA 分出去 ✓（这正是 §4-F1 要修的）

### 1.2 发射侧（CS）`csf/panvk_vX_utrace.c:150-172`
```c
static void
panvk_utrace_capture_data(struct u_trace *ut, void *cs, void *dst_buffer,
                          uint64_t dst_offset_B, void *src_buffer,
                          uint64_t src_offset_B, uint32_t size_B)
{
   const struct panvk_utrace_buf *dst_buf = dst_buffer;
   const uint64_t dst_addr = dst_buf->dev + dst_offset_B;   /* ← 目标 = 堆 VA + 偏移 */
   const uint64_t src_addr = src_offset_B;                  /* ← 绝对地址，或寄存器捕获 */
   assert(!src_buffer || (uintptr_t)src_buffer == PANVK_UTRACE_CAPTURE_REGISTERS);
   if ((uintptr_t)src_buffer == PANVK_UTRACE_CAPTURE_REGISTERS)
      cmd_store_regs(b, dst_addr, src_addr, size_B, cs_info->capture_data_wait_for_ts);
   else
      cmd_copy_data (b, dst_addr, src_addr, size_B, cs_info->capture_data_wait_for_ts);
}
```
**⇒ 捕获目标算式 = `dst_buf->dev + dst_offset_B`，长度 `size_B`**，其中 `dst_buf->dev` 来自 `util_vma_heap_alloc`（堆内）✓
**⚠️ 这里没有任何 `dst_offset_B + size_B <= dst_buf->size` 的校验** ⇒ 若上层给出的 `dst_offset_B/size_B` 越出所分配的 clone 缓冲，CS 就会**写到/读到堆的更远处** ✓

---

## 2. 故障算式对位（你第 2 点）

已知（你 E-a）：
```
fault = heap_base + heap_size + C,   C = const = 0x1E000   （0xa00000→0x100000 时 fault 左移 0x100000 ⇒ C 不变）
```
`C = 0x1E000 = 122880 = 30 × 4 KiB`。**三种候选来源（按可能性）**：
| # | 候选 | 说明 |
|---|---|---|
| **C1** | **BO 的"请求尺寸 vs 映射尺寸"差** | `bo->bo->size = 0xa00000`，但实际映射（kbase 可能按对齐/页数取整、或带了尾部守卫/填充）比它大/小 `0x1E000` ⇒ **堆把 BO 之外的 VA 分了出去** ⇒ 与 §1.1 的唯一残余通道**完全一致** ✓✓ |
| **C2** | **`dst_offset_B` 越界** | 上层把 offset 算到 buffer 之外（`dst_buf->dev + dst_offset_B` 落在堆尾之后）⇒ 与 §1.2 缺失的校验对应 ✓ |
| **C3** | 故障访问其实是**源**（`src_addr`） | 但 `src_offset_B` 是绝对地址、且异常地址随**堆**大小移动 ⇒ **排除** ✗ |

**⇒ C1 与 C2 是仅有的两个自洽解释**，且**都能用一条日志区分**（§3-E-c）。

---

## 3. 下一个单变量实验（env 优先、零重编；目标是"**消失**"而不只是"移动"）

| # | 实验 | 改动 | 判据 |
|---|---|---|---|
| **E-a2（★零代码，5 秒）** | `PANVK_UTRACE_CLONE_MEM_SIZE=0x10000`（64 KiB，**故意小到让 clone 分配失败**） | **0** | ① **故障消失** ⇒ 根因在 **capture 使用**（clone 分配失败 ⇒ 走 skip 路径）✓✓；② 仍出现（地址 = base+0x10000+C）⇒ clone 仍成功 ⇒ 需更小（如 `0x1000`）；③ **device 创建失败**（0 尺寸）⇒ 记录即可 |
| **E-b2（零代码）** | `PANVK_UTRACE_CLONE_MEM_SIZE=0x10000000`（256 MiB） | 0 | 只用于进一步确认 **C 恒定**（fault 应右移 0xF000000）；不作为"消失"实验 |
| **E-c（1~3 行，最省的重编）** | 在 `:190-204` 之后打：`copy_buf_heap_bo->addr.dev`、`bo->bo->size`、**实际映射尺寸**（若 priv BO 能取到）、`util_vma_heap` 的空闲总量 | 3 行 | **直接判 C1**：三者不一致 ⇒ 堆范围大于映射 ⇒ 根因落定 ✓ |
| **E-d（1 行，强判别）** | 在 `panvk_utrace_create_buffer()` **开头直接 `return NULL;`**（临时） | **1 行** | 所有 capture 被跳过（会打 `:91-94/:123-126` 的 "clone alloc failed" 日志）⇒ **若故障消失 ⇒ capture 使用路径就是根因** ✓✓（这是"关掉 capture"的最小做法；比找 env 开关更确定） |

**建议顺序**：**E-a2（零代码）→ E-d（1 行）→ E-c（定位 C1/C2）**。

---

## 4. 修复候选（若 C1/C2 之一确认）

| 候选 | diff 要点 | 风险 |
|---|---|---|
| **F1（C1，首选）** | `utrace_context_init()` 里把堆范围钉到**真实映射尺寸**：`util_vma_heap_init(&heap, bo->addr.dev, MIN2(bo->bo->size, <real_size>))`；若无 API 取 real_size，则**在 `panvk_priv_bo_create` 里保证 `bo->size` == 映射尺寸**（或加一条启动断言 `mapped_size >= bo->size`） | 低 |
| **F2（C2，必备）** | 在 `panvk_utrace_capture_data()` 里加边界断言/钳制：`assert(dst_offset_B + size_B <= dst_buf->size);`（release 下改为 `if (…) return;` + 限流日志） | **0**（纯防御） |
| **F3（可观测性）** | 在 `panvk_utrace_create_buffer()` 成功路径加一行限流日志：`buf->dev, size_B, heap free` ⇒ 以后任何"堆越界"都能立刻定位到具体 clone | 0 |
| — | **不要**去改 `PANVK_UTRACE_CLONE_MEM_SIZE` 默认值来"修"它：E-a 已证明那只是**平移**故障地址，不是修复 | — |

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1 的两段代码**逐行核实**（运行时 `:26-57`、CS 侧 `:150-172`）——**本报告最硬的部分**。
2. **`cmd_copy_data` / `cmd_store_regs` 的实际 CS 编码我没读** ⇒ **"为什么是 READ 而不是 WRITE"我尚未解释**（若 `cmd_copy_data` 是 `LOAD_MULTIPLE`(源) + `STORE_MULTIPLE`(目标)，越过堆尾应报 WRITE；出现 READ 说明可能还有一次**目标侧的读**或**长度驱动的地址游走**）。**这是我下一轮第一件要读的**（在 `csf/panvk_vX_utrace.c` 上部，`cmd_copy_data` 的定义处）。
3. **C1 的"实际映射尺寸"我无法从代码断言**（要看 `panvk_priv_bo_create`/`panvk_priv_mem` 里 `bo->size` 的赋值是否经过取整）⇒ E-c 的一行日志即可定。
4. **什么在开启 u_trace 我未查明**（`PANVK_DEBUG=0` 下仍崩 ⇒ 不是 kbase_diag；可能是这个 fork 无条件记录 trace，或 app/库侧触发）⇒ E-d 的 `return NULL` 正好绕过这个问题（不依赖"谁开启"）。
5. `C = 0x1E000` 的三种解释里，**C3（源地址）已被"随堆大小平移"排除**；C1/C2 仍是候选 ⇒ E-c/E-d 决定。
