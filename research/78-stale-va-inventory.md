# 78 — 换方向：4 个故障地址的**算术指纹**（同一基址 + 两个常数偏移）+ "过期 VA"的清点与判别实验

日期：2026-10-06 · 只读分析 · 配套：`77-geometry-never-written.md`（其结论已被你的 v119/v120 推翻，见 §0）
**已接受你的三条修正**：①"kbase 漏填 geometry"不是根因（v120 填了也崩）② geometry 内容不是触发条件 ③ v118 的"精确移动 0x8000"是"多个故障点谁先触发"的巧合 ✓

---

## 0. 把 v115–v120 的净结论写清楚（避免第 5 次反复）

| 事实 | 状态 |
|---|---|
| 存在**一小组离散的未映射地址**：`0x5fffe1e000`、`0x5fffe26000`、`0x5fffd1e000`、`0x5fffd8e000` | ✅ 实测（v120 同时出现两个 ⇒ 是"谁先触发"而非"随尺寸移动"） |
| 只有**真 draw** 触发；clear/copy/AHB/WSI 全通 | ✅ 实测 |
| 故障地址随**设备初始化期**布局变、不随**每 cmdbuf** 池 slab 变 | ✅ 实测 |
| "geometry 内容/是否填充" | ❌ 不是触发条件（但 v120 的填充**仍该保留**：它是正确性修复） |
| "堆耗尽"、"关诊断" | ❌ 都不是触发条件 |

---

## 1. 你第 1 点：4 个地址的**算术指纹**（**这可能是本轮最有用的一步**）

把它们对到 1 MiB 栅格上：

| 地址 | 距 1 MiB 边界 | 与 `0x5fffe1e000` 的差 |
|---|---|---|
| `0x5fffe1e000` | = `0x5fffe00000 + 0x1E000` | 0（基准） |
| `0x5fffe26000` | = `0x5fffe00000 + 0x26000` | **+0x8000** |
| `0x5fffd1e000` | = `0x5fffd00000 + 0x1E000` | **−0x100000（正好 1 MiB）** |
| `0x5fffd8e000` | = `0x5fffd00000 + 0x8E000` | −0x900000 |

⇒ **三个观察（都是纯算术、无需新实验）**：
1. **`0x…E1E000` 与 `0x…D1E000` 的偏移完全相同（都是 `+0x1E000`，分别落在 1 MiB 边界 `0x5fffe00000` / `0x5fffd00000` 之上）** ⇒ **同一结构在两种布局下的同一相对位置** ✓✓
2. `0x5fffe26000` = 同一边界 `+0x26000`（比 `0x1E000` 多 `0x8000`）⇒ **同一族的第二个偏移**；
3. `0x5fffd8e000` = `0x5fffd00000 + 0x8E000` ⇒ 第三个偏移。
⇒ **⇒ 这不像"随机野指针"，更像"某个基址 + 少数几个固定偏移"** —— 即 **一个/几个被反复使用的结构，其地址由"某基址 ± 常数"算出**（与你的新方向一致：**过期 VA / 未填字段**）✓✓
**⇒ 请给你的完整 BO 表（同局、同 PID）**，列就这五列即可：`va_start, va_end, size, flags, site_off`。我会用这两件事定性：
- 这 4 个地址是否**恰好等于某个 BO 的 `va_end` 再加一个常数**（= "下一个槽"）；
- 或者它们落在**两个 BO 之间**（= 分配器间隔），从而判断"基址"是哪个 BO。

---

## 2. 你第 2 点：**"把 `addr.dev` 存进长期结构"的清单**（清点 + grep 配方）

**已读过并确认会长期持有 GPU 地址的结构**（这些是"过期 VA"的候选持有者）：

| 持有者 | 存的地址 | 写入时机 | 失效风险 |
|---|---|---|---|
| `subq->context`（CS 侧 ctx BO） | `syncobjs`、`regs_save`、`dump_region_size` 等（`gpu_queue.c:2815-2822` 一带） | **queue init 一次** | 若这些 BO 被**迁移/重建**而 ctx 不更新 ⇒ 过期 ✓ |
| `cs_ctx->render.tiler_heap` / `geom_buf` | `tiler_heap.desc`（BO）与其 `+4096` 打包值 | `:2832-2838` | `desc` 在 `init_tiler` 分配（`:3202`）、**续租时若重建则必须同步**（`:3360-3400`） |
| `cs_ctx->render.desc_ringbuf.ptr/syncobj` | `render_desc_ringbuf`（设备级） | `:2845-2850` | 设备级 ⇒ 一般稳定 |
| `cs_ctx->tiler_oom_ctx.ir_scratch_fbd_ptr` | `tiler_heap.oom_fbd` | `:2856` | 同 `desc` 一族 |
| **cmdbuf 侧**：池支撑的结构（descriptor chunk、命令数据） | `panvk_pool_alloc_mem` 的 `addr.dev` | **每个 cmdbuf 记录期** | **★ 池在 cmdbuf reset 时回收 ⇒ 若某个长期结构仍指向它 ⇒ 过期** ✓✓ |

**通用 grep 配方（把"缓存地址"的地方一次找全）**：
```bash
M=/root/zenithblue/work/mesa/src/panfrost/vulkan
grep -rn "= panvk_priv_mem_dev_addr(\|= panvk_priv_bo.*addr\.dev\|->addr\.dev;" $M | grep -v "\.bak"
grep -rn "dev_addr" $M/csf/panvk_cmd_buffer.h | head -30     # ctx 结构里所有 GPU 地址字段
```
⇒ **判据**：对每个"写入点在 init、而对象来自池"的字段 ⇒ **高嫌疑**（池回收后不更新）。

---

## 3. 你第 3 点：**"BO 销毁后地址仍被引用"的路径**

| 路径 | 会释放什么 | 谁可能仍持有旧地址 |
|---|---|---|
| **cmdbuf reset / 重新记录** | `cmdbuf->cs_pool`/`desc_pool`/`tls_pool` 的 slab（池回收） | 若某个 **descriptor/命令流仍留在旧 slab**，或某个长期结构（`cs_ctx`）指向 slab 内 ⇒ 过期 ⇒ GPU 读到**已还给系统/未映射**的页 ⇒ TRANSLATION_FAULT ✓✓ |
| **queue/subqueue 销毁重建** | `subq->context`、`init_cs`、`tracebuf` | `cs_ctx` 里的地址若未随重建刷新 ⇒ 过期 |
| **swapchain 重建** | WSI 图像/内存 | 与本故障（draw-only、无 present）关系较小 |
| **tiler heap 续租/retire** | 旧 heap context（`KBASE_IOCTL_CS_TILER_HEAP_TERM`） | `cs_ctx->render.tiler_heap`（= `desc` BO，不重建）与 **`HEAP_SET` 用的 `context.dev_addr`（会换！）** ⇒ **★ 续租换 context 时，必须确保"在飞的 ring entry"不再持旧 context**（本树用 `emitted_jobs` 计数判断"固件不再可见"，`:2172-2190`）⇒ **tri 只有 1~3 个作业 ⇒ 够不到续租阈值，所以对 tri 无效**，但对**游戏长跑**是同一族的候选 ✓ |

---

## 4. 你第 4 点：下一个 5 分钟实验（我给两个，第一个更省）

### E1（零重编 / 探针 2 行）：把 `tri` 的 draw 连做 N 次
现在的 `mode_triangle()` 只画一次。改成**同一 cmdbuf 内连续记录/提交 N 次 draw**（或循环 N 次 acquire+submit），N=2..64：
- **第 1 次就崩** ⇒ 与"重复/生命周期"无关 ⇒ 纯"首次 draw 的某个结构"问题；
- **第 1 次好、第 k 次崩** ⇒ **拿到"第几次触发的准确序号"** ⇒ 直接指向"每次 draw 会消耗/轮转的结构"（descriptor chunk / ring 游标）✓✓ 这是**信息量最高**的一步，且几乎零成本（1 个 for 循环）。

### E2（★更强的仪器，1 次重编）：**GPU 地址的"来源校验器"**
在把任何 GPU 地址交给 CS 之前（或至少在 `cmd_draw.c:1356` 装 `tiler_heap/geom_buf` 之前、以及每个 descriptor/ring 指针写入处），加一个 **DEBUG-only** 校验：该地址必须落在**当前存活**的某个 BO/slab 区间内（区间表由 `panvk_pool_alloc_backing` + `panvk_priv_bo_create` 维护，两者都已有打印点）。
- **优点**：它直接**点名那个过期指针**（谁、哪个字段、指向哪），比继续猜地址快得多 ✓✓
- **代价**：一张区间表 + 一个 `bool in_live_range(u64 va)`，~40 行，DEBUG-only ⇒ 零发布风险 ✓

**建议顺序**：**E1（先拿到"第几次"）→ E2（哪个指针过期）**。

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1 的算术**完全基于你给的 4 个地址**（我未复核原始 logcat）；§2 的表格**部分来自我此前逐行读过的代码**（`:2815-2858`、`:3202`、`:3218`、`:3360-3400`、`:2172-2190`），**部分是按结构名推断**（凡推断处我都标了"若/可能"）。
2. **我没有读到完整 BO 表** ⇒ "4 个地址 = 某 BO 的 `va_end + 常数`"**尚未对位**（这是 §1 末尾那五列数据的用途）。
3. §2 的"cmdbuf 池支撑的地址被长期持有"是**假设**（尚未定位到具体的过期字段）⇒ **E2 的校验器就是为它设计的**。
4. §3 的续租/retire 对 **tri 无效**（阈值够不到）——它对**游戏长跑**才是候选，不要把它当 tri 的解释 ✓
5. 所有改动**未编译、未上机**（按纪律只出提案）。
