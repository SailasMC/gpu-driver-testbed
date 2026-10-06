# 47 — v74：延迟路径状态 dump + 原子目标 uncached 决定性读

**状态**：编译 ✅ exit 0 · 打包 ✅ · 切分分发 ✅ · 确定性对照 ✅ 逐位等于 v73
**未在本会话做**：真机运行 ✗（纪律：禁操作手机）。本文件所有"判读表"均为**发射侧/寄存器语义的推断**，
标注为「推断（未真机验证）」；只有确定性对照、字节数、sha256 是实测。

---

## 0. 一句话结论

v73 已排除"地址非法"与"属性不匹配"两个假设，并把嫌疑指向**延迟执行路径**（完成计数由
`cs_sync64_add(..., cs_defer(0, SB_ID(DEFERRED_SYNC)))` 延迟发射）。v74 做两件事，都是**纯只读**：

1. 把这条延迟路径上**本树真正有记账的字段**全部打出来（`kbase: DEFER …`），没有记账的地方**明确写 unknown**；
2. 在超时那一刻，对**超时子队列的原子目标本身（seqno cell +0）**做 **invalidate-then-read**，
   同时打印读到的值与期望值（`kbase: SEQNO cell+0 = N (expected target M) cpu-read-uncached`）——
   这一条是**决定性**的：它把"原子没落"与"原子落了但固件依赖/等待没满足"分开。

---

## 1. 改动清单（3 个 hunk，**纯新增 +234 行 / −0 行**）

`diff` 与 v73 源文件逐行比对结果：`removed lines (<): 0`，`added lines (>): 234`，hunks: `830a831,1056` / `972a1199,1200` / `1652a1881,1886`。
⇒ **没有任何一行被删改**，v71/v72/v73 的检查点、ATOMIC 打印、修复 A、C1/C2/B、超时常量、MobileGL 全部原样。

| # | 位置（v74 文件行号） | 内容 |
|---|---|---|
| 1 | `panvk_vX_gpu_queue.c:831–1056`（新增） | `kbase_log_defer_diag()`（:865）+ `kbase_log_seqno_uncached()`（:1003） |
| 2 | `panvk_vX_gpu_queue.c:1199`（新增调用） | 在 `kbase_log_subqueue_state()` 内、v73 ATOMIC 块之后、`kbase_log_callee_diag()` 之前调用 `kbase_log_defer_diag(dev, queue, subqueue, reason)` |
| 3 | `panvk_vX_gpu_queue.c:1881–1886`（新增调用） | 超时分支内、`kbase_log_queue_syncobjs()` / 快照循环之前调用 `kbase_log_seqno_uncached(queue, subqueue, target_seqno, "timeout snapshot")` |

三条打印**都不加 `PANVK_DEBUG(KBASE_DIAG)` 门**（与 v73 的 ATOMIC 块一致），且
`kbase_log_subqueue_state()` 在本树**只有超时路径一个调用点**（v74 `:1891`；v73 原调用点 `:1657`）
⇒ 这些打印**只在超时快照里出现**，正常运行完全静默。**无任何等待、无 CS 寄存器写、无提交。**

---

## 2. `kbase: DEFER` 四个字段：来源 file:line（全部实测自本树，非推测）

### 2.1 `DEFER slot state`（打印于 `:882`）

| 字段 | 来源 file:line | 说明 |
|---|---|---|
| `DEFERRED_SYNC` 槽号 = **1** | `panvk_cmd_buffer.h:369`（`PANVK_SB_DEFERRED_SYNC = 1`，enum `panvk_sb_ids` 起于 `:366`） | 驱动侧槽号定义 |
| 槽掩码 `0x0002` | `panvk_cmd_buffer.h:375` `#define SB_MASK(nm) BITFIELD_BIT(PANVK_SB_##nm)` | `BIT(1)` |
| `slot_count` | `panvk_device.h:129`（`dev->csf.sb.count`，`uint8_t`） | 设备初始化时由 `csif_info->scoreboard_slot_count` 填入 |
| `all_mask` | `panvk_device.h:130` `uint16_t all_mask`；填于 `panvk_vX_device.c:535–536` | `BITFIELD_MASK(slot_count)` |
| `all_iters_mask` | `panvk_device.h:131`；填于 `panvk_vX_device.c:539–547` | 迭代槽位掩码（bit3..），**与 DEFERRED_SYNC 无关**，打出来正是为了证明这一点 |
| wrapper 自己的 defer 描述符 | 发射点字面量 `panvk_vX_gpu_queue.c:1585`（v73 为 `:1357`）：`cs_defer(0, SB_ID(DEFERRED_SYNC))` → `wait_mask 0x0000, signal_slot 1`（`cs_defer()` 定义 `cs_builder.h:783`，结构 `cs_builder.h:775`） | 即任务书里的 :1357/:1358 |
| 固件实时槽位位图 | `KBASE_CS_STATUS_SCOREBOARDS`（本文件 `:495` = `0x005C`），寄存器名 `CS_STATUS_SCOREBOARDS`（`mali_kbase_csf_registers.h:120`），字段 `CS_STATUS_SCOREBOARDS_NONZERO_*`（`:758–762`） | 从 user_io page 2 读；先 `kbase_cache_invalidate_range(page+0x5C, 4)` |

输出里 `=> DEFERRED_SYNC slot pending-in-firmware bit = 0/1` 就是 `live_sb & BIT(1)`。

### 2.2 `DEFER list`（打印于 `:909`）—— **本树没有这个结构（已明确写出，非猜测）**

grep 结论（可复现）：
```
grep -rn "cs_defer"      src/panfrost/   # 定义 cs_builder.h:783；全部调用点均为传值实参
grep -rn "pending_defer|defer_list|async_list|num_async|deferred_ops|pending_async" src/panfrost/
  → 仅命中 pan_kmod 的 has_pending_deferred_syncs（和 CS scoreboard defer 无关）
```
- `cs_defer()` 返回**按值**的 `struct cs_async_op {wait_mask, signal_slot, indirect}`（`cs_builder.h:775–782`），
  由调用者直接交给发射函数（`cs_builder.h:1682 / 2263 / 2278 / 2322 / 2380 / 2389 / 2395 / 2401 / 2408` …）。
- builder 内部**没有**队列 / dynarray / 链表保存"待处理延迟操作"。
⇒ 所以打印的是 **`length 0 (unused)`**，并附上上述证据链。

### 2.3 `DEFER pending ops`（打印于 `:918`）—— **unknown（本树无此记账，明确写 unknown）**

- 本树**没有任何字段统计"挂起未完成的延迟操作数"**。
- 最接近的既有记账是 `pan_kmod_bo::has_pending_deferred_syncs`（`pan_kmod.h:194`，`bool`），
  但它属于 **panthor 的 dma-fence deferred-sync 路径**（赋值在 `pan_kmod.c:384`，消费在 `pan_kmod_backend.h:106`），
  **不是** CS scoreboard 的 `cs_defer()`。
- 仍然把它当作一个**真实可读字段**打印出来（seqnos BO 的那个 bool），但**明确标注它属于另一套机制**，
  并写 `unknown` 而不是编一个数。

### 2.4 `DEFER last submit`（打印于 `:953`）

| 字段 | 来源 file:line |
|---|---|
| 最后一次 submit 序号 | `subq->kbase.emitted_jobs`（`panvk_queue.h:90`；`panvk_vX_gpu_queue.c` 内 `emitted_jobs++` 于 v73 `:1397` / v74 `:1625`） |
| 该次 submit 的 target seqno | 同一计数器：发射时 `target_seqno = emitted_jobs + 1`，末尾才 `emitted_jobs++` ⇒ **最后一次 submit 的编号就等于它武装的 target seqno** |
| ring entry 大小 | `subq->kbase.last_job_entry_size`（`panvk_queue.h:95`）、`last_job_offset`（`panvk_queue.h:93`） |
| wrapper 自身的延迟发射条数 | **静态可判定**：恒为 `1`（完成 `SYNC_ADD64`，`:1585`）+ `last_stream_size ? 1 : 0`（`cs_flush_caches(... cs_defer(0, SB_ID(IMM_FLUSH)))`，v73 `:1263` / v74 `:1491`） |

**对被 CALL 的命令流内部的 `cs_defer()` 条数**：本树**没有计数器**。v74 改成
**对已发射的 ring entry 做只读解码**并**注明这是"测量"而不是"记账"**：

- 指令**固定 8 字节**，依据是树自己的解释器：`genxml/decode_csf.c:32` `uint64_t *ip`，
  `qctx->ip++`（`:1725 / :1743 / :1788`）；以及 `cs_alloc_ins()` 每条只分配 1 个 qword（`cs_builder.h:734–742`）。
- 三个计数值与各自规则（都写在打印串里）：
  1. `defer_mode == DEFER_INDIRECT`（**bit 52**）= 由 `cs_defer_indirect()` 发射的条数。
     依据：`v14_pack.h` 中**全部 10 个** async-capable 结构体的 `defer_mode` 都打包在 bit 52
     （`grep -c "defer_mode, 20, 20" v14_pack.h` = 10；`unpack(..., opaque[0], 52, 52)`），
     且 `cs_apply_async()`（`cs_builder.h:875–889`）只在 indirect 时置 `DEFER_INDIRECT`。
  2. `wait_mask != 0`（**bits 16..31**）——沿用本文件**已有的**抽取方式 `kbase_log_entry_bytes()`（v73 的 `:428`）：
     `wait_mask = (uint16_t)((q0 >> 16) & 0xffff)`；同法可见 `v14_pack.h` 的 `SYNC_ADD64` 打包 `wait_mask, 16, 31`。
  3. `async-class` = 用**树自己的** `cs_instr_is_asynchronous()`（`cs_builder.h:818–864`）判定的异步类指令数
     （`RUN_*` 恒异步；`SYNC_ADD*/SYNC_SET*/FINISH_FRAGMENT/STORE_STATE/TRACE_POINT/HEAP_OPERATION/SHARED_SB_INC`
     当且仅当 `wait_mask != 0`）。
- **注意**：`cs_defer(0, …)` 的 `wait_mask` 就是 0，所以**光靠 wait_mask 抓不到**完成用的那条
  `SYNC_ADD64`——这就是为什么 **wrapper 自身那条被单独静态计数**（2.4 表格最后一行）。
  这一点已写进打印串，避免误读。

---

## 3. 原子目标的 uncached 读：实现方式与理由

**位置**：`kbase_log_seqno_uncached()`，`panvk_vX_gpu_queue.c:1003`；调用点 `:1885`（超时分支）。

**读的地址**：`kbase_subqueue_seqno_cell(queue, subqueue)` + 0，即该子队列 seqno cell 的 **+0 字**。
- `kbase_subqueue_seqno_cell()` 定义于 `:237`（v73/v74 同号）：`queue->kbase_seqnos.cpu + subqueue * kbase_seqno_stride()`。
- `kbase_seqno_stride()` = `ALIGN_POT(sizeof(struct panvk_cs_sync64), 64)`；`struct panvk_cs_sync64`
  是 `{u64 seqno; u32 error; u32 pad;}`（`panvk_cmd_buffer.h:55–59`）= 16 字节 ⇒ **stride = 64**
  ⇒ 子队列 0/1/2 的 cell 落在 **+0 / +64 / +128**，与 v73 读数
  `ATOMIC target addr = 0x5ff8afb040 / 0x5ff8afb080（cell base +64/+128）` **完全吻合** ✓
- 这个地址就是 `cs_sync64_add()` 的原子目标：v73 注释已论证 `addr64 = seqno_addr` 且
  `cs_sync64_add()` 经 `cs_src64()` **不带偏移**（`cs_builder.h` CS_SYNC_OPS）。

**读法（两层，都打印）**：

1. **方法 B ＝ 上报值**（打印 `cpu-read-uncached`）：
   ```
   kbase_cache_invalidate_range((const void *)cell0, sizeof(uint64_t));   /* dc civac */
   __asm__ volatile("dsb sy" ::: "memory");
   uncached_read = *(volatile uint64_t *)cell0;
   __asm__ volatile("dsb sy" ::: "memory");
   ```
   `kbase_cache_invalidate_range()`（v73 与 v74 均在 `:205`；本轮改动全在 :831 之后）对本树**就是**这个封装：
   `#if defined(__aarch64__)` 下逐 64 字节线发 **`dc civac`**（clean+invalidate 到 point of coherency），
   末尾再 `kbase_gpu_wmb()`（`:176–185`，aarch64 上是 `dsb sy`）。
   本会话早前已在同一地址范围上用过它（超时路径每个快照都调）。
   额外两处显式 `dsb sy` 是为了让编译器与 CPU 都不能把 load 挪到 invalidate 之前或复用它。
2. **方法 A ＝ 对照**（打印 `plain volatile read taken before the invalidate`）：
   在**任何 cache 维护之前**先做一次裸 volatile 读。**若 A ≠ B，就是 CPU 缓存陈旧的直接证据**；
   若相等，则该读数不是缓存假象。（诚实备注：等待循环每轮已调过一次 invalidate（v73 `:1515`），
   所以 A 通常也会是新鲜值——A 的价值是**交叉验证**，不是"陈旧缓存探针"。）
3. 读后立刻再读一次（`re-read after`）确认稳定。

**理由**：cell 是普通 cacheable `MAP_SHARED` 映射，裸读可能命中 CPU 陈旧行；
`dc civac` 之后下一次 load **必须**回内存取，这才是"这个原子到底落没落"的确定性证据。
**它只读、不引入等待**（`dc civac` 是 cache 维护，不是对 GPU 的等待），不写任何 CS 寄存器。

**打印格式**（已按任务要求，含字面串 `kbase: SEQNO cell+0 = N (expected target M) cpu-read-uncached`）：
```
kbase: SEQNO cell+0 = <N> (expected target <M>) cpu-read-uncached [timeout snapshot subqueue <q>; method = dc civac via kbase_cache_invalidate_range() (panvk_vX_gpu_queue.c:205) + dsb sy + volatile ldr + dsb sy; plain volatile read taken before the invalidate = <A>; re-read after = <B>; cell host 0x… dev 0x…; atomic target == cell+0 by construction, see panvk_vX_gpu_queue.c:1357] => <判读>
```

---

## 4. 判读表

### 4.1 ★ `kbase: SEQNO`（决定性的那一行）

| `cell+0`（uncached） | 含义 | 下一步 |
|---|---|---|
| **== target**（例：339 == 339） | **原子已落盘**。本 job 的 deferred `SYNC_ADD64` **确实执行并退休**了。⇒ **问题不在原子本身，而在固件对它的依赖/等待**：CQS/notification 没送达、CS_EXTRACT/CS_ACTIVE 没推进、`kbase_kmod_csf_wait_cqs64` 看的地址/条件不对、或唤醒路径没走通。 | 转查"完成已写好但没人认账"：CQS 事件、EXTRACT/ACTIVE、ppoll 唤醒、等待侧比较的地址与 target |
| **== target − 1**（例：338 vs 339） | **原子没落**。正好欠这一个 ⇒ 本 job 的 deferred `SYNC_ADD64` **尚未执行**。⇒ 卡点在**延迟路径**：该 deferred op 没被取到 / 其 `wait_mask`（wrapper 是 `dev->csf.sb.all_mask` 全槽等待，v73 `:1313` / v74 `:1541`）始终没满足。 | 继续查延迟路径：看同一份快照里 `DEFER slot state` 的固件 bit1 与 `DEFER last submit` 的解码计数 |
| **> target** | 计数器**越过**本 job（后续 job 的 add 已落）⇒ 原子与计数器**都在工作**，卡的是本 job 的**等待/通知**条件。 | 查 notification / wait 条件（与"== target"同方向，但更强） |
| **< target − 1** | 欠了不止一个 ⇒ 延迟路径**已经停滞超过一个 submit**，不是偶发时序。 | 说明是持续性延迟路径故障；配合 `DEFER last submit` 的 `async-class` 计数看发射量 |
| **== 0 且 target > 0** | 该子队列**从未完成过任何 submit**（或 cell 被重置）⇒ 不是"最后一个原子卡住"，而是**整条链没起来**。 | 回到更早的检查点（ID 24 CMDBUF_DONE / 40 / 41）判断 CALL 是否返回过 |

判读表对应的自动 verdict 串（代码里）：`atomic DID land: …` / `atomic DID NOT land: …` /
`cell+0 is PAST the target …` / `cell+0 is more than one behind the target …`。

### 4.2 `kbase: DEFER` 各字段异常值的含义

| 字段 | 正常/基准 | 异常值 | 含义 | 强度 |
|---|---|---|---|---|
| `DEFER slot state` 的 `pending-in-firmware bit`（`CS_STATUS_SCOREBOARDS` bit1） | 挂起时**通常应为 1**：wrapper 在 `:1313`（v74 `:1541`）用 `all_mask` 等**全部**槽归零**之后**才发射完成 `SYNC_ADD64`，而它是**最后一条**、其 `signal_slot = 1` 之后**没有任何人消费** ⇒ 它一退休 bit1 就应置起。 | **bit1 = 0 且 SEQNO == target** | 自相矛盾：说明"计数落了但信号没落"，或 slot1 被别处消费/清零 ⇒ 重点转向 scoreboard/信号路径 | 推断（未真机验证） |
| 同上 | | **bit1 = 1 且 SEQNO == target−1** | 也自相矛盾（有 DEFERRED_SYNC 信号但没有计数推进）⇒ 需重新审视"谁在 signal slot1" | 推断 |
| 同上 | | **bit1 = 1 且 SEQNO == target** | 两个独立观测量互相印证 ⇒ 强烈支持"原子已落，卡在依赖/等待" | 推断 |
| `slot_count` / `all_mask` | G720 应为非 0 且 `slot_count > PANVK_SB_ITER_START(=3)`（`panvk_vX_device.c:538` 有 assert） | `all_mask` 的 bit1 = **clear** | 与 `SB_MASK(DEFERRED_SYNC)` 矛盾 ⇒ 槽位表配置错误，完成 op 的 `signal_slot=1` 无人等待/无意义 | 推断 |
| `all_iters_mask` 的 bit1 | **应为 clear** | bit1 = **set** | 驱动把 DEFERRED_SYNC 槽误当迭代槽 ⇒ 记账错位 | 推断 |
| `DEFER list` | 恒为 `length 0 (unused)` | 出现非 0 | **不可能**（本树无该结构）；若真出现说明树被改过 | 实测（grep） |
| `DEFER pending ops` | 恒为 `unknown` | — | 本树无记账。打印里附带的 `seqnos BO has_pending_deferred_syncs` 属于 panthor dma-fence 路径，**不能**用来推断 CS scoreboard 延迟条数 | 实测（grep） |
| `DEFER last submit` 的 `wrapper-own deferred ops` | `1`（无 stream）或 `2`（有 stream，多一条 FLUSH_CACHE2） | 其它值 | 与发射点不符 ⇒ 树被改过 | 实测（静态） |
| `DEFER last submit` 的 `defer_mode == DEFER_INDIRECT`（bit52） | 取决于命令流；本 wrapper 自身**不发射 indirect** | 明显偏大 | 命令流里 indirect 延迟操作多 ⇒ 它们都挂在 `SB_MASK_WAIT`/`SB_SEL_DEFERRED`（`cs_builder.h:1835` 一带）上，是"全槽等待"之外的另一类依赖 | 实测（解码） |
| 同上 `wait_mask != 0` | 同上 | 明显偏大 | 等待中被 scoreboard 挡住的指令多 ⇒ 若 SEQNO == target−1，说明**这些等待里有槽一直没归零** | 实测（解码） |
| 同上 `async-class` | 同上 | — | 用树自己 `cs_instr_is_asynchronous()` 的口径给出"异步类指令"总量；**不是** `cs_defer()` 计数（原因见 §2.4 注意） | 实测（解码，口径已注明） |
| `method` 里的 `plain volatile read taken before the invalidate` | 应与上报值**相等** | **不相等** | CPU 缓存陈旧被当场抓到：这是一条**独立的**内存一致性线索（v73 已证 bit10 COHERENT_SYSTEM=set，但那不等于 CPU 侧不回读到旧行） | 实测（读数比较） |

---

## 5. 确定性对照（实测）

```
基准  v73 驱动 : 1f7697c08b0246ae4af81fd57f16c6e8a2ab14f0c0c255f63e8e54ac49cdb610   20047704 B
```

| 步骤 | 动作 | 结果 |
|---|---|---|
| 0 | 构建前 | `build/.../libvulkan_panfrost.so` 已 == v73 基准 ✅（起点干净） |
| 1 | 打 v74 补丁 → 编译 | exit 0；`.so` = `1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56` 20055024 B |
| 2 | **撤掉本轮改动**（`cp` 回 `.bak-v74-1791211949`）→ 重编 | **`1f7697c08b0246ae4af81fd57f16c6e8a2ab14f0c0c255f63e8e54ac49cdb610` 20047704 B ✅ 逐位等于 v73** |
| 3 | 贴回 v74 → 重编（build A） | `1df10436…4a56` 20055024 B |
| 4 | v74 二次重编（build B，`touch` 源文件后重编） | `1df10436…4a56` 20055024 B ✅ **可复现** |

- 编译命令：`cd /root/zenithblue/build/android-v4 && export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH && ninja src/panfrost/vulkan/libvulkan_panfrost.so`
- 编译 **exit 0**；**唯一**警告是既有的 `:1786:7 warning: label followed by a declaration is a C23 extension`（`kbase_wait_continue:` 标签，v73 就有，非本轮引入）；**本轮新增代码零警告**。
- 该 .so 同时编入 `libpanvk_v10/v11/v12/v13/v14` 五个 arch 变体，按 `pan_arch(gpu_id)`（`src/panfrost/model/pan_model.h:101`）在运行时选；G720 属第 5 代（`pan_model.c:111` FIFTHGEN 组，树内有 `v14.xml`），**预期走 v14**（此项为**推断**，本会话未从真机回读 gpu_id）。
- 新符号已确认进入二进制：`strings` 命中 5 个新格式串，含
  `kbase: SEQNO cell+0 = %lu (expected target %lu) cpu-read-uncached` ✅
- 备注：v74 的 `.c` 内注释对**发射点**行号按 v73 实际行号写为 `:1357`（完成 `SYNC_ADD64`）与 `:1263`（FLUSH_CACHE2 defer），与本文引用一致。

---

## 6. APK 与分发（实测）

| 项 | 值 |
|---|---|
| 打包脚本 | `/root/pack_v74.sh` — 由 `/root/pack_v73.sh` 派生；`diff` 仅 3 行不同：`W=/root/v74`、`OUT=/root/final/mgl-panvk-v74.apk`、sed 行（输入 manifest `/root/v73/AndroidManifest.xml`，`versionCode 73→74`，`6.13-atomic-diag→6.14-defer-diag`） |
| **env 串一致性** | `pojavEnv` / `boatEnv` 的 sha256 在 v73 与 v74 **完全相同**（`7376481214b1b9f6…` / `846a5bb9d558af04…`）✅；`aapt2 dump xmltree` 的 manifest diff **只有** versionCode / versionName 两行 |
| APK | `/root/final/mgl-panvk-v74.apk`，10215983 B，sha256 `74b173cea11587af7e8d0aa38cb8830db6733779233e7f48e16ed2b99f4b0217`（前缀 24 = `74b173cea11587af7e8d0aa3`） |
| APK 内驱动 | `unzip -p … lib/arm64-v8a/libvulkan_freedreno.so \| sha256sum` = `1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56` **== 新 .so** ✅ 且非空（20055024 B）✅ |
| badging | `versionCode='74' versionName='6.14-defer-diag'` ✅ minSdk 26 / targetSdk 34（与 v73 同） |
| v71/v72/v73 未被改 | `918928cc989bebde…`(v71) / `37169cc4b1042385…`(v72) / `e5b3fb869db4a176…`(v73) — 与打包前一致 ✅ |
| 切分分发 | `cp -f` 到 `/data/dsh_downloads/`，`split -n 8 -d mgl-panvk-v74.apk v74p8_` ⇒ `v74p8_00…v74p8_07`（各 1276998 B，末片 1276997 B）；`cat v74p8_0* \| sha256sum` = `74b173cea11587af…` **== 源 APK** ✅ 合计 10215983 B ✅ |

---

## 7. 回滚命令

**A. 把源码退回 v73（保留可复现的 v73 驱动）**
```bash
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf
cp -f panvk_vX_gpu_queue.c.bak-v74-1791211949 panvk_vX_gpu_queue.c   # v74 前备份
cd /root/zenithblue/build/android-v4
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
ninja src/panfrost/vulkan/libvulkan_panfrost.so
sha256sum src/panfrost/vulkan/libvulkan_panfrost.so
# 期望（已实测）：1f7697c08b0246ae4af81fd57f16c6e8a2ab14f0c0c255f63e8e54ac49cdb610  (20047704 B)
```

**B. 重新拿回 v74**
```bash
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf
cp -f panvk_vX_gpu_queue.c.v74-patched panvk_vX_gpu_queue.c
cd /root/zenithblue/build/android-v4
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
ninja src/panfrost/vulkan/libvulkan_panfrost.so
sha256sum src/panfrost/vulkan/libvulkan_panfrost.so
# 期望（已实测 ×2）：1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56  (20055024 B)
```

**C. 重新打包**
```bash
bash /root/pack_v74.sh
# => /root/final/mgl-panvk-v74.apk  sha256 74b173cea11587af7e8d0aa38cb8830db6733779233e7f48e16ed2b99f4b0217
```

保留的工件：
- 源码备份（v74 前）：`src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c.bak-v74-1791211949`
- 源码（v74）：`src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c.v74-patched`
- 驱动：`/root/final/libvulkan_panfrost_v74.so`
- 打包脚本：`/root/pack_v74.sh`

---

## 8. 本轮**没有**改动的东西（显式确认）

- v71/v72/v73 全部检查点与 `kbase: ATOMIC *` 打印：`diff` **0 行删除**，原样保留。
- 修复 A、C1/C2/B：未触碰。
- 超时常量（`KBASE_WAIT_TIMEOUT_NS` 等）：未触碰。
- 任何行为逻辑：3 个 hunk **全是新增**；新增代码只有 `mesa_loge()`、volatile 读、
  `kbase_cache_invalidate_range()`（既有 helper，同一路径已在用）与两处 `dsb sy`。
  **不写任何 GPU 可见内存、不写 CS 寄存器、不提交、不等待。**
- MobileGL（`libMobileGL.so`）：打包时按原样从 `/root/v54` 复制，未改。

## 9. 未验证 / 需要真机才能确认的

1. 所有 §4 判读表的**因果解释**均为发射侧语义推断，**未在真机验证**。
2. `PAN_ARCH == 14`（G720）为**推断**（`pan_model.h:101` + `pan_model.c:111` 第 5 代分组 + 树内 `v14.xml`），
   本会话未从真机回读 `gpu_id`。
3. `CS_STATUS_SCOREBOARDS` 的"非零即 pending"语义取自寄存器字段名
   `CS_STATUS_SCOREBOARDS_NONZERO_*`（`mali_kbase_csf_registers.h:758–762`），
   未与 ARM 硬件手册逐条比对。
4. ring entry 解码假定"指令固定 8 字节且 ring entry 是线性流"——依据是树自己的解释器
   (`decode_csf.c:32`) 与 `cs_alloc_ins()`（`cs_builder.h:734`）；对**本 wrapper 的定长 ring entry**
   成立（同文件 v73 `:1197` / v74 `:1425` 的注释亦声明定长 entry 不触发 chunk linking），但**未在真机 dump 上逐条校验**。
5. 未做真机运行；因此 `DEFER` / `SEQNO` 的实际数值与判读**尚未取得**。
