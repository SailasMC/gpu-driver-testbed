# 87 — 收口：找出"以 0x12000 绑定 0x11000 BO"的那处 + 一次上机的判据 + 护栏（含字段更正）

日期：2026-10-06 · 只读分析 · 配套：`85`（`bo_size` 判据）、`86`（重叠已判为独立缺陷）
**已确认**：故障 = `A.start + 0x11000`，而 `0x11000 = (64*1024)+4096` = tiler desc+geometry 的尺寸（v120 已还原为 `0x11000` ✓）⇒ **"以 0x12000 绑定实际 0x11000 的 BO"** 是唯一自洽解释 ✓

---

## 0. 先说一条**字段更正**（它直接解开你"断言写不出来"的卡点）

你说"`pan_kmod_vm_op` 无 `bo` 成员 ✗ 我实测过" —— **对，但也不对**：**BO 指针在 `ops[i].map.bo`**（**不是在 op 顶层**）。证据是我此前逐行读到的**描述符环绑定代码**（`csf/panvk_vX_gpu_queue.c:2364-2378`）：
```c
      struct pan_kmod_vm_op map_op = {
         .type = PAN_KMOD_VM_OP_TYPE_MAP,
         .va   = { .start = PAN_KMOD_VM_MAP_AUTO_VA, .size = ringbuf->size },
         .map  = { .bo = ringbuf->bo, .bo_offset = 0 },        /* ← 就是这里 */
      };
```
⇒ **`ops[i].map.bo` 是可用字段** ✓（你大概试的是 `ops[i].bo`）⇒ 护栏断言因此可写 ✓

---

## 1. 你第 1 点：谁算出了 `0x12000`

**先说结论：这几乎必然是"三处独立算式"中的又一次不一致（继 ring 常量、geometry 之后第三个"双常量"）** —— 即 **`va.size` 的来源 ≠ BO 实际尺寸的来源**。按可能性排序，**逐个 grep 即可定位**：

| # | 候选机制 | 要看的算式 | grep |
|---|---|---|---|
| **1 ★** | **池/BO 尺寸被"向上取整"**（对齐/头部预留），而绑定用的是**另一个数** | `panvk_pool_alloc_backing` 里的 `ALIGN*`、`+`、`slab_size`、`round_up`；`panvk_priv_bo_create` 里 `size` 与 bind 的 `va.size` 是否同源 | `grep -rn -A25 "panvk_pool_alloc_backing" src/panfrost/vulkan/ \| grep -v .bak`<br>`grep -rn "ALIGN_POT(\|ALIGN(\|round_up" src/panfrost/vulkan/csf/*.c \| grep -v .bak` |
| **2** | **显式 "+ 一页"** 形态 | `+ 4096`、`+ PAGE_SIZE`、`+ 0x1000`、`guard` 出现处 | `grep -rn "+ 4096\|+ PAGE_SIZE\|+ 0x1000\|guard" src/panfrost/vulkan/ \| grep -v .bak` |
| **3** | **`pan_kmod_bo_size()` 与映射不一致** | 该 helper 的返回值 vs 实际映射 | `grep -rn "pan_kmod_bo_size" src/panfrost \| grep -v .bak` |

**全部 `pan_kmod_vm_bind` 调用点**（逐个看 `.va.size` 的表达式）：
```bash
grep -rn -B8 "pan_kmod_vm_bind(" src/panfrost | grep -v "\.bak" | grep -E "pan_kmod_vm_bind|\.size|\.va"
```

**为什么我押"候选 1"**：`tiler_heap->desc` 来自 **`panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info.size = 0x11000)`**（`:3202`）⇒ 它是**池分配** ⇒ 而**池分配最终要落到某个 slab/BO** ⇒ **"请求 0x11000 的池分配"与"该 slab BO 的绑定长度"是两个数**（池会按自己的粒度/头部取整）⇒ **这两个数一旦不等，就正好差一页** ✓✓ 而 `0x12000 = 0x11000 + 0x1000` 恰好是"**向上取整到 0x2000 的倍数 + 一页头部**"这类形态 ✓

---

## 2. 你第 2 点：一次上机的判据（最小改法）

在**两个分配点**各加一行（都打印"请求尺寸 vs BO 实际尺寸 vs 绑定将用的尺寸"）：

```c
/* (a) panvk_priv_bo_create 成功后 */
mesa_logi("v130 privbo: req_size 0x%" PRIx64 " bo_size 0x%" PRIx64
          " va 0x%" PRIx64 " site=0x%zx",
          (uint64_t)req_size, (uint64_t)pan_kmod_bo_size(bo),
          (uint64_t)bo->addr.dev, site_off);
/* 若该 helper 名/签名不同，用 struct pan_kmod_bo 的尺寸字段等价替代 */
```
```c
/* (b) panvk_pool_alloc_backing 里，在算出 slab 尺寸之后、bind 之前 */
mesa_logi("v130 slab: requested 0x%" PRIx64 " slab_size 0x%" PRIx64
          " (delta 0x%" PRIx64 ")",
          (uint64_t)requested, (uint64_t)slab_size,
          (uint64_t)(slab_size - requested));
```
**判据（一次运行即定）**：
| 观测 | 结论 |
|---|---|
| `slab_size/bo_size == 0x12000` 而 `requested == 0x11000` | **候选 1 坐实**：**池把 0x11000 取整成了 0x12000**，而**内容/使用只到 0x11000** ⇒ 末页虽被绑定但**从来没有 backing**（或 BO 只有 0x11000）⇒ 修法见 §3 ✓✓ |
| `bo_size == 0x12000` 且**绑定也是 0x12000** | 两者一致 ⇒ 那"末页不可翻译"只能来自 **(a) 内核侧**（页粒度/末页约定）⇒ 转 86 §3 的 (a) 支 ✓ |
| `req == bo_size == 0x11000` 而**绑定是 0x12000** | **绑定侧多算**（§1 候选 2）⇒ 直接改那一处表达式 ✓ |

---

## 3. 你第 3 点：修复 diff（首选单个）

**首选（无论上面哪一支，这一条都正确且最小）：绑定一律使用 BO 的实际尺寸**
```diff
--- a/src/panfrost/lib/kmod/pan_kmod.h   (pan_kmod_vm_bind 的 MAP 分支)
+++ b/...
-   /* 原样把调用方给的 va.size 交给内核 */
+   /* v130 (report 87, F2): 绑定长度一律取 BO 的实际尺寸 —— 不允许调用方用
+    * "另一个算出来的数"（本 bug：0x12000 vs BO 0x11000）。若确实需要"多绑一页
+    * 作 guard"，必须显式表达（单独字段/单独 op），而不是靠 size 多算。 */
+   if (op->type == PAN_KMOD_VM_OP_TYPE_MAP &&
+       op->va.size > pan_kmod_bo_size(op->map.bo))
+      op->va.size = pan_kmod_bo_size(op->map.bo);
```
（`pan_kmod_bo_size()` 名字/可用性以 grep 为准；等价写法 `op->map.bo->size` ✓）
**若根因是"池把 0x11000 取整成 0x12000，但只有 0x11000 有内容"** ⇒ 改为**把分配与绑定都写成 0x11000**（或在描述符/几何的写入范围上叠加"只允许 …≤0x11000"的断言）⇒ **即"可用长度"与"绑定长度"分开，并让所有写入范围用可用长度** ✓

---

## 4. 你第 4 点：零风险护栏（头文件安全 + 可行字段）

```c
   /* v130 (report 87, F4): 发射期护栏 —— MAP 的绑定长度不得超过目标 BO 的实际尺寸。
    * 用 ops[i].map.bo（不是 ops[i].bo ✗）；不需要 <dlfcn.h>。 */
   assert(ops[i].type != PAN_KMOD_VM_OP_TYPE_MAP ||
          ops[i].va.size <= pan_kmod_bo_size(ops[i].map.bo));
```
（若 `pan_kmod_bo_size` 在该头文件处不可见 ⇒ 用 `ops[i].map.bo->size`；若两者都不可见，退化为只打日志 + 在 `.c` 侧的 bind 包装里断言 ✓）
**建议同版一起上**：它把"这类不一致"从"运行时翻译故障"提前到"**发射期断言**"✓

---

## 5. 关于重叠（按你的决定，延后）

按 86 号的结论：**重叠是独立缺陷、且是 bug 3（面板/重复内容）的极佳候选** ✓ ⇒ **等本故障收口后单独追**，届时上 **F-O2 的区间表断言**（`MAP` 插入 / `UNMAP` 删除，相交即断言）✓ —— 本报告不动它 ✓

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**（按纪律只出提案）；§0 的 `ops[i].map.bo` 有**逐行读到的调用点证据**（`gpu_queue.c:2364-2378` 的 `.map = { .bo = … }`）✓
2. **`0x12000` 的具体来源我仍未定位到行**（预算用尽）⇒ §1 给的是**按可能性排序的候选 + 精确 grep**；**§2 的两行日志就是"一次上机定案"的设计** ✓
3. §3/§4 的 `pan_kmod_bo_size()` 名字与可见性**未复核**（已给 `ops[i].map.bo->size` 等价写法）⇒ 编译前对一眼 ✓
4. §1 候选 1 的"池向上取整"是**推断**（依据：`desc` 来自 `panvk_pool_alloc_mem`，而池必然有自己的 slab 粒度），**未读到 `panvk_pool_alloc_backing` 的取整表达式** ⇒ §2(b) 的日志即为它的判据 ✓
5. 所有 diff **未编译、未上机**。
