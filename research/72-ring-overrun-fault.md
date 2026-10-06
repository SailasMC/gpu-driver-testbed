# 72 — 钉死：故障地址 = **一个 64 KiB（ringbuf 尺寸）BO 末尾之后 + 分配器间隔 0x7000** ⇒ 嫌疑从"heap 链"转向"**CS 读过 ring 末尾**"

日期：2026-10-06 · 只读分析 · 配套：`71-h2-heap-context-va.md`、`70`、`13`
**v109 的 BO 表是本轮的决定性数据**（你已在 `panvk_priv_bo.c:78` 打印 VA 区间）

---

## 0. 先把算式摆出来（本次最重要的三行）

```
10 MiB BO : [0x5fff400000, 0x5fffe00000)         size=0xa00000  flags=0x0
64 KiB BO : [0x5fffe07000, 0x5fffe17000)         size=0x10000   flags=0x40
故障地址  :  0x5fffe1e000  (= 0x5fffe17000 + 0x7000)
```
**两次都出现同一个间隔 `0x7000`**：
- `0x5fffe07000 − 0x5fffe00000 = 0x7000`（10 MiB BO 结束 → 64 KiB BO 开始）
- `0x5fffe1e000 − 0x5fffe17000 = 0x7000`（64 KiB BO 结束 → **故障地址**）

⇒ **`0x7000` 是本分配器"每个分配预留的间隔"** ⇒ **故障地址正好是那个 64 KiB BO 之后的下一个分配槽位**（尚未分配 / 未映射）⇒ **GPU 越过一个 64 KiB 分配的末尾去读** ✓
⇒ 而且 `0x5FFE800000`（0xc2 那次）在 `0x5fff400000` **下方 12 MiB**，同样是**未映射区**⇒ **两次都是"越界读"**，只是越界方向/位置不同 ✓

---

## 1. 问题 1/2：这两个 BO 是什么（flag 表已定论）

`pan_kmod.h:100-139`（**已核**）：

| 值 | flag | 含义 |
|---|---|---|
| `0x01` | `EXECUTABLE` | 可执行（shader BO） |
| `0x04` | `NO_MMAP` | 不可 mmap |
| `0x20` | `GPU_UNCACHED` | — |
| **`0x40`** | **`WB_MMAP`** | **写回可 mmap（宿主映射、可缓存）** |
| `0x80` | `IO_COHERENT` | — |
| `0x100` | `CSF_EVENT` | CSF 事件 BO（**不是 0x40**） |

⇒ **Q2 答案**：`flags=0x40` 的 64 KiB BO = **`WB_MMAP`（宿主可写映射）**，**不是** `CSF_EVENT`（那是 `0x100`）；`flags=0x41` = **`EXECUTABLE|WB_MMAP`**（shader BO，且落在 `0x8000_0000_1000` = EXEC_VA 区 ✓ 与 `kbase_kmod.c:1258` 的 EXEC_VA 初始化吻合）；`flags=0x0` = 普通 GPU-only BO。
⇒ **64 KiB 正好等于 `KBASE_RINGBUF_SIZE (64 * 1024)`**（`csf/panvk_vX_gpu_queue.c:57`），且 ring 必须是**宿主可写**（我们在里面构建命令流）⇒ **该 64 KiB BO 极可能就是某个子队列的 ring buffer** ✓
⇒ **Q1 答案（重要纠正）**：那个 **10 MiB 的 `flags=0x0` BO 不可能是内核 tiler heap** —— heap context 与 initial chunks 都是内核经 `KBASE_IOCTL_CS_TILER_HEAP_INIT` 在 **CUSTOM_VA/JIT 区**创建的，**不会出现在 panvk 的 priv BO 表里**（v109 打印的是**我们自己的** priv BO）。⇒ 它是**我们的**某个 10 MiB 分配；**仅凭尺寸无法唯一确定** ⇒ 见 §4 的"用返回地址标识"（强烈建议）。
   *旁注*：v108/v109 的 `chunk_size` 已是 **2 MiB**（`panvk_physical_device.c` 的 v108 注释与 `device->csf.tiler.chunk_size = 2 * 1024 * 1024;` 我已核到），所以 10 MiB 既可能是 `2 MiB × 5` 也可能是 `1 MiB × 10` ⇒ **不能用尺寸反推，必须打标签**。

---

## 2. 问题 3：谁是"链"的所有者 + 末尾应当有什么

**§0 的算式把嫌疑从"heap chunk 链"移到了"ring 的末尾"**：

| 结构 | 所有者 | 末尾应当有什么 | 我们是否可能漏写 |
|---|---|---|---|
| **子队列 ring buffer（64 KiB，`WB_MMAP`）** | **我们**（`panvk_priv_bo_create`；命令流由宿主写入） | **换行（wrap）处必须是一段合法的"跳转到 buffer 起点"的填充指令**；内核/固件靠 `CS_INSERT/CS_EXTRACT` 的**模 `KBASE_RINGBUF_SIZE`** 语义在环内循环 | **这是我们现在唯一的"自己写字节"的末尾结构** ⇒ 见 §3 的代码嫌疑点 |
| tiler heap **chunk 链** | **内核**（`encode_chunk_ptr`/`link_chunk`；`first_chunk_va` 指向 chunk 头） | 末块需由内核放终止编码 | 我们**无法**漏写（不是我们的内存）⇒ **H1 的"我们漏写终止符"这一支不成立** |
| heap context / heap descriptor | 内核（context）/ 我们（`tiler_heap.desc`） | — | heap context 不在我们表里；`desc` 是我们的 BO，可 dump |

**⇒ 结论**：既然 faulting 地址是"**越出一个 64 KiB（ring 尺寸）BO 的末尾** + 分配器间隔"，**下一个该查的是 ring 的 wrap 逻辑**，而不是 heap 链。

---

## 3. ring 末尾的代码嫌疑点（可直接查）

`csf/panvk_vX_gpu_queue.c:1399-1408`（作业发射前的换行处理，**逐行已核**）：
```c
   uint32_t offset = subq->kbase.insert % KBASE_RINGBUF_SIZE;
   if (offset + kbase_ring_job_max_size() > KBASE_RINGBUF_SIZE) {
      /* 在 ring 末尾发一段填充，然后把 insert 推到 buffer 末尾 */
      … cs_emit(… KBASE_RINGBUF_SIZE - offset);
      subq->kbase.insert += KBASE_RINGBUF_SIZE - offset;
   }
```
（`:1319-1336` 是同一逻辑的"空间计算"版；`:255/428` 是读侧 `% KBASE_RINGBUF_SIZE` 的用法）
**嫌疑点**：**这段填充必须是固件能识别并跳过的合法指令序列**（否则 CS 会**继续取指/继续按 job 长度前进**⇒ 越过末尾 ⇒ 而"数据读"（LSU READ）正是固件在解析/执行紧随其后的内容时发生的）。⇒ 建议实验见 §4-(c)：**dump ring 末尾那 64 字节与前 64 字节**，人工检查填充是否完整、长度字段是否覆盖到 `KBASE_RINGBUF_SIZE`。

---

## 4. 下一步最小实验（按"5 分钟 + 判别力"排序；**我推荐先做 (a)**）

| # | 实验 | 改动 | 判据（决定性） |
|---|---|---|---|
| **(a) 改 ring 尺寸** | `csf/panvk_vX_gpu_queue.c:57`：`KBASE_RINGBUF_SIZE 64*1024 → 256*1024`（1 行） | 一次重编 | **故障地址是否随 ring 尺寸平移**：平移 `+0x30000`（或至少改变）⇒ **"CS 读过 ring 末尾"定案** ✓✓；**完全不变** ⇒ 与 ring 无关 ⇒ 转 (b)/(c) |
| **(b) 给每个 priv BO 打标签** | 在 v109 已有的打印里**加一个字段**：`__builtin_return_address(0)`（调用点 PC） | 1 行 | 服务端用 **`addr2line -e /root/final/libvulkan_panfrost_v109.so <pc>`**（该 .so 带 `-g`）⇒ **10 MiB BO 与 64 KiB BO 各是谁，一次运行永久确定**（解 Q1 的歧义）✓ |
| **(c) dump ring 末尾** | 在 `:1399-1408` 的填充之后，把 `ring[KBASE_RINGBUF_SIZE-64 .. )` 与 `ring[0..64)` 各 8 个 64 位字打出来（宿主可读，`WB_MMAP`） | ~6 行 | 填充是否合法完整；`insert` 是否恰在 buffer 末尾；**若填充缺失/长度不足，就是根因** |
| (d) dump 10 MiB BO 头尾 | 在 (b) 确定它是谁之后再做 | ~8 行 | 只有当 (a) 排除 ring 时才需要（避免盲 dump 10 MiB） |

**同时建议的一个"零成本交叉验证"**：把 **`subq->kbase.insert / extract / last_job_offset / last_last_job_offset`** 与**三张 ring 的 VA** 一起打到 timeout 快照里 ⇒ 若 `last_job_offset` 接近 `KBASE_RINGBUF_SIZE`（换行点）而故障地址 = `ring_va + KBASE_RINGBUF_SIZE + 0x7000` ⇒ **闭环** ✓

---

## 5. 问题 5：S3"地址不变"的矛盾已经解开（**我 70/71 号的结论要收窄**）

- v109 表里那个 64 KiB BO 的**尺寸与 `chunk_size` 无关**；`0x7000` 是**分配器间隔**，也与 `chunk_size` 无关 ⇒ **故障地址本来就不该随 `chunk_size` 变** ✓
- ⇒ **S3 的正确结论是**：**"故障地址不依赖 `chunk_size`"**（把 **chunk 链几何**从嫌疑里摘掉），**而不是**"H2/heap 整体出局"。
- ⇒ **对我 70/71 号的自我更正**：71 号把 H2 指向 `gpu_heap_va`（heap context）是**基于"故障地址是固定结构地址"的推断**；现在有了分配器几何（**故障地址 = 某个 64 KiB BO 末尾 + 0x7000**），**更简约的解释是"越出 ring 末尾的数据读"**，而"内核 heap context 未被映射"**反而不必要**（且它在 CUSTOM_VA 区、不在我们的 BO 表里，无法用这张表验证）。**⇒ 优先级：ring 越界读 > heap context 未映射。**

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§0 的算式**完全基于你给的 v109 表**（我未复核打印本身是否包含全部 BO——**若表被截断**，"故障地址不属于任何 BO"这一结论就需要重判，请确认打印是否完整）。
2. **10 MiB BO 与 64 KiB BO 的身份仍未确定**（§1 只给了尺寸/flag 层面的强提示：64 KiB = ringbuf 尺寸 + `WB_MMAP` 合理；10 MiB 不可能是内核 heap）⇒ **(b) 的返回地址打印是唯一无歧义的答案**。
3. "`0x7000` 是分配器固定间隔"是**由两个观测点归纳**的（同一局里出现两次），**不是**从代码读出的 ⇒ 若 (a) 显示地址不随 ring 尺寸变，应改查分配器（`panvk_priv_bo.c`）的间隔规则。
4. §3 的"填充必须合法"是**结构性推断**；我**没有逐字节核对** `:1399-1408` 发出的填充内容 ⇒ (c) 就是为它设计的。
5. 我**未读** `panvk_priv_bo.c` 的分配器实现（间隔 0x7000 的来源、VA 方向是向下还是向上增长）⇒ 若你需要，我下一轮可专查这一处并把 (a)/(b)/(c) 的插入点写成精确 diff。
