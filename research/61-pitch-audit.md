# 61 — 楔形新方向：WSI pitch / 布局失配审计（配合 v87 高清现场）

日期：2026-10-06 · 树 `/root/zenithblue/work/mesa`（未提交；**行号基线 = 本次只读时的当前树**）
只读分析；本文件不含代码改动，修复候选在 §5（纯提案）。配套：`60b-hang-playbook.md`（卡死侧，本文件与之无关）

---

## 0. 一句话 + 结论先行

现场（v87：秋树/河/云正确、画面被切成**直边黑带分隔的面板**、左侧一份右侧又一份相似场景、底部粉色/品红渐变带）与**"生产者按 pitch A 写、消费者按 pitch B 读"**高度吻合：每个目标行由"源行尾 + 下一源行头"拼成 ⇒ **左侧+右侧两份内容 + 固定 x 处的直边缝**；读指针走进未分配区 ⇒ **黑带**；gralloc 未初始化内存的经典毒色是**品红** ⇒ **粉色渐变带**。

**我在代码上找到了"双来源 pitch"这一结构性问题**（不是猜的）：**driver 侧用的 pitch 与 present 侧用的 pitch 来自两个不同的 API**，二者没有任何一致性校验：

| 侧 | pitch 来源 | 单位 | 位置 |
|---|---|---|---|
| **driver 写**（Vulkan image 布局） | `u_gralloc_get_buffer_basic_info().strides[i]`（u_gralloc 路径）**或** 我们回退里的 `desc->stride * bpp` / `lockPlanes.rowStride` | 字节 | `src/vulkan/runtime/vk_android.c:187`（u_gralloc）/ `:776-798`（回退）→ `panvk_image.c:538`（`wsi_row_pitch_B`）→ `pan_mod.c:67-104,412-427` |
| **present 读**（CPU memcpy） | `AHardwareBuffer_describe().stride * 4` | 像素×4（硬编码） | `src/panfrost/vulkan/panvk_wsi.c:285-286` |

⇒ **只要这两个数不相等，就是"逐行漂移"的 pitch 失配**，且**没有任何日志会告诉你它们不相等**（两个数都在代码里算出来了，但从不一起打印）。

**并且有一处是"可证明"的不一致**：回退路径里若 `desc->stride == 0`，driver 会用 `lockPlanes` 探到的真实 pitch 建 image（`:778`），而 present 侧仍用 `desc.stride * 4 = 0` ⇒ **每一行都从第 0 行拷贝**（整屏糊成一条行）。这一分支的存在本身就说明作者见过 `stride == 0`。

---

## 1. WSI / AHB 布局的完整回退链（问题 1）

### 1.1 调用链

```
vkCreateSwapchainKHR
  └─ panvk_android_swapchain_create()                    panvk_wsi.c:340+
       ├─ ANativeWindow_setBuffersGeometry(RGBA_8888)     :357-359   ← 返回值未检查
       └─ for each image:
            ├─ AHardwareBuffer_allocate(&desc,&ahb)       :375       ← desc.stride 未设（NDK：allocate 忽略 stride）
            ├─ VkExternalMemoryImageCreateInfo(ANDROID_HARDWARE_BUFFER)  :380-383
            ├─ VkImageCreateInfo{ .tiling = VK_IMAGE_TILING_OPTIMAL, 格式 = 应用请求的 imageFormat }  :384-393
            └─ vkCreateImage → vk_android_get_ahb_layout()  vk_android.c
                 ├─[首选] vk_gralloc_to_drm_explicit_layout()
                 │     └─ u_gralloc_get_buffer_basic_info(&info)   vk_android.c:~150
                 │        out->drmFormatModifier = info.modifier             :183
                 │        out_layouts[i].rowPitch = info.strides[i]          :187   ← 字节
                 └─[回退] vk_android_ahb_layout_from_desc()   :744+
                        usage 必须声明 CPU 可访问（否则拒绝）                 :~762-772
                        row_pitch = desc->stride * bpp                       :776
                        若 ==0 → vk_android_ahb_probe_row_pitch()            :724-742（lockPlanes.rowStride，字节）
                        row_pitch < width*bpp → 拒绝                          :784-788
                        drmFormatModifier = DRM_FORMAT_MOD_LINEAR             :796
```

### 1.2 "回退成 LINEAR 之后，pitch/stride 按什么算？会不会与 getStride 不一致？"

- **回退路径**：`row_pitch = (uint64_t)desc->stride * bpp`（`vk_android.c:776`），`bpp` 由 **AHB 的 `desc->format`** 映射得到（`vk_android_ahb_format_to_drm()`，`~:697-730`）。NDK 规定 `AHardwareBuffer_Desc.stride` 是**像素**单位 ⇒ `× bpp` 得字节 ✓；`stride==0` 时用 `lockPlanes().rowStride`（**字节**，含 gralloc 对齐）✓。
- **答案**：**不必然一致**，而且有两个独立的不一致点：
  1. **双来源**：driver 用 `info.strides[0]`（u_gralloc，字节），present 用 `desc.stride*4`（describe，像素×4）。**同一个缓冲区、同一行，两个数字，互不校验。**
  2. **`desc.stride == 0` 时**：driver 走 probe（真 pitch），present 走 `0*4`（**恒 0**）⇒ 必然错。
- **"我们用 width*bpp 而实际分配用对齐后 pitch（或反之）"**：**代码里没有 `width*bpp` 当 pitch 的写法**（唯一的 `width*bpp` 是**下界校验** `:784`，不通过就拒绝）⇒ 这一具体形态**可以排除**；真正的问题形态是上面两点。

### 1.3 present 侧（`panvk_wsi.c:261-295`）的三个硬编码假设

```c
   dev->dispatch_table.DeviceWaitIdle(swapchain->device);          /* 每帧全设备同步 */
   ANativeWindow_lock(chain->window, &win_buf, NULL);
   AHardwareBuffer_describe(chain->images[image_index].ahb, &desc);
   AHardwareBuffer_lock(..., CPU_READ_OFTEN, ..., &ahb_data);
   copy_h = MIN(win_buf.height, desc.height);
   copy_w = MIN(win_buf.width,  desc.width);
   for (y) memcpy(win_buf.bits + y*win_buf.stride*4,
                  ahb_data      + y*desc.stride*4, copy_w*4);
```
- **硬编码 `*4`（两处）与 `copy_w*4`**：假设两侧都是 4 B/px。`ANativeWindow_setBuffersGeometry(..., WINDOW_FORMAT_RGBA_8888)` 与 `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` 都是 4 B/px ⇒ **在几何设置成功的前提下是对的**；但 **`win_buf.format` 从未校验**（若合成器给了 RGB_565 或 geometry 调用失败 ⇒ 2×/错位）。
- **`win_buf.stride` 与 `desc.stride` 都按"像素"解释** —— 与 NDK 文档一致；**若某个 gralloc HAL 按字节报 `desc.stride`（不合规但存在），就会 4× 错**（这正是"看起来像同一画面被压缩/重复出现"的形态）。
- **性能红旗（与本 bug 无关但顺带记录）**：每帧 `DeviceWaitIdle` + 整帧 CPU 锁+逐行 memcpy ⇒ 这就是 v87 只有 ~26 FPS 的一个直接原因，也意味着**每一帧都经过这份 pitch 代码**（所以 pitch 错会表现为"稳定同形"的伪影，而不是偶发）。

---

## 2. 问题 2：modifier / tiling 是否可能与实际分配不符？

- **本树对显式 modifier+pitch 是"要么严格遵守，要么建 image 失败"**：
  `panvk_image.c:535-541` 把 `pPlaneLayouts[plane].{offset,rowPitch,arrayPitch}` 原样转成 `pan_image_layout_constraints{offset_B, wsi_row_pitch_B, wsi_array_pitch_B}`；`pan_layout.c:85` 判定 `use_explicit_layout = (wsi_row_pitch_B != 0)`，且若 `depth>1 || nr_samples>1 || dim!=2D || nr_slices>1 || crc` 则**直接 return false**（⇒ image 创建失败，**不会静默改用自算 pitch**）。`pan_mod.c:67-104 / :412-427` 的线性/tiled handler 都用 `wsi_row_pitch_B` 算 slice stride。
  ⇒ **"查询出来的 modifier 与实际绑定不一致"在 AHB 路径上没有找到静默旁路**（这是好消息）。
- **仍需注意的两个次级路径**：
  1. `panvk_wsi.c:391` 交给 `vkCreateImage` 的是 **`VK_IMAGE_TILING_OPTIMAL`**（不是 `DRM_FORMAT_MODIFIER_EXT`），modifier 由 `VkExternalMemoryImageCreateInfo` + 布局函数决定 ⇒ **最终绑定的是哪个 modifier 取决于 `vk_android_get_ahb_layout()` 的返回值**，而该函数**在 `info.modifier` 非法时可能给我们 LINEAR**（§1.1 回退）⇒ 若应用/驱动两侧对"这张图是 LINEAR 还是 AFBC"的理解不同，就会是**布局级**错配（症状=块状重复/黑块，而非逐行漂移）。
  2. **格式一致性**：`panvk_wsi.c:388` 用 `pCreateInfo->imageFormat`（应用请求，例如 `B8G8R8A8_UNORM`）建 image，而 AHB 是 `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM`。二者在 RGBA/BGRA 上会**互换红蓝通道**（颜色错，不是几何错）；但若应用的 imageFormat 与 4 B/px 不同族（如 565），则 `*4` 的假设同时失效 ⇒ **派生出几何错**。
- **可 grep 的判据（问题 3）**：
  ```bash
  # ① 我们的回退有没有被走到、它算出多少 pitch（mesa_logi，已在码里）
  adb logcat -d | grep -n "AHB layout fallback:"
  # ② 有没有 AHB 布局失败（会 reject/建 image 失败）
  adb logcat -d | grep -nE "AHB layout fallback: (usage|format|stride==0|pitch)"
  # ③ 应用请求的 imageFormat vs AHB 实际 format（**当前没打印，见 §5-F1**）
  # ④ pandecode 侧：RT 描述符的宽/stride（**v12 的 RT 描述符是否有显式 stride 字段我未验证**）
  adb logcat -d | grep -nE "RT|Width|Stride|stride|pitch"   # pandecode 未开时不适用
  ```
  ⇒ **结论**：**目前唯一现成的对账入口是 `AHB layout fallback:` 那条日志**（它只在回退路径打印）。若 logcat 里**没有**这条 ⇒ 走的是 u_gralloc 路径，而**该路径的 pitch 完全没有日志** ⇒ 必须加 §5-F1 才能对账。

---

## 3. 现场形态与"pitch 失配"的定量对应（为何我认为方向对）

| 现场特征 | pitch 失配如何产生 | 备注 |
|---|---|---|
| 直边黑带分隔的面板 | 目标行由"源行尾 + 下一源行头"拼成 ⇒ 在固定 x 处出现**笔直竖缝**；缝的右侧是下一行的开头 ⇒ 看上去像"又一份相似场景" | ✓ 与"左一份右一份"吻合 |
| 内容重复 | 同上（相邻源行被拼进同一目标行） | ✓ |
| 黑带 | 目标行里没被写到的区域（源数据不足/geometry 不匹配） | ✓ |
| 粉色/品红渐变带 | 读指针越界进入未初始化 gralloc 内存（品红是经典毒色） | ✓（v82 帧也见过） |
| 与卡死无关 | pitch 错是**静态布局错**，不依赖同步 | ✓ 与 v87 不卡死一致 |

**若 pitch 比值恰为整数**（例如 2× 或 4×），还会表现为**周期性重复/压缩**——建议 Lead 在帧上量一下"重复周期"（像素）与图像宽度的比值，这是**不用改代码就能反推误差倍数**的证据（例如缝出现在 x≈W/2 ⇒ 源 pitch ≈ 2×目标 pitch）。

---

## 4. 我**没有**发现的东西（诚实排除）

- ❌ **没有** `width*bpp` 被当作 pitch 使用的写法（唯一 `width*bpp` 是下界校验，`vk_android.c:784`）。
- ❌ **没有**在 AHB 路径上发现"显式 pitch 被静默忽略"的旁路（不满足条件时 `pan_layout.c:88-93` 直接 return false ⇒ 创建失败）。
- ❌ 本文件**未**发现 tiling/AFBC 与线性之间的**静默**混用；但**未验证** `vk_android_get_ahb_layout()` 在 u_gralloc 成功时返回的 `info.modifier` 与设备实际分配是否一致（需要 §5-F1 的日志），也未验证 v12 RT 描述符是否有可对账的 stride 字段。

---

## 5. 最小修复候选（按风险排序；**均先需 F1 的读数**）

| 候选 | 内容 | 风险 | 说明 |
|---|---|---|---|
| **F1（零风险，先做）** | 在**同一帧**打印两个来源的 pitch：① u_gralloc 路径里 `mesa_logi("AHB layout: mod=0x%llx pitch=%u (u_gralloc)", info.modifier, info.strides[0])`（`vk_android.c:187` 旁）② present 里打印 `win_buf.{width,height,stride,format}` 与 `desc.{width,height,stride,format}`（`panvk_wsi.c:279` 旁，限一次/每秒） | **0** | 一行日志就能判定"两个 pitch 是否相等"；这是**唯一**能把方向从"假设"变"结论"的手段 |
| **F2（最小正确修复）** | 创建 swapchain image 时**把 driver 实际用的 pitch 缓存到 `chain->images[i].pitch_B`**，present 的 memcpy 用它（而不是重新 `describe`）⇒ **消灭双来源**；顺带修掉 `desc.stride==0` 分支的必然错 | 低 | 语义等价于"让消费者用生产者告诉它的 pitch"；不触碰渲染路径 |
| **F3（防御性）** | present 里用真实 bpp 替代硬编码 `*4`（分别按 `win_buf.format` 与 `desc.format` 求 bpp）、校验 `ANativeWindow_setBuffersGeometry` 返回值与 `win_buf.format` | 低-中 | 防"2 B/px 或几何设置失败"这一类；若 F1 显示两侧 format 都是 8888，则此项收益小 |
| **F4（架构，高风险）** | **去掉每帧 CPU memcpy present**（改用 SF 直接消费 AHB 的路径），顺带去掉 `DeviceWaitIdle` | **高** | 一次性修掉"第二份 pitch"与性能红旗，但改动大、跨 WSI 架构，须单独实验 |

**推荐顺序**：**F1（今晚就能定案）→ 若两个 pitch 不等则 F2 → F3 视 F1 结果 → F4 另开**。

---

## 6. 未验证 / 限制

1. 全部为**代码级**分析；**未在设备上验证**任何 pitch 数值（这正是 F1 的目的）。
2. 未验证 `u_gralloc_get_buffer_basic_info().strides[]` 在本设备上**是否等于** `AHardwareBuffer_describe().stride * bpp`（这是本报告的核心待测量）。
3. 未验证 `desc.stride` 在本设备/该 gralloc 上**是否真的按像素**（NDK 如此规定，但本报告把它列为待确认项）。
4. 未验证 v12 的 RT/FB 描述符里是否有可对账的显式 stride 字段（未读 genxml 的 RT 结构）⇒ §2 的 ④ 只是"可试"的 grep。
5. 未量化"重复周期/宽度"比值（需要帧的像素测量，设备侧归 Lead；这是零成本的反推手段）。
6. **行号基线**：本文件行号取自**当前树**（`panvk_wsi.c` mtime 01:25、`vk_android.c` 13:19、`panvk_image.c` 未改动）；若 Lead 已再次编辑，请按函数名定位（本文件每处都给了函数名）。
