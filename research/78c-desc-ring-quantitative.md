# 78c — 你三点定线之后：**ctx 残留假设被代码否证**；新的定量线索指向 **descriptor ringbuf 越界**

日期：2026-10-06 · 只读分析 · 配套：`78b-grid-hole-stale-va.md`、`78-stale-va-inventory.md`
**已接受你的三点定线**：`CLONE_MEM_SIZE 10 MiB / 2 MiB` 两配置下 **utrace BO 末尾固定（`0x5fffe00000`）、故障地址完全相同（`0x5fffe1e000`）** ⇒ **故障是"每配置确定、但不随堆尺寸变"的固定值** ⇒ 所有"耦合/移动"读法作废 ⇒ **"固定且确定的未映射地址 = 某字段未初始化或是过期 VA"** ✓✓

---

## 1. 你第 1 点先给一个**否证**（省掉一次实验）：`panvk_cs_subqueue_context` **不会有残留**

`csf/panvk_vX_gpu_queue.c:2801-2810`：
```c
   panvk_priv_mem_write(subq->context, 0, struct panvk_cs_subqueue_context, cs_ctx) {
      *cs_ctx = (struct panvk_cs_subqueue_context){
         .syncobjs = panvk_priv_mem_dev_addr(queue->syncobjs),
         .debug.tracebuf.cs = subq->tracebuf.addr.dev,
         .reg_dump_addr = panvk_priv_mem_dev_addr(subq->regs_save),
      };
```
⇒ **这是"复合字面量整体赋值"**（C 语义：**未列出的字段一律置 0**）⇒ **整个 ctx 被完整初始化**，**不存在"上一任 slab 住户的残留值"** ✗（这一支可以划掉）✓
随后两个 `if` 再补写（`:2831-2858`）：`subqueue != COMPUTE` 写 `render.tiler_heap`/`geom_buf`/`desc_ringbuf`；`subqueue == FRAGMENT` 写 `tiler_oom_ctx.ir_scratch_fbd_ptr` ⇒ **仍是完整覆盖** ✓
（唯一"没写"的是 `debug.kbase_progress_addr`——它只在 `HAVE_PAN_KMOD_KBASE && uses_kbase && KBASE_DIAG` 下写 ⇒ **非诊断版留 0**，而它只被诊断路径读 ⇒ 无害 ✓）

**⇒ 所以"未初始化字段"这一支在我们树里基本被排除**；剩下的只能是 **"被写入了过期值"**（你 78 号方向的另一半）✓

---

## 2. 满足"固定 + 只在 draw 时被消费"的字段（穷举，按嫌疑）

| 字段 | 何时写 | 谁在 draw 时读 | 为什么能产生"固定错误地址" |
|---|---|---|---|
| **★ `render.desc_ringbuf{ptr, syncobj, pos}`** | **queue init 一次**（`:2845-2850`，BO 在 `:2480` 一带分配） | **每次 draw 都消费描述符**（`calc_render_descs_size` / `cs_render_desc_ringbuf_move_ptr`，`cmd_draw.c` 的 "chunks not released prematurely" 一带） | **`ptr`+`pos`+size 的算术一旦超过该 BO ⇒ 固定落到 BO 之后** ✓✓✓（见 §3） |
| `render.tiler_heap` / `geom_buf` | queue init 一次 | tiler 路径（真 draw） | 你 v120 已证"内容无关"，但**打包指向的 BO 若被搬走**仍会过期 |
| `tiler_oom_ctx.ir_descs[3]` | **每次 draw**（`get_fb_descs`，`:1926-1928`） | **仅 OOM 处理器** | `tiler_oom.counter=0` ⇒ 处理器不该跑 ⇒ 降级 |
| `render.oq_chain`/`ts_chain`/`ts_done_chain` | per-draw（仅用到查询/时间戳时） | 链表遍历 | 探针不做查询 ⇒ 降级 |
| `syncobjs`/`regs_save`（`subq->context`） | queue init | 每次 entry | 设备级、稳定 ⇒ 低 |

---

## 3. ★ 本轮最有价值的定量线索：两个故障偏移**看起来就是描述符环的尺寸**

- 故障 = utrace BO 末尾 `+0x1E000`（= **120 KiB**）与 `+0x26000`（= **152 KiB**）；
- 这两个数**恰好是"一个环/缓冲尺寸"的量级**，而 `render.desc_ringbuf` 正是**设备初始化期创建、每次 draw 消费、地址固定**的对象 ✓
- ⇒ **假设 H-Desc**：**描述符环（或其 chunk 记账）越过了它自己的 BO** ⇒ 地址 = `desc_ringbuf BO 基址 + 尺寸 + ε` ⇒ **在两种堆尺寸下都不变**（因为描述符环与 utrace 堆各自独立分配）✓ 与你的三点定线**完全一致** ✓✓
- **⇒ 一次 grep 就能验证一半**：
```bash
M=/root/zenithblue/work/mesa/src/panfrost/vulkan
grep -rn "render_desc_ringbuf" $M/csf/panvk_vX_gpu_queue.c | head
grep -rn -B6 -A10 "render_desc_ringbuf\s*=" $M/csf/panvk_vX_gpu_queue.c | grep -E "alloc_info|size|ringbuf|init_desc" | head -20
```
把它的 **分配尺寸** 与 `0x1E000`/`0x26000` 对照：**若尺寸 ≈ 120/152 KiB（或它们的整数分之一/几倍）⇒ 直接命中** ✓✓

---

## 4. 你第 2 点：最省的 5 分钟实验（按"信息量/成本"排序）

| # | 实验 | 改动 | 判据 |
|---|---|---|---|
| **E1（★最省）** | **打印描述符环的身份**：在 `:2480` 一带分配成功后打印 `va_start/va_end/size`，并在 `:2845-2850` 打印 `desc_ringbuf.ptr`；再在每次 draw 前打印 `ctx->render.desc_ringbuf.pos` | **~4 行** | 把 `va`/`size` 与 `0x1E000`/`0x26000` 对照 ⇒ **若 `desc_ringbuf基址 + 尺寸 ≈ 故障地址` ⇒ H-Desc 命中** ✓✓ |
| **E2** | **改描述符环尺寸（单变量）**：把它的 `alloc_info.size` 改大/改小（例如 +0x8000） | 1~2 行 | **故障地址是否随之移动**（这次要盯住"是否**固定在新的**值上"）⇒ 移动即锁定 ✓ |
| **E3（护栏，零风险）** | 在把 `desc_ringbuf.ptr` 交给 CS 之后加断言：`assert(pos + needed <= size)`；并在 `calc_render_descs_size` 的记账处断言不越过环尾 | 3~5 行 | 越界时立刻在**发射期**报错（而不是等固件翻译故障）✓ |
| **E4（诊断增强）** | 你提的"打印整块 ctx"——**建议做，但用筛选**：只打印**看起来像 VA 的 64 位字段**（值落在 `0x5ff…`/`0x8000…` 段），与故障地址直接对位 | ~10 行 | 一次运行即可看出**哪个字段等于 `0x5fffe1e000`**（若有）⇒ 最直接的定位 ✓✓ |

**⇒ 建议顺序：E4（筛选版，一次运行就能指出"哪个字段等于故障地址"）→ E1/E2（锁定描述符环）→ E3（护栏）。**
**E4 的筛选判据**：`(v >> 40) == 0x5ff || (v >> 40) == 0x80` 之类的粗筛，打印字段名 + 值；**若某个字段的打印值恰是 `0x5fffe1e000`/`0x5fffe26000` ⇒ 凶手当场落网** ✓✓（比继续推断快一个数量级）

---

## 5. 修复方向（若 E4/E1 命中）

- **描述符环越界** ⇒ 修记账（`calc_render_descs_size` / `cs_render_desc_ringbuf_move_ptr` / chunk 生命周期）并在环尾加**边界断言 + 一个 NOP/终止块**（本树在 ring 的换行处已有同类做法，可复用）；
- **过期 VA** ⇒ 在持有者处"**重建时刷新**"（例如 queue/子队列重建、池回收后清空相关 ctx 字段），并加"地址来源校验器"（78 §4-E2）；
- 无论哪一支，**E3 的护栏都该留下**：它把"静默的翻译故障"变成"发射期的明确断言" ✓

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1 的否证**逐行核实**（`:2801-2810` + 字段全表来自 `panvk_cmd_buffer.h`）；§2 的"写入时机/消费方"**部分逐行核实**（`:2831-2858`、`:1926-1928`），**部分按结构名推断**（已标注）。
2. **H-Desc 是假设**：我只做了"故障偏移量级 ≈ 环尺寸量级"的**定量类比**，**没有读到描述符环的实际分配尺寸**（§3 的 grep 就是它）⇒ **E1 一次运行即可证伪/证实**。
3. `panvk_pool_alloc_mem` **是否清零我未能读到**（`csf/panvk_pool.c` 的 grep 无输出 ⇒ 文件路径/名与我的假设不同）⇒ 但它对 ctx 已不重要（§1 复合字面量覆盖）✓；对**其它**池对象仍有意义，留给下一轮。
4. §2 表格里 `ir_descs`/链表的"降级"依据是 `tiler_oom.counter=0` 与"探针不用查询" ⇒ 若这两个前提在 v120 里不成立，需要重新提级。
5. 所有改动**未编译、未上机**（按纪律只出提案）。
