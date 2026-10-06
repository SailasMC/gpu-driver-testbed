# 85 — 把 72 KiB 对到调用点（**不用 `<dlfcn.h>`** 的写法）+ 尺寸/粒度审计 + 一处机制更正

日期：2026-10-06 · 只读分析 · 配套：`84-bind-minus-0x11000.md`（算术命中）、`77`（`0x11000` 来源）
**已收到**：22 条全序列 ⇒ **没有任何 MAP→UNMAP→再 MAP** ⇒ **(b) 支"重绑/搬走"在你这一局不成立** ✗ ✓；且 `0x12000` **不是字面量 ⇒ 是算出来的** ✓

---

## 0. 先把形态钉死（与我 84 号的算术合起来）

```
MAP  : [0x5fffd8d000, 0x5fffd9f000)  size = 0x12000 = 18 页
故障 :  0x5fffd9e000 = 0x5fffd9f000 − 0x1000   ← 该区间的**最后一页的起点**
     且 = 0x5fffd8d000 + 0x11000               ← 而 0x11000 正是 tiler desc+geometry 的尺寸
```
⇒ **"最后一页"这个形态 + `0x11000` 这个已知常量合起来 ⇒ 最简解释是：这块区域的 BO 只有 `0x11000`（17 页），而绑定按 `0x12000`（18 页）登记 ⇒ 第 18 页没有 backing ⇒ GPU 访问 `start + 0x11000` 就翻译失败** ✓✓✓
⇒ **⇒ 只需要一个字段就能判死：`ops[i].map.bo->size`**（下面 §1 的补丁，**不需要 `dlfcn.h`**）✓

---

## 1. 你第 1 点：在 `pan_kmod.h` 的 `inline pan_kmod_vm_bind()` 里**打 bo 指针 + bo->size**（头文件里不宜引 `<dlfcn.h>` ⇒ 用"指针配对"）

**为什么会有效**：分配侧（`panvk_priv_bo_create` / `panvk_pool_alloc_backing` / `pan_kmod_bo_alloc`）**已经有打印**，而绑定侧的 `ops[i].map.bo` 就是**同一个 `pan_kmod_bo *`** ⇒ **两侧按指针值配对即可**，完全不需要 `dladdr` ✓✓

**补丁（`panfrost/lib/kmod/pan_kmod.h`，`pan_kmod_vm_bind()` 内你已加打印的那个循环）**：
```c
      /* v128 (report 85): 用"BO 指针 + BO 尺寸"配对，避免在头文件里引 <dlfcn.h>。
       * 分配侧已有 (panvk_priv_bo_create / panvk_pool_alloc_backing /
       * pan_kmod_bo_alloc) 的 va/size 打印 ⇒ 按指针值即可把本区间对到调用点。 */
      const struct pan_kmod_bo *logbo = ops[i].map.bo;      /* ← 字段名以头文件为准 */
      mesa_logi("v128 bind: ret=%d type=%d va=[0x%" PRIx64 ", 0x%" PRIx64
                ") size=0x%" PRIx64 " bo=%p bo_size=0x%" PRIx64 " bo_va=0x%" PRIx64,
                ret, (int)ops[i].type, ops[i].va.start,
                ops[i].va.start + ops[i].va.size, ops[i].va.size,
                (void *)logbo, logbo ? (uint64_t)logbo->size : 0,
                logbo ? (uint64_t)logbo->addr.dev : 0);
```
（`ops[i].map.bo`/`bo->size`/`bo->addr.dev` 的**字段名请以头文件为准** —— 我按你已用的 `op->map.bo` 形态写；若 `bo_va` 字段名不同，退化为只打 `bo` 与 `bo_size` 也够用 ✓）
**若你想连"调用点"也一并拿到（可选、且不污染头文件）**：在**分配侧**（`.c` 文件里，可以安全引 `<dlfcn.h>`）给每块 BO 打 `site_off`；因为**一个 BO 只被分配一次**，所以"分配侧的 site"就是这块 VA 的所有者 ✓✓

**判据（一次运行）**：
| 观测 | 结论 |
|---|---|
| **`bo_size == 0x11000` 而 `va.size == 0x12000`** | **R1 坐实**：**绑定比 BO 多一页** ⇒ 第 18 页无 backing ⇒ 故障点正是它 ✓✓✓ |
| `bo_size == 0x12000`（与绑定一致） | ⇒ 转 (a)/粒度支（§2 下半 + §3） |
| `bo == NULL`（非 MAP op）或 `bo_size` 与分配侧对不上 | ⇒ 说明这个 op 的类型/结构与我假设不同 ⇒ 把该行原文贴我 ✓ |

---

## 2. 你第 2 点：尺寸/粒度审计 —— **并附一处机制更正（很重要）**

**⚠️ 更正**："非 4096 对齐的 size ⇒ 尾部不满一页不进页表"**通常不成立**：页表按**页**映射，`size` 不是页的整数倍时，内核会把**最后一页整体**映射（多出的部分是填充）⇒ **`ceil(size/4096)` 页都在** ⇒ **不会因此产生翻译故障**。
**⇒ 能产生"最后一页不可翻译"的只有两种**：
1. **绑定跨过了 BO 的末尾**（`va.size > bo->size`，或 VA 区间落在 BO 之外）⇒ **R1** ✓✓ 最符合 `start + 0x11000` 这个精确值；
2. **内核在映射时按某种粒度向下取整/丢弃末页**（例如按大页/2 MiB 粒度、或末页留作元数据）⇒ 需要看 kbase 的 map 实现与 uapi 约定（§3）。

**⇒ 因此审计的重点不是"是否 4096 对齐"，而是"`va.size` 是否 == `bo->size`，以及 `va` 区间是否落在 BO 之内"**。审计命令：
```bash
M=/root/zenithblue/work/mesa/src/panfrost
grep -rn "pan_kmod_vm_bind(" $M | grep -v "\.bak"            # 全部调用点
grep -rn "PAN_KMOD_VM_MAP_AUTO_VA" $M | grep -v "\.bak"      # AUTO_VA 的调用点（内核回写地址）
```
对每个 `MAP` op 检查三件事：① `va.size` vs `bo->size`；② `va.start + va.size` vs `bo->addr.dev + bo->size`；③ 是否 4096 对齐（对齐**仍值得看**，但**不是**本故障的成因）。
**关于 `0x12000` 是"算出来的"** —— 优先在这两处找：
- **分配侧**：`(64*1024) + 4096 = 0x11000`（tiler desc+geometry，`gpu_queue.c:3196-3202`）⇒ 谁把它**再加一页**（`+ 4096`、`ALIGN_POT(x, 0x2000)`、`+ guard`）？
- **池/对齐**：`panvk_pool` 的 slab 取整（`mempools.rw` 的 slab = 16 KiB）⇒ 若某处按 slab 粒度向上取整，`0x11000 → 0x14000`（**不是** `0x12000`）⇒ 所以**更可能是"`bo->size + 4096`"这类显式加页** ✓

---

## 3. 若 R1 命中 ⇒ 修复 diff（你第 3 点）

```diff
--- a/src/panfrost/lib/kmod/pan_kmod.h
+++ b/ (pan_kmod_vm_bind 的 MAP 分支，提交 ioctl 之前)
+   /* v129 (report 85, F2): 绑定不得跨过 BO 末尾 —— 跨过去的那一页没有 backing，
+    * 固件访问它就报 TRANSLATION_FAULT（v126 现场：故障 = 绑定起点 + BO 尺寸）。 */
+   assert(ops[i].type != PAN_KMOD_VM_OP_TYPE_MAP ||
+          ops[i].va.size <= ops[i].map.bo->size);
```
**根因侧的修法（择一，视 §1 的读数）**：
- **F2a**：**让 bind 用 `bo->size`**（把那处 `+ 4096`/多算的一页删掉）；
- **F2b**：若"多一页"是**有意的 guard**（想让越界立刻崩）⇒ 那就必须让**那一页真的不可访问**，且**固件不该访问它** ⇒ 说明我们的**描述符/几何写入范围**用错了长度 ⇒ 把"绑定长度"与"可用长度"分成两个数，并让**所有写入范围用可用长度** ✓
- **F2c（若 §2-2 命中）**：修内核侧粒度/末页处理（我们树内能看 map 实现与 uapi 约定）✓

---

## 4. F1（`MEM_JIT_INIT` 的 `va_pages`）

**继续留着** ✓ —— v126 已把故障钉在**我们自己的绑定区间内**（`bo` 一把就能定 R1），F1 的价值只在"§1/§2/§3 全出局"时体现 ✓

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**（按纪律只出提案）；§0 的算术**完全基于你 v126 的数据**；`0x11000` 与 tiler desc+geometry 的对应来自我逐行读过的 `:3196-3202` + 你 v120 表同尺寸 BO ✓
2. **§1 的字段名（`ops[i].map.bo`、`bo->size`、`bo->addr.dev`）我只按你已用的形态写**，未逐字复核 `pan_kmod.h` 的结构定义（预算用尽）⇒ **编译前请对一眼**；`bo` 指针配对法**不依赖 `dladdr`**，这是它的设计目的 ✓
3. **§2 的"更正"是机制层面的**：页表按页映射 ⇒ 非对齐 size 一般仍是 `ceil()` 页 ⇒ **不要把"未对齐"当根因**；真正的判据是"`va.size` vs `bo->size`" ✓
4. §2 末尾"0x12000 更可能是 `bo->size + 4096`"是**推断**（排除了 slab 粒度取整：那会得 0x14000）⇒ 请在树内 grep `+ 4096`、`ALIGN_POT(..., 0x2000)`、`guard` 这类形态确认 ✓
5. 所有 diff **未编译、未上机**。
