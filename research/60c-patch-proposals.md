# 60c — 下一批补丁提案（照报告 57 格式；**只提案，不落地**）

来源：`/root/research/59-hunk-presence-audit.md` 的 MISSING 清单。按"与卡死/渲染的相关性"排序后**只取 2 条**。
纪律：本文件所有代码块均为**提案草案**；未改源码、未编译、未碰设备。

---

## 提案 1（推荐）：`4487dd7b89e` — queue submit storage 的 malloc 返回值检查

### 1.1 为什么是它

- **与本项目的崩溃族（bug ②：空指针/越界）直接同族**：三个 `malloc` 的结果**完全未检查**，失败后立刻被解引用。
- **落在我们改动最重的文件**（`csf/panvk_vX_gpu_queue.c`，本树 +3000 行本地改动）⇒ 该函数的上下文我们最熟，冲突最小。
- **触发条件与"重负载"吻合**：per-submit 的 `qsubmits` / `wait_ops` / `utrace.data_storage` 只在一次提交需要超过栈上预留（8 个 qsubmit / 8 个 syncop）时才 `malloc` ⇒ 正是**大帧/多命令缓冲/多信号量**的帧才会走到；内存紧张时该 `malloc` 失败是真实可能的。
- **风险低**：只多出"失败就干净返回 VK_ERROR_OUT_OF_HOST_MEMORY"，不改变成功路径的任何行为。

### 1.2 本树锚点（file:line，已实测）

| 锚点 | 位置 |
|---|---|
| 函数签名（当前仍是 `static void`） | `csf/panvk_vX_gpu_queue.c:3418-3421` |
| `… : malloc(sizeof(*submit->qsubmits) * submit->qsubmit_count);` | `:3521` |
| `submit->wait_ops = … : malloc(sizeof(*submit->wait_ops) * syncop_count);` | `:3523-3525` |
| `submit->utrace.data_storage = malloc(…);` | `:3532` |
| 调用点（在返回 `VkResult` 的 `gpu_queue_submit` 内） | `:4162` |

### 1.3 提案 diff（草案）

```diff
@@ panvk_vX_gpu_queue.c :3418 @@
-static void
+static VkResult
 panvk_queue_submit_init_storage(
    struct panvk_queue_submit *submit, const struct vk_queue_submit *vk_submit,
    struct panvk_queue_submit_stack_storage *stack_storage)
@@ :3519 @@
       submit->qsubmit_count <= ARRAY_SIZE(stack_storage->qsubmits)
          ? stack_storage->qsubmits
          : malloc(sizeof(*submit->qsubmits) * submit->qsubmit_count);
+   if (!submit->qsubmits)
+      return panvk_error(submit->dev, VK_ERROR_OUT_OF_HOST_MEMORY);

    submit->wait_ops = syncop_count <= ARRAY_SIZE(stack_storage->syncops)
                          ? stack_storage->syncops
                          : malloc(sizeof(*submit->wait_ops) * syncop_count);
+   if (!submit->wait_ops)
+      return panvk_error(submit->dev, VK_ERROR_OUT_OF_HOST_MEMORY);
+
    submit->signal_ops = submit->wait_ops + vk_submit->wait_count;
@@ :3530 @@
    if (submit->utrace.queue_mask) {
       submit->utrace.data_storage =
          malloc(sizeof(*submit->utrace.data_storage) *
                util_bitcount(submit->utrace.queue_mask));
+      if (!submit->utrace.data_storage)
+         return panvk_error(submit->dev, VK_ERROR_OUT_OF_HOST_MEMORY);
    }
+
+   return VK_SUCCESS;
 }
@@ :4162 @@（调用点）
    panvk_queue_submit_init(&submit, vk_queue);
-   panvk_queue_submit_init_storage(&submit, vk_submit, &stack_storage);
+   result = panvk_queue_submit_init_storage(&submit, vk_submit, &stack_storage);
+   if (result != VK_SUCCESS)
+      goto out;                      /* 若本树该函数无 out: 标签，则改为 return result; */
    panvk_queue_submit_init_utrace(&submit, vk_submit);
```

### 1.4 落地前必须确认的两点（**我没做，因为要改代码/编译**）

1. **调用点的清理语义**：上游用 `goto out;`（`out:` 处统一释放 `qsubmits`/`wait_ops` 等）。本树 `gpu_queue_submit`（`:4158` 附近）**是否有 `out:` 标签与对应清理**需要 Lead 确认；没有就用 `return result;`（此时三个 buffer 里最多分配了一个，`submit->qsubmits` 会泄漏 —— 若在意，需要在错误路径里按已分配情况释放，那也是几行的事）。
2. **`panvk_error()` 的返回类型/用法**：本树里 `panvk_error(dev, …)` 是否可直接 `return`（返回 `VkResult`）。本文件其它处已在用同样的模式（如上游同款），风险极低。

### 1.5 风险与证伪

| 项 | 说明 |
|---|---|
| 风险 | **低**：仅新增错误返回；成功路径零变化；不会影响 TERM/排水/描述符 |
| 若做错会怎样 | 若把 `return VK_SUCCESS` 漏在末尾 ⇒ 编译器 `-Werror=return-type` 会拦住；若 `goto out` 用了不存在的标签 ⇒ 编译失败（安全失败） |
| **证伪/验证（零成本，先做）** | 到**已有 tombstone / logcat** 里找历史上崩溃栈是否落在 `panvk_queue_submit_init_storage` 或其调用链（`grep -A15 "panvk_queue_submit_init_storage\|gpu_queue_submit" tombstones/*`）。若历史崩溃栈里**从未**出现该函数 ⇒ 这条的**优先级下降**（但仍是一个廉价健壮性修复）；若出现过 ⇒ 立刻做 |
| 收益上限 | 只覆盖"malloc 失败"这一条路径；不解决其它空指针/越界（那些另有候选） |

---

## 提案 2（**建议暂缓**）：`260ed1bafa2` — resolve transfer function flags

### 2.1 为什么它**不是**当前 bug 的候选（诚实评估）

- 它修的是 **sRGB 在 tile 内 resolve 时是否应用传递函数**（`VK_RENDERING_ATTACHMENT_RESOLVE_SKIP_TRANSFER_FUNCTION_BIT_KHR`）⇒ **颜色正确性**，与"巨型三角楔形/几何偏移/卡死"**没有因果链**。
- 而且该 bit 只在**应用显式设置**时才生效（Vulkan 1.4 / KHR 扩展路径）⇒ 对 Minecraft（Java，经 MobileGL DirectVulkan）**大概率从不设置** ⇒ **实际效果 ≈ 0**。
- ⇒ 单变量纪律下，**现在不该动它**。列在这里是为了把 59 号清单里的"真缺口"交代完整。

### 2.2 好消息：**依赖全部齐备**（我实测过）

| 依赖 | 本树状态 |
|---|---|
| `vk_get_rendering_attachment_flags()` | ✅ 存在（`src/vulkan/runtime/`） |
| `VK_RENDERING_ATTACHMENT_RESOLVE_SKIP_TRANSFER_FUNCTION_BIT_KHR` | ✅ 存在 |
| `vk_format_is_srgb()` | ✅ 存在 |
| `VkRenderingAttachmentFlagsInfoKHR` | ✅ 存在 |
| `src/panfrost/vulkan/panvk_vX_cmd_meta.c` | ✅ 存在（**不是** `csf/` 下） |

⇒ 上游 3 文件的补丁**可以近乎逐字套用**（这也是"若将来要做，成本很低"的依据）。

### 2.3 本树锚点

| 锚点 | 位置 |
|---|---|
| `struct panvk_resolve_attachment`（**缺** `flags` 字段） | `vulkan/panvk_cmd_draw.h:49-52` |
| `.mode = att->resolveMode,` 的 resolve 初始化块 | `vulkan/panvk_vX_cmd_draw.c`（resolve struct 初始化处，紧接 `assert(resolve.dst_iview != NULL)`） |
| `avoid_direct_resolve_to(resolve_pimage)` 判定 | `vulkan/panvk_vX_cmd_draw.c:220-221` |
| `VkRenderingAttachmentInfo color_atts[MAX_RTS];` | `vulkan/panvk_vX_cmd_meta.c`（`cmd_meta_resolve_attachments` 内） |

### 2.4 提案 diff（草案，**暂不落地**）

```diff
@@ vulkan/panvk_cmd_draw.h :49 @@
 struct panvk_resolve_attachment {
    VkResolveModeFlagBits mode;
+   VkRenderingAttachmentFlagsKHR flags;
    struct panvk_image_view *dst_iview;
 };
@@ vulkan/panvk_vX_cmd_draw.c :13 @@
 #include "pan_util.h"
+#include "vk_render_pass.h"
@@ render_state_set_color_attachment() 的 resolve 初始化 @@
       const struct panvk_resolve_attachment resolve = {
          .dst_iview = ms2ss ? iview_ss : resolve_iview,
          .mode = att->resolveMode,
+         .flags = vk_get_rendering_attachment_flags(att),
       };
@@ :218 @@（本树 :220-221 的判定）
+      /* tile buffer 里是线性值：tile 内 resolve 一定应用传递函数 */
+      const bool skip_transfer_function =
+         (resolve.flags &
+          VK_RENDERING_ATTACHMENT_RESOLVE_SKIP_TRANSFER_FUNCTION_BIT_KHR) &&
+         vk_format_is_srgb(fmt);
+
       if ((ms2ss || att->storeOp != VK_ATTACHMENT_STORE_OP_STORE) &&
-          !avoid_direct_resolve_to(resolve_pimage)) {
+          !avoid_direct_resolve_to(resolve_pimage) &&
+          !skip_transfer_function) {
@@ vulkan/panvk_vX_cmd_meta.c（cmd_meta_resolve_attachments）@@
    VkRenderingAttachmentInfo color_atts[MAX_RTS];
+   VkRenderingAttachmentFlagsInfoKHR color_att_flags[MAX_RTS];
    for (uint32_t i = 0; i < color_att_count; i++) {
       …
+      color_att_flags[i] = (VkRenderingAttachmentFlagsInfoKHR){
+         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR,
+         .flags = resolve_info->flags,
+      };
       color_atts[i] = (VkRenderingAttachmentInfo){
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
+         .pNext = &color_att_flags[i],
          …
```

### 2.5 风险与证伪

| 项 | 说明 |
|---|---|
| 风险 | **中低**：触碰 resolve 路径；但新逻辑只在"应用显式设置该 bit"时改变行为 |
| 潜在副作用 | 数组 `color_att_flags[]` 是函数栈上的，`pNext` 指向它 —— 只要 `color_atts[]` 的使用不超出该函数作用域就安全（上游即如此） |
| 证伪/验证 | 想确认"是否真的从不生效"：在 resolve 处加一行一次性日志打印 `flags`（或直接 grep 应用侧是否用 `VkRenderingAttachmentFlagsInfoKHR`）；**预期恒为 0** |
| 结论 | **暂缓**：与 bug 3/4 无因果；等颜色类问题出现时再按 §2.4 十分钟落地 |

---

## 3. 两条提案的优先级结论

1. **只推提案 1**（`4487dd7b89e`）——低风险、与我们改动最重的文件同域、命中"重负载"路径；但**先花五分钟 grep 历史 tombstone** 决定它是"高优先修复"还是"廉价健壮性"。
2. **提案 2 暂缓**（`260ed1bafa2`）——依赖齐备、成本低，但与当前两个 bug 无因果，现在动它会破坏单变量纪律。
3. 两条**都不要**与卡死/楔形的正在跑的变量（v84/v84d/v85/v83 env）混在一起落地。
