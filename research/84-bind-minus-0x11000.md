# 84 — 精确算术命中：故障 = **绑定起点 + 0x11000** = 那个 72 KiB 绑定的**最后一页**；而 `0x11000` 正是 tiler desc+geometry 的分配尺寸

日期：2026-10-06 · 只读分析 · 配套：`83-gpu-va-inventory.md`（E-A 咽喉，奏效 ✓）、`77/79`（`0x11000` 与描述符环）

---

## 0. 先把 v126 的两条区间与故障地址做**精确算术**

```
MAP #1: [0x5fffd8d000, 0x5fffd9f000)   size = 0x12000 = 72 KiB
MAP #2: [0x5fffd97000, 0x5fffd9b000)   size = 0x4000  = 16 KiB
故障  :  0x5fffd9e000
```
**⇒ `0x5fffd9e000 − 0x5fffd8d000 = 0x11000`** ⇒ **故障地址 = 绑定 #1 的起点 + `0x11000`** ✓✓✓
**⇒ 而 `0x11000 = (64 × 1024) + 4096`** —— **正是 tiler heap descriptor + geometry 那个 BO 的分配尺寸**（`csf/panvk_vX_gpu_queue.c:3196-3202`：`alloc_info.size = (64*1024) + 4096`；你 v120 表里也见过同尺寸的 BO `0x5ffffa5000..0x5ffffb6000 size=0x11000 flags=0x40 site=0x9c8270` ✓✓）
**⇒ 同时**：`0x5fffd9f000 − 0x5fffd9e000 = 0x1000` ⇒ **故障点恰好在绑定 #1 的最后一页的起点** ✓✓

**⇒ 两种（都不需要新概念）读数，一次运行即可分开**：
| # | 读数 | 含义 |
|---|---|---|
| **R1 ★** | **绑定尺寸（0x12000）> BO 实际尺寸（0x11000）** | **多绑了一页** ⇒ 该页无 backing ⇒ GPU 访问它必 `TRANSLATION_FAULT` ✓✓✓ **机制最简、与我们树内已知的 `0x11000` 完全对上** |
| R2 | BO 真为 `0x12000`，但**内容/可用区只到 `0x11000`** | 则末页是"BO 内但未被初始化/未被映射进 kctx"⇒ 落到 (a) 内核侧 |

⇒ **判别只需一个字段**：在 v126 的打印里**加上 `op->map.bo->size`（BO 的真实尺寸）** ⇒ `0x11000 + 绑定 0x12000` ⇒ **R1 坐实** ✓

---

## 1. 你第 1 点：补 `dladdr` 取 `site_off` 的**精确写法**（`pan_kmod_vm_bind()`，`pan_kmod.h:1081`）

**取"调用者"的返回地址**（这才是"这块 VA 是谁要的"）——在函数**最开头**取一次：
```c
static inline VkResult
pan_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                 struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   /* v127 (report 84): 记录"谁要了这块 VA"。返回地址在函数入口取一次即可
    * （它指向调用者，正是我们想识别的对象所有者）。 */
   void *const ra = __builtin_return_address(0);
   Dl_info di; const char *site_fn = "?"; uintptr_t site_off = 0;
   if (dladdr(ra, &di)) {
      site_off = (uintptr_t)ra - (uintptr_t)di.dli_fbase;      /* ← 文件偏移 */
      site_fn  = di.dli_sname ? di.dli_sname : "?";
   }
   …
   /* 你已有的逐 op 打印，把 site 与 BO 尺寸一起补上： */
   for (uint32_t i = 0; i < op_count; i++) {
      mesa_logi("v127 bind: ret=%d type=%d va=[0x%" PRIx64 ", 0x%" PRIx64
                ") size=0x%" PRIx64 " bo=%p bo_size=0x%" PRIx64
                " site=%s+0x%zx site_off=0x%zx",
                ret, (int)ops[i].type, ops[i].va.start, ops[i].va.start + ops[i].va.size,
                ops[i].va.size, (void *)ops[i].map.bo,
                ops[i].map.bo ? ops[i].map.bo->size : 0,             /* ← ★ R1 的判据 */
                site_fn, (size_t)(di.dli_saddr ? (uintptr_t)ra - (uintptr_t)di.dli_saddr : 0),
                (size_t)site_off);
   }
```
**要点**
- 需要 `#define _GNU_SOURCE` + `#include <dlfcn.h>`（`pan_kmod.h` 里可能需要补这两个；若该头被大量 C 文件包含，**建议把这段打印抽成一个 `static inline void pan_kmod_vm_bind_log(...)` 放在 `pan_kmod.h` 内**，避免到处引 `<dlfcn.h>`）
- **`site_off` 用"文件偏移"** ⇒ 服务端 `addr2line -e /data/dsh_downloads/probe_v127.so 0x<off>`（该 .so 带 `-g` ✓）⇒ **一次就能把 `0x5fffd8d000` 这块 72 KiB 对到具体 BO/调用点** ✓✓
- **`bo_size` 是 R1 的判据**，务必一起打 ✓
- 若 `dladdr` 返回失败（`dli_fbase == NULL`），退化为打裸 `ra`，服务端按 `maps` 基址换算 ✓

---

## 2. 你第 2 点：同一 VA 是否出现 **MAP → UNMAP → MAP**

**你日志里已有全部 22 条** ⇒ 一条命令/一眼即可判：
```bash
grep -nE "va=\[0x5fffd8d000|va=\[0x5fffd97000" <你的 v126 logcat 文件>
```
**三种结果与含义**：
| 结果 | 含义 |
|---|---|
| **只有一对 MAP（22 秒前）与一对 UNMAP（27 秒后，超时清理）** | **没有"重绑"** ⇒ **(b) 支（生命周期/顺序）在你这次的现场里不成立** ⇒ 火力转 **R1/(a)** ✓（这与"故障地址**在**已成功绑定的区间内"一致 ⇒ 问题在"**这段区间是否真的被映射/是否覆盖到 BO**"） |
| **同一个 `start` 出现两次 MAP（中间夹 UNMAP）** | **⇒ (b) 支坐实**：第二次 MAP 换了 backing（BO 不同）⇒ 第一次的持有者仍持旧地址 ⇒ 命中 ✓ |
| `start` 相同但 **size 不同** | ⇒ 尺寸/布局在同一 VA 上变过 ⇒ **(b) 支的变体** ✓ |

**⇒ 若要我判，请把 `0x5fffd8d000`/`0x5fffd97000` 这两个 start 的**全部行**贴我（含时间戳），我直接给结论** ✓

---

## 3. 你第 3 点：(a) 支（内核侧）要看的三个点

若 R1 不成立（`bo_size == 0x12000`）且 §2 显示"无重绑" ⇒ **(a) 支**：绑定**没有真正落到 kctx 页表**。我们树内可看/可试的：
1. **kbase 的 map 实现**：`pan_kmod_vm_bind()` 在 kbase 后端最终走哪个 ioctl（`KBASE_IOCTL_MEM_MMAP`/`SAME_VA mmap`/`MEM_ALIAS`），**它是否校验 `va.size` 与 BO 尺寸**、是否把整段计入 kctx 页表 ⇒ 就在你已插桩的那个咽喉的**下游分支**里 ✓
2. **页表粒度**：`0x12000`(18 页) 与 `0x11000`(17 页) 差一页 ⇒ 若内核按"页对齐后再 +1 页"或"末页用于元数据"处理 ⇒ 末页不可翻译 ⇒ **恰好命中 R1 的形态** ✓
3. **kbase uapi 注释**：`lib/kmod/kbase_csf_uapi.h` / `mali_kbase_*.h` 里关于 **SAME_VA / MEM_MMAP 的对齐与尺寸要求**（grep `align`、`page`、`size`、`granularity`）⇒ 看是否存在"**必须按 … 对齐/向上取整**"的约定 ✓

---

## 4. F1 的重新排序（你要的 re-rank）

**F1（`MEM_JIT_INIT` 的 `va_pages`）现在应当降到第三位** ✗：
- v126 已把故障钉在**一个我们自己的、成功的、紧邻故障的绑定**上 ⇒ **"内核 JIT 区对象被访问"这一支的解释力大幅下降**（故障地址在用户态绑定的区间内，不需要 JIT 区来解释）；
- **新顺序**：**① §1 的补丁（`site_off + bo_size`）⇒ 定 R1 还是 R2** → **② 若 R1 ⇒ 修"绑定尺寸/BO 尺寸不一致"（下面 §5 F2）** → **③ 若 R2 且无重绑 ⇒ (a) 内核侧（§3）** → **④ F1（JIT 参数）留作最后手段** ✓

---

## 5. 修复候选

| 候选 | 内容 | 风险 |
|---|---|---|
| **F2（★R1 命中的话就是它）** | **绑定尺寸必须等于 BO 尺寸**：在 `pan_kmod_vm_bind()` 入口加断言 `assert(op->type != MAP \|\| op->va.size == op->map.bo->size)`（或按内核要求向上取整成**同一粒度**，把"多出的页"显式当作 guard 而不是待访问区） | 低（断言先行，修法视 R1/R2 定） |
| **F3** | 若是"**末页留作元数据/未映射**"的内核约定 ⇒ 让**用户的可用区**也从 `va.size` 里扣掉那一页（即把"可用长度"与"绑定长度"分成两个数，并让描述符/几何的**写入范围**用"可用长度"） | 中 |
| **F4** | 把 `pan_kmod_vm_bind` 的 VA 绑定**全部纳入来源校验器**（你 v126 已经天然做到了这一步 ✓）⇒ 让"任何被访问的地址"都能在任何时刻对到"当时是否在一个已绑定区间内" | 低 |
| **F1（降级）** | `MEM_JIT_INIT` 参数单变量 —— 仅在 R1/R2/§3 都出局时才上 | 低 |

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**（按纪律只出提案）；**§0 的算术完全基于你给的 v126 数据**，`0x11000` 与 tiler desc+geometry 尺寸的对应来自我逐行读过的 `:3196-3202`（以及你 v120 表里同尺寸 BO）✓
2. **R1 vs R2 我无法从现有数据分出**（缺 `bo_size`）⇒ **§1 的补丁就是判据**；§1 的 `dladdr` 片段**未编译**，`pan_kmod.h` 是否适合引 `<dlfcn.h>` 我未核实（故建议抽成 `static inline` 打印函数）✓
3. §2 的三种结果**我给的是判读表**，而**你的 22 条日志我没看到全文** ⇒ 请贴那两个 start 的全部行，我立刻给结论 ✓
4. §3 的 (a) 支我**没有读 kbase 的 map 实现与 uapi 对齐约定**（预算用尽）⇒ 它只是"若 R1 不成立时"的方向 ✓
5. F1 的降级是**基于 v126 这条新证据**的判断（故障在用户态绑定区间内）⇒ 若 §1 显示 `bo_size == 0x12000` 且无重绑，请把它回到候选表（(a) 支）✓
