# 81 — 别名区假设**定量命中**：两个故障地址正好相差 `2 × RENDER_DESC_RINGBUF_SIZE = 1 MiB`

日期：2026-10-06 · 只读分析 · 配套：`80-unlogged-bos.md`、`79-desc-ring-capacity.md`
**已收到**：E4 空结果（v123，不再迭代 ✓）、E-A 落点问题（`pan_kmod_bo_alloc` 只给尺寸、VA 在 bind 时赋 ⇒ 要在 `ringbuf->addr.dev` 赋值后打 ✓）

---

## 1. 别名机制逐行核实（`lib/kmod/kbase_kmod.c:1097-1175`）

```c
uint64_t kbase_kmod_alias_create(dev, bo_va, size, nents)
{
   uint64_t total = size * nents;                 /* ← nents=2 ⇒ total = 2 × ring size */
   for (i < nents) ai[i] = { .handle = bo_va, .offset = 0, .length = size / 4096 };
   for (attempt = 0; attempt < 16; attempt++) {
      union kbase_ioctl_mem_alias req = { .in = {
         .flags = GPU_RD | GPU_WR | CPU_RD, .stride = size / 4096, .nents = nents,
         .aliasing_info = (uintptr_t)ai } };
      ioctl(dev->fd, KBASE_IOCTL_MEM_ALIAS, &req);
      void *ptr = mmap(NULL, total, PROT_READ, MAP_SHARED, dev->fd, req.out.gpu_va);
      uint64_t va = (uintptr_t)ptr;
      if ((va >> 32) == ((va + total - 1) >> 32)) return va;   /* ← 不跨 4G 才接受 */
      munmap(ptr, total);
   }
   return 0;
}
```
**要点（逐条）**
| 项 | 结论 |
|---|---|
| 请求的区间尺寸 | **`total = size × nents`**；调用点 `gpu_queue.c:2384-2400` 传 **`nents = 2`** ⇒ **`2 × RENDER_DESC_RINGBUF_SIZE = 2 × 512 KiB = 1 MiB = 0x100000`** ✓✓ |
| VA 由谁选 | **内核给 cookie，`mmap` 决定地址**；注释明说 kbase 拒绝 MAP_FIXED/地址提示 ⇒ **地址由内核/内核 mmap 决定** |
| 约束 | **整个 `total` 必须落在同一个 4 GiB 窗口内**（否则 `munmap` 重试，最多 16 次）⇒ 供 **32 位回绕算术**使用 ✓ |
| 是否进 kctx 页表 | **是**：SAME_VA 区域（注释：*"the resulting address being both the CPU and GPU VA"*）⇒ GPU 可直接访问 ✓（**这也解释了为什么它不在 priv-BO 表里**：它是 `KBASE_IOCTL_MEM_ALIAS` + `mmap` 的 SAME_VA 区域，不是我们的 priv BO ✓✓） |

---

## 2. ★★ 定量命中：两个故障地址**正好相差 1 MiB**，而别名区正是 1 MiB

你手上一共出现过的四个地址里，有两个是**反复出现的**：
```
0x5fffd1e000   （v112 那局）
0x5fffe1e000   （v115/v120/v121 那局，最稳定）
```
**差值 = `0x100000` = 1 MiB = `2 × RENDER_DESC_RINGBUF_SIZE`** ✓✓✓
⇒ **⇒ 二者恰好是"一个 1 MiB 别名区的起点与终点"**：
```
若别名区 = [0x5fffd1e000, 0x5fffe1e000)   （base = 0x5fffd1e000，total = 1 MiB）
   ⇒ 故障 A 0x5fffd1e000 = 该区的**起点**
   ⇒ 故障 B 0x5fffe1e000 = 该区的**终点**
```
（我在 78 号把这两个地址读成"同一偏移落在两个 1 MiB 栅格上"；**现在有了 `nents=2` 这条代码证据，更具体的解释是"它们是同一个 1 MiB 别名区的两端"** ✓ —— 这两个读法**共享同一个 1 MiB 量**，而别名读法**多解释了"为什么是映射区边界"**。）
⇒ **并且它同时解释**：**固定且确定**（mmap 每次给同一地址）✓、**不随 utrace 堆尺寸变**（别名区独立分配）✓、**只有真 draw 触发**（只有 draw 消费描述符环）✓、**priv-BO 表里看不到**（SAME_VA 别名区）✓✓✓ —— **四条全中**，这是目前唯一四条全中的解释 ✓

---

## 3. 回绕算术逐行核实（`csf/panvk_vX_cmd_draw.c:1109-1165`）⇒ **洞在 `wrap_around == false` 这条路**

```c
cs_render_desc_ringbuf_move_ptr(struct cs_builder *b, uint32_t size, bool wrap_around)
{
   cs_load_to(b, cs_scratch_reg_tuple(b, 2, 3), ctx, …, offsetof(render.desc_ringbuf.ptr));  /* ptr_lo, pos */
   cs_add_imm32(b, ptr_lo, ptr_lo, size);        /* 绝对地址 += size */
   cs_add_imm32(b, pos,    pos,    size);
   if (likely(wrap_around)) {                     /* ← 只有这里才回绕 */
      cs_add_imm32(b, scratch_reg, pos, -RENDER_DESC_RINGBUF_SIZE);
      cs_if(b, MALI_CS_CONDITION_GEQUAL, scratch_reg) {
         cs_add_imm32(b, ptr_lo, ptr_lo, -RENDER_DESC_RINGBUF_SIZE);
         cs_add_imm32(b, pos,    pos,    -RENDER_DESC_RINGBUF_SIZE);
      }
   }
   cs_store(b, …ptr/pos…);
}
```
**核对结论**
- **`wrap_around == true` 时**：`ptr_lo` 永远被夹在 `[base, base + RINGBUF_SIZE)` ⊆ **别名区前半** ⇒ **不可能碰到别名区终点** ✓（前提：**唯一的调用者都传 true**，见下）
- **`wrap_around == false` 时**：**完全不做回绕** ⇒ 只要 `pos + size > 2 × RINGBUF_SIZE`，`ptr_lo` 就会**越过别名区终点** ⇒ **正好落到你观测的那个"固定地址"** ✓✓✓ **⇒ 这就是洞**：
  - 调用点：`:4847` `cs_render_desc_ringbuf_move_ptr(b, calc_render_descs_size(cmdbuf), …)` —— **`wrap_around` 实参是什么？** 这是下一轮**必须核对**的一行（若它在"环尾/预留给下一帧"的情况下传 `false`，且 `pos` 已接近 2×，就会越界）；
  - 与之配套的是 `cs_render_desc_ringbuf_reserve()`（`:1109-1135`）里那句 `assert(size <= RENDER_DESC_RINGBUF_SIZE)`：**它只保证"单次预留 ≤ 512 KiB"，不保证"pos + size ≤ 别名区 1 MiB"** ⇒ **边界上缺的正是这条不变量** ✓
- **⇒ 因此 §2 的"别名区终点"与 §3 的"`wrap_around=false` 越界"是同一个故事的两半** ✓

---

## 4. 5 分钟实验（你要的精确插入点与代码）

### E-α（★一行，直接定案）：打印别名区
**插入点**：`csf/panvk_vX_gpu_queue.c:2394-2400`，即 `ringbuf->addr.dev = kbase_kmod_alias_create(...)` **成功之后**、`goto ringbuf_mapped;` **之前**（用 `grep -n "ringbuf->addr.dev = kbase_kmod_alias_create"` 定位，唯一 ✓）：
```c
         uint64_t alias_va = kbase_kmod_alias_create(dev->kmod.dev, map_op.va.start,
                                                    ringbuf->size, 2);
         if (!alias_va)
            return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                "Failed to GPU map ringbuf BO (alias)");
         ringbuf->addr.dev = alias_va;
         mesa_logi("kbase: desc ringbuf alias: bo_va 0x%" PRIx64 " size 0x%" PRIx64
                   " alias=[0x%" PRIx64 ", 0x%" PRIx64 ") (total 0x%" PRIx64 ")",
                   (uint64_t)map_op.va.start, (uint64_t)ringbuf->size,
                   alias_va, alias_va + 2 * ringbuf->size, 2 * ringbuf->size);
```
**判据（决定性）**：
- **`alias_va == 0x5fffd1e000`**（或 `alias_va + 2*size == 0x5fffe1e000`）⇒ **§2 定量命中，根因锁定** ✓✓✓
- `alias_va` 是某个**低地址**（< 4 GiB 常见）⇒ 与本故障无关 ⇒ 别名这一支降级，回到 80 号的 E-A（unlogged BO）✓

### E-β（零成本，与 E-α 同版）：把 `wrap_around` 实参打出来
在 **`:4847`** 与任何其它 `cs_render_desc_ringbuf_move_ptr` 调用点打印 `wrap_around` 与 `calc_render_descs_size(cmdbuf)`；**判据**：是否在"接近环尾"时出现了 `false` ✓

### E-γ（单变量）：把别名区做成 **3×**
`kbase_kmod_alias_create(..., nents=3)`（`nents <= ARRAY_SIZE(ai)=4` ✓）：**若故障消失/地址改变 ⇒ 别名边界确认** ✓（等价于临时 guard）

---

## 5. 修复候选（按风险）

| 候选 | 内容 | 风险 |
|---|---|---|
| **F1（护栏，先上）** | 别名区 `nents: 2 → 3`（多留 512 KiB 余量）**或**在 `kbase_kmod_alias_create` 里把 `total` 向上留一页且保证**末尾页不映射** | **低**（只多要 VA，不改语义） |
| **F2（真正的修复）** | **审 `wrap_around=false` 的调用点**：要么保证"传 false 时 `pos + size ≤ 2 × RINGBUF_SIZE`"（加断言：`assert(pos + size <= 2 * RENDER_DESC_RINGBUF_SIZE)`），要么**删掉 `false` 这条捷径**（永远回绕） | 中（触及描述符环的记账，须单变量上机） |
| **F3（不变量补全）** | `cs_render_desc_ringbuf_reserve()` 里把断言加强为"**本次预留后仍不越过别名区**"（`pos + size <= 2*RINGBUF`），并在发射期（宿主侧）校验 | 低 |
| **F4** | 把别名区的 VA/尺寸纳入**地址来源校验器**（78 §4-E2）⇒ 以后任何"指向别名区外"的指针都会在发射期被抓住 | 低 |

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1 的别名机制**逐行核实**（`kbase_kmod.c:1097-1175` + 调用点 `gpu_queue.c:2355-2400`）；§3 的游标算术**逐行核实**（`cmd_draw.c:1109-1165`）。
2. **§2 是"定量命中"但不是"已证实"**：它是**两个观测地址之差 = 1 MiB = `2 × RINGBUF_SIZE`** 与**别名区尺寸公式**的吻合 ⇒ **E-α 一行即可证实/证伪**；在那之前请把它当**最强假设**而非结论。
3. **80 号 §0（unlogged BO）与 §2（别名区）共享同一个 1 MiB 量** ⇒ 两者**不互斥**：别名区本身就是一个"不进 priv-BO 表的 1 MiB 区域" ⇒ **它们其实是同一件事的两种说法** ✓（这一点我在 78 号没看出来，现在合流了）
4. **`wrap_around` 的实参我没读到**（`cmd_draw.c:4847` 只看到 `calc_render_descs_size` 作为 size）⇒ §3 的"洞"仍是**结构推断**，E-β 一行可判 ✓
5. 所有改动**未编译、未上机**（按纪律只出提案）。
