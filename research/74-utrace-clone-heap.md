# 74 — 靶子定死：那 10 MiB BO = **utrace 的 clone/capture 堆**（`PANVK_UTRACE_CLONE_MEM_SIZE`，默认 0xa00000）

日期：2026-10-06 · 只读分析 · 配套：`73b-pool-slabs-audit.md`（审计表第 2/3 项的核对，正是本报告）、`72`、`73`
**你 v115 的 `addr2line` 把对象定死了**：`0xbe8e4c → panvk_v12_utrace_context_init` ⇒ **那 10 MiB BO 是 utrace 的堆** ✓

---

## 1. 尺寸链**已核实**（你要的第 1 点）

```c
/* csf/panvk_vX_utrace.c:176-178 */
static uint64_t
get_utrace_clone_mem_size(void)
{
   return debug_get_num_option("PANVK_UTRACE_CLONE_MEM_SIZE", 0xa00000);   /* 默认 10 MiB */
}

/* csf/panvk_vX_utrace.c:181-206  utrace_context_init() */
   u_trace_context_init(&dev->utrace.utctx, …);
   panvk_priv_bo_create(dev, get_utrace_clone_mem_size(), 0,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
                        &dev->utrace.copy_buf_heap_bo);                    /* ← 10 MiB BO */
   …
   util_vma_heap_init(&dev->utrace.copy_buf_heap,
                      dev->utrace.copy_buf_heap_bo->addr.dev,              /* 堆基址 */
                      dev->utrace.copy_buf_heap_bo->bo->size);             /* 堆大小 = BO size */
```
⇒ **对象 = `dev->utrace.copy_buf_heap_bo`**（10 MiB = `0xa00000`），它是 **`util_vma_heap` 的宿主**，供 **utrace 的 clone/capture** 从中切出 GPU VA。
⇒ 而故障地址 **正好 = 堆基址 + `0xa00000`（堆大小）+ `0x1E000`**：
```
heap base 0x5fff400000 + size 0xa00000 = 0x5fffe00000   ← 堆的末尾
                                   故障 = 0x5fffe1e000 = 末尾 + 0x1E000
```
⇒ **CS 访问到了"这个堆自己的范围之外"** ⇒ **不是"越出池 slab"，而是"越出 utrace 堆"** ✓（这也解释了为何 v115/116/117 三版**地址完全稳定**：该 BO 由 **device 级 init** 早期创建，基址不受后面那些池 slab 变动影响 ✓）

**而代码里已经有"堆不够用"的告警**（`panvk_utrace.c:40`）：
> `"Provide larger PANVK_UTRACE_CLONE_MEM_SIZE (current = 0x%llx)"`
⇒ **说明作者预期到会耗尽**。**§3 的第一件事就是看它的调用者在耗尽时怎么做**（跳过？还是用了被截断/溢出的地址？）——这是本轮最可能直接出根因的地方。

---

## 2. 与"只有 draw 才触发"是否自洽 —— **自洽，而且很强**

- utrace 的记录点是围绕 **draw/IDVS/fragment** 作业发的（`cs_trace_run_idvs` / `cs_trace_run_fragment`，见 `csf/panvk_vX_cmd_draw.c` 里的 `cs_trace_*` 调用）；
- **clear/copy/AHB/WSI 走的是 `vk_meta` 的 fragment-only fullscreen 路径**（13 号已定论：**从不消费 tiler heap、也没有这些 trace 点**）⇒ 不碰这个堆 ⇒ **不故障** ✓
- ⇒ "**draw-only + 地址稳定 + 访问类型 READ**"三条同时被解释：只有 draw 会触发 **capture**（capture 的语义正是"**把一段数据/时间戳读出来克隆到堆上**"——**读源 + 写堆**，所以出现 **READ** 完全合理）✓✓

---

## 3. 下一步：**先用零代码的两个 A/B（各 5 秒），再考虑重编**

| # | 实验 | 改动 | 判据 |
|---|---|---|---|
| **E-a（★首选，零代码！）** | 用**不同的 `PANVK_UTRACE_CLONE_MEM_SIZE`** 跑 tri：`=0x100000`（1 MiB）与 `=0x4000000`（64 MiB） | **0**（该 env 在 `utrace_context_init()` 时读，**不用重编**） | **故障地址是否随堆大小平移**（例如 64 MiB 时故障应到 = base+0x4000000+0x1E000，或直接消失）⇒ **平移/消失 ⇒ 对象 = utrace 堆，定案** ✓✓ |
| **E-b（零代码）** | `PANVK_DEBUG=0`（nodiag）跑 tri | 0 | 故障消失 ⇒ **tracing/diag 路径被牵入**（与本次诊断版相关；也提示"游戏里是否开 diag"会改变结果） |
| **E-c（若 E-a 确认，1~3 行）** | 在 `:190-204` 后加日志：`copy_buf_heap_bo->addr.dev`、`bo->bo->size`、`get_utrace_clone_mem_size()`、以及 **实际映射大小** | 3 行 | 核对 **`bo->bo->size` 是否 == BO 的真实映射尺寸**（若不等 ⇒ 堆会切出 BO 之外的 VA ⇒ 直接命中根因） |
| **E-d（真正修复候选）** | ① 把堆的范围**钳到实际映射尺寸**（`MIN2(bo->bo->size, real_size)`）；② 在每次 `util_vma_heap_alloc` 后**检查返回值与请求尺寸**（尤其 `panvk_utrace.c:40` 那条告警的调用者：**耗尽必须"跳过 capture"而不是"用残缺地址"**）；③ capture 的目标/长度**双重校验** `target + len <= heap_end` | 中 | 与 E-a/E-c 的读数一致时上机 |

**若 E-a 显示"地址不变"** ⇒ utrace 堆也被否证 ⇒ 那就回到"`0x5fffe1e000` 是某个**稳定地址**的消费者"这一族，下一步应查 **`util_vma_heap` 之外**还有什么按**固定 device 级基址 + 常量偏移**算地址的地方（我可以下一轮专查）。

---

## 4. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1 的尺寸链**逐行核实**（`:176-178` 与 `:181-206`），**这是本报告最硬的部分**。
2. **"故障 = 堆末尾 + 0x1E000"是算术对位**（用你给的 heap base/BO 区间与故障地址），**尚未证明 CS 访问的就是这个堆的内部/尾部**——E-a 就是它的判别实验。
3. **我未读** `panvk_utrace_capture_data`/`get_data`/`read_ts` 的实现（只 grep 到 `:40` 的告警文本与 `:164-166` 的 `PANVK_UTRACE_CAPTURE_REGISTERS` 分支）⇒ "耗尽时的行为"与"capture 的长度/目标校验"**都还没核对**；**这是下一轮我最该先读的两个函数**（它们决定 E-d 的具体写法）。
4. **`bo->bo->size` 是否等于实际映射尺寸未核对**（`panvk_priv_bo_create` 可能有对齐/取整 ⇒ 又是一处"双尺寸"风险）⇒ E-c 的一行日志即可定。
5. E-b 的"nodiag 是否关闭 tracing"我不确定（tracing 可能由 app/驱动另外开启）⇒ 它是**便宜的排除项**，不是结论。
6. 若 E-a/E-b 都无效，请把 **v115 的完整 BO 表**（含所有 site_off）发我，我可以把"稳定地址 `0x5fffe1e000`"与**每一个** device 级 BO 的区间做一次穷举对位（我目前只对到 utrace 这一个）。
