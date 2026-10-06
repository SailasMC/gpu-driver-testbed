# 66 — M7' 记账（真缺陷，非面板成因）+ 剪切形态的行距对账 + 判别插桩

日期：2026-10-06 · 只读分析 · 行号基线 = 我读到的当前树
配套：`65-m7prime-and-endgame.md`（M7' 初判）· `61-pitch-audit.md`（pitch 链首查）

---

## 1. M7' 记账：**确证为真缺陷，但不是面板成因**（两条都保留）

| 结论 | 证据 | 归因 |
|---|---|---|
| **缺陷 A/B 成立（API 层面）** | `queue_present(swapchain, image_index, present_id, damage)` —— **入口根本没有 wait-semaphore 参数**（你核对的签名）；`acquire_next_image` 忽略 `info` 并恒返回 `VK_SUCCESS`（`panvk_wsi.c:222-231`） | 结构性缺陷，应修 |
| **`DeviceWaitIdle` 不是栅栏** | `idle_wait = 11–25 µs`（负载下） | 结构性缺陷，应修 |
| **应用不轮转用图** | `image_index=2 → 32 次`，`image_index=0 → 1 次`（32:1） | 与我们的自增轮转**指向不同图** ⇒ 解释**全黑 / 随机 stale 帧** |
| **A/B：3000 µs settle 下面板依旧** | 你那一局 `timeout=0`、存活、截图仍见面板 | ⇒ **面板另有来源**（M7' 降级为"全黑与随机帧"的解释） |

⇒ **建议**：M7' 的修复（acquire 补 signal、present 补等信号量、别只靠 idle）**照做**，但**不要**期待它修掉面板；它修的是"黑帧/随机错帧"这一族。

---

## 2. 剪切形态的行距算术：**0.3 px/行不是一个合法的行距差**

Δ（源/目的行距之差）与每行平移量的关系（4 B/px）：**平移像素/行 = Δ_bytes / 4**。你观测到的 ~0.3 px/行 ⇒ **Δ ≈ 1.2 B/行** —— **不是整数**。而线性行距必然是（至少）4 B 对齐的整数字节数 ⇒ **"一个恒定的行距差"无法产生 0.3 px/行的剪切**。
⇒ 因此更可能是下列三者之一（按可能性排序）：

| 可能 | 机制 | 每行位移如何产生 | 与"直边 + 平移复制 + 黑区" |
|---|---|---|---|
| **S1 显示端按"比例"而非"行距"消费** | SF/合成器把窗口缓冲按**略不同的尺寸/比例**采样（例如它认为是 2390 px 宽而实际 2384） | 每行按比例累积 ⇒ **等效小剪切** | ✅ 直边（缝随行移动）+ 内容重复（源行尾接下一行头）+ 黑区（源数据耗尽） |
| **S2 读到"未写完"的图（M7'/M9 族）** | 拷贝发生在渲染中间；**已写区域偏移不一致**（同一帧不同 pass 的写入时间差） | 位移呈"随内容/时间"变化，不是严格 0.3 px/行 | ✅ 直边 + 重复（新旧内容同场景）+ 黑区（只 clear 过） |
| **S3 应用侧自身的缩放/投影**（MobileGL 渲染分辨率 ≠ 呈现分辨率，或 glScale 残余） | 不在本树 | 严格比例 ⇒ **每条竖线都是同一斜率** | ✅ 但**与驱动器无关** |

**判据差别（关键）**：S1/S3 是**比例型**（每条竖直特征线斜率相同、与内容无关）；S2 是**时间型**（同一画面不同帧斜率不同、且随负载变化）。⇒ 一次实验即可区分（见 §4）。

---

## 3. 四对行距对账表：**哪一对还没对过**

| # | 对 | 值 | 状态 |
|---|---|---|---|
| 1 | gralloc 分配（`AHardwareBuffer_allocate` desc，无 stride 字段） | `describe().stride = 2384 px`（=**9536 B**，有 32 B/行填充） | ✅ 已测（v89/v91） |
| 2 | **GPU 写 RT 用的 row stride** | `panvk_image.c:538 .wsi_row_pitch_B = rowPitch(9536)` → `pan_mod.c:904-947` 线性 handler **逐字采用**（`slayout->tiled_or_linear.row_stride_B = layout_constraints->wsi_row_pitch_B;`，且对齐不符就 `return false` 让建 image 失败）→ `pan_desc.c:259 *row_stride = slayout->tiled_or_linear.row_stride_B` → `pan_desc.c:318 cfg.row_stride = row_stride`（`SET_SURFACE_STRIDE` 同处） | ✅ **代码链完整**：GPU 的 `cfg.row_stride` **必然等于 9536**（否则建 image 失败）。**AFBC/CRC/`has_zs_ext` 不改变线性行距**（它们影响 fbd 尺寸/结构，不改 `row_stride`）——⚠️ 我仅逐行核了 **linear** handler |
| 3 | 我们 CPU 拷贝读源/写目的 | `y*desc.stride*4` 与 `y*win_buf.stride*4`，`desc.stride = win_buf.stride = 2384 px`；`copy_w = 2376` | ✅ 两者相同 ⇒ **拷贝自身 1:1、不产生剪切** |
| 4 | **显示端（SF）消费窗口缓冲时的 stride** | **从未对账** | ❌ **唯一未测的一对** ⇒ 但注意：SF 消费的是**窗口缓冲**（它自己分配的），不是我们的 AHB |

**⇒ 结论（重要）**：1/2/3 三对**都已一致**，且我们的拷贝是 1:1 ⇒ **剪切不可能来自"我们的拷贝"或"源 AHB 的写入"**。剩下的可能只有：**SF 对我们写入的窗口缓冲的消费方式（#4）**，或**应用自己的缩放（S3）**，或**时间型（S2）**。

---

## 4. 一次运行可判别的插桩与实验

### 4.1 你要的对账日志（最小实现位置：`panvk_wsi.c` 的 `queue_present`，`:274-308`）
在 `AHardwareBuffer_describe()` 之后、拷贝循环里 `y == 0/1/2` 时各打一行（DIAG 门控、每帧最多 3 行）：

```c
   /* 行距对账：同一行的源/目的偏移 + 该行前 8 字节，任何行距差立刻显形 */
   if (PANVK_DEBUG(KBASE_DIAG) && y < 3) {
      const uint8_t *s = (const uint8_t *)ahb_data + (uint64_t)y * desc.stride * 4;
      const uint8_t *d = (const uint8_t *)win_buf.bits + (uint64_t)y * win_buf.stride * 4;
      mesa_loge("kbase: present row %u: src off %" PRIu64 " (pitch %u px/%u B) "
                "dst off %" PRIu64 " (pitch %u px/%u B) copy_w %u | src %02x%02x%02x%02x "
                "dst %02x%02x%02x%02x",
                y, (uint64_t)y * desc.stride * 4, desc.stride, desc.stride * 4,
                (uint64_t)y * win_buf.stride * 4, win_buf.stride, win_buf.stride * 4,
                copy_w, s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3]);
   }
```
**判读**：`src pitch == dst pitch` ⇒ **拷贝无 Δ**（预计如此）⇒ **剪切在别处**（这正是本插桩的价值：**"干净"的结果本身就是判别**）。若两者不等 ⇒ 立刻得到 Δ 的具体字节数（并直接落在我们的拷贝代码上）。

### 4.2 零代码/低成本的决定性实验（按性价比）
| 实验 | 做法 | 切开什么 |
|---|---|---|
| **E1（最强）** | **往窗口缓冲写合成图案**：在 present 里（临时、DIAG）把 `win_buf.bits` 填成"每 100 px 一条竖线 + 每 100 行一条横线 + 行号渐变"，跳过 AHB 拷贝，然后 `unlockAndPost`，看屏幕 | **屏幕上的图案是否精确**：精确 ⇒ SF 消费窗口缓冲正常（⇒ S1 出局，转 S2/S3）；**被剪切** ⇒ **S1 成立（显示端）**，且斜率可量化 |
| **E2** | **全尺寸多帧探针**：把 `mode=win` 从 64×64 放大到 2376×1080，连续渲染 ≥300 帧（带同样 3 图轮转与 CPU 拷贝） | **驱动+WSI 在真尺寸多帧下是否出现面板**：出现 ⇒ 驱动侧可复现，转 §3-#2 的进一步插桩；不出现 ⇒ **S3（应用侧）**优先 |
| **E3** | 相机平移/转动，观察面板边界 | **S1/S3（比例型：边界锁屏幕/同斜率）** vs **S2（时间型：随帧变化）** |
| **E4** | 打印 `win_buf.format` 并**验证 stride 单位**（像素 vs 字节）：`assert(win_buf.stride * 4 >= copy_w * 4)` 与 `win_buf.format == WINDOW_FORMAT_RGBA_8888` | 排除"目的 stride 单位误判"（虽然那会给出 4× 而非 0.3 px 的错，但零成本） |

---

## 5. 两条小事的确认

1. **`emitted_jobs` 确实已在快照里**（`kbase_log_subqueue_state()` 的 `jobs` 字段，`:1100-1102`，且对三个子队列都打）⇒ **你的 v100(b) 是冗余的**，保留无害 ✓。
2. **每图毒色（`0/1/2` 各一色）是个好仪器** ✓ —— 建议在日志里同时印出"本次 present 的 `image_index` 与其毒色"，这样"屏幕上出现某色 ⇒ 呈现了一张从未被渲染的图"可以**一眼归因**（配合 M7' 族）。

---

## 6. 未验证 / 限制（诚实清单）

1. 全部为**代码级**分析；§4 的代码**未编译**、实验**未做**。
2. **§3-#4（SF 对窗口缓冲的消费）我无法从本树核对**（SF 在我们进程之外）⇒ 只能靠 E1 实测判定。
3. §2 的"S1/S2/S3"是**形态学推断**，`0.3 px/行` 是目测值 ⇒ **请以 E1/E3 实测斜率为准**（若斜率在不同帧变化 ⇒ S2；若恒定且与内容无关 ⇒ S1/S3）。
4. §3-#2 的"AFBC/CRC/`has_zs_ext` 不改线性行距"**只逐行核了 linear handler**；若该 RT 实际走了 tiled/AFBC 路径，需要另核（但我们的 WSI 图像是 LINEAR，已实测 modifier=0）。
5. E2 的探针改造属于代码改动（不在我纪律范围内）⇒ 我只给方案与判读表。
