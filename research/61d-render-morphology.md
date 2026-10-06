# 61d — 渲染侧形态学：在"布局/pitch/覆盖"全部出局之后，还有哪些机制能造出「面板 + 直边黑带 + 内容重复」

日期：2026-10-06 · 只读分析 · 行号按我读到的树（`vulkan/panvk_vX_cmd_draw.c` 在 v88+ 有改动 ⇒ 一律以**函数名**为准）
配套：`61-pitch-audit.md` / `61b-afbc-vs-pitch.md` / `61c-ahb-ownership.md`

---

## 0. 先给一条**必须马上用上**的指纹：**数一数画面里有几个面板**

现场是"若干面板"。而本树里可能产生"同一内容重复 N 份"的量都是**小整数**：

| 量 | 典型值（v12） | 若它 = 面板数 ⇒ 指向 |
|---|---|---|
| `PANVK_IR_PASS_COUNT`（增量渲染 pass 数） | **1 或 3**（需 grep 确认） | **M3**：IR pass / spill 写回错位 |
| `cmdbuf->state.gfx.render.layer_count`（`calc_enabled_layer_count()`） | 1（非分层/非 multiview） | **M2**：layer 偏移 ⇒ 同内容被画到多个 x |
| `td_count = DIV_ROUND_UP(layer_count, MAX_LAYERS_PER_TILER_DESC)`（v12 `MAX_LAYERS_PER_TILER_DESC = 8`） | =1（layer_count ≤ 8） | **M2**：多 tiler descriptor 路径（**它会改变 heap op 序列**，与卡死族同域） |

⇒ **请先量"面板数"**（连拍帧上直接数）。它是**零成本**的，而且能把候选从 6 条压到 1~2 条：
- 面板数 = 3 且 `PANVK_IR_PASS_COUNT` = 3 ⇒ M3 第一；
- 面板数 = 2 ⇒ 优先怀疑 **layer/td=2**（M2）或"同一帧被画两次"（M3/M6）；
- 面板数是"很多、随帧变化" ⇒ 更像 **M6（应用侧 blit/rect）** 或 **M5（黑被画上去）**。

---

## 1. 仍可产生该形态的机制清单（逐条：机制 / 落点 / 与已知事实的关系 / 证伪手段）

### M1 — **fb extent 被"最小附件"压低**（fb 比 surface 小 ⇒ 超出区域保持旧内容）

- **机制**：`cmd_init_render_state()`（`vulkan/panvk_vX_cmd_draw.c`，v81 时读到的 `att_width/att_height` 段，约 `:649-690`）对每个绑定附件取 **MIN**：
  `att_width = MIN2(att_width, iview->vk.extent.width)`（z/s 同理），随后
  `if (render->bound_attachments) { layout.width_px = att_width; layout.height_px = att_height; } else { 用 ra_px }`。
  ⇒ **只要有一个绑定附件比 WSI 图像小**（小中间 RT、MS shadow、子视图、或 app 传了较小的 `VkRenderingInfo` 附件），**整个 fb 就按更小尺寸渲染**；其余区域**本 pass 不写**。
- **与已知事实**：✅ **与"毒色填满后没有品红"完全自洽**——因为 app/SF 很可能**先全屏清黑**（或上一帧/别的 pass 写过），没被本 pass 覆盖的区域就是**被写过的黑**。⇒ 这一条**不因"无品红"而出局**，反而是最省事的解释之一。
- **证伪**：打印 `layout.width_px/height_px`、`ra_px{min,max}`、**每个附件的 `iview->vk.extent`**、WSI 图像 extent ⇒ 四者一比即知（见 §2-#2）。

### M2 — **layer_count / td_count / layer_offset 算错** ⇒ 同一内容被画到多个 x 位置（"左一份右一份"）

- **机制**：本树对"分层渲染"的支持是**每个 layer 一段横向区域**（tiler descriptor 里 `layer_offset`/`remaining_layers` 的 8-bit 打包），入口与计数在：
  `calc_enabled_layer_count()`（`csf/panvk_vX_cmd_draw.c:1074-1078`）、`td_count = DIV_ROUND_UP(layer_count, MAX_LAYERS_PER_TILER_DESC)`（`:1092-1093`、`:1258-1259`）、`prepare_layer_count_inherited_ctx()` 把 `layer_count`/`td_count` 写进 subqueue ctx（`:1173-1220`）。
  若 `layer_count` 被算成 >1（app 用了 layered/array 图像、`viewMask`/multiview、或 **MobileGL 的 framebuffer 仿真**），而实际只有一份内容 ⇒ **第二份会被画到另一个 x 偏移** ⇒ 左右两份 + 中间未覆盖的直边带。
- **与已知事实**：✅ **与"卡死耦合"自洽**：`td_count > 1` 时会走**多 tiler descriptor** 路径，而 `cs_frag_end`（`FRAGMENT_COMPLETED` heap op）**只在 `td_count > 1` 时发射**（v81 已确认）⇒ **heap op 序列改变** ⇒ 正是我们之前看到的卡死/时序敏感区。v91 `timeout=1` 与此吻合。
- **证伪**：打印 `layer_count` / `td_count` / `VkRenderingInfo.layerCount` / `viewMask`；并数面板数（§0）。

### M3 — **IR pass / spill 写回错位**（"同一帧被画两次"的候选之一）

- **机制**：`get_fb_descs()`（`csf/panvk_vX_cmd_draw.c:1857-1935`）按 `PANVK_IR_PASS_COUNT` 循环：非最终 pass 用 `render->fb.spill.store`，最终 pass 用 `render->fb.layout`（`:1872-1874`、`:1886-1887`）；load 侧 `ir_pass == FIRST ? &fb.load : &fb.spill.load`（`:1895-1897`）。
  ⇒ 若 **spill 图像的 extent/row_stride/offset 与主 RT 不一致**，中间 pass 的结果在最终 pass **load 回来时会错位** ⇒ 内容重复 + 直边。
- **与已知事实**：⚠️ MR!44816 只排除了"spill **对齐**扩张 tiling area"（v81 实测楔形不变），**没有**排除"spill 的尺寸/偏移/load 区域本身算错"。但**前提是 `PANVK_IR_PASS_COUNT > 1`** —— v12 上很可能是 **1**（则 M3 直接出局）。
- **证伪（零成本先做）**：`grep -n "define PANVK_IR_PASS_COUNT" -A4 panvk_cmd_buffer.h`；若 =1 ⇒ **M3 出局**，不必再查。若 >1，再打印每个 pass 的 `fbd_info.fb` 指针（`&fb.layout` vs `&ir_fb_layout`）与 spill extent。

### M4 — **MSAA shadow / resolve 目标按错误 extent 写**（pitch 正确也会错位）

- **机制**：`image->ms_imgs[]`/`ms_views[]` 是 `VK_EXT_multisampled_render_to_single_sampled` 的影子图（上游 MR!43484 修的正是 **AHB/ANB 路径没建 MS shadow** ⇒ NULL 解引用）。若影子图的 extent/stride 与单采样 AHB 不一致，**resolve 会把内容写到错误位置**。
- **与已知事实**：⚠️ 探针 `mode=ahb` 是**单采样 64×64** ⇒ **不能覆盖**"2376×1080 + MS/resolve"组合 ⇒ 与探针 PIXEL PASS **不冲突**。
- **证伪**：打印 `samples` / `resolve mode` / 是否存在 ms shadow；或跑一个 `samples=1` 的强制对照（app 侧若能设）。

### M5 — **GPU 真的画了黑**（clear 区域错 / 某个 draw 把黑覆盖上去）

- **机制**：① `render_area_px` 若比 image 大 ⇒ clear 覆盖到别处；② app 自己画的黑矩形（letterbox/UI）；③ 某个 draw 的 viewport/scissor 为黑。
- **与已知事实**：✅ "无品红"**正好支持**"黑是被写过的"；但**内容重复**这条 M5 解释不了（需与 M2/M3 组合）。
- **证伪**：连拍帧看黑带是否**随视角/内容变化**（app 画的黑随之变、clear 的黑稳定）；打印每个 pass 的 `render_area_px` 与 clear 目标。

### M6 — **应用侧（MobileGL/ZL2）自己的 blit/rect 计算**（不在我们代码内，但同级候选）

- **机制**：GL→VK 翻译层常自己维护 framebuffer 对象、做 `glBlitFramebuffer`/`glCopyTexSubImage` 式的 rect 拷贝；**rect 用一个错误的 pitch/offset**（在它自己的纹理上）就会产生"面板 + 重复 + 黑边"，而**驱动侧一切正确**。
- **与已知事实**：✅ 与"我们已排除驱动侧布局"完全兼容；✅ 与"探针四模式全 PASS"兼容（探针不走 MobileGL）。
- **证伪（信息量最高）**：见 §2-#1——**用探针在 2376×1080 下跑一个全屏画面 + 交换链**。探针不复现 ⇒ M6 优先；探针复现 ⇒ 驱动侧复现成功，M1/M2 加日志即可定。

### M7（补充）— **present 时取了错误的 image_index / 复用了上一帧的 AHB**

- **机制**：`queue_present` 用 `chain->images[image_index].ahb`；若 `image_index` 与"实际渲染的那张"不一致，present 出的是**别帧/别的 image** ⇒ 表现为"内容与当前视角不符/重复"。
- **与已知事实**：⚠️ 与"面板直边"不太吻合（更可能是整帧错位而非面板），但**与"左右两份相似场景"有一点像**（相似=相邻帧）。
- **证伪**：打印 `image_index` 与 acquire 返回值、渲染目标 image 的 handle 是否一致（1~2 行）。

---

## 2. 可上机的判别实验（按"信息量/代价"排序；你只需跑一轮）

| 序 | 实验 | 代价 | 一次切开什么 |
|---|---|---|---|
| **1** | **探针放大到真尺寸**：`mode=win`（或 `mode=ahb`）改成 **2376×1080**，画"棋盘+渐变+全屏三角形"，present 后读回 | **零驱动改动**（探针已有四模式） | **驱动 vs 应用（M6）**：不复现 ⇒ MobileGL 侧优先；复现 ⇒ 驱动侧复现成功，转 #2 |
| **2** | **一次性 fb 对账日志**（10~15 行，`PANVK_DEBUG(DUMP)` 或 `kbase_diag` 门控，限每帧一次）：`layout.width_px/height_px`、`render_area_px{min,max}`、`tiling_area_px`、**每个附件的 `iview->vk.extent`**、`layer_count`、`td_count`、`PANVK_IR_PASS_COUNT`、`samples`、每个 RT 的 `pan_image` extent 与 `row_stride_B`、以及 present 的 `image_index` | 低（一个函数里加一段，不触碰任何发射逻辑） | **M1 / M2 / M3 / M4 / M7** 一次全查（全部是"读值对比"，不改行为） |
| **3** | **env 对照**（零代码）：`PANVK_DEBUG=no_afbc`（若还怀疑 tiling）、`PANVK_DEBUG=linear`（强制线性）、`PANVK_DEBUG=no_crc`（排除 CRC/事务消除路径）、以及关闭任何 IR 的开关（若有） | 零 | 排除 AFBC / CRC / IR 三类 |
| **4** | 数面板（§0）+ 连拍看黑带是否随内容变化 | 零 | M5 vs 其它 |
| **5** | 换一个非 MobileGL 的 VK 应用/例程（或我们的探针做长时运行 + 多帧） | 零 | 进一步确认 M6 |

**上机前的三条零成本 grep（先在服务器确认，我可代做）**：
```bash
C=/root/zenithblue/work/mesa/src/panfrost/vulkan/csf
grep -n -A4 "define PANVK_IR_PASS_COUNT" $C/panvk_cmd_buffer.h     # v12 是 1 还是 3（决定 M3 生死）
grep -n -A10 "calc_enabled_layer_count" $C/panvk_vX_cmd_draw.c     # layer_count 来源
grep -n "layer_offset" $C/panvk_vX_cmd_draw.c                      # layer → x 偏移的打包点
```

---

## 3. 我的排序（如果只能选一条）

1. **M1（fb extent = 最小附件）**：与"无品红"最自洽、最容易证伪（一条日志）、而且**能单独解释"面板 + 直边黑带"**（不能解释重复）。
2. **M2（layer/td）**：能解释**内容重复**，且与"卡死耦合"（`cs_frag_end` 只在 td>1 发）自洽 ⇒ 如果 v91 又卡、且面板数是 2/8 的倍数，优先它。
3. **M6（应用侧）**：探针放大实验是**唯一能一刀切开**的手段，且零成本 ⇒ 建议**与 #2 同时做**。
4. M3（IR）先看 `PANVK_IR_PASS_COUNT` 的值即可生死；M4/M5/M7 作为次级。

---

## 4. 未验证 / 限制

1. 全部为**代码级**推断；本轮未做设备实验，§2 都是提案。
2. **M1 的行号**取自 v81 时期的 `vulkan/panvk_vX_cmd_draw.c`（该文件在 v88+ 已被改过）⇒ 请**按函数名 `cmd_init_render_state`** 定位 `att_width/att_height` 的 MIN2 段。
3. **`PANVK_IR_PASS_COUNT` 的实际取值未验证**（我未 grep）⇒ M3 的生死未定；§2 已给出 grep。
4. "面板数 = layer_count/td_count"是**假设**（建立在"每个 layer 一段横向区域"的读码印象上，v81 时看到 8-bit 打包但**未逐个核对 x 偏移公式**）⇒ 建议与 §2-#2 的日志一起验证。
5. 我**没有**检查 MobileGL 的源码（不在本工作区）⇒ M6 只是同级候选，无法给出其 file:line。
6. M7（present 取错 image）**未**在代码里核对 acquire/present 的索引一致性 ⇒ 属"待查"，非结论。
