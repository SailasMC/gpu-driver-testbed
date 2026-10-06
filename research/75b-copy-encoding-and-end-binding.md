# 75b — 修正框架下：`cmd_copy_data` 的逐行编码 + "终点绑定 vs 尺寸绑定"的判别 + 无开关（改法）

日期：2026-10-06 · 只读分析 · 配套：`75-capture-path-and-vanish-test.md`、`74`、`73b`
**已接受你的撤回**：`0xa00000→0x100000`（堆缩 9 MiB）而故障只移 1 MiB ⇒ **不是 1:1** ✓；256 MiB 仍崩 ⇒ **堆耗尽被排除** ✓

---

## 0. 但请先补一个**还没被这两个数据点排除**的可能（1 行日志即可判）

你的两个故障地址：`0x5fffe1e000`（10 MiB 堆）与 `0x5fffd1e000`（1 MiB 堆）。
**若第二次的堆基址也随之移动了**（BO 变小 ⇒ 分配器把它放到别处），那么"**故障 = 堆终点 + 常数**"**仍然成立**：
- 10 MiB 配置：`base 0x5fff400000 + 0xa00000 = 0x5fffe00000` ⇒ 故障 = 终点 + **0x1E000**
- 1 MiB 配置：若 base 变成 `0x5fffd00000` ⇒ 终点 = `0x5fffd100000` ⇒ 故障 `0x5fffd1e000` = 终点 + **0x1E000** ✓ **同一个常数**

⇒ **"缩了 9 MiB 只移 1 MiB"完全可以用"基址上移了 8 MiB + 终点只移 1 MiB"解释**，而这**不等于**"不绑定"。
**判据（零代码/一行日志）**：把两次配置的 **`copy_buf_heap_bo->addr.dev`（基址）**都打出来，算 `fault − (base + size)`：
- **两次都 = `0x1E000`** ⇒ **绑定在"堆终点"**（你的"不是 1:1"是**基址运动造成的表象**）⇒ §3 的 capture 目标算式就是靶心；
- **两次不同** ⇒ 才是真正的"按设备初始化布局漂移"⇒ 走你 §1 要求的"穷举对位"（我需要那张表的 `site_off/va_start/va_end/size/flags` 全部列，尤其**基址**）。

这不是要翻你的账，而是这**两个数不足以区分**，而区分成本只有一行日志 ✓

---

## 1. `cmd_copy_data` 逐行（`csf/panvk_vX_utrace.c:41-76`）—— **写侧是 STORE，读侧才是 LOAD**

```c
   const struct cs_index dst_addr_reg = cs_scratch_reg64(b, 0);   /* 0..1 */
   const struct cs_index src_addr_reg = cs_scratch_reg64(b, 2);   /* 2..3 */
   while (size) {
      cs_move64_to(b, dst_addr_reg, dst_addr);      /* ← 目标 = 堆 VA + dst_offset_B */
      cs_move64_to(b, src_addr_reg, src_addr);      /* ← 源   = src_offset_B（绝对地址） */
      uint32_t copy_count = MIN2(size, 1 << 16) / 4;
      uint32_t offset = 0;
      while (copy_count) {
         const uint32_t count = MIN2(copy_count, temp_count);
         const struct cs_index reg = cs_scratch_reg_tuple(b, 4, count);
         cs_load_to(b, reg, src_addr_reg, BITFIELD_MASK(count), offset);   /* ← **READ** 在这里 */
         cs_wait_slot(b, SB_ID(LS));
         cs_store  (b, reg, dst_addr_reg, BITFIELD_MASK(count), offset);   /* ← 写堆在这里（WRITE） */
         copy_count -= count; offset += count * 4;
      }
      dst_addr += offset; src_addr += offset; size -= offset;
   }
```
**⇒ 关键结论（直接回答你 Q2 的前半）**：
- **对堆的写是 `cs_store`（WRITE）**；**越出堆终点若发生在写侧，应报 WRITE** —— 而我们的故障是 **READ** ✗
- ⇒ **故障只能来自 `cs_load_to(...)`，它的基址是 `src_addr_reg = src_addr`（源地址）** ⇒ **要让 READ 落在"堆终点 + 0x1E000"，那个 `src_addr` 必须是堆相关的地址** ⇒ 只有两种可能：
  1. **有 capture 的"源"本身就在 utrace 堆里**（读回先前捕获的数据）⇒ 语义上可能（clone 链），但需要 caller 侧证据；
  2. **`src_addr` 被算错/被当成"堆基址 + 偏移"** ⇒ 见下 §2 的**契约违反**（这才是可 grep 的具体 bug 类）。

---

## 2. `capture_data` 的**契约**与最可能的违反（可 grep）

```c
   const uint64_t dst_addr = dst_buf->dev + dst_offset_B;   /* ✓ 堆内 */
   const uint64_t src_addr = src_offset_B;                  /* ← 注释：**absolute** */
   assert(!src_buffer || (uintptr_t)src_buffer == PANVK_UTRACE_CAPTURE_REGISTERS);
```
⇒ **本 fork 的契约是**：调用方必须传 `src_buffer = NULL`（或 `PANVK_UTRACE_CAPTURE_REGISTERS` 哨兵）且 `src_offset_B` 是**绝对 GPU 地址**。
⇒ **若任何调用方按上游 u_trace 的惯例传了"真实 buffer 指针 + buffer 内偏移"**：
- 该 `assert` 在 **release 构建（NDEBUG）下被编译掉** ⇒ 不会拦住；
- `src_addr` 会变成一个**很小的偏移值**（而非绝对地址）⇒ LOAD 打向低地址（**故障地址形态不符**）……
⇒ **但反过来**：若调用方传 `src_buffer = <某个 BO 指针>` 而**没有**用哨兵，同时又期望"框架把 src 当成 buffer 处理"——本 fork 不做这种转换，于是**语义静默错位**。
**⇒ 可 grep 的落点**：全树找 `u_trace_capture_data(` 的调用方（含 `inst->bo`、`PANVK_UTRACE_CAPTURE_REGISTERS` 用法），确认**每个调用方是否都遵守"NULL 或哨兵 + 绝对偏移"**——`csf/panvk_vX_instr.c:82/86/99/103/116/120/134/138` 已经出现 8 处 `PANVK_UTRACE_CAPTURE_REGISTERS`，**这些是合规的**；**不合规的那些**才是靶子。

---

## 3. 你要的第 3 点：**没有"关闭 utrace"的 env 开关**

我把相关选项都 grep 了：
- 只有 **`PANVK_UTRACE_CLONE_MEM_SIZE`**（堆大小，`panvk_utrace.c:177`）与 **`PANVK_UTRACE_CAPTURE_REGISTERS`**（哨兵常量，非开关）；
- 使能/处理由 **u_trace 框架**决定：`u_trace_should_process(&dev->utrace.utctx)`（`csf/panvk_vX_gpu_queue.c:3504`）、`u_trace_context_process(...)`（`:4157`）、`u_trace_init(...)`（`csf/panvk_vX_cmd_buffer.c:1018/1125`）⇒ **由 app/perfetto/trace-file 触发，不是驱动 env** ✗
⇒ **⇒ 想"一次运行判是不是 capture 路径"，最可靠的是 1 行补丁**（我 75 号 §3-E-d 已给）：
```c
void *
panvk_utrace_create_buffer(struct u_trace_context *utctx, uint64_t size_B)
{
   return NULL;                 /* ← 临时：让所有 clone 分配失败 ⇒ capture 全部跳过 */
   …
}
```
配套日志会打 `:91-94` / `:123-126` 的 "clone alloc failed"（确认路径被走到）⇒ **若故障消失 ⇒ 就是 capture 使用路径** ✓（**不需要**先搞清"谁开启的 u_trace"）

---

## 4. 修复候选（在你确认 §0 的"终点绑定"后按此顺序）

| 候选 | 内容 | 风险 |
|---|---|---|
| **F2（★零风险，先上）** | `panvk_utrace_capture_data()` 里加边界与契约检查：`assert(src_buffer == NULL \|\| (uintptr_t)src_buffer == PANVK_UTRACE_CAPTURE_REGISTERS);` **改为 release 也生效的** `if (src_buffer != NULL && … != REGISTERS) { 限流日志; return; }`；并加 `if (dst_offset_B + size_B > dst_buf->size) { 限流日志; return; }` | **0** |
| **F3** | `cmd_copy_data()` 里在发射前对 `size` 与 `src/dst` 的可达范围做一次断言（DEBUG）+ 把 `size` 钳到 clone 缓冲余量 | 低 |
| **F1** | 堆范围钉到 BO **真实映射尺寸**（`MIN2(bo->bo->size, real)`），并在 init 断言两者相等 | 低 |
| **F4（可观测性）** | 每次成功 clone 打一行 `buf->dev / size_B / heap free`；每次 capture 打一行 `dst/src/size`（限流）⇒ 以后任何越界都能立刻定位到**具体那条 capture** | 0 |

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1/§2 的代码**逐行核实**（`cmd_copy_data` `:41-76`、`capture_data` `:150-172`）——本报告最硬的部分。
2. **"故障是 READ ⇒ 来自 LOAD ⇒ src 必须是堆相关地址"这一推论**依赖"WRITE 会报 WRITE、READ 会报 READ"这一常识 + 我们已解码的 `ACCESS_TYPE=READ(0x2)` ✓；但**我仍未找到那条把 `src_addr` 变成堆地址的具体调用** ⇒ 需要 §2 末尾的 grep（**下一轮第一件事**）。
3. §0 的"终点绑定"**是可能而非结论**：它的判据（两次都算 `fault − (base+size)`）需要**基址**，而我手上只有尺寸与故障地址 ⇒ 请补一行日志或直接给我两次的 BO 区间。
4. §3 的"无 env 开关"是**我的 grep 结论**（`PANVK_UTRACE*` 只有那两个符号）；u_trace 框架自身是否还有其它 `MESA_*`/perfetto 开关我**未穷举**（`/root/zenithblue/work/mesa/src/util/u_trace*` 的 grep 无输出，可能路径不同）。
5. F1~F4 **均未编译、未上机**。
