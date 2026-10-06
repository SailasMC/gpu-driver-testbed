# 61b — 对 Lead「AFBC vs pitch」新假设的审读：三条回答 + 下一步

日期：2026-10-06 · 只读分析 · 行号基线 = 我读到的当前树（`vk_android.c` 已被 Lead 改到 v88/v89，按函数名定位）
配套：`61-pitch-audit.md`（pitch 链）· `60b-hang-playbook.md`（卡死侧）

---

## 0. 先给结论（含对你结论的**加强**与一处**推回**）

| 你的结论 | 我的审读 |
|---|---|
| "pitch 三者一致 ⇒ 不是逐行漂移" | ✅ **同意，而且我可以给出更强的代码证据**：线性 handler **逐字采用**显式 pitch 并**硬校验对齐**，不满足就直接让 image 创建失败（`pan_mod.c:904-947`）。游戏能渲染 ⇒ GPU 的 stride **必然等于** 9536 ⇒ **pitch 被代码级排除**（不只是被测量排除）。见 §1 |
| "坏的只能是 AHB 里的内容本身" | ✅ 同意 |
| "底层可能按 AFBC 组织 ⇒ 头号假设" | ⚠️ **我推回一档**：这个 AHB 是**我们自己 `AHardwareBuffer_allocate` 的**、且声明了 `CPU_READ_OFTEN`，而 `AHardwareBuffer_lock(CPU_READ_OFTEN)` **成功了**并给出连贯的 2376×1080/stride 2384px ⇒ **gralloc 必须给 CPU 可映射的布局**，plain AFBC 基本被排除。另：`usage` 的位语义**不足以支持**该推断（见 §3）。建议把 AFBC 降到第 2~3 位，把"**GPU 只写了缓冲的一部分/写错区域**"提到前面 |

---

## 1. pitch 为何可以被**代码级**排除（新增证据）

`src/panfrost/lib/pan_mod.c:904-947`（`pan_mod_linear_init_slice_layout`，LINEAR modifier 的 slice 布局）：

```c
   const bool use_explicit_layout = layout_constraints && layout_constraints->wsi_row_pitch_B;
   unsigned align_mask = pan_linear_or_tiled_row_align_req(PAN_ARCH, props->format, plane_idx) - 1;
   …
   if (use_explicit_layout) {
      unsigned width_from_wsi_row_stride = layout_constraints->wsi_row_pitch_B / fmt_blksize_B;
      if (!util_format_is_compressed(props->format))
         width_from_wsi_row_stride *= util_format_get_blockwidth(props->format);
      if (width_from_wsi_row_stride < mip_extent_el.width) {
         mesa_loge("WSI pitch too small"); return false;            /* 建 image 失败 */
      }
      slayout->tiled_or_linear.row_stride_B = layout_constraints->wsi_row_pitch_B;   /* 逐字采用 */
      if (slayout->tiled_or_linear.row_stride_B & align_mask) {
         mesa_loge("WSI pitch not properly aligned"); return false; /* 建 image 失败 */
      }
      slayout->offset_B = layout_constraints->offset_B;
      if (slayout->offset_B & align_mask) { mesa_loge("WSI offset not properly aligned"); return false; }
   }
   …
```
- **没有"取整/对齐后改用别的 pitch"这一支**：要么逐字用 `wsi_row_pitch_B`，要么**直接失败**。
- 该值一路传到 RT 描述符：`pan_desc.c:259` `*row_stride = slayout->tiled_or_linear.row_stride_B;`（`:318` `cfg.row_stride = row_stride;`）。
- ⇒ 你的日志说 pitch=9536 且**没有** `WSI pitch not properly aligned / too small` ⇒ **GPU 写行距 = 9536 = CPU 读行距** ⇒ **pitch 失配彻底出局**。

---

## 2. 问题 1：代码内能否判定"这个 AHB 是不是 AFBC"？**不能**——而唯一权威来源（u_gralloc）正是失败的那一个

**可用 API 的判定能力（都是"不能"）**：
| API | 有布局/modifier 字段吗 | 结论 |
|---|---|---|
| `AHardwareBuffer_describe()` | width/height/layers/format/usage/stride/rssi | ❌ 无布局信息 |
| `AHardwareBuffer_getUsage()` | 只有 usage 位 | ❌ 无压缩位（且位语义有歧义，见 §3） |
| `AHardDataSpace` | 色彩空间/range | ❌ 与内存布局无关 |
| **gralloc mapper**（`libgralloctypes` / AIDL `IMapper.getMetadata` / `getFormatLayout`） | **有**（format layout / modifier） | ✅ **唯一权威**，而它就是我们 `u_gralloc` 走的那条路 |

**我们调用 u_gralloc 的完整路径与失败分支**：
```
vk_android_get_ahb_layout()                        vk_android.c（本文件）
 └─ vk_gralloc_to_drm_explicit_layout(...)
      └─ u_gralloc_get_buffer_basic_info(_gralloc, handle, &info)
           ├─ 失败 → mesa_loge("u_gralloc_get_buffer_basic_info failed")   :152
           │        return VK_ERROR_INVALID_EXTERNAL_HANDLE
           └─ 成功 → out->drmFormatModifier = info.modifier               :183
                     out_layouts[i].rowPitch = info.strides[i]            :187
 └─ 我们加的失败分支（Lead 的日志就在这里）：
      mesa_logw("u_gralloc cannot describe AHB (%ux%u fmt=0x%x stride=%u usage=0x%llx); "
                "using self-described LINEAR layout", …)                   :866   ← 完整格式串已核对
      → vk_android_ahb_layout_from_desc()（LINEAR 回退）
```
- `_gralloc = u_gralloc_create(U_GRALLOC_TYPE_AUTO)`（`vk_android.c:63`）；**u_gralloc 确实被编进 .so**（`src/vulkan/runtime/meson.build:64`：`vulkan_lite_runtime_deps += [dep_android, idep_u_gralloc]`）⇒ **不是"没编进去"**。
- **"为什么失败"在 u_gralloc 内部**（`U_GRALLOC_TYPE_AUTO` 会依次尝试 gralloc4(AIDL mapper)/gralloc1/**fallback**，并各自打印自己的原因）⇒ **从我们这份代码无法判定**是符号/服务不可达、还是后端不支持该 handle。
  ⇒ **需要你补一条证据**：`u_gralloc_get_buffer_basic_info failed` **之前紧邻的 u_gralloc/gralloc 自己的日志行**（它会说明是 AIDL 服务不可达、dlopen 失败、还是 `getMetadata` 返回错误）。
- **零代码 A/B（强烈建议先做）**：`PANVK_GRALLOC_NO_FALLBACK=1`（`vk_android.c:861-863`）⇒ 回退被禁用。
  - 若 **swapchain 创建直接失败/黑屏** ⇒ 证实"u_gralloc 永不成功、回退是唯一让画面出来的原因"（你的前提成立）；
  - 若**仍能出画面** ⇒ 说明 u_gralloc 有时成功（与"一条成功日志都没有"矛盾）⇒ 直接推翻前提。

---

## 3. 问题 3：`usage=0x303` 与 `GPU_DATA_BUFFER` —— 不能据此推断 AFBC

- 解码：`0x303 = 0x3 | 0x100 | 0x200` = `CPU_READ_OFTEN(3)` | `GPU_SAMPLED_IMAGE(0x100)` | `GPU_FRAMEBUFFER(0x200)`。
- `AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER = 1UL << 0`；而 **`CPU_READ_OFTEN = 3` 本身就包含 bit0**（3 = bit0|bit1），NDK 头文件**明确说明** `CPU_READ_RARELY` 与 `GPU_DATA_BUFFER` **同值、无法区分**。
  ⇒ **"0x303 里没有 GPU_DATA_BUFFER"这个判断在语义上不成立**（bit0 是置位的，只是含义两义）；更关键的是 **`GPU_DATA_BUFFER` 与"是否允许 AFBC"没有关系**（它描述的是"当作原始数据缓冲用"，不是布局）。
- **对 AFBC 真正有意义的是 CPU 那两位**：`CPU_READ/WRITE` 声明 ⇒ gralloc **必须**给 CPU 可访问的内存；Android 上普通 AFBC 分配**不可 CPU 映射**（mapper 会拒绝 lock）。我们有 **运行时反证**：`AHardwareBuffer_lock(..., CPU_READ_OFTEN, ...)` **成功**并给出连贯数据。
  ⇒ **有依据的判断：`usage=0x303` 这组位 + 锁成功，是"线性/可映射"的证据，而不是 AFBC 的证据。**（`0x100|0x200` 单看确实是 AFBC 常见组合，但加上 `CPU_READ_OFTEN` 之后不成立。）

---

## 4. 问题 2：最小绕开/判定方案（按风险与性价比排序）

| # | 方案 | 风险 | 说明 / 期望 |
|---|---|---|---|
| **W1** | **用缓冲区本身当 oracle 判定布局**（零代码）：CPU 往 AHB 写已知图案（逐行递增帧号/棋盘）→ 让 **GPU 采样**该 AHB 输出到一张**普通 linear image** → CPU 读回比对 | **0** | 绕开不可用的 metadata。忠实 ⇒ **布局是 linear**（AFBC 假设被否，转"GPU 只写了部分区域"）；块状/错位 ⇒ 布局确非 linear。**这是唯一能真正判定 AFBC 的实验** |
| **W2** | `PANVK_GRALLOC_NO_FALLBACK=1` 跑一小段 | 0 | 见 §2；证实/推翻"u_gralloc 永不成功" |
| **W3** | **主动初始化 AHB**：`AHardwareBuffer_allocate` 后立刻 `lock`→写毒色/黑→`unlock`，再交给渲染 | 低 | **直接干掉"品红未初始化带"这个变量**，并把"未初始化内存"从嫌疑名单里移除；也能让"哪些区域 GPU 真的写了"变得可见（黑底上只有写过的区域有内容 ⇒ 一眼看出 fb extent 问题） |
| **W4** | 给 AHB 加 `CPU_WRITE_OFTEN`（或 `allocateWithUsage`） | 低 | 进一步钉死"必须 CPU 可映射"；对"是否抑制 AFBC"是唯一可用的**倾向性**手段（Android 无"禁止压缩"公开位） |
| **W5** | 直接用 `ANativeWindow_lock` 的窗口缓冲当渲染目标（你的 (a)） | **中高** | 仍要过同一条 `vk_android_get_ahb_layout()` ⇒ 仍走回退；且缓冲由 SF/gralloc 分配，布局**更不受我们控制** ⇒ 收益不确定，**不建议先做** |
| **W6** | `PANVK_GRALLOC_ANY_USAGE=1`（你的 (c)） | 0 | 对"我们自己分配、已声明 CPU"的 AHB **无影响**（不改变走哪条路）⇒ 不解决问题 |

---

## 5. 我建议的下一步（按性价比；**都只读/零代码，除 W3 需一行改动**）

1. **W3（主动初始化）**：最低成本、直接移除"品红=未初始化"这一变量，并且**让"GPU 到底写了多大区域"可视化**——这正好检验我现在排第 1 的候选：**"GPU 只写了缓冲的一部分/写错区域"**（面板 + 黑带 + 未初始化带的最自然解释，且与 fb extent/tiling area/scissor 这一族完全吻合）。
2. **W1（oracle 判定）**：只有当 W3 显示"GPU 写了整幅、内容却错位"时才需要它来定布局。
3. **W2 + 补一条 u_gralloc 失败原因日志**：一条日志就能把"为什么 gralloc 描述不了这个 AHB"钉死（决定后续是修 mapper 可达性，还是彻底放弃 metadata 路线）。
4. 若 W3 显示"只有部分区域被写" ⇒ **回到 fb extent / tiling area / scissor 这条线**（与 bug 3 的老线索合流），此时应查 WSI image 的 `extent` 与 fb layout `width_px/height_px`、`render_area_px`、`tiling_area_px` 是否一致（`panvk_vX_cmd_draw.c` 的 `cmd_init_render_state`/`get_fb_descs` 一带）。

---

## 6. 未验证 / 限制

1. 全部为**代码级**审读；**未在设备上做任何实验**（W1/W2/W3 都是提案）。
2. "AFBC 不可 CPU 映射 ⇒ 锁成功即线性"是**平台经验+运行时证据**的合成判断，**不是** NDK 明文保证（个别 gralloc 可能对压缩缓冲提供 CPU 别名/解压路径）⇒ 这正是 W1 存在的理由。
3. `u_gralloc` 失败的具体原因**未确定**（需要它自己那行日志 + §2 的 A/B）。
4. `usage` 的 bit0 两义性按 NDK 头注释判定；**未查看本设备 `hardware_buffer.h` 的实际数值**（若该版本已把 `GPU_DATA_BUFFER` 挪位，§3 的结论需要重算——请附一行 `grep -A2 GPU_DATA_BUFFER hardware_buffer.h` 即可定案）。
5. 我**未**检查"GPU 只写部分区域"这条候选的代码细节（WSI image extent vs fb layout）——本轮预算用尽，若 W3 指向那边，请告知，我下一轮直奔那里。
