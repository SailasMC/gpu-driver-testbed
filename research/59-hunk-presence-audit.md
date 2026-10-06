# 59 — 基线之后「碰 src/panfrost/vulkan」的上游提交：逐条**代码级在否**审计

日期：2026-10-06 · 树 `/root/zenithblue/work/mesa`（= 基线 `5a07217f034` + **未提交**工作区改动）· 上游参照 `origin/main`
本会话：**只读**（`git show`/`grep`/`sed`）+ 只写本文件与 `/root/v85work/`；未改源码、未编译、未碰设备、未 `rm`、未 `git checkout/stash/reset`、**未 fetch**

---

## 0. 一句话 + 方法学更正（先行）

- **方法学更正（采纳 Lead 的指正，并在我自己的旧报告上生效）**：本树 = 基线 + **未提交**工作区改动，本项目从不 commit 上游补丁 ⇒ **`git merge-base --is-ancestor` / `git log <base>..main` 只能用来"列举候选提交"，绝不能用来判"代码缺失"**。判缺口**必须比对 hunk 的代码行本身**。
  - 本轮**两个方向**都抓到了 ancestry 判据的反例：① `fbb4993c5d7`（flush FAU stores）不在我们任何 commit 里，但**代码在**（见 §5.1）；② `22ddbe3067c`（layered FB barrier in secondary cmdbufs）**根本不在 main**（`in_main=False`），但它的改动**已经在本树里**（PRESENT）。
- **结论摘要**（`5a07217f034..origin/main` 碰 `src/panfrost/vulkan` 的 **21 条**）：**已在树内 3 条**（`4ecc4966334`、`fbb4993c5d7`、以及 `98ed811046b` 的部分）、**真缺口且值得动 1 条**（`81db71cfba0` compute TLS size）、**真缺口但与本 bug 无关/不适用 17 条**（特性广告、AFBC-WSI（**已知有害，勿采纳**）、API 迁移、内存报告重构等）。详见 §2/§4。

---

## 1. 方法与可复现命令

```bash
cd /root/zenithblue/work/mesa
# ① 权威候选集（只用它"列候选"，不用它判缺失）
git log --format='%H|%h|%ad|%s' --date=short 5a07217f034..origin/main -- src/panfrost/vulkan   # → 21 条
# ② 逐条取 hunk（对象在本地仓库里都在；无需网络、无需 fetch）
git show --format= --unified=3 <sha>
# ③ 到工作区按**改动行本身**核对（不是 ancestry）
#     新增行是否已存在（整行规范化后比对）／删除行是否已消失
python3 /root/v85work/hunk_audit2.py     # 机械矩阵 → /root/v85work/hunk_audit2.json
```

**判定规则（三态）**：

| 态 | 机械判据（`MINLEN=8`，整行规范化后比对） |
|---|---|
| **PRESENT（已在树内）** | 该 hunk 的**新增行全部存在**且**删除行已消失**（纯删除 hunk 则要求删除行消失） |
| **MISSING（缺失）** | 新增行**全不存在**（纯删除 hunk 则要求删除行仍在） |
| **PARTIAL（需人看）** | 其它（通常是本树有局部改动 / 只 port 了一部分 / 或短行误匹配） |

**这是机械筛，不是结论**：短且通用的行（如 `cs_flush_stores(b);`）会在文件里多处出现 ⇒ 机械命中只说明"这行在文件里"，**不说明在正确位置**。因此**每条 PRESENT/PARTIAL 的关键项都做了人工上下文核对**（§5）；`first_added_line` 字段在短行情形下**不可信**（例：脚本给 `fbb4993c5d7` 报的是 :1165，人工核对实际在 `launch_indirect_draw()` 内 ~:3489，见 §5.1）。

**局限（诚实）**：
- 只覆盖 `src/panfrost/vulkan/**`（含 `csf/`、`jm/`、`bifrost/`）；`docs/features.txt`、`src/panfrost/lib/**` 等不在本表（若某提交同时改这两处，我只判 vulkan 侧）。
- 我们的文件名与上游一致（`panvk_vX_*.c`、`csf/panvk_vX_*.c` 两类都在），**没有路径重写问题**；但本树对若干文件做过**结构性改写**（见 `f333dd6d`/`c4673eab`/`98ed8110` 的 PARTIAL 原因），此时"缺失"往往应读作**不适用**（§4）。
- 判据只看"行在不在"，**不判语义等价**（本树可能用别的方式实现了同一修复）⇒ 判定为 MISSING 的条目仍需一句人工理由（§4 已逐条给）。

---

## 2. 21 条总表（按 §4 的相关性排序）

| # | 提交 | 日期 | 标题（截断） | 三态 | 触及文件 | 相关性 |
|---|---|---|---|---|---|---|
| 1 | `81db71cfba0` | 09-27 | panvk/csf: give compute jobs the TLS size of the shared allocation | **MISSING** | `csf/panvk_vX_cmd_dispatch.c`、`csf/panvk_vX_cmd_precomp.c` | **高（真缺口）** |
| 2 | `4487dd7b89e` | 09-22 | panvk/csf: check malloc return values in queue submit storage init | **PARTIAL**(3P/1M) | `csf/panvk_vX_gpu_queue.c` | 中（崩溃族） |
| 3 | `d7d2cd7b19e` | 09-22 | panvk: validate descriptor counts in shader deserialization | **MISSING** | `panvk_vX_shader.c` | 中（崩溃族） |
| 4 | `90be04d45a7` | 09-08 | panvk: Zero images built for device-level queries | **PARTIAL**(2) | `panvk_image.c` | 中低（未初始化栈读） |
| 5 | `260ed1bafa2` | 09-30 | panvk: Honor resolve transfer function flags | **MISSING** | `panvk_cmd_draw.h`、`panvk_vX_cmd_draw.c`、`panvk_vX_cmd_meta.c` | 中低（颜色） |
| 6 | `fbb4993c5d7` | 09-23 | panvk/csf: flush the FAU stores before indirect draws read them | **PRESENT** ✅ | `csf/panvk_vX_cmd_draw.c` | 已含（本树 v76 port；§5.1） |
| 7 | `4ecc4966334` | 09-24 | panvk: Don't downcast CS func address to a 32-bit VA | **PRESENT** ✅（= v82） | `csf/panvk_vX_cmd_draw.c:2114` | 已含 |
| 8 | `c4673eab286` | 09-23 | panvk: Revisit the heap selection logic | **PARTIAL**(23M/2P) → **不适用** | 7 文件（`as.heaps[…]` API） | 不适用（§4.2） |
| 9 | `f333dd6d1c8` | 09-23 | panvk: Don't force non-executable buffers to live in the first 4G | **PARTIAL**(9P/1M) → **不适用** | 7 文件 | 不适用（§4.2） |
| 10 | `98ed811046b` | 08-05 | panvk: Query modifier capabilities for format property advertisement | PARTIAL(5M/5P/1PRESENT) | 3 文件 | 低（modifier 广告） |
| 11 | `50bda0b8978` | 08-05 | panvk: Enable AFBC for WSI images by default | PARTIAL(3M/1P) | 3 文件 | **勿采纳**（交接 §4：P5 AFBC 有害） |
| 12 | `21365fbaca1` | 09-30 | panvk: Advertise VK_KHR_maintenance10 | **MISSING** | `panvk_vX_physical_device.c` | 不适用（仅广告） |
| 13 | `8b2d038e852` | 09-30 | panvk: Move to CmdEndRendering2KHR(..) | **MISSING** | `csf/…`、`jm/…` | 不适用（纯改名/API） |
| 14 | `83cda621398` | 09-03 | panvk: advertise VK_ANDROID_external_format_resolve on v10+ | **MISSING** | `panvk_vX_physical_device.c` | 不适用（我们未实现 EFR） |
| 15 | `842ff4662ff` | 09-03 | panvk: patch null color attachment and format for efr | **MISSING** | `csf/…`、`…` | 不适用（同上） |
| 16 | `6409a5b16af` | 09-09 | panvk: Advertise VK_EXT_image_compression_control | **MISSING** | `panvk_vX_physical_device.c` | 不适用（仅广告） |
| 17 | `e3a315d636a` | 09-09 | panvk: Honor VkImageCompressionControlEXT | PARTIAL(5P/2M) | `panvk_image.c`、`panvk_physical_device.c` | 不适用（压缩控制） |
| 18 | `19f5cf79c5e` | 09-08 | panvk: Report image compression properties | PARTIAL(6M/2P) | 3 文件 | 不适用 |
| 19 | `78f9b71dd7c` | 09-28 | anv,panvk,…: use new device memory report function | PARTIAL(1) | `panvk_device_memory.c` | 不适用（API/诊断） |
| 20 | `eef8db800ac` | 07-03 | panvk: Advertise VK_EXT_swapchain_colorspace | **MISSING** | `panvk_instance.c` | 不适用（仅广告） |
| 21 | `fb3853764b2` | 07-03 | panvk: Advertise VK_KHR_incremental_present | **MISSING** | `panvk_vX_physical_device.c` | 不适用（仅广告） |

审计原始矩阵：`/root/v85work/hunk_audit2.json`（每 hunk 的新增/删除行、命中数、首行行号）。

---

## 3. 已在树内的三条（PRESENT）

| 提交 | 在树内的位置 | 依据 |
|---|---|---|
| `4ecc4966334`（CS 函数地址 64 位） | `csf/panvk_vX_cmd_draw.c:2114` | v82 就是它（`uint32_t fn_addr` → `uint64_t`，含上下文逐行一致；报告 56 §B.2） |
| `fbb4993c5d7`（间接绘制前 flush FAU store） | `csf/panvk_vX_cmd_draw.c` → `launch_indirect_draw()`（:3338 起）内、`if (patch_faus)` 块（:3412/:3489）里，紧随 `shader_remapped_sysval_offset(vs, sysval_offset(graphics, vs.base_instance))` 之后 | **§5.1 的人工上下文核对**（机械命中给的 :1165 是同一行的另一处调用，不可信） |
| `98ed811046b`（modifier 能力查询） | `panvk_physical_device.c:2270` | 1 个 hunk PRESENT（其余 5M/5P ⇒ 只 port 了一部分） |

---

## 4. 按「巨型三角楔形 / 几何被画到错误偏移」相关性排序的候选与风险

### 4.1 ★ 唯一值得动的高相关缺口：`81db71cfba0` — compute job 的 TLS size

- **改动内容**（2 个 hunk，2 个文件）：把"只复制全局 TLS **指针**到 per-job TSD"改成"**指针 + 尺寸**"：
  ```diff
  -      /* Copy the global TLS pointer to the per-job TSD. */
  +      /* Copy the global TLS pointer and size to the per-job TSD. */
  +      cs_load32_to(b, cs_scratch_reg32(b, 4), cs_scratch_reg64(b, 0), 0);
  ```
  （`csf/panvk_vX_cmd_dispatch.c`、`csf/panvk_vX_cmd_precomp.c`）
- **为何与我们的症状相关**：per-job TSD 里的 TLS **尺寸**若保持陈旧/为零，计算作业的 TLS 分配会被低估 ⇒ **共享/TLS 内存越界写**。若 MobileGL（DirectVulkan 后端）或 Minecraft 路径用 compute 做缓冲搬运/间接参数生成，越界写会污染顶点/索引/描述符缓冲 ⇒ **几何被画到错误位置 / 巨型三角**这条症状链是成立的。
- **风险**：**低**（1 条 `cs_load32_to` + 注释 ×2；不触碰 TERM/排水/寄存器分配约定）。**前提**：本树是否走 compute 需要 Lead 用现有仪器确认（`grep -c "dispatch"` 的日志/@ 或直接看 MobileGL 是否发 compute）。
- **证伪方法**：先确认"我们的 compute 作业是否真的用了 TLS"（若 TLS size 恒为 0 且无人用 TLS，则该修复对本 bug 无效）。这可用 pandecode 看 `cs_load32_to` 是否落在 TSD 的 size 字段 + 运行时是否有 compute 提交。

### 4.2 不适用（**不是缺口**，附证据）：`c4673eab` + `f333dd6d`

- 这两条改的是**上游的 VA 堆选择机制**：`device->as.heaps[PANVK_PRIV_VA_HEAP / PANVK_PUB_VA_HEAP / PANVK_FIXED_PUB_VA_HEAP]` 数组、`panvk_va_heap_fallback()`、`flags & PAN_KMOD_BO_FLAG_EXECUTABLE` 选堆。
- **本树不存在这些标识符**（`grep PANVK_PRIV_VA_HEAP|PANVK_PUB_VA_HEAP|PANVK_EXEC_VA_HEAP|PANVK_NO_EXEC_VA_HEAP` = **0 命中**），API 已被重构成 `panvk_as_alloc(dev, struct util_vma_heap *, …)`；更要命的是 **kbase 上强制 `AUTO_VA`**（`panvk_vX_device.c:512-519`），所有 VA 由内核给 ⇒ 那套堆逻辑在设备上**整条是死代码**（详见报告 56 §A.8）。
- ⇒ 结论：**MISSING 应读作"不适用"**；若照搬 `f333dd6d`（**放宽** 4G 限制）而不同时清掉全部 32 位地址收窄，等于把 `4ecc4966` 那一类风险放大（在我们的 AUTO_VA 模型下该风险**本来就已经是活的**）。

### 4.3 中相关（崩溃族，非楔形）

- `4487dd7b89e`（3P/1M，`csf/panvk_vX_gpu_queue.c` 的 queue submit storage init 里检查 malloc 返回值）：**与本树改动最重的文件重叠**（该文件我们有 3000+ 行本地改动）。若某次 malloc 失败被静默忽略 ⇒ 后续解引用 NULL ⇒ **崩溃族（bug ②）**。风险低（加返回值检查）。
- `d7d2cd7b19e`（MISSING，`panvk_vX_shader.c` 校验描述符计数）：畸形着色器导致越界读 ⇒ 崩溃/垃圾。风险低。
- `90be04d45a7`（PARTIAL，`panvk_image.c` 里 device-level query 用的 `struct panvk_image image;` 未清零）：**未初始化栈读** ⇒ 随机字段参与计算 ⇒ 偶发错误。我 56 号报告的人工核对：3 处中 1 处已 `= {0}`、2 处仍未清零 ⇒ 属"补一半"。风险极低（2 处 `= {0}`）。

### 4.4 不适用（特性广告 / API 迁移 / 诊断重构）

`21365fbaca1`（maintenance10）、`8b2d038e852`（CmdEndRendering2KHR，纯改名，我们仍用 1.x 入口，语义等价）、`83cda621398`+`842ff4662ff`（Android EFR，我们未实现该扩展）、`6409a5b16af`+`e3a315d636a`+`19f5cf79c5e`（image compression control 广告/报告）、`eef8db800ac`/`fb3853764b2`（swapchain colorspace / incremental present 广告）、`78f9b71dd7c`（device memory report API 重构）。
⇒ 这些的"缺失"**不代表功能缺陷**：它们要么只增加**能力广告**（我们本就不该在没实现时广告），要么是纯 API 迁移。

### 4.5 ⚠ 明确**不要**采纳：`50bda0b8978`（Enable AFBC for WSI by default）

- 交接说明 §4 记载 **P5 的 AFBC 改动有害**；该提交把 WSI 的 AFBC 从 opt-in 翻成 opt-out（默认开）。本树仍是 `PANVK_DEBUG_WSI_AFBC`（默认不开）⇒ **保持现状是安全的一侧**。
- 风险：**高**（Android AHB 的 modifier/stride 一旦不匹配，症状正是"整屏拉伸/错位"，且很难与我们的 bug 3 区分）。⇒ **记录为禁区**。

---

## 5. 对 Lead 指定条目的逐条复核

| Lead 给的 sha | 在 main? | 三态 | 复核结论 |
|---|---|---|---|
| `67d469c8541` place coherent memory type before cached type | ❌ **不在 main** | PARTIAL | **不是 main 缺口**（`git log origin/main --grep="coherent memory type"` 无命中）。它是"内存类型排序"修复：应用按索引选内存类型时可能选到 cached 而非 coherent ⇒ 未显式 flush 时会读到陈旧数据。与"局部画面错"有**潜在**关系，但**属未合并 MR**，建议只跟踪、不采纳；风险低-中 |
| `4ab3bd4087c` compute TLS size | ❌ **不在 main** | MISSING | 与 `81db71cfba0` **同标题** ⇒ 它是**合并前的前身**（rebase 后成为游离对象）。**以 `81db71cfba0` 为准**（§4.1） |
| `81db71cfba0` compute TLS size | ✅ | **MISSING** | **真缺口，最高优先**（§4.1） |
| `260ed1bafa2` resolve transfer function flags | ✅ | **MISSING** | 真缺口，**颜色**正确性（sRGB 在 tile 内 resolve 是否跳过传递函数），与楔形无直接因果；风险中（新增 `VkRenderingAttachmentFlagsKHR` 字段 + `vk_render_pass.h` 依赖） |
| `50bda0b8978` AFBC for WSI by default | ✅ | PARTIAL | **勿采纳**（§4.5） |
| `22ddbe3067c` layered FB barrier in secondary cmdbufs | ❌ **不在 main** | **PRESENT** ✅ | **已在本树**（v76 期手工 port 的证据）——**这正是"ancestry 判据会错"的反例** |
| `8b2d038e852` Move to CmdEndRendering2KHR | ✅ | MISSING | 纯 API 改名/迁移 ⇒ **不适用**（我们的 `CmdEndRendering` 路径语义等价；且 `suspending` 处理见 `cmd_draw.c:4866-4882`） |
| `fbb4993c5d7` flush FAU stores | ✅ | **PRESENT** ✅ | **代码在、位置对**（§5.1）。我 56 号报告 A.3 曾把它列为"未定论"，**此处更正为已含** |

### 5.1 `fbb4993c5d7` 的人工上下文核对（为什么必须看上下文）

```bash
grep -n "patch_faus\|^launch_indirect_draw" src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
#  :3338  launch_indirect_draw(...)
#  :3354  bool patch_faus = shader_uses_sysval(vs, graphics, vs.first_vertex) || …vs.base_instance);
#  :3412  if (patch_faus) { … }        ← base_instance 重映射
#  :3489  if (patch_faus) { … }        ← 这里才是上游新增 cs_flush_stores(b); 的位置
```
在 `launch_indirect_draw()` 的第二个 `if (patch_faus)` 块内（~:3489），`base_instance` 的 remap store 之后紧跟 `cs_flush_stores(b);` ⇒ **与上游 hunk 一致**（上游 MR 为 !44659，本树 v76 已手工 port；报告 52 审计过它）。
**方法学要点**：机械"行存在"判据在短行上会给出错误行号（脚本报 :1165，实际 ~:3489）⇒ **PRESENT 结论必须配一次上下文核对**。

---

## 6. 对报告 56 附录 A.3 的更正

- 报告 56 §A.3 的表里，我对 21 条给的是**启发式文本抽样**结论，并已声明"其中 17 条未逐条核验、脚本有假阳性"。**本报告取代那张表**：正确结论见 §2/§3/§4。
- 具体更正两条：`fbb4993c5d7` **已在树内且位置正确**（不是"未定论"）；`90be04d45a7` 为 **PARTIAL**（3 处里 1 处已改）。
- 判据本身也换了：从"行抽样命中数"改为"**新增行全在 + 删除行已消失**（两侧）"，并对关键项做上下文核对。

---

## 7. 未验证 / 限制

1. **全部结论是代码级文本比对**，未编译、未上机；"等价/不适用"的人工判断（§4.2、§4.4）**未做语义级验证**（例如未验证本树是否用别的方式实现了 efr/compression 的等价物）。
2. `PARTIAL` 不区分"本树有等价实现"与"只 port 一半"；本报告只对 §4/§5 的关键项给了人工理由，其余 PARTIAL **未逐 hunk 深挖**。
3. 未覆盖 `src/panfrost/lib/**` 与 `docs/**` 的同步（若某提交同时改这两处，本表只反映 vulkan 侧）。
4. `67d469c8541`/`4ab3bd4087c`/`22ddbe3067c` 三个游离对象**不在 main**：我只做了"是否 main 祖先"与内容比对，**未追它们来自哪个 MR/分支**（`git log --all` 也查不到引用，属 rebase 残留）。
5. §4.1 的"compute 是否真用 TLS"**未验证**（需要设备侧日志或 pandecode），因此该修复的**收益上限未被证实**。
6. 未验证 `81db71cfba0` 的补丁能否直接套在本树上（本树 `csf/panvk_vX_cmd_dispatch.c` / `cmd_precomp.c` 有本地改动，可能有上下文冲突）——落地前需 Lead 做 `patch --dry-run`。
