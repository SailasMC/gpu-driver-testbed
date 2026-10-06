# 61c — 探针结果后的分叉口：那个 2376×1080 的 AHB **是谁的**，以及如何判定它的真实布局

日期：2026-10-06 · 只读分析（本轮未再读新代码，依据 = 我上一轮已逐行核对的 `panvk_wsi.c` / `vk_android.c` + 你的探针日志）
配套：`61-pitch-audit.md`、`61b-afbc-vs-pitch.md`

---

## 0. 先说一处需要纠正的前提（这条改变了整个分叉口）

你的表述是"游戏的那个 AHB 是**通过 ANativeWindow/SurfaceFlinger 拿到的窗口缓冲**"。按代码，**不是**：

- `panvk_android_swapchain_create()`（`panvk_wsi.c:366-380`）**自己**为每个 swapchain image 调
  `AHardwareBuffer_allocate(&desc, &chain->images[i].ahb)`，descriptor 就是
  `{.width=extent.width, .height=extent.height, .layers=1, .format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
    .usage=GPU_COLOR_OUTPUT|GPU_SAMPLED_IMAGE|CPU_READ_OFTEN}`（**= 0x303**，与日志一致）。
- present 路径（`panvk_android_swapchain_queue_present`，`panvk_wsi.c:261-295`）里 `AHardwareBuffer_describe()`/`lock()` 的对象，就是
  **`chain->images[image_index].ahb`** —— **即我们自己分配的那个**。你那条 `present: ahb 2376x1080 …` 日志打的正是它。

⇒ **结论：探针与游戏用的是同一条分配路径（都是 `AHardwareBuffer_allocate`）、同一组 usage、同一 format；唯一差别是尺寸（64×64 vs 2376×1080）与随之而来的对齐（stride 64px 恰好 vs 2384px 有填充）。**
⇒ 因此问题不是"窗口缓冲布局不受控"，而是收窄成一个**更尖锐、也更容易判定**的问题：
> **为什么同一个进程里，`AHardwareBuffer_allocate(fmt=0x1, usage=0x303)` 在 64×64 时 `u_gralloc` 能描述（modifier=0x0 LINEAR），在 2376×1080 时却描述不了？**
两种可能：(S) **尺寸/对齐相关**（2384px 的填充触碰了 u_gralloc 的某条假设/限制）；(C) **上下文相关**（分配时机、mapper 服务状态、内存压力、或该缓冲在 `describe` 之前被别的组件动过）。
⇒ **下面 §2 的"探针复现实验"能一刀切开 S 与 C**，而且**零驱动改动**。

---

## 1. 问题 1：能否在设备上判定那个 AHB 的真实布局？——能，而且按性价比排序如下

| 序 | 手段 | 能判定什么 | 代价/风险 |
|---|---|---|---|
| **A1** | **在失败分支里补 `AHardwareBuffer_lockPlanes()`**（复用现成的 `vk_android_ahb_probe_row_pitch()`，`vk_android.c:724-742`，它已经会返回 `planes.planes[0].rowStride`）并打印 `planeCount / rowStride / pixelStride / offset` | **可映射性**（压缩布局通常**无法 lock** ⇒ lock 成功本身就是"非压缩/可映射"的强证据）；`rowStride`（**字节**，含填充）可直接与 `desc.stride*4` 对账 | **3~5 行**，DIAG/常开皆可，风险 0 |
| **A2** | **直接调 mapper 的 `getMetadata`**（你说的 `PlaneLayoutComponentType`/`GET_FORMAT_LAYOUT` 那套）——**在同一个失败分支里**调一次并打印 modifier/compression | **权威布局**：modifier（0=LINEAR / AFBC / ...）。若这里**也**失败 ⇒ 说明失败在 **handle 本身**（不是 u_gralloc 的管线） | 中：需要能用 `native_handle`（探针 mode=mapper 已证明本进程可调 mapper）⇒ 把探针的调用搬进驱动即可；**建议先做成一个新的 probe 模式**（`mode=mapper` 已存在，加一个"打印 format layout"的分支），避免动驱动 |
| **A3** | **`AHardwareBuffer_getUsage` / `AHardDataSpace` / `AHardwareBuffer_describe`** | ❌ 都没有布局字段（已在 61b §2 论证） | 0，但无用 |
| **A4** | 在 present 里对同一个 AHB 再 `describe` 一次并与创建时对比 | 只查一致性，不查布局 | 0，边际 |

**排序建议：A1（3 行，先做）→ A2（用探针，不改驱动）→ 其余无用。**
**判据**（我的建议写法）：
- A1 **lockPlanes 成功且 `rowStride == desc.stride*4`** ⇒ 缓冲可映射且行距自洽 ⇒ **AFBC 假设基本出局**，火力转向"GPU 只写了部分区域/写错区域"（面板+黑带+品红未初始化的最自然解释）。
- A1 **lockPlanes 失败** ⇒ 缓冲**不可 CPU 映射** ⇒ 压缩布局（AFBC/UBWC）**高度可能** ⇒ 走 A2 取 modifier 定案。
- A2 若拿到 `modifier != 0` ⇒ **根因确定**：我们向 Vulkan 声明的 LINEAR 与真实布局不符（我们违约在先：usage 声明了 CPU 可读而 gralloc 仍给了压缩布局）。

---

## 2. ★ 我认为现在**最该做的一个实验**（零驱动改动、用你已有的探针）

**把探针的 `mode=ahb` 跑到 2376×1080、usage=0x303、fmt=0x1**，并让探针打印：
1. `AHardwareBuffer_describe()` 的 `stride`（看是否复现 2384 px 的填充）；
2. `u_gralloc` 是否成功、`modifier` 是多少（探针已有这条日志：`AHB layout from u_gralloc: modifier=… stride[0]=…`）；
3. A1 的 `lockPlanes` 结果。

**判读**：
- 若 **2376×1080 也失败** ⇒ **S（尺寸/对齐相关）**成立 ⇒ 可以在**隔离环境**里复现并修（例如 u_gralloc 对 `stride≠width` 的缓冲算错/拒绝），**不必再碰游戏**；
- 若 **2376×1080 成功（modifier=0）** ⇒ **C（上下文相关）**成立 ⇒ 那就不是布局问题，而是"游戏那个缓冲在分配后被打上了不同的元数据/被复用" ⇒ 火力转向渲染侧。

这个实验的价值在于：**它把"游戏现场"这个不可控变量变成可控变量**，而且完全复用你已有的工具链。

---

## 3. 问题 2：能不能让窗口缓冲以 LINEAR 分配？——机制级答案（并指出这个问题现在**不成立**）

- **公开 API 没有"强制 LINEAR"这一说**：`ANativeWindow_setBuffersGeometry(window, w, h, format)` 只设**几何+格式**；usage 由**生产方**通过私有的 `native_window_set_usage()`（`system/window.h`，非 NDK）或 `AHardwareBuffer_allocate` 的 desc 决定。⇒ **若缓冲是 SF/应用分配的，我们改不了它的布局。**
- **但在本 bug 里这个问题不成立**：按 §0，渲染目标那个 AHB 是**我们自己 allocate** 的 ⇒ **usage 由我们写死**（`panvk_wsi.c:366-375` ⇒ 0x303）。所以我们能做的不是"让窗口缓冲变 LINEAR"，而是：
  1. **在 AHB descriptor 里加 `CPU_WRITE_OFTEN`**（进一步钉死"必须 CPU 可映射"；Android 无"禁止压缩"公开位，这是**倾向性**手段）；
  2. **主动初始化**（allocate 后立刻 `lock`→写黑/毒色→`unlock`）——**直接消灭"品红=未初始化"这个变量**，并让"GPU 究竟写了多大区域"一眼可见（黑底上只有被写过的区域有内容）；
  3. **如果 A2 证明真实布局不是 LINEAR** ⇒ 那说明 gralloc **违反了我们声明的 usage** ⇒ 此时唯一可靠的自保是**别再假设布局**：要么改走 `VK_ANDROID_native_buffer`（§4），要么在接受该缓冲前用 A1/A2 **拒绝**它（fail-fast，避免"渲染出坏画面"）。

---

## 4. 问题 3：能不能改成"不经 AHB"的呈现路径（`VK_ANDROID_native_buffer`）？

**机制**：把现在的
`自己 allocate AHB → 用外部内存导入建 image → 渲染 → present 时 CPU 拷到 ANativeWindow_lock 的缓冲`
换成
`ANativeWindow_dequeueBuffer() → 取该 ANativeWindowBuffer 的 handle/stride → 建 VkImage（外部内存导入）→ 渲染 → queueBuffer()`
（即标准 Android WSI：`vkGetSwapchainGrallocUsageANDROID` / `vkAcquireImageANDROID` 那一套；freedreno/turnip 就是这么做的）。

- **最小改动面**：`panvk_wsi.c` 的 swapchain 创建/acquire/present 三个函数 + 一组 `VK_ANDROID_native_buffer` 入口（或直接改现有 `panvk_android_swapchain_*` 的实现，不走 extension 也能做）+ fence/sync 处理。**这不是"几行"**，属于 WSI 重写。
- **风险：高**，而且**方向未必变好**：
  1. 换来的缓冲**不再由我们分配**，其 usage/布局由 **SF/应用**决定 ⇒ **更容易是 tiled/AFBC**，而解析它的仍然是同一条 `vk_android_get_ahb_layout()`（u_gralloc 失败时仍回退 LINEAR）⇒ **同一个不可判定问题原封不动地搬过去**；
  2. 少了 CPU 拷贝（性能收益），但引入了 dequeue/queue 生命周期与 fence 正确性风险；
  3. 与"面板/黑带/品红"的症状**没有直接因果**（现在没有证据表明窗口缓冲是坏的）。
- ⇒ **建议：不作为第一步。** 只有在 A1/A2 明确"渲染目标那个 AHB 真实布局非 LINEAR 且我们无法修正"之后，才考虑它；那时更**小**的一步是"**在导入前用 mapper 校验布局，不符就拒绝/报错**"，而不是立刻重写 WSI。

---

## 5. 建议的动作序列（按性价比）

1. **探针 2376×1080 复现**（§2，零驱动改动）→ 切开 S/C。
2. **A1：失败分支补 `lockPlanes` 日志**（3~5 行）→ 可映射性判据。
3. **A2：探针加"打印 mapper format layout/modifier"模式**（不动驱动）→ 权威布局。
4. **W3：主动初始化 AHB**（61b）→ 消掉"未初始化"变量并可视化"写了多大区域"。
5. 若 1–4 指向"布局确实是 LINEAR 且整幅被写、内容却错位" ⇒ 转 **fb extent / tiling area / scissor** 那条线（我可以下一轮直奔 `cmd_init_render_state`/`get_fb_descs` 与 WSI image `extent` 的对账）。
6. §4 的 WSI 重写**放最后**。

---

## 6. 未验证 / 限制

1. 全部为**代码级**+**你已给日志**的推断；本轮**未做任何设备实验**（§2/§5 都是提案）。
2. "§0：那个 AHB 是我们自己分配的"依据是 `panvk_wsi.c:366-380` 的 `AHardwareBuffer_allocate` 与 present 里 `chain->images[…]` 的用法（我上一轮逐行读过）；**若游戏的 2376×1080 AHB 其实来自另一条路径**（例如 MobileGL 自己的外部图像导入），那么 §0 的纠正本身就被推翻 —— 请用一条日志确认：在 `vk_android_get_ahb_layout()` 里打印**调用者**（是 swapchain 创建、还是某个 `VkImage` 的外部导入），即可定案。**这是本轮最需要你确认的一件事。**
3. `u_gralloc` 失败的具体原因仍未取得（需要它自己那行日志 + §2 的复现）。
4. 探针在 2376×1080 下是否真的复现"u_gralloc 失败"未验证（这正是 §2 的目的）。
5. "压缩布局通常不可 lockPlanes"是平台经验，非 NDK 明文保证 ⇒ 这正是 A2 存在的理由。
