# 75c — 用你给的精确数据解两个疑问：**那两行 BO 不可能同属一个进程**（区间重叠）+ 目标算式 + 真·零重编实验

日期：2026-10-06 · 只读分析 · 配套：`75-capture-path-and-vanish-test.md`、`75b-copy-encoding-and-end-binding.md`

---

## 1. 疑问 1 的答案：**那两行 BO 不可能同属一个 device/进程**（有硬证据）

你给的两行：
```
#1 va=[0x5fefe00000, 0x5fffe00000) size=0x10000000   ← 256 MiB
#2 va=[0x5fffd1f000, 0x5fffe1f000) size=0x00100000   ← 1 MiB
```
**#1 覆盖到 `0x5fffe00000`，#2 从 `0x5fffd1f000` 开始** ⇒ **两者的 VA 区间重叠** `[0x5fffd1f000, 0x5fffe00000)`（≈900 KiB）✗
⇒ **同一个 GPU VA 空间里两个 BO 不可能重叠** ⇒ **这两行不可能来自同一个 device**（同一进程里 `PANVK_UTRACE_CLONE_MEM_SIZE` 也不可能在一次运行中变两个值：`get_utrace_clone_mem_size()` 每次调用都 `debug_get_num_option`，而 `os_get_option` 缓存的是**同一个 env 字符串**）✓
⇒ **⇒ 结论：这份日志是两次运行（或两个进程/两个 device）的行混在一起**；**你的 env A/B 本身是可靠的**，但**"故障 A"和"故障 B"必须各自只与"自己那一局的堆 BO"对位** ✓
**（请补一行即可永久消灭这个歧义）**：在 v115 的 BO 打印里加 **PID + device 指针 + 本次读到的 `PANVK_UTRACE_CLONE_MEM_SIZE`**：
```c
   mesa_logi("v115 bo: pid %d dev %p clone_size 0x%llx va=[0x%" PRIx64 ", 0x%" PRIx64
             ") size=0x%" PRIx64 " flags=0x%x site_off=0x%zx",
             getpid(), (void *)dev, (unsigned long long)get_utrace_clone_mem_size(),
             va_start, va_end, size, flags, site_off);
```
**判据**：两行若 PID/dev 相同 ⇒ **那就是同一进程里两个 heap BO**（⇒ `utrace_context_init` 被调用两次、或 device 创建走了重试路径 ⇒ 那是**另一个独立 bug**，也可能就是根因）；若不同 ⇒ **日志混行**，按 §2 各自对位。

---

## 2. 疑问 2 的答案：两个故障都**紧贴各自堆 BO 的边界**，形态是"±1 页"

按 §1 拆开看（各自只与本局的堆对位）：

| 局 | 堆 BO 区间 | 故障 | 相对位置 |
|---|---|---|---|
| 1 MiB | `[0x5fffd1f000, 0x5fffe1f000)` | `0x5fffd1e000` | **起点之前恰好 0x1000（一页）** |
| （另一局） | 256 MiB 结束于 `0x5fffe00000`；1 MiB 结束于 `0x5fffe1f000` | `0x5fffe1e000` | = **1 MiB BO 终点之前 0x1000（最后一页）**；同时 = 256 MiB 终点之后 0x1E000 |

⇒ **共同点：都只差一页**（一页在堆**前**、一页在堆**末页的位置**）⇒ 这是**"边界页/守卫页"级**的形态，而不是"堆内大幅溢出" ✓
**它意味着什么（结合我 75b 读到的编码）**：
1. `util_vma_heap_alloc` **不可能**返回 `base − 0x1000` 或 `base + size`（它的返回值必在 `[base, base+size)` 内，或 0）⇒ **所以 `dst_buf->dev` 不是"跑到堆外"的那个地址** ⇒ **写侧目标不背这个锅**；
2. 而 `cmd_copy_data` 的**写是 `cs_store`（WRITE）**、**读是 `cs_load_to`（READ）** ⇒ 我们的故障解码是 **READ** ⇒ **故障来自 `cs_load_to`，它的基址是 `src_addr_reg = src_addr`（源地址）**；
⇒ **⇒ 因此：那个"堆边界 ±1 页"的地址只能是**：
   - **(S1)** 某条 capture 的**源本身就在堆里**（把先前 clone 出来的数据再读回去）——**语义上完全可能**，而且 `dst_buf->dev + dst_offset_B` 也可能正好落在堆的末页；
   - **(S2)** 调用方违反了本 fork 的 src 契约（`src_buffer == NULL` 或 `PANVK_UTRACE_CAPTURE_REGISTERS` 哨兵 + **绝对** `src_offset_B`）⇒ 于是 `src_offset_B` 被当成绝对地址用 ⇒ 若它其实是个**负偏移/`size−0x1000`** 之类的值，就会精确落到"堆边界 ∓1 页" ✓✓
   **（S2 更符合"恰好一页"的规律，而 `assert` 在 release 下被编译掉 ⇒ 拦不住）**
   ⇒ **下一轮第一件（grep，一处即可定性）**：全树找 `u_trace_capture_data(` 的**调用方**，逐个核对传参是否满足上述契约（`csf/panvk_vX_instr.c:82/86/99/103/116/120/134/138` 的 8 处哨兵是合规的；**不合规的那些**就是 S2 的实例）。

---

## 3. 你要的"确认 GPU 被编程访问的目标地址算式"（已逐行核实）

- `panvk_utrace_capture_data()`（`csf/panvk_vX_utrace.c:150-172`）：**`dst_addr = dst_buf->dev + dst_offset_B`**（`dst_buf->dev` 来自 `util_vma_heap_alloc`，必在堆内），**`src_addr = src_offset_B`（绝对地址）**，长度 `size_B`；
- `cmd_copy_data()`（`:41-76`）：**`cs_load_to(src_addr_reg, …)` 读源 → `cs_store(dst_addr_reg, …)` 写目标**，源/目标各用 scratch 0..1 / 2..3，分块 ≤64 KiB；
- **⇒ 目标地址算式 = "堆基址 + 堆内偏移"（写，STORE）；源地址算式 = "调用方给的绝对地址"（读，LOAD）** ⇒ **READ 故障 ⇒ 查源**（§2 的 S1/S2）。
- **边界检查**：`capture_data` 里**没有** `dst_offset_B + size_B <= dst_buf->size`，`cmd_copy_data` 里**也没有**任何范围校验 ⇒ 两者都该补（§4-F2/F3）。

---

## 4. 修复候选

| 候选 | 内容 | 风险 |
|---|---|---|
| **F2（★零风险，先上）** | `capture_data()`：① 把 `assert(!src_buffer \|\| …REGISTERS)` 改成 **release 也生效**的 `if (…) { 限流日志; return; }`；② 加 `if (dst_offset_B + size_B > dst_buf->size) { 限流日志; return; }` | 0 |
| **F3** | `cmd_copy_data()`：发射前断言/钳制 `dst/size` 落在该 clone 缓冲内（把 buffer 尺寸传进来或加参数） | 低 |
| **F1** | 堆范围钉到 BO **真实映射尺寸**（`MIN2(bo->bo->size, real)`）+ init 断言 | 低 |
| **F5（新，针对 §1）** | `utrace_context_init()` 里加一条**不重叠断言**：本 BO 区间不得与任何已登记的 device 级 BO 区间相交（并把 PID/dev/尺寸打进一行日志）⇒ 本次那个"重叠"若真发生在同进程，会被立刻抓住 | 0（日志+断言） |
| **F4** | 每次成功 clone / 每条 capture 各打一行（`buf->dev/size_B`、`dst/src/size`，限流）⇒ 以后越界能定位到**具体那条 capture** | 0 |

---

## 5. 真·零重编的判别实验（考虑"env 只读一次"）

**先澄清**：`get_utrace_clone_mem_size()` **不是** `DEBUG_GET_ONCE_*`，它**每次调用都读** —— 但同一进程里 env 不会变 ⇒ **"读一次"与"每次都读"在本实验里等价** ⇒ §1 的重叠**只能由"两次运行/两个进程"解释**，所以你的 env A/B **是可靠的** ✓

**⇒ 真正能"让 capture 全部失败 ⇒ 看故障是否消失"的 env 做法（零重编）**：把堆调到**比任何一个 clone 请求都小**：
```
PANVK_UTRACE_CLONE_MEM_SIZE=0x40      # 或 0x1000
```
预期：`util_vma_heap_alloc` 返回 0 ⇒ 打 `"Couldn't allocate utrace buffer … Provide larger PANVK_UTRACE_CLONE_MEM_SIZE"`（`panvk_utrace.c:40`）⇒ **capture 全部跳过** ⇒
- **故障消失** ⇒ **根因确认在 capture 使用路径** ✓✓（这就是你要的"消失"而不是"移动"）
- **仍崩** ⇒ 与 capture 无关 ⇒ 回到"堆边界那页是被谁访问的"（此时 §2 的 S1/S2 之外还要查别处）

**若要坚持"完全不依赖 env"** ⇒ 1 行补丁（我 75 号 §3-E-d）：`panvk_utrace_create_buffer()` 开头 `return NULL;`（并靠 `:91-94/:123-126` 的 "clone alloc failed" 日志确认路径被走到）✓

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§3 的算式**逐行核实**（`:41-76` 与 `:150-172`）。
2. §1 的"重叠 ⇒ 不可能同进程"是**硬推理**（同一 GPU VA 空间的两个 BO 不能重叠）；但**"两次运行/两个进程"是推论**——原始 logcat 的 PID/时间戳我没有，**请按 §1 的一行日志确认**。
3. §2 的 S1/S2 是**两个候选**，我**未定位到具体那条 capture/caller**（grep `u_trace_capture_data(` 的调用方是下一轮第一件事）；"恰好一页"的规律支持 S2，但**未证实**。
4. `util_vma_heap_alloc` 的返回值范围（必在 `[base, base+size)` 或 0）来自我对 Mesa `util_vma_heap` 的既有认知，**本轮未逐行复核**该实现 ⇒ 若它其实会返回 `base−page` 之类的哨兵，S1/S2 的取舍要改（**你质疑这一点是对的，值得复核**）。
5. 所有修复候选**未编译、未上机**。
