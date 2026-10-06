# 86 — 重叠是真的，但**它解释不了这个故障地址**；而它可能是 **bug 3** 的成因 —— 并给出决定性字段（`vm` + `was_auto_va`）

日期：2026-10-06 · 只读分析 · 配套：`85-bo-size-vs-bind-size.md`（R1）、`83`（VA 全表）
**已收到你的重叠算术**（A 起点在 B 内、C 完全被 A 包住，三次 ret=0）✓

---

## 0. ★ 先做一次**把重叠与故障地址分开**的算术（很重要，不然会白改一版）

```
A = [0x5fffd8d000, 0x5fffd9f000)   72 KiB
B = [0x5fffd57000, 0x5fffd97000)  256 KiB
C = [0x5fffd97000, 0x5fffd9b000)   16 KiB
故障 = 0x5fffd9e000
```
- **A ∩ B** = `[0x5fffd8d000, 0x5fffd97000)` ⇒ 故障 `0x5fffd9e000` **不在**其中 ✗
- **C** = `[0x5fffd97000, 0x5fffd9b000)` ⇒ 故障 `0x5fffd9e000` **≥ `0x5fffd9b000`** ⇒ **不在** C 内 ✗
- ⇒ **故障落在"A 独有、且不被任何其它区间覆盖"的尾段 `[0x5fffd9b000, 0x5fffd9f000)`** ✓

**⇒ 结论（本轮最重要的一句）**：**"重叠导致页表项被替换/被摘掉"这套机制，解释不了这个故障地址** ✗ —— 因为那个地址**不在任何重叠区内**。
⇒ **所以**：
1. **R1（绑定 `0x12000` > BO `0x11000`，第 18 页无 backing）仍然是这个故障地址的最简解释** ✓（它恰好等于 `A.start + 0x11000`）
2. **重叠本身很可能是一个"真实且独立"的缺陷** —— 而且**它是 bug 3（面板/内容重复）的极佳候选**：重叠区内的访问会被路由到**另一块 BO 的 backing** ⇒ **像素数据错乱** ⇒ 面板/重复/黑带 ✓✓（这条值得单独追，即使它与 0x5fffd9e000 无关）

---

## 1. 你第 1 点 + 我要加的两个决定性字段：**`vm` 指针** 与 **`was_auto_va`**

### 1.1 先别急着认定为"重叠 bug"：**先打 `vm` 指针**
`pan_kmod_vm_bind(vm, …)` 的**第一个参数就是 `vm`**。**若 A 与 B/C 属于不同的 `pan_kmod_vm`，则不构成重叠**（不同 VM 各自有独立页表）⇒ 整套推论作废 ✓
**⇒ 所以第一件事是加两个字段，而不是改行为。**

### 1.2 最小改法（头文件安全，按 85 号的"指针配对"路线）
```c
static inline VkResult
pan_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                 struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   /* v129 (report 86): 记录"这次绑定之前，va.start 是否还是 AUTO_VA 哨兵"。
    * 这一位把"内核分配"与"我们自己算的 VA"分开 —— 重叠的来源由此可判。 */
   bool was_auto[8];                       /* op_count 上限按调用点实际值调整 */
   for (uint32_t i = 0; i < op_count && i < ARRAY_SIZE(was_auto); i++)
      was_auto[i] = (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP &&
                     ops[i].va.start == PAN_KMOD_VM_MAP_AUTO_VA);
   …
   /* 在已有的逐 op 打印里补四项：vm / was_auto / bo / bo_size */
   mesa_logi("v129 bind: vm=%p ret=%d type=%d was_auto=%d va=[0x%" PRIx64 ", 0x%" PRIx64
             ") size=0x%" PRIx64 " bo=%p bo_size=0x%" PRIx64,
             (void *)vm, ret, (int)ops[i].type, was_auto[i],
             ops[i].va.start, ops[i].va.start + ops[i].va.size, ops[i].va.size,
             (void *)ops[i].map.bo, ops[i].map.bo ? (uint64_t)ops[i].map.bo->size : 0);
```
（字段名以 `pan_kmod.h` 为准；`bo` 指针用于与分配侧配对 —— 分配侧已有打印，**每个 BO 只分配一次** ⇒ 分配侧的 site 就是这块 VA 的所有者 ✓）

**判据表（一次运行即可分四支）**：
| 观测 | 结论 / 下一步 |
|---|---|
| **A 与 B/C 的 `vm` 不同** | **不是重叠** ⇒ 该理论作废 ⇒ 回 **R1**（§3 的 `bo_size` 判据）✓ |
| `vm` 相同 **且** 有的是 `was_auto=1`、有的是 `=0` | **★ 两个 VA 分配器在同一地址空间里各自为政**（内核 AUTO_VA vs 我们的 `panvk_as_alloc`/priv_heap）⇒ **这就是重叠的根因** ✓✓ |
| `vm` 相同 **且** 全是 `was_auto=1` | **内核 AUTO_VA 给了重叠地址**（严重，需内核侧）⇒ 加断言 + 考虑全面改用显式 VA ✓ |
| `vm` 相同 **且** 全是 `was_auto=0` | **我们自己的算式重叠** ⇒ 直接查那几个显式 VA 的分配点 ✓ |

---

## 2. 你第 2 点：AUTO_VA 为什么会重叠 —— 按我们树内的分配路径判断

**树内有两条并行路径**（这正是重叠的天然来源）：
| 路径 | VA 来源 | 已知调用点 |
|---|---|---|
| **内核 AUTO_VA** | `PAN_KMOD_VM_MAP_AUTO_VA`（内核回写 `va.start`） | **描述符环**（`gpu_queue.c:2364-2400`：注释 *"kbase assigns the BO VA itself"*）、以及其它走 `AUTO_VA` 的 bind |
| **我们自己算** | `panvk_as_alloc(dev, dev->as.priv_heap, size, alignment)` | priv BO / 池 slab 一族（`panvk_priv_bo_create`、`panvk_pool_alloc_backing`） |
⇒ **两者若都作用在同一个 `dev->kmod.vm` 上、且互不知情，就必然可能重叠** ✓✓
**⇒ 请跑这两条把"谁用哪条"列全**（一次 grep）：
```bash
M=/root/zenithblue/work/mesa/src/panfrost
grep -rn "PAN_KMOD_VM_MAP_AUTO_VA" $M | grep -v "\.bak"                 # 用内核 VA 的
grep -rn "panvk_as_alloc\|priv_heap" $M/vulkan/ | grep -v "\.bak" | head -20   # 用我们算的 VA 的
```
**判据**：若**同一 `vm` 上两者都存在** ⇒ **重叠是结构性的**（不是偶发）✓；若 priv 路径在 kbase 上其实也走 AUTO_VA ⇒ 那重叠就只能来自内核 ⇒ 转"全是 was_auto=1"那一支 ✓

---

## 3. 最小修复候选（按"这是本故障的成因"与"这是 bug 3 的成因"分开）

### 3.1 针对本故障（`0x5fffd9e000`）：**R1 的判据与修法（与重叠无关）**
- **判据**：在 bind 打印里看 **`bo_size`**（85 号）⇒ **`bo_size == 0x11000` 而 `va.size == 0x12000`** ⇒ **R1 坐实** ✓
- **修法**：`assert(ops[i].type != MAP || ops[i].va.size <= ops[i].map.bo->size);` + 修那处"多算一页"的表达式 ✓

### 3.2 针对重叠（**bug 3 的头号候选**）
| 候选 | 内容 | 风险 |
|---|---|---|
| **F-O1（★首选：只用一条分配器）** | 在 kbase 上**统一用我们的显式 VA**（`panvk_as_alloc`/priv_heap）并把 `AUTO_VA` 从 MAP 路径去掉；或反向（全用内核 AUTO_VA 并让 priv_heap 让位）。**只要"同一个 `vm` 上只有一条分配器"，重叠在构造上不可能** ✓✓ | 中（要改 VA 分配路径，须单变量上机） |
| **F-O2（护栏，零风险，建议先上）** | 在 `pan_kmod_vm_bind` 里维护一张**主机侧区间表**（MAP 插入 / UNMAP 删除），**任何新增区间与既有区间相交 ⇒ 打印并断言**（DEBUG-only）⇒ **发射期就抓住重叠**，而且能直接告出"A 与 B 重叠、C 被 A 包住"这类关系 ✓✓ | **0**（纯 DEBUG 校验） |
| **F-O3** | 若确认是内核 AUTO_VA 给重 ⇒ 把交集区间的 `vm` 与 VA 序列一并报给内核侧；临时办法是用 F-O1 绕开 | 中 |

---

## 4. 5 分钟实验（按"先观测、后行为"排序）

1. **E-Obs（★先做，零行为改变）**：§1.2 的补丁（`vm` + `was_auto` + `bo` + `bo_size`）⇒ **一次运行即可同时判**：(i) A/B/C 是否同一 VM；(ii) 重叠是否来自"两条分配器"；(iii) R1 是否成立（`bo_size`）✓✓ **这一步信息量最大**。
2. **E-Fix-A（单变量，若 §2 显示两条分配器并存）**：把**描述符环那条**（唯一确认用 AUTO_VA 的路径）**改成显式 VA**（用 `panvk_as_alloc`），其余不动 ⇒ **若重叠消失 / 故障消失 / 面板改善 ⇒ 双分配器坐实** ✓✓
3. （保留）F-O2 的区间表断言：即使 E-Fix-A 有效，也建议**同时上 F-O2**，把这类冲突永久挡住 ✓

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§0 的重叠/故障地址算术**完全基于你给的四个区间与故障地址** ✓（这条算术是本报告的核心，请复核）
2. **`vm` 指针这一支我无法从现有日志判断**（你的打印里没有它）⇒ **它必须排在"认定重叠为 bug"之前** ✓ 若 A/B/C 属于不同 VM，整套重叠推论作废 ✓
3. §2 的"priv BO 走 `panvk_as_alloc` 显式 VA"是**我对此前读码的印象**（`panvk_priv_bo_create`/`panvk_as_alloc` 的存在与用途），**未在 kbase 路径上逐行确认**"它是否真的用显式 VA"⇒ 两条 grep 就是它 ✓
4. §1.2 的 `was_auto[8]` 数组上限、以及 `ops[i].map.bo`/`bo->size` 的字段名**未逐字复核**（预算用尽）⇒ 编译前对一眼 ✓
5. **重叠与 bug 3 的关联（"访问被路由到另一块 BO 的 backing ⇒ 像素错乱"）是机制级推断**，尚未有观测支撑；但它与"面板/重复内容/黑带"高度吻合，值得在 E-Obs 之后单独追 ✓
6. 所有 diff **未编译、未上机**。
