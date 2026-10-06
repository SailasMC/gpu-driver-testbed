# 71 — 钉死 H2：`gpu_heap_va`（内核 CUSTOM_VA 区的 tiler heap context）+ 一个 5 分钟单变量实验

日期：2026-10-06 · 只读分析 · 配套：`70-tri-reproducer-prior-art.md`、`13-csf-exception-c3.md`
**已收到的结论**：S3 让故障地址**不随 chunk_size 变** ⇒ H1（chunk 链）出局；H2（固件读固定结构）成立 ✓；且 `0xc2(0x5ffe800000)` 与 `0xc3(0x5fffe1e000)` 是**两次独立事件** ✓

---

## 1. 那两个地址是什么结构（从我们自己的代码反推）

**结论：它们落在 kbase 的 CUSTOM_VA / JIT 区，而该区里唯一会被固件"读"的、由 `HEAP_SET` 交给它的对象就是 `panvk_tiler_heap.context.dev_addr`（即 ioctl 返回的 `gpu_heap_va`）。**

### 1.1 对象与字段（逐行）
| 对象 | 出处 | 说明 |
|---|---|---|
| `struct panvk_tiler_heap { uint32_t chunk_size; struct panvk_priv_mem desc; struct panvk_priv_mem oom_fbd; struct { uint32_t handle; **uint64_t dev_addr**; } context; }` | `vulkan/csf/panvk_queue.h:28-36` | `context.dev_addr` = **heap context 的 GPU VA** |
| kbase 路径取 VA | `vulkan/csf/panvk_vX_gpu_queue.c:3208-3229`：`uint64_t heap_ctx_va, first_chunk_va; … kbase_kmod_csf_tiler_heap_init(…, &heap_ctx_va, &first_chunk_va)` → `tiler_heap->context.dev_addr = heap_ctx_va;` `first_heap_chunk = first_chunk_va;` | **原样来自内核** |
| ioctl 包装（无截断） | `lib/kmod/kbase_kmod.c:1079-1080`：`*heap_ctx_va = req.out.gpu_heap_va; *first_chunk_va = req.out.first_chunk_va;` | 两个字段都是 `__u64` ✓ **不存在 32 位截断** |
| uAPI 语义 | `lib/kmod/kbase_csf_uapi.h:365-369`：`@out.gpu_heap_va: GPU VA of **Heap context** that was set up for the heap`；`@out.first_chunk_va: … points to the **header** of heap chunk` | 权威定义 |
| **这些 VA 从哪来** | `lib/kmod/kbase_kmod.c:1240-1246` 的注释（我们自己的）：`KBASE_IOCTL_MEM_JIT_INIT` *"on 64-bit clients this is what **carves the CUSTOM_VA zone out of the top of the SAME_VA zone** — and **kernel-internal allocations such as tiler heap contexts/chunks come from that zone**"* | **⇒ 高位 `0x5ff…` 正是 CUSTOM_VA 的特征**；而 panvk 自己的 BO 是低位 VA（13 号记录实测 `va=0x41000`） |

### 1.2 两个地址的"算式"
- `0x5fffe1e000` − `0x5ffe800000` = **0x161E000 ≈ 23.2 MB**；两者**同段**（`0x5ff…`）⇒ **同一 CUSTOM_VA/JIT 区里的两个对象**。
- `MEM_JIT_INIT` 的参数是 `va_pages = 1<<25`、`max_allocations = 255`（`kbase_kmod.c:1248-1252`）⇒ 该区是一个**大的 VA 池**，内核按 JIT 规则在其中放置 heap context 与 chunk；两个对象相距 ~23 MB 完全可能（JIT 的 bin/对齐 + 多次分配）。
- **关键判别（你 S3 已经做了）**：chunk_size 1 MiB→2 MiB **地址不变** ⇒ 故障地址**不是**由 chunk 链/几何推导出来的 ⇒ **不是 chunk**，而是**每次都会落在同一 VA 的固定结构** ⇒ 与"H2 = heap context"一致 ✓（heap context 的大小与 chunk_size 无关，JIT 的放置也随之不变）
- 为什么是**两个**地址 ⇒ 两次事件的固定结构各不相同（例如**两个队列各自的 heap context**，或 heap context 与同族的另一个内核结构）；`0xc2`/`0xc3` 只是**走表失败层级**不同（L2/L3），不代表两类对象。

### 1.3 我**没能**断定的
- 我**没有**读内核侧（`/root/zenithblue/patches/kbase-common/` 与设备内核）里 `gpu_heap_va` 的映射实现 ⇒ **"该 VA 是否被映射进 kctx 的 GPU 页表"仍未在代码层确证**；§3 的插桩 + §4 的实验就是为它设计的。

---

## 2. 为什么"第一次 draw"就碰它（H2 的机制链，已代码落地）

1. **每个图形 ring entry 的 wrapper 序言都会 `HEAP_SET`**：`panvk_vX_gpu_queue.c:1506`（`cs_heap_set(&b, addr64)`）与 `:2922-2926`：
   ```c
   struct cs_index heap_ctx_addr = cs_scratch_reg64(&b, 0);
   cs_move64_to(&b, heap_ctx_addr, queue->tiler_heap.context.dev_addr);
   cs_heap_set(&b, heap_ctx_addr);
   ```
   ⇒ **固件从这一刻起拿到了 heap context 的 VA**。
2. **`HEAP_SET` 只是写寄存器**（13 号 §H2 已指出）；**真正去读 context 的是随后的固件**，而且**只有走 tiler 的作业才会读**（chunk 管理/分配）。
3. ⇒ 这就精确解释了三条实测：**clear/copy/AHB/WSI 全通**（走 `vk_meta` fragment-only fullscreen，`cmd_draw_fullscreen`，**不做 tiler 处理**）✓；**真 draw 必挂**（第一次 tiler 处理就去读 context）✓；**换 chunk_size 地址不动**（读的是 context 而非 chunk）✓；**换驱动版本（v53/v79/v107/v108）都挂**（一直以来如此，与 12/13 号前作一致）✓
4. 故障语义也吻合：`READ` + 请求方 = **CSF 的 LSU**（core_id 62 "csf"、requester 12 "lsu"）⇒ **是固件自己去读它以为有效的地址**，不是 shader、不是 tiler 单元 ✓

---

## 3. S2 插桩（插入点 + 代码；含一条必须说明的"读不到"）

**插入点 A：kbase 路径的 heap 初始化之后**（`panvk_vX_gpu_queue.c:3228-3229` 之后，`first_heap_chunk = first_chunk_va;` 下一行）
```c
   mesa_logi("kbase: tiler heap init: ctx_va 0x%" PRIx64 " first_chunk 0x%" PRIx64
             " chunk_size %u handle %u (group %u)",
             (uint64_t)tiler_heap->context.dev_addr, (uint64_t)first_chunk_va,
             tiler_heap->chunk_size, tiler_heap->context.handle,
             tiler_heap_mem_group);
```
**插入点 B：续租路径**（`:3389` 拿到 `new_ctx, first_chunk` 之后、`:3399 cfg.base = first_chunk;` 前后）同样打印 `new_ctx` —— **如果故障发生在续租之后，B 才是罪证**（但 tri 是首次 draw，B 不会触发；留给长跑游戏）。
**插入点 C：每次 entry 的 `HEAP_SET` 值**（`:2926` 之后，限流打印）
```c
   mesa_logi("kbase: HEAP_SET ctx 0x%" PRIx64 " (subqueue %u entry %" PRIu64 ")",
             (uint64_t)queue->tiler_heap.context.dev_addr, subqueue,
             queue->subqueues[subqueue].kbase.emitted_jobs + 1);
```
**判据**：把这两/三处打印出来的 VA 与 `0x5fffe1e000`（以及另一局的 `0x5ffe800000`）**逐个对位**：
- **相等（或同页）⇒ H2 定案**：故障地址就是 `gpu_heap_va`（第一次 draw 时固件去读它，而该 VA 在该 kctx 的 GPU 页表里不可翻译）。
- 都不等 ⇒ H2 需要再择一（例如 `oom_fbd` 的 VA、或 `desc` 的 dev_addr —— **顺手把这两个也打出来**，一次性覆盖全部候选）：
```c
   mesa_logi("kbase: heap objs: desc 0x%" PRIx64 " oom_fbd 0x%" PRIx64,
             (uint64_t)panvk_priv_mem_dev_addr(queue->tiler_heap.desc),
             (uint64_t)panvk_priv_mem_dev_addr(queue->tiler_heap.oom_fbd));
```

⚠️ **你要求的"打印 heap context 内容前 16 字节"——做不到**：heap context 是**内核在 CUSTOM_VA 区创建的**对象，**没有映射到我们的进程**（不在任何 `panvk_priv_mem` 里）⇒ 用户态**没有可读指针**。能读的只有**我们自己建的 `tiler_heap.desc`**（TILER_HEAP 描述符，host-mapped），所以建议改打**描述符的字段值**（`cfg.base/bottom/top`、`size`、`chunk_size`），而不是 context 的内容 —— 见下。

---

## 4. 最小修复候选（按风险；**先跑 §5 的 F1，它才是能 5 分钟出结论的那一步**）

| 候选 | 内容 | 风险/成本 | 依据 |
|---|---|---|---|
| **F1（首推，Mesa 侧 1~3 行、5 分钟）** | **改 `KBASE_IOCTL_MEM_JIT_INIT` 的参数**（`kbase_kmod.c:1248-1252`：`va_pages 1<<25`、`max_allocations 255`、`phys_pages 1<<25`）——例如把 `va_pages` 显著调小（如 `1<<20`）或换 `max_allocations`，让 CUSTOM_VA 区的**位置/大小**改变 | **低**（只影响 JIT 区几何；失败会 `mesa_logw` 而不是崩） | 我们的注释明说该 ioctl *"carves the CUSTOM_VA zone out of the **top of the SAME_VA zone**"*，而故障地址正是 `0x5ff…`（顶部）⇒ **若被"挖出去"的那段不在 kctx 的 GPU 页表覆盖范围内，固件就会在读 heap context 时翻译失败** ✓ **实验判据：故障地址是否随之改变 / 是否消失** |
| **F2（内核侧，需读 patch/设备内核）** | 查 `gpu_heap_va` 是否被 `kbase_csf_tiler_heap_init()` 映射进 kctx 页表（kernel/mm 侧），以及 MTK 内核版本是否**跳过**该映射 | 中（要动内核模块/需 root 重载） | uAPI 说它是 "GPU VA of Heap context"，**设计上必须可被 GPU 访问** ⇒ 若不可访问，是实现缺陷 |
| **F3（绕开内核 heap，架构级）** | 自己分配 heap context BO 并自行初始化 —— **不可行**：context 的内部结构（chunk 链指针等）由内核/firmware 私有约定维护（`KBASE_IOCTL_CS_TILER_HEAP_INIT/TERM` 成对），Mesa 无法伪造 | 高 | 上游 panthor 也是**内核**建 heap ⇒ 这条路没有旁门 |
| **F4（诊断性，0 风险）** | 若 F1 无效：把 `MEM_JIT_INIT` 的**返回值/错误**也打出来（当前只在失败时 `mesa_logw`），并把 JIT 区实际范围打印出来与 `0x5ff…` 对位 | 0 | 判定"区位置"与"故障地址"是否同段 |

---

## 5. 建议的下一次上机（单变量、5 分钟）

1. **S2 插桩（本报告 §3 的 A + C + 两个对象）** ⇒ 把故障地址对位到具体对象（**这是把 H2 从"成立"变成"定案"的唯一一步**）。
2. **同时/紧接着 F1**：只改 `MEM_JIT_INIT` 的 `va_pages`（一行），重跑 `tri`：
   - **故障消失** ⇒ 根因 = CUSTOM_VA 区与 kctx 页表不匹配 ⇒ 继续收敛 JIT 参数（Mesa 侧可修）✓✓
   - **故障地址跟着变** ⇒ 同上，且证明地址由该区几何决定 ✓
   - **完全不变** ⇒ 该区不是决定因素 ⇒ 转 F2（内核侧映射）。

---

## 6. 未验证 / 限制（诚实清单）

1. 本报告**未做设备实验**；§1 的"两个地址属于 CUSTOM_VA 区的 heap context"是**由 uAPI 语义 + 我们自己的 JIT 注释 + S3 的"不随 chunk_size 变"三方面夹逼**得出，**尚未有"打印 ctx_va == 故障地址"的直接证据**（§3 的插桩就是它）。
2. **我未读内核侧的 tiler heap 实现**（`/root/zenithblue/patches/kbase-common/` 与设备内核）⇒ "该 VA 是否映射进 kctx GPU 页表"仍是**未知**；F2 只是方向。
3. `0x5ffe800000`（0xc2）与 `0x5fffe1e000`（0xc3）**我无法断定各自是哪个对象**（只在 §1.2 给了"同区、相距 ~23 MB、与 chunk_size 无关"的几何事实）；S2 的两处打印 + 两个对象 VA 即可分别对位。
4. §3 的"内容前 16 字节读不到"是**结构性限制**（内核对象无用户映射），不是我没找：若确实想看 context 内容，需要内核侧加一个 debug 通道（成本高、本轮不建议）。
5. F1 的"改 JIT 参数"依据是**注释 + 地址段特征**，属**推断性实验**（但零风险、可逆、5 分钟）——**这正是它值得先跑的原因**。
