# 76b — tiler heap descriptor + geometry 链逐行核对：**打包一致（不是 bug）**，但发现一处**硬编码的 64 KiB 与 init stream 的潜在越界**

日期：2026-10-06 · 只读分析 · 配套：`76-bo-pool-not-mempool.md`（设备级 mempool 已被数据推翻 ✓ 你的修正我接受）

---

## 1. 逐行核对（你第 ① 点）—— 打包**两半一致**，`| 16` 是**设计如此**

**分配**（`csf/panvk_vX_gpu_queue.c:3196-3218`，`init_tiler()`）：
```c
   /* We allocate the tiler heap descriptor and geometry buffer in one go,
    * so we can pass it through a single 64-bit register to the VERTEX_TILER
    * command streams. */
   struct panvk_pool_alloc_info alloc_info = { .size = (64 * 1024) + 4096, .alignment = 4096 };
   tiler_heap->desc    = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);   /* :3202 */
   …
   tiler_heap->oom_fbd = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);   /* :3218 同尺寸！ */
```
**打包**（`cs_ctx` 初始化，`:2832-2838`）：
```c
   cs_ctx->render.tiler_heap = panvk_priv_mem_dev_addr(queue->tiler_heap.desc);
   /* Our geometry buffer comes 4k after the tiler heap, and we encode the
    * size in the lower 12 bits so the address can be copied directly
    * to the tiler descriptors. */
   cs_ctx->render.geom_buf = (cs_ctx->render.tiler_heap + 4096) | ((64 * 1024) >> 12);
```
**几何视图**（`:2884-2885`，同一布局的第三种表述）：
```c
   .cpu = panvk_priv_mem_host_addr(queue->tiler_heap.desc) + 4096,
   .gpu = panvk_priv_mem_dev_addr (queue->tiler_heap.desc) + 4096,
```
| 核对项 | 结论 |
|---|---|
| 偏移 4096 是否一致 | ✅ 三处一致（打包 `+4096`、cpu/gpu 视图 `+4096`、注释"geometry comes 4k after"） |
| geometry 长度 | ✅ `(64*1024)>>12 = 16 页` = 64 KiB，与 `alloc_info.size = 64 KiB + 4096` 的"余下部分"**精确相等** ⇒ **geometry 的末尾 == 该 BO 的末尾** ✓ |
| `\| 16` 是否会污染地址 | ❌ **不会（设计如此）**：注释明写"**把尺寸编码进低 12 位，以便整条地址可直接拷进 tiler descriptor**" ⇒ 低 12 位在该 **descriptor 格式**里**不是地址位**（是页数/尺寸字段）⇒ `base+4096` 本身 4096 对齐、低位为 0，OR 上 `0x10` 恰好落在"尺寸字段"里 ✓（若硬件把整 64 位当地址，就会读 `base+0x1010`；但按注释的用法不是） |
| 对齐 | ✅ 4 KiB 对齐、两半各自独立 ⇒ **打包本身没有 off-by-one / 没有双常量** |

**⇒ 结论（回答 ①）**：**打包算式与分配是一致的**（descriptor `[0,4K)` + geometry `[4K,68K)`，总计 `0x11000`）⇒ **不要**在这一处找"算错"；要找的是"**谁读了 geometry 之外**"，见 §3。

---

## 2. 固件从哪里读、读多长（你第 ② 点）

- **`HEAP_SET` 用的是内核创建的 heap context**（不是这个 BO）：`:2935` `cs_move64_to(&b, heap_ctx_addr, queue->tiler_heap.context.dev_addr)` ⇒ context 的读是另一条链（你 v110 已排除它：`ctx_va=0x6000000000` ≠ 故障地址 ✓）；
- **这个 BO（descriptor+geometry）通过 `cs_ctx->render.{tiler_heap,geom_buf}` 进入 tiler/render descriptor**，固件据此：
  1. 读 **descriptor**（`[0,4K)`）—— 里面是 TILER_HEAP 描述符（`base/bottom/top` 等，本树在 `:2838` 附近与 `renew` 路径 `:3399` 处写）；
  2. 读 **geometry**（`[4K,68K)`）—— **里面放着一段 CS init stream**：`:2950` `panvk_priv_mem_flush(queue->tiler_heap.desc, 4096, init_stream_size)` ⇒ **固件在初始化/续租时要读这 `init_stream_size` 字节的指令** ✓✓
- ⇒ **"固件越过 64 KiB 区域读"的唯一自然通道就是：`init_stream_size > 64 KiB`** —— 即 **init stream 比 geometry 还大** ⇒ 固件读到 geometry 之外、进而越过整个 BO ⇒ **正好是"越过一个 64 KiB 区域"的形态** ✓✓✓

---

## 3. ★ 本轮最值钱的发现：`init_stream_size` 与硬编码的 64 KiB 之间**没有任何校验**

- `:2950` 的 flush 长度是 `init_stream_size`，**起点 4096**，而 BO 的总余量只有 `64*1024`（geometry）⇒ **只要 `init_stream_size > 64*1024`，flush 与固件的读都会越界**；
- **打包处 `:2838` 的 `(64 * 1024) >> 12` 是硬编码**，`alloc_info.size`（`:3197`）也是硬编码 —— **两处各写一遍**（**又一处"双常量"**，与 73 号 ring 那个同类！）⇒ 若有人只改一处，就会出现"固件被告知 16 页 vs 实际更大/更小"的不一致；
- **⇒ 需要一条 grep/一眼确认**：`init_stream_size` 在哪里算出来、上限是多少：
```bash
grep -rn "init_stream_size" /root/zenithblue/work/mesa/src/panfrost/vulkan/ | head
```
  若它**可以**超过 64 KiB（例如随 `chunk_size`/`max_chunks`/描述符数量增长），**这就是根因**；若不可能超过，则这条支线降级。

---

## 4. 单变量实验（你第 ②点；**必须"两处一起改"**，否则重演 v112 的陷阱）

**改动（配对，缺一不可）**：
```diff
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ b/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
@@ (init_tiler, :3196-3198)
-   struct panvk_pool_alloc_info alloc_info = { .size = (64 * 1024) + 4096, .alignment = 4096 };
+   /* v119: geometry 加倍（单变量：只改几何大小，同时同步打包常量） */
+   struct panvk_pool_alloc_info alloc_info = { .size = (128 * 1024) + 4096, .alignment = 4096 };
@@ (cs_ctx 初始化, :2836-2838)
-   cs_ctx->render.geom_buf = (cs_ctx->render.tiler_heap + 4096) | ((64 * 1024) >> 12);
+   cs_ctx->render.geom_buf = (cs_ctx->render.tiler_heap + 4096) | ((128 * 1024) >> 12);
```
（**两处都要改**：前者决定 BO 实际大小，后者是告诉固件的页数。）
**判据**：
- **故障地址移动 / 消失** ⇒ **就是 geometry/init stream 的越界** ⇒ 按 §5 修 ✓✓
- **完全不动** ⇒ 与该 BO 无关 ⇒ 回到"故障绑在 `0x5fffe1e000` 那个 64 KiB slab"（§6 的诚实边界）
**另一种更省的做法（零重编）**：先跑 §3 的 grep；若 `init_stream_size` 有上限且接近 64 KiB，**直接把它调小**（若它由 env/常量决定）也能作同一实验。

---

## 5. 修复候选

| 候选 | 内容 | 风险 |
|---|---|---|
| **F1（首选）** | **让 geometry 大小与打包常量同源**：`#define PANVK_TILER_GEOM_SIZE (64*1024)`，分配用 `PANVK_TILER_GEOM_SIZE + 4096`、打包用 `(PANVK_TILER_GEOM_SIZE >> 12)`，并加 `static_assert`（打包值 < 4096 页） | **0** |
| **F2（必备，直接防越界）** | 在 `:2950` 的 flush 前加断言：`assert(init_stream_size + 4096 <= panvk_priv_mem_size(tiler_heap->desc));`（release 下改为限流日志 + 截断） | 0 |
| **F3** | 若 §3 显示 `init_stream_size` 可能大：**按它算出 geometry 大小**（页对齐）再分配（`MAX2(64*1024, ALIGN_POT(init_stream_size, 4096))`），并让打包常量随之计算 | 低 |
| **F4** | `oom_fbd` 用同一个 `alloc_info`（`:3218`）⇒ 若改大小，**两处一起改**（否则两者尺寸不一致，异常路径读取会错位） | 0 |

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1/§2 的算式**逐行核实**（`:2832-2838`、`:2884-2885`、`:2950`、`:3196-3218`）——本报告最硬的部分。
2. **`init_stream_size` 的来源与上限我没读到**（§3 给了 grep）⇒ "它可能 > 64 KiB"**仍是假设**；这正是最该先跑的一条命令。
3. **一个必须正视的矛盾**：你说的 `0x11000` BO 在 v115 里是 `[0x5ffffa5000, 0x5ffffb6000)`，而故障地址是 `0x5fffe1e000` ⇒ **两者不相邻** ✗ ⇒ **若故障真是"读 geometry 越界"，故障地址应出现在 `0x5ffffb6000 + gap` 附近** ⇒ 所以本报告**不宣称**"这就是根因"，而是给出一条**有明确预言的可判实验**（§4）✓ **请把 v115 里那个 `0x11000` BO 的 flags 与它同局的位置一并确认**（若它在下一次运行里恰好挪到 `0x5fffe0_xxxx`，那 §4 的预言就直接成立）。
4. §3 的"双常量"（`:2838` 硬编码 vs `:3197` 硬编码）**确实存在**，但当前两者数值一致 ⇒ **今天不是 bug，是隐患** ✓
5. 所有改动**未编译、未上机**。
