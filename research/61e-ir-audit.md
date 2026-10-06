# 61e — IR 线专项审计：三趟**不是**三条区域；真正的 IR 空间行为在 **TILER_OOM 处理器**

日期：2026-10-06 · 只读分析 · 行号按我读到的当前树（以函数名为准）
配套：`61d-render-morphology.md`（形态学清单）· 61/61b/61c（布局侧，已全部出局）

---

## 0. 先给一条会改变结论的更正（有代码证据）

**`PANVK_IR_PASS_COUNT == 3` 确实成立（`csf/panvk_cmd_buffer.h:184-189`），但三趟 IR **不是三条空间区域**，而是同一个 framebuffer 上的三次"时序"趟** —— 对本项目的场景（**LINEAR** WSI RT）它们**在空间上完全等价**：

1. **三趟的目标图像是同一张**：`spill.load/store` 指向的就是主附件**同一个 image view**
   `vulkan/panvk_vX_cmd_draw.c:192-194`（color）/`:440-456`（zs）——"spill" 指的是**在趟与趟之间把 tile 内容落回同一张图**，不是一张中间图。
2. **三趟的几何换挡只换 `fbd_info.{fb,load,store,resolve}` 的指向**，位置参数只有两个候选：
   `fbd_info.fb = ir_pass == LAST ? &render->fb.layout : &ir_fb_layout`（`:1887`）；
   而 `ir_fb_layout` 只是 `render->fb.layout` 再对 `spill.store` 做一次
   `pan_align_fb_tiling_area()`（`:1861-1862`）。
3. **而 `pan_align_fb_tiling_area()` 对 LINEAR 图像是空操作**：`lib/pan_fb.c:99-108` 里只有 AFBC（block size）/AFRC（clump size）才对齐，**"No alignment requirements → return"**。
   ⇒ 我们的 WSI RT 是 LINEAR（已实测：modifier=0、linear handler）⇒ **`ir_fb_layout ≡ render->fb.layout`** ⇒ **三趟的 `tiling_area_px`/`render_area_px` 完全相同**。

**⇒ 结论：「3 面板 ⇔ 3 趟 IR」在空间上不成立**（三趟覆盖同一区域、同一张图、同一 pitch）。3 这个数字对上更像是巧合，或者来自另一条能把画面切开的机制（我在 61d 列的 M2 layer/td 才是"空间切开"的那一族）。

---

## 1. 那 IR 与"重载才坏"到底怎么联系起来？——**只有 TILER_OOM 处理器**

- `ir.fbds[ir_pass]`（每趟一个 framebuffer-descriptor）与 `TILER_OOM_CTX_FIELD_OFFSET(ir_descs)` 的写入在
  `csf/panvk_vX_cmd_draw.c:1926-1928`（存 `ir.fbds`）与 `:4421-4427`（写进 TILER_OOM 上下文）。
- **它们的唯一消费者是 TILER_OOM 异常处理器**：`csf/panvk_vX_exception_handler.c:235-323`
  （`:257` 取 `ir_descs[PANVK_IR_MIDDLE_PASS]`、`:262` 取 `[FIRST_PASS]`、`:323` 取 `[LAST_PASS]`；
  `:270 copy_fbd(&b, has_zs_ext, rt_count, current_fbd_ptr_reg, ir_descs_ptr, …)`；
  `:291 cs_add_imm64(ir_descs_ptr, ir_descs_ptr, fbd_size)`）。
- ⇒ **IR 的 descriptor 机制只在"tiler 堆 OOM"时被固件/处理器使用**。这正是你观察到的相关性：
  **轻载探针不 OOM ⇒ 不碰这些 descriptor ⇒ 像素精确；重载游戏 OOM ⇒ 处理器开始按趟复制/切换 fbd** ⇒ **一旦那里的尺寸/字段算错，写出来的 framebuffer 描述符就是错的 ⇒ 面板/黑带/重复**。
- 与既有事实的吻合度：✅ "重载才坏"；✅ 与 `td_count>1`（`cs_frag_end` 只在 td>1 发）同域（OOM 处理器同样处理多 tiler desc）；✅ 与卡死族同域（OOM 路径会长时间停留/走异常处理）。
- ⚠️ 需要一条数据来确认这条链**真的被走到**：**`tiler_oom.counter != 0`**（你在 v84d 快照里见过这个字段，当时是 0 ⇒ 那一局没走；坏帧上必须重测）。

---

## 2. 问题 2 的答案（修正版）：没有"某一趟静默不写"，但**可能"某一趟的 fbd 被复制错"**

- `get_fb_descs()` 的三趟**都**会把 fbd 写进各自的 desc（`:1926-1928` 无条件写 `ir.fbds[ir_pass]`）；
  **没有** `ir_enabled` 门控，也**没有**"spill 未建立就跳过某趟"的分支（我逐行看了 `:1864-1930`）。
  ⇒ 所以"三趟里有一趟静默不写"**不成立**（至少在这段代码里）。
- 真正可疑的是 **OOM 处理器对 `ir_descs` 的使用**：
  `copy_fbd(..., has_zs_ext, rt_count, …)` 用**运行时才决定的尺寸/字段集**去**整块复制**一个 fbd，并在循环里按 `fbd_size` 前进（`:270`、`:291`）。
  如果 **`fbd_size`（处理器侧）与生成侧 `calc_fbd_size(cmdbuf)`/`fbds_sz`（`csf/panvk_vX_cmd_draw.c` 内）不一致**，或 `has_zs_ext`/`rt_count` 与实际 fbd 布局不一致，复制出的描述符就会**错字段/错偏移** ⇒ **GPU 按错误的 framebuffer 描述符渲染 ⇒ 面板 + 直边 + 内容重复/错位**。
  ⇒ **这是我给出的头号 IR 候选**（file:line 已给：`exception_handler.c:235-323` 对 `csf/panvk_vX_cmd_draw.c:1085-1105`、`:4360-4380` 的 `fbd_size/fbds_sz` 计算）。
  ⚠️ **我本轮没有逐行核对这两处尺寸公式**（预算耗尽）——**这是下一轮第一件要做的事**，而且它是**纯代码对比**，不需要设备。

---

## 3. 一次运行的判别插桩（最小集，按你倾向：`KBASE_DIAG` 门控 + 每帧一次）

在 `get_fb_descs()` 的 IR 循环内（`csf/panvk_vX_cmd_draw.c:1864` 循环体首/尾）加一条 `mesa_logi`（用 `PANVK_DEBUG(KBASE_DIAG)` + 一个"每帧只打一次"的静态计数），打印：

| 字段 | 切开哪条分支 |
|---|---|
| `ir_pass`（0/1/2）+ `fbd_info.fb == &fb.layout` 还是 `&ir_fb_layout` | **证实/否证"三趟几何相同"**（我预测：LINEAR 下三趟全同）⇒ 若打印显示两者不同（例如有 AFBC RT）⇒ IR 空间假设复活 |
| `layout.width_px/height_px`、`render_area_px{min,max}`、`tiling_area_px`（每趟） | M1（fb extent 被 MIN 压低）与"三趟几何不同"两支 |
| `fbd_info.load/store` 指向（`&fb.load` / `&spill.load` / `&fb.store` / `&spill.store`） | 换挡是否符合 `FIRST/MIDDLE/LAST` 预期 |
| **每个附件的 `iview->vk.extent`** + `cmdbuf->state.gfx.render.layer_count`/`td_count` + `VkRenderingInfo.layerCount/viewMask` | **M2（layer/td 空间切开）**——这是我建议与 IR 同时测的重点 |
| `render->tiler_oom.counter`（若有该字段/等价计数） | **本轮真正的分水岭**：坏帧上 counter 是否 ≠ 0 ⇒ OOM 处理器是否被走到 |
| WSI image 的 extent（present 侧已有日志） | 与上面的 fb extent 对账（=M1） |

**代价**：一处 8~12 行 `mesa_logi`（不触碰任何发射逻辑、不改行为），`PANVK_DEBUG=1,kbase_diag` 下每帧一行。
**风险**：0（只读值 + 日志）。

---

## 4. 最小修复候选（**条件性**，需先有 §3 的读数）

| 候选 | 内容 | 风险 |
|---|---|---|
| **F-IR-1** | **修 `copy_fbd`/`fbd_size` 的一致性**：让 OOM 处理器复制 fbd 时用的尺寸/字段集与生成侧**同源**（同一个 `calc_fbd_size()`/`has_zs_ext`/`rt_count` 派生量，最好共用一个函数而不是各算一遍） | 低-中（改的是异常处理器，**正常路径零影响**；但要保证异常路径本身仍能跑通） |
| **F-IR-2** | 若 §3 显示 `ir_fb_layout != fb.layout`（非 LINEAR 场景）：回到 MR!44816 的语义讨论（我们已在 v81 回移过） | 中 |
| **F-IR-3** | 若 §3 显示 **M2（layer/td）**：查 `calc_enabled_layer_count()` 与 `layer_offset` 打包（`csf:1074-1078`、`:1092-1093`） | 中-高（动分层渲染） |
| — | **不要**为了"三趟"去改 IR 趟数/几何 —— 在 LINEAR 下三趟本就等价，改了只会掩盖真因 | — |

---

## 5. 建议的下一步（信息量排序）

1. **纯代码对比（我下一轮就能做，不需设备）**：`exception_handler.c` 的 `copy_fbd`/`fbd_size`/`has_zs_ext`/`rt_count` ↔ 生成侧 `calc_fbd_size()`/`fbds_sz`/`get_fbd_size()` 的**逐公式核对**。⇒ 这一步**可能直接给出 file:line 的根因**。
2. **§3 的插桩**（一次运行）：尤其 **`tiler_oom.counter`**（分水岭）+ **layer_count/td_count**（M2）。
3. 连拍帧**数面板数**并记录**坏帧上 `tiler_oom.counter`** —— 若坏帧 counter=0，则 OOM 路径出局，回到 M1/M2/M5。

---

## 6. 未验证 / 限制

1. 全部为**代码级**推断；**未做设备实验**。
2. **我未逐行核对 `copy_fbd` 与 `calc_fbd_size` 的公式一致性**（§2 的头号候选仍是**假设**，依据是"两处各算一遍尺寸"这一结构风险 + 只在 OOM 时被执行 ⇒ 与"重载才坏"吻合）。
3. `pan_align_fb_tiling_area` 对 LINEAR 是空操作这一结论，依据是我在 v81 期读过的 `lib/pan_fb.c:99-108`（AFBC/AFRC 分支 + "No alignment requirements → return"）；**未重新逐行复核**。
4. `tiler_oom.counter` 的**字段名与语义**来自你给的快照输出（`subqueue 0 callee ctx: … tiler_oom.counter 0`）；我**未核对**它在代码里的读写点 ⇒ 插桩前请确认它在哪被递增。
5. "面板数 = 3 与 IR 趟数相撞是巧合"这一判断，建立在 §0 的三条代码事实上；若你的现场面板其实**不是**三条竖直条带而是别的形态，请给一张帧的描述/尺寸比例，我可以据此重排形态学优先级（尤其区分 M1 的"右下角黑"与 M2 的"横向多条"）。
