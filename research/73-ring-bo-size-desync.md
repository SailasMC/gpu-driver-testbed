# 73 — 收口：ring 的 **BO 尺寸与换行算术是两套常量**（v112 的新故障由此而来）；正确的 site 取法；越界读的机制与修复

日期：2026-10-06 · 只读分析 · 配套：`72-ring-overrun-fault.md`、`71`（H2 已被你 v110 否证 ✓）
**先说结论**：**你 v112 的新故障地址（`0x5fffd8e000`）很可能是"我的 (a) 实验设计不当"造成的**——只改 `KBASE_RINGBUF_SIZE` 会让**换行算术**与**BO 实际尺寸**脱钩。**原故障（v107/v109/v110/v111 的 `0x5fffe1e000`）仍需另找原因**，本轮把范围收敛到"换行填充的合法性"。

---

## 1. 硬事实：ring 的 **BO 尺寸是硬编码 `64 * 1024`**，而换行算术用 `KBASE_RINGBUF_SIZE`

| 位置 | 内容 |
|---|---|
| `csf/panvk_vX_gpu_queue.c:3114 / 3119 / 3124` | **`.ringbuf_size = 64 * 1024,`** —— **三个子队列各一处**，**字面量硬编码**，**不引用 `KBASE_RINGBUF_SIZE`** |
| `:57` | `#define KBASE_RINGBUF_SIZE (64 * 1024)` —— 只被**换行/取模/空间计算**使用：`:255`、`:428`、`:1319-1336`、**`:1399-1413`**、`:1906`、`:1981-1983` |
| `:3041` | 本文件**唯一**的 `panvk_priv_bo_create(dev, alloc_info.size, …)`（ring/相关 BO 的实际分配点） |

⇒ **Q2 答案**：**ring BO 的实际尺寸由 `:3114/3119/3124` 的硬编码 `64*1024` 决定**（经 `:3041` 落地），**与 `KBASE_RINGBUF_SIZE` 无关** ⇒ **v112 里 BO 仍是 `0x10000` 完全符合预期** ✓
⇒ **Q5 答案（对 v112 两个故障地址的解释）**：v112 只把 `:57` 改成 256 KiB，于是
```c
   if (offset + kbase_ring_job_max_size() > KBASE_RINGBUF_SIZE) {        /* 现在用 256 KiB 判断 */
      memset(ringbuf_cpu + offset, 0, KBASE_RINGBUF_SIZE - offset);      /* 最坏写 192 KiB！ */
      kbase_cache_clean_range(ringbuf_cpu + offset, KBASE_RINGBUF_SIZE - offset);
      subq->kbase.insert += KBASE_RINGBUF_SIZE - offset;
```
在 **BO 只有 64 KiB** 的情况下：**宿主侧 `memset` 越出 BO 末尾最多 ~192 KiB**（写到相邻分配里），并且 **`insert` 把固件的"消费指针"推到 BO 之外** ⇒ **固件会在 BO 之外继续取指/取数** ⇒ **新故障地址出现** ✓
⇒ **⇒ 我 72 号 §4-(a) 的实验设计有缺陷**：**改 define 必须与 BO 尺寸同时改**（两套常量必须一致）。**v112 的结果不能作为"原机制"的证据** —— 请把它记为"未同步常量导致的副作用"，**原故障要用 v109/v110/v111 的读数判断**。

---

## 2. 问题 1：`site` 的正确取法（你的 `-38520` 是算法错，不是取法错）

`__builtin_return_address(0)` 给的是**进程内的绝对 PC**（PIE 已重定位）。**减去"函数地址"是错的**：那个函数地址同样是重定位后的绝对地址，两者相减得到的偏移**与 `addr2line` 需要的"文件偏移"不是一回事**，而且若返回地址落在**调用者**里、而你减的是**被调函数**地址，差值为负完全正常（**你看到的 `-38520` 就是这个**）。
**两种正确取法（任选）**：
```c
/* 取法 A（最省事）：打裸 PC，服务端用 .so 自己算 —— 需要减去模块基址 */
void *ra = __builtin_return_address(0);
Dl_info di;
uintptr_t file_off = 0;
if (dladdr(ra, &di) && di.dli_fbase)
   file_off = (uintptr_t)ra - (uintptr_t)di.dli_fbase;
mesa_logi("v111 bo: … site_file_off=0x%zx", (size_t)file_off);

/* 取法 B：直接用 dladdr 打符号名（省掉 addr2line） */
mesa_logi("v111 bo: … site_fn=%s+0x%zx", di.dli_sname ? di.dli_sname : "?",
          di.dli_saddr ? (size_t)((uintptr_t)ra - (uintptr_t)di.dli_saddr) : 0);
```
- **取法 B 最快**（`dladdr` 能解析到我们 `.so` 里的符号，因为该 `.so` 带 `-g` 且未 strip；若 `dli_sname` 为 NULL，用 A 再 `addr2line -e /root/final/libvulkan_panfrost_v109.so 0x<file_off>`）✓
- 注意：**`dladdr` 需要 `#define _GNU_SOURCE` + `<dlfcn.h>`**（Mesa 内部通常已有）。
- **为什么同一 site 有 4 个 BO**：**:3114/3119/3124** 是**三个子队列**（VERTEX_TILER/FRAGMENT/COMPUTE）各一份 ⇒ **三个连续** ✓；那个**孤立的第 4 个**（同在故障位置附近、同 site）说明**同一段代码后来又跑了一次**——最可能是**第二个 `VkQueue`/第二次 queue 初始化**（或 `win`/compute 路径另建队列）。**要坐实只需在取法 A/B 的输出里再加一个"第几个 ring"的计数器**（一行静态变量），或直接把 `queue` 指针打出来 ✓

---

## 3. 问题 3：越界读的机制（逐步核对 + 唯一还剩的嫌疑）

**已核对（`csf/panvk_vX_gpu_queue.c:1399-1413`，逐行）**：
```c
   uint32_t offset = subq->kbase.insert % KBASE_RINGBUF_SIZE;
   /* If the entry would straddle the end of the ring, pad with NOPs
    * (zero-filled instructions) and restart at the beginning. */
   if (offset + kbase_ring_job_max_size() > KBASE_RINGBUF_SIZE) {
      memset(ringbuf_cpu + offset, 0, KBASE_RINGBUF_SIZE - offset);
      kbase_cache_clean_range(ringbuf_cpu + offset, KBASE_RINGBUF_SIZE - offset);
      subq->kbase.insert += KBASE_RINGBUF_SIZE - offset;
      offset = 0;
   }
```
| 检查项 | 结论 |
|---|---|
| 覆盖范围（配置一致时，define == BO == 64 KiB） | ✅ `memset` 覆盖 `[offset, 64 KiB)`，**恰好到 BO 末尾**，不越界 |
| `insert` 推进 | ✅ 推到 `KBASE_RINGBUF_SIZE` 的整数倍（即下一圈起点） |
| **填充内容** | ⚠️ **全零**。注释断言"zero-filled instructions"= NOP。**这正是现在唯一的机制嫌疑**：若 CSF 的**取指/解码对全零字不是合法 NOP**，固件会**执行填充区里的内容**（或越过末尾继续），从而**用它自己算出的操作数地址做一次 LSU 读**——**恰好落在 BO 末尾之后一个分配槽（`+0x7000`）** ✓✓ 与"**READ** + **CSF 的 LSU** + 地址恰在 BO 末尾之后"三条全部吻合 |

⇒ **要在 5 分钟内判定**：把填充区**前 16 字节**换成"已知合法的最小指令序列"（例如把填充改成 **`cs_end`/跳转**类指令，或**用一个真实的小 job 填满**），看故障是否消失/改变。这需要知道 CSF 的 NOP 编码——**我没在树内找到"NOP=0"的明文定义**（`genxml/v11.xml` 的 opcode 表我没逐个核对）⇒ **这是本轮唯一未闭合的点**，见 §6。

---

## 4. 问题 3 附：**(c) dump ring 末尾 64 字节** 的精确插入点

插在 `:1413`（`offset = 0;`）**之后**、`struct cs_buffer ring_buf = {…}` **之前**（此时 `offset` 已被重置为 0，所以要先保存原值）：
```c
   uint32_t wrapped_at = subq->kbase.insert % KBASE_RINGBUF_SIZE;   /* 取模后的原始位置 */
   … 填充与 insert 推进（原代码）…
   offset = 0;
   if (PANVK_DEBUG(KBASE_DIAG)) {
      const uint8_t *base = subq->kbase.ringbuf_cpu;
      const uint32_t end = KBASE_RINGBUF_SIZE;
      mesa_logi("kbase: ring wrap subq %u: wrapped_at %u, insert %" PRIu64
                " (mod %u), job_max %u, BO size 0x%zx",
                subqueue, wrapped_at, (uint64_t)subq->kbase.insert, end,
                kbase_ring_job_max_size(), (size_t)KBASE_RINGBUF_SIZE);
      for (uint32_t k = 0; k < 8; k++) {
         uint64_t lo, hi;
         memcpy(&lo, base + end - 64 + k * 8, 8);
         memcpy(&hi, base + k * 8, 8);
         mesa_logi("  wrap word %u: [end-64+%u] 0x%016" PRIx64 " | [%u] 0x%016" PRIx64,
                   k, k * 8, lo, k * 8, hi);
      }
   }
```
**判读**：填充区**应全零**（若你看到非零 ⇒ 别的东西写到那里了 ⇒ 就是**宿主侧越界写**的实证）；`wrapped_at` 与 `job_max` 应满足 `wrapped_at + job_max > KBASE_RINGBUF_SIZE`（否则不该走填充）；`insert % end == 0` ✓。
⚠️ **在 v112 那棵树上跑这段会读到 BO 之外**（因为 define 已与 BO 不同步）⇒ **请先回到 v109/v111 的配置（define = BO = 64 KiB）再插桩**。

---

## 5. 最小修复候选（按风险；**v107~v111 配置下**）

| 候选 | 内容 | 风险 | 说明 |
|---|---|---|---|
| **F-A（首选，可 5 分钟验证）** | **把"填充"从全零改成显式的合法终止序列**：若 CSF 有 NOP/`cs_end` 级别的安全填充，用 `cs_*` 生成器写进去；否则**让作业永不跨越末尾**——把 `kbase_ring_job_max_size()` 的空间预留**同时用于"环形对齐"**，使 `insert` 永远落在能放下整条 entry 的位置（即**在预留阶段就跳过尾部**，而不是把"跳过"交给固件解析零字节） | 低-中 | 直接针对 §3 的唯一嫌疑；**若故障消失 ⇒ 零填充不是合法 NOP，定案** |
| **F-B（一致性加固，零风险，建议同版一起上）** | **把两套常量合二为一**：`:3114/3119/3124` 的 `64 * 1024` 改为 `KBASE_RINGBUF_SIZE`，并加 `STATIC_ASSERT`/编译期断言把两者绑死 | **0** | **本次 v112 的副作用就是它们脱钩造成的**；这一条能防止同类实验再次自伤 ✓ |
| **F-C（防御）** | 在 `:3041` 的 `alloc_info.size` 处加一条 `mesa_logi`（实际 BO 尺寸），并在 `:3114` 附近断言 `BO size >= KBASE_RINGBUF_SIZE` | 0 | 让"两套常量不一致"在**启动时**就暴露 |

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1 的两套常量**是我逐行读出的**（`:57` vs `:3114/3119/3124`）——**这是本报告最硬的发现**，也直接解释了你 v112 的困惑。
2. **`KBASE_RINGBUF_SIZE` 的完整使用点**我只列到 `grep` 命中的那些（`:255/428/1319-1336/1399-1413/1906/1981-1983`）；**`:3041` 的 `alloc_info.size` 表达式我没读到**（它决定 BO 真实尺寸）⇒ **"BO 尺寸由 `:3114` 的 `ringbuf_size` 字段传递到 `:3041`"这条链路是我的推断**（两者数值一致、且都在同一初始化路径），**应由 F-C 的一条日志确认**。
3. **"全零 = NOP" 是否成立我没能证实**（`genxml/v11.xml` 的 opcode 表我未逐个核对）⇒ §3 的机制**仍是假设**，F-A 就是它的判别实验。
4. **第 4 个同 site BO 的出身未确定**（§2 给了"第二个队列/第二次初始化"的推断与一行计数器验证法）。
5. 我 72 号 §4-(a) 的实验设计缺陷**已在本报告更正**；**v112 的数据不应再用于判断原机制**（请以 v109/v110/v111 为准）。
6. 若 F-A 无效，则"越界读"的指向会回到**固件自身的消费指针**（`insert/extract` 与内核已知的 ring size 是否一致——**内核从 `:3114` 的 `ringbuf_size` 得知环大小**，若它 ≠ `KBASE_RINGBUF_SIZE`，固件的环绕点与我们的填充点就会错位 ⇒ **这与 F-A 是同一族的两个分支**，靠 F-B/F-C 一起排除）。
