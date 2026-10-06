# 70 — `tri` 复现器：**这不是新线索，项目已有 12/13 号报告**（含码表与三个待做实验）

日期：2026-10-06 · 只读分析 · 配套：`12-probe-run-results.md`、**`13-csf-exception-c3.md`（本条的完整前作）**
**一句话**：你找到的"统一复现器"与 `12/13` 号报告里记录的现象**逐字一致**（`tri` ⇒ `VK_ERROR_DEVICE_LOST` + 三个 group 同报 `0xc3`，而 clear/copy/AHB/WSI 全通）⇒ **不要重新挖，直接接上 13 号的三个待做实验**。

---

## 0. 前作坐标（请先读这两份，避免重复劳动）

| 报告 | 内容 | 关键行 |
|---|---|---|
| `/root/research/12-probe-run-results.md` | **首次**记录 `tri` 必掉 GPU | `:8`、`:263-270`、`:305`（"真正会让 GPU 掉线的是绘制"） |
| **`/root/research/13-csf-exception-c3.md`** | **整份报告就是解这个码**：逐位解码、触发面、嫌疑点排序 H1–H4、三个待做实验 S1/S2/S3 | 见下 |

⇒ 你这次的 log 与 13 号 §1.1 引用的现场**同码同源**（`0x7dc002c3`）；**新增的**只是 `0xc2`（同一族的 L2）与 ckpt 23/26 的读数。

---

## 1. 问题 1：`0xc2` / `0xc3` 是什么（**已定论，逐位可算**）

**结论：它们不是 CS 指令异常，而是 GPU MMU 的 `AS_FAULTSTATUS` 原值**（`status & 0xff` 就是 exception 字节，打印点：`src/panfrost/lib/kmod/kbase_kmod.c:633` 的 `BASE_GPU_QUEUE_GROUP_ERROR_FATAL` 分支）。

| 位域 | 掩码/移位 | 你这次的值 | 含义 |
|---|---|---|---|
| EXCEPTION_TYPE | bits[7:0] | **0xC2 / 0xC3** | **TRANSLATION_FAULT_L2 / L3** —— 页表走表失败：L0→L1→L2 描述符有效，**第 L2 / 最后一级 PTE 无效** |
| ACCESS_TYPE | bits[9:8] | `0x2` | **READ**（不是写、不是取指） |
| SOURCE_ID | bits[31:16] | `0x7DC0` | core_id=62 ⇒ **"csf"**（CSF 固件本体）；internal requester=12 ⇒ **"lsu"** |

**码表出处（都在树内/前作里）**：
- 本 fork 自带头：`src/panfrost/lib/kmod/mali_kbase_csf_registers.h:817`：**`CS_FAULT_EXCEPTION_TYPE_TRANSLATION_FAULT_L3 = 0xC3`** ✔（⇒ `0xC2` 就是同族的 **L2**）
- 公开 kbase：`hw_access/regmap/mali_kbase_regmap_csf_macros.h:166-170`：**`TRANSLATION_FAULT_0..4 = 0xC0..0xC4`** ✔
- 名字表/access type/source id 的完整推导见 **13 号 §1.2**（三个来源互相印证）

**⇒ 你问的那个"反差"由此解释**：`0xC0–0xC4` 是**通用地址翻译故障**——**凡是让 GPU 去碰一个未映射 VA 的路径都会报同一族码**。所以"AFBC 实验触发 0xc3"和"一次普通 draw 触发 0xc3"**不矛盾**：两者都是"某处给出的地址映射不存在"（AFBC 是布局/偏移算错，draw 是 tiler 侧指针错），**码本身只说明"翻译失败 + 是读 + 请求方是 CSF 的 LSU"**，不携带"AFBC"语义 ✓

**"三个 group 同时 fatal"不是三次故障**：kbase 对 kctx 级 MMU 故障会把**同一份 payload 复制给该 kctx 所有在位 CSG**（公开 kbase `csf/mali_kbase_csf.c:1687-1713`）⇒ **全过程只有一次错误访问** ✓
`sideband 0x5ffe800000 / 0x5fffe1e000` **就是那个故障 GPU VA**（kbase 把 `fault->addr` 填进 sideband）。

---

## 2. 问题 2：为什么"一次 draw 就致命"，以及 ckpt 23/26 该怎么读

**ckpt 读法（你这次的新数据）**：`0xc0de0017`=23 `FINISH_AFTER_WAIT`、`0xc0de001a`=26 `BARRIER_AFTER_SB_WAIT`，`seqno 2/3` ⇒ **作业 3 永不完成**。
⇒ 这不是"等待条件写错"，而是**CS 已经死了**：固件 MMU 故障 ⇒ 该 group 被 kbase 判为 fatal ⇒ 队列上的作业再也不会退休 ⇒ `vkWaitForFences` 得 `VK_ERROR_DEVICE_LOST`（`-4`）✓ ⇒ **症状是"等不到"，根因在"等待之前的那次访问"**。

**为什么偏偏是 draw（13 号 §2.1 已定论）**：`tri` 相比 `render/ahb/win` **唯一新增的机制 = VERTEX_TILER 子队列的 tiler 作业**。
- clear/copy 走的是 `vk_meta` 的 **fragment-only fullscreen** 路径（`panvk_vX_cmd_draw.c:5020` → `cmd_draw_fullscreen`，`panvk_vX_cmd_meta.c:365-383`），**从不消费 tiler heap 的 chunk**；
- **只有真正的 `vkCmdDraw` 才会让固件按 chunk 链走 tiler heap**。
⇒ 所以"pipeline 编译成功、submit 成功、然后掉线"完全自洽：**故障发生在固件执行 tiler 作业、第一次读 tiler 相关地址的那一刻**。

**旁证（13 号 §2.2）**：`csf/panvk_vX_cmd_draw.c` 相对上游**只改了 1 行**（`panthor_kmod_get_csif_props`→`panvk_get_csif_props`）⇒ **"描述符怎么填"不是嫌疑点**，嫌疑集中在 **kbase 平台管道（kmod + gpu_queue + physical_device 的 props）与 tiler heap 的生命周期/几何**。

**嫌疑点排序（13 号 §2.3，我原样沿用并加一句自己的判断）**
- **H1【推断，最高】tiler heap 的 chunk 链被固件走到未映射页**：kbase 内核把"下一个 chunk 指针"编码在 chunk 头部，`first_chunk_va` 指向 **chunk 头**而非空闲区起点；本 fork 的 `chunk_size` 从上游 **2 MiB 改成 1 MiB**、initial 5→10、max 64→400（`panvk_physical_device.c:1231-1236` vs `:1365-1373`，注释说是避开 order-9 大页分配失败）⇒ **这个值与 kbase 的编码约束没有对过账** ⇒ 固件在错误偏移读指针 ⇒ 跟着垃圾走到未映射高位 VA ⇒ **READ + L3 + 高位** ✓✓
- **H2【推断】固件读的是 heap context 本身（`gpu_heap_va`）**：clear 路径也 `HEAP_SET` 但不做 tiler 处理 ⇒ **context 的首次真实读取同样只在 tri**；若该 VA 未映射进 kctx 页表则故障地址 = ctx VA。
- **H3【推断，低——但它才是"面板"的候选】** renew/retire 破坏固件仍在引用的 context：`kbase_renew_tiler_heap()`（`gpu_queue.c:2192-2230`）、`kbase_try_destroy_retired_heap()`（`:2172-2190`）；**作者自己在注释里记录过同类事故**："…the firmware then walks whatever now lives at the old chunk VAs as a chunk list and faults on a garbage pointer (**observed as an exception 0xc0 CSG fatal**…)"（`gpu_queue.c:2160-2170`）。
  ⚠️ 但它**解释不了 tri 必现**（tri 是**全新进程的第一次 draw**，renew 需要 `submit_count>=128`/`work_count>=65536`，`:2785-2800`）⇒ **tri = H1/H2（首次接触就错地址）；长跑游戏 = H3（续租后固件按旧 chunk VA 链走）** —— **这两条正好对应"探针轻载全通 / 游戏长跑面板"的分工** ✓✓（见 §4）

---

## 3. 问题 3：前三个单变量实验（**13 号 §0.6 已给，我按你"5 分钟一个"的节奏重排并补判据**）

| # | 实验 | 成本 | 判据（**决定性**） |
|---|---|---|---|
| **S1** | `tri` 重跑时加 `MESA_LOG_LEVEL=debug`，抓 `kbase: tiler heap desc 0x…: base 0x…` | **零改动、零重编** | 把 `base`（= kbase 给的 `first_chunk_va`）与故障地址 `0x5fffe1e000`（本次 `0xc3` 的 sideband）**对位**：同页/相邻 ⇒ **H1 坐实**；相差很远 ⇒ 转 H2 |
| **S2** | 2 行 `mesa_logi` 打出 `gpu_heap_va`（heap context VA，补丁 A 在 13 号 §4） | 一次重编 | 故障地址 == `gpu_heap_va` ⇒ **H2 坐实** |
| **S3** | 把 kbase 的 `chunk_size` **1 MiB → 2 MiB**（`panvk_physical_device.c:1371`，**1 行**）后重跑 | 一次重编 | **故障地址是否跟着变**：变 ⇒ 故障地址由 heap 布局推导（固件在走 chunk 链）⇒ H1；不变 ⇒ 固定结构地址（heap context）⇒ H2 |

**两个前置提醒（省你时间）**
1. **不是我的探针补丁引入的**：`tri` 在 `12` 号报告时代（`panvk_wsi_probe.orig`，10-05）就已同样掉线；我的补丁只动了 `W/H` 宏、staging buffer 的 usage 位与 shear 分支。**若要回归对照，直接用 `/root/research/probe10/panvk_wsi_probe.orig`** ✓
2. **`PANVK_KBASE_HEAP_RENEW_INTERVAL=0` 对这一局无效**（tri 只有 1~3 个作业，够不到 renew 阈值）⇒ **别把它当 S1~S3 的替代**；它对应的是 H3（长跑）。

---

## 4. 问题 4：它能解释 bug 3 的面板吗？——**分两条，一条不能、一条能**

| 路径 | 能否解释面板 | 依据 |
|---|---|---|
| **tri 本身的致命路径** | ❌ **不能直接解释** | 它是**致命**的（group fatal ⇒ DEVICE_LOST）；而游戏是"活着 + 画面面板" ⇒ 面板不是"每次 draw 都掉线"的产物 |
| **H3（续租/retire 后固件按旧 chunk VA 走）** | ✅ **能，且分工正好对上** | 它需要 `submit_count≥128`/`work_count≥65536` ⇒ **tri（首次 draw）够不到 ⇒ 探针全通；长跑游戏必然跨过 ⇒ 面板/花屏** ✓ 而作者注释记录的正是"固件把旧 chunk VA 当 chunk list 走 ⇒ **0xc0 fatal** 或读到垃圾" ⇒ **同一族的非致命形态就是"画面被写成垃圾/部分内容"** ✓ |

**⇒ 最小模型与判读方法（你要的"三角形应出现在哪里、探针能读回什么"）**
- 现在 `mode=tri` **只检查两个点**：中心像素应为红（`:946-959`，`want red 255 0 0 255` 或 BGR），角落应是 clear 色。
- **建议扩展为"整幅 bbox"判读**（这是面板的最小模型）：
  1. 读回整张 64×64（探针已有 `CopyImageToBuffer` + `rb.mapped`）；
  2. 统计**红色像素的包围盒** `(minx,miny)-(maxx,maxy)`；
  3. 与**期望**对比：视口 0..63、三角形顶点在 shader 里给定 ⇒ 期望 bbox ≈ 覆盖下半幅/中心的已知区域；
  4. 判据：**bbox 偏移/尺寸异常、或出现"同一三角形出现两份"、或出现直边黑带 ⇒ 面板在 64×64 上复现** ⇒ bug 3 与 tiler heap 同源坐实；**bbox 精确 ⇒ 面板不在 draw 的几何路径上**（转 H3 的非致命残影/应用侧）。
- 另：把 `mode=tri` 连跑 N 次（每次新进程）+ `PANVK_KBASE_HEAP_RENEW_INTERVAL` 小值（如 1）**让 renew 在探针里也发生**（例如循环提交 >128 次 draw）⇒ **这是把 H3 变成可复现器的那一步**（探针里也踩续租路径）✓

---

## 5. 我给你的执行顺序（把 5 分钟粒度用满）

1. **S1（零成本）** → 拿到 `first_chunk_va`，与 `0x5fffe1e000` 对位 ⇒ H1/H2 二选一。
2. 同时**回归对照**：`panvk_wsi_probe.orig` 跑同一条 `tri` 命令 ⇒ 确认与我的补丁无关（预计同样掉线）。
3. **S2 或 S3**（按 S1 结果二选一，一次重编）⇒ 锁定 H1 或 H2。
4. 若锁定 H1 ⇒ 按 13 号 §4 的补丁 B（chunk 几何与 kbase 编码约束对账）走；**并把 `chunk_size` 的取值来源（order-9 大页 vs kbase 编码约束）作为单一变量**。
5. **H3 探针化**（上述"连跑 >128 次 draw"）⇒ 一旦能在探针里复现面板类残影，bug 3 就有了 5 分钟级的复现器（这比现在的游戏现场强得多）。

---

## 6. 未验证 / 限制

1. 本报告**没有做新的设备实验**；§1 的码表、§2 的触发面与嫌疑排序**全部来自 12/13 号前作**（我逐行引用，未独立复核公开 kbase 源码，也未重算 fault 位域）。
2. **`0xC2` 与 `0xC3` 同族（L2/L3）**这一句是我按 `TRANSLATION_FAULT_0..4 = 0xC0..0xC4` 推的（13 号只落了 L3）——**低风险但仍属推断**；若要坐实，查 `mali_kbase_csf_registers.h` 里 `..._TRANSLATION_FAULT_L2` 那一行即可。
3. `0x5ffe800000`（`0xc2` 的 sideband）与 `0x5fffe1e000`（`0xc3` 的 sideband）**是两个不同地址** ⇒ 可能是**两次不同的故障访问**（不是同一次的两份复制）；这一点前作没有覆盖，**S1/S2 要把两个地址分别对位** ✓
4. H1 里 13 号明确标注：`CHUNK_SIZE_MASK` / `CHUNK_HDR_NEXT_*` 常量**未取到** ⇒ "1 MiB 与 kbase 编码约束不符"仍是**未验证**的推断（S3 就是它的判别实验）。
5. §4 的"H3 ⇒ 面板"是**机制级推测**（依据是作者自己的注释 + renew 阈值与 tri 的时间线错位）；**尚未在探针里复现**，我把复现步骤写在 §4 末。
