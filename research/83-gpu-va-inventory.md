# 83 — 别名出局后的重新排序：**所有会创建 GPU VA 的路径全表** + 两个"从未被观测"的类别 + 落点

日期：2026-10-06 · 只读分析 · 配套：`80-unlogged-bos.md`（E-A 的来源）、`81/82`（别名，已被 v125 否证）
**已接受 v125 的三条**：别名路径**确实被执行**（必然执行点奏效 ✓）· 别名 GPU VA = **`0x41000`（低）** ⇒ 与 `0x5ff…` 故障地址无关 ⇒ **"1 MiB 之差 = 别名尺寸"是巧合** ✗ · E-γ（nents 2→3）无效，故障仍随布局移动（`0x5fffd9e000` 新 + `0x5fffe1e000` 老）✓

---

## 1. ★ 全表：**所有会创建/决定 GPU VA 的路径**，以及"我们观测过没有"

| # | 路径 | 谁分配 VA | 现在能观测到吗 | 判据/落点 |
|---|---|---|---|---|
| 1 | **`panvk_priv_bo_create`（priv BO / 池 slab）** | `pan_kmod_vm_bind`（AUTO_VA 或指定） | ✅ **已全量观测**（你打了 alloc/free 时间线） | 故障不落在任何 priv BO 上 ✓ |
| 2 | **`pan_kmod_bo_alloc` + `pan_kmod_vm_bind`（AUTO_VA）** | **内核**（kbase 自行选 VA，回写到 `map_op.va.start`） | ❌ **从未观测** ★★ | **E-A 的落点见 §2** —— 这一类包括**描述符环的 BO 本体**（v125 日志里 `bo_va=0x5ffff1f000 size=0x80000` 就是它 ✓） |
| 3 | **`kbase_kmod_alias_create`（`KBASE_IOCTL_MEM_ALIAS` + `mmap`）** | 内核 `mmap`（SAME_VA） | ✅ **v125 已观测并否证**（VA=`0x41000`） | 出局 ✓ |
| 4 | **`panvk_as_alloc` / `dev->as.priv_heap`（panvk 自己的地址空间分配器）** | 驱动 | ✅ 走 kbase 时**基本不用**（kbase 路径明确用内核 AUTO_VA，注释：*"kbase assigns the BO VA itself"*） | 低优先（若要用，是 `0x5fff…` 以外的段） |
| 5 | **EXEC_VA 区（`KBASE_IOCTL_MEM_EXEC_INIT` + 可执行 BO）** | 内核（EXEC zone） | ✅ 在 priv-BO 表里可见（`0x800000001000`，flags `0x41`） | 与本故障（`0x5ff…`）不同段 ⇒ 出局 |
| 6 | **内核 CUSTOM_VA / JIT 区**：tiler heap **context** + **chunks**（`KBASE_IOCTL_CS_TILER_HEAP_INIT`） | **内核** | ❌ **用户态完全观测不到** ★★（无 BO 记录、无映射） | 见 §3 |
| 7 | swapchain / AHB 导入（external memory） | 驱动 | ✅ | 与本故障（draw-only、无 present）无关 |

**⇒ 结论：仍然"从未被观测"的只有两类**：
- **(A) `pan_kmod_bo_alloc` + AUTO_VA bind**（用户态可观测，只要你把打印放在 **bind 点**）★
- **(B) 内核 CUSTOM_VA/JIT 区对象（tiler heap context 与 chunks）**（**用户态不可观测**，只能靠间接实验）★

---

## 2. E-A 的正确落点（你要的 file:line 与唯一锚点）

**坑（你已发现）**：`pan_kmod_bo_alloc`（`lib/kmod/pan_kmod.c:134`）**只给尺寸**，VA 在 **bind 时**由内核赋值；而**环的 BO 走的是 `PAN_KMOD_VM_MAP_AUTO_VA`** ⇒ **不要在 `bo_alloc` 打，要在 `pan_kmod_vm_bind()` 里"AUTO_VA 回写"的那一处打** —— **它是所有这类 BO 的公共咽喉**（覆盖环 BO、以及任何未来的调用方）✓✓

**定位命令（唯一权威）**：
```bash
M=/root/zenithblue/work/mesa/src/panfrost/lib/kmod
grep -n "PAN_KMOD_VM_MAP_AUTO_VA" $M/pan_kmod.c $M/pan_kmod.h
grep -n "PAN_KMOD_VM_MAP_AUTO_VA" $M/kbase_kmod.c
```
**预期形状**（我对代码的读码印象，**请以 grep 结果为准**）：`pan_kmod_vm_bind()` 在提交 ioctl 后，把内核返回的 VA **回写**到 `ops[i].va.start`（或 `req.out.gpu_va`）——**在那一次回写之后**插：
```c
         /* v126 (report 83, E-A): 记录"内核分配的 GPU VA"。
          * 这一类 BO（pan_kmod_bo_alloc + AUTO_VA）不进 priv-BO 日志，
          * 是"未记录地址"的唯一用户态来源。 */
         if (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP &&
             ops[i].va.start != PAN_KMOD_VM_MAP_AUTO_VA) {
            mesa_logi("kmodva: [0x%" PRIx64 ", 0x%" PRIx64 ") size=0x%" PRIx64
                      " bo=%p site=0x%zx",
                      ops[i].va.start, ops[i].va.start + ops[i].va.size,
                      ops[i].va.size, (void *)ops[i].map.bo,
                      (size_t)((uintptr_t)__builtin_return_address(0) -
                               (uintptr_t)__builtin_extract_return_addr(
                                  __builtin_return_address(0))));
         }
```
（`site` 请用你 v115 已用对的 **`dladdr`** 写法：`(uintptr_t)ra - (uintptr_t)di.dli_fbase` ⇒ 服务端 `addr2line -e <该 .so> 0x<off>` ✓；我上面那行只是占位，**不要**照抄那个减法 ✓）
**判据（一次运行即可对位）**：
- 若某条 `kmodva` 记录的 **`va_end == 0x5fffe1e000`**（或 `va_start == 0x5fffd9e000`、或区间覆盖这两个地址）⇒ **"未记录 BO"这一类命中** ✓✓ ⇒ 接着查"谁算出了指向它边界/内部的地址"；
- 若**所有** `kmodva` 区间都远离两个故障地址 ⇒ **(A) 类出局** ⇒ 火力集中到 **(B) 内核 JIT 区**（§3）✓

**附加一条零成本交叉核对（用你已有的 v125 数据）**：v125 里环 BO 的 VA 是 **`0x5ffff1f000`（512 KiB）**，而两个故障是 `0x5fffd9e000` 与 `0x5fffe1e000` ⇒ 二者**分别位于 utrace 堆末尾（`0x5fffe00000`）的 −0x60000 与 +0x1E000** ⇒ **它们恰好"骑"在那块 10 MiB 堆的两侧** ⇒ 与"这一带住着未记录的 BO"完全一致 ✓（E-A 的打印会把这一带的真实住户列出来）

---

## 3. 类别 (B)：内核 CUSTOM_VA / JIT 区（**用户态不可观测**）—— 只能做**间接实验**

- 依据（我们自己的注释，`lib/kmod/kbase_kmod.c:1240-1246`）：*"`KBASE_IOCTL_MEM_JIT_INIT` … **carves the CUSTOM_VA zone out of the top of the SAME_VA zone** — and kernel-internal allocations such as **tiler heap contexts/chunks** come from that zone"* ✓
- **tiler heap context** 你已实测在 `0x6000000000`（v110）⇒ 远离故障 ✗；但 **chunks** 仍在同一 JIT 区，**没有任何用户态记录** ⇒ 仍可能是"故障地址那一带的住户"
- **⇒ 唯一的间接实验（1~3 行、5 分钟，且它是我 71 号就提过、至今没人跑过的）**：
```diff
--- a/src/panfrost/lib/kmod/kbase_kmod.c
+++ b (KBASE_IOCTL_MEM_JIT_INIT 那一段，:1248-1252)
    struct kbase_ioctl_mem_jit_init jit_init = {
-      .va_pages = 1ull << 25,
+      /* v126 (report 83): 单变量 —— 改变 CUSTOM_VA/JIT 区的位置/大小 */
+      .va_pages = 1ull << 20,
       .max_allocations = 255,
       .phys_pages = 1ull << 25,
    };
```
**判据**：**故障地址随 JIT 区参数改变/消失** ⇒ **(B) 命中**（即"内核 JIT 区里的对象被访问到了"）✓✓；**完全不变** ⇒ (B) 也出局 ⇒ 只剩 (A) 或"计算出来的地址"✓
（注：`va_pages` 改小会让 JIT 区更小 ⇒ 若 tiler heap 创建失败会打 `KBASE_IOCTL_CS_TILER_HEAP_INIT failed` ⇒ **那也是有用的读数** ✓）

---

## 4. 建议执行顺序（一次一个变量）

1. **E-A（bind 点打印，纯观测）** ⇒ 把"未记录 BO"这一类**全部列出来**，与两个故障地址对位 ✓✓（若命中，直接进下一步；若都不沾边，进 2）
2. **F1（`MEM_JIT_INIT` 参数单变量）** ⇒ 判类别 (B) ✓
3. 若 1、2 都出局 ⇒ 剩下的只有"**算出来的地址**"（你 80 号 §1 那张表里仅剩的两处：**描述符环游标越界** 与 **init stream 的地址/长度**）⇒ 用 79 号的 E3'/E-C 收口 ✓

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**（按纪律只出提案）；§1 的全表**基于我逐行读过的代码**（`kbase_kmod.c`、`pan_kmod.c/h`、`gpu_queue.c` 的环路径、`panvk_device_memory.c` 的 priv BO 路径），但**部分条目（4/5/7）是按命名与注释归类**，未逐行复核其"在 kbase 上是否真的走" ⇒ 如果你要我逐条钉死，请指定条目。
2. **§2 的落点形状是读码印象**：`pan_kmod_vm_bind()` 里 AUTO_VA 的**回写语句文本我没读到**（预算用尽）⇒ 给了 grep 命令与预期形状；**`site` 一栏请务必用你已用对的 `dladdr` 写法**（我占位那行不是可用代码）。
3. **§3 的 (B) 类"用户态不可观测"**这一判断来自我们自己的注释 + tiler heap 的分配方式 ⇒ 若内核把 chunks 也映射进 kctx，则它们**能被 GPU 访问**（这正是要害），但**用户态依然看不到它们的 VA** ⇒ 只能靠间接实验 ✓
4. `va_pages = 1<<25` 的含义（页数 vs 字节）我按注释与既有取值处理；**改小它是否会导致 `CS_TILER_HEAP_INIT` 失败**我用"会打印失败日志"兜住 ⇒ 请把日志一起看 ✓
5. 所有改动**未编译、未上机**。
