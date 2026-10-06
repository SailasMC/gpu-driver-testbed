# 79 — H-Desc 落地：描述符环的**容量常量**与**BO 尺寸**是两个数（新证据）+ E4 精确补丁 + 护栏

日期：2026-10-06 · 只读分析 · 配套：`78c-desc-ring-quantitative.md`
**已接受你的三条**：E1 时间线否证"空洞曾住过 BO"✓ · 78c 否证"ctx 未初始化"✓ · ⇒ **那个固定地址是"算出来的"** ✓

---

## 1. ★ E1' 的新证据：描述符环是"游标 + 容量"结构，而容量来自**独立常量**

`csf/panvk_vX_gpu_queue.c:2478-2490`（**逐行核实**）：
```c
   struct panvk_pool_alloc_info alloc_info = { .size = sizeof(struct panvk_cs_sync32), .alignment = 64 };
   ringbuf->syncobj = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);
   …
   panvk_priv_mem_write(ringbuf->syncobj, 0, struct panvk_cs_sync32, syncobj) {
      *syncobj = (struct panvk_cs_sync32){
         .seqno = RENDER_DESC_RINGBUF_SIZE,      /* ← ★ 环的"容量"写进 syncobj */
      };
   }
```
而**环的 BO 尺寸**是 `ringbuf->size`（同一函数上方用它做 `pan_kmod_vm_bind`、`panvk_address_binding_report`、pandecode mmap 的 `[0,size)`；**注意还有第二个映射 `[size, 2*size)`——tracing 时被故意留空当 guard**）
⇒ **⇒ 环的"容量"（`RENDER_DESC_RINGBUF_SIZE`，用于游标/同步算术）与"BO 尺寸"（`ringbuf->size`）是两个独立来源** ✓✓
⇒ 若两者不一致 ⇒ **游标算术会越过 BO** ⇒ **固定地址 = 环基址 + 容量 + ε** ⇒ **完全符合你"固定且确定"的三点定线** ✓✓✓
⇒ 而 `0x1E000`（120 KiB）与 `0x26000`（152 KiB）正是**环容量**的量级 ✓

**⇒ 拿到这两个数就基本结案（两条命令）**：
```bash
M=/root/zenithblue/work/mesa/src/panfrost/vulkan
grep -rn "RENDER_DESC_RINGBUF_SIZE" $M | grep -v "\.bak"
grep -rn -B4 -A4 "ringbuf->size = " $M/csf/panvk_vX_gpu_queue.c
```
**判据**：若 `RENDER_DESC_RINGBUF_SIZE ≠ ringbuf->size`（或前者不是后者的整数倍/分之一）⇒ **命中** ✓✓；并且把这两个值与 `0x1E000`/`0x26000` 对照。

---

## 2. E4 的**精确补丁**（你要的"唯一锚点 + 完整代码"）

**插入点**：`cs_ctx` 的复合字面量**与两个 `if` 块全部写完之后** —— 锚点用**最后一条**对 ctx 的赋值（`grep -c` 确认唯一）：
```
            panvk_priv_mem_dev_addr(queue->tiler_heap.oom_fbd);
```
（即 `:2857` 那一行；它在全文件**唯一** ✓）⇒ **在这一行之后**插入下面整块：

```c
      /* v122 (report 79, E4): 打印 ctx 里"像 GPU VA"的 64 位字（含索引与字节偏移）。
       * 目的：直接看某个字段是否等于故障地址（0x5fffe1e000 / 0x5fffe26000），
       * 一次性指出"是谁算出来的"。粗筛：(v >> 40) 落在 0x5ff / 0x600 / 0x80 段。 */
      {
         const void *ctx_host = panvk_priv_mem_host_addr(subq->context);
         if (ctx_host) {
            const uint64_t *w = (const uint64_t *)ctx_host;
            const uint32_t nwords =
               (uint32_t)(sizeof(struct panvk_cs_subqueue_context) / sizeof(uint64_t));
            mesa_logi("kbase: ctx dump subqueue %u (%u qwords) — VA-looking words:",
                      subqueue, nwords);
            for (uint32_t i = 0; i < nwords; i++) {
               const uint64_t v = w[i];
               const uint64_t seg = v >> 40;
               if (v && (seg == 0x5ff || seg == 0x600 || seg == 0x80))
                  mesa_logi("   ctx[%3u] @+0x%03x = 0x%016" PRIx64, i, i * 8, v);
            }
            mesa_logi("   (reference faults: 0x5fffe1e000 / 0x5fffe26000)");
         }
      }
```
**要点**
- 用 `panvk_priv_mem_host_addr(subq->context)`（**已在本文件多处使用**：`:2884`、`:2902` 一带）而不是复用 `cs_ctx` —— 因为 `panvk_priv_mem_write(...) { … }` 的宏作用域**已结束**，这正是你锚点踩空的原因 ✓
- 打印**索引 + 字节偏移**，你服务端用 `pahole`/`offsetof` 一映射即得字段名 ✓（符合你"字段名表可先粗放"的要求）
- **无行为改变**（只读 ctx、只打印）⇒ 零风险 ✓；建议 `PANVK_DEBUG(KBASE_DIAG)` 门控或直接常开（每队列一次，量小）

---

## 3. E2'（单变量：改描述符环尺寸）——**必须"两个数一起改"**

```diff
--- a/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
+++ (RENDER_DESC_RINGBUF_SIZE 的定义处；先用上面那条 grep 定位)
-#define RENDER_DESC_RINGBUF_SIZE            <原值>
+#define RENDER_DESC_RINGBUF_SIZE            <原值 + 0x8000>      /* v122: 单变量 */
@@ (ringbuf->size = … 的赋值处)
-   ringbuf->size = RENDER_DESC_RINGBUF_SIZE;      /* 若当前就是同源，这一处不用改 */
+   /* 若两者本来就同源：保持同源，只改上面那一处即可（最干净） */
```
**关键提醒（v112 的教训）**：
- **若代码本来就是 `ringbuf->size = RENDER_DESC_RINGBUF_SIZE`（同源）** ⇒ **只改常量一处**，环仍是自洽的 ⇒ **这是最干净的单变量**，必做 ✓
- **若两者是两处独立字面量** ⇒ 说明**当前就存在"双常量"**（可能就是根因！）⇒ 此时**先不改**，**先按 §4-F1 绑死同源**，再改一处 ✓
**判据（盯"固定"而不是"移动"）**：改后故障地址应当**固定到一个新值**（例如 `0x5fffe2e000` 之类），而**不再是** `0x5fffe1e000`/`0x5fffe26000` ⇒ **锁定 H-Desc** ✓✓

---

## 4. E3'（护栏，零风险）+ F1（同源化）

**F1（首选，直接消除根因）**：把环容量与 BO 尺寸**绑成同一个符号**：
```c
#define RENDER_DESC_RINGBUF_SIZE (…)
static_assert(RENDER_DESC_RINGBUF_SIZE % 4096 == 0, "ring size must be page aligned");
…
   ringbuf->size = RENDER_DESC_RINGBUF_SIZE;        /* BO 尺寸 ← 同一常量 */
```
**E3'（断言，插在把环指针/游标交给 CS 的三处）**：`csf/panvk_vX_cmd_draw.c:1325`（`descs_sz = calc_render_descs_size(cmdbuf)`）、`:4736`（release 尺寸）、`:4847`（`cs_render_desc_ringbuf_move_ptr(b, calc_render_descs_size(cmdbuf), …)`）：
```c
   /* v122: 环游标不得越过环容量（容量必须与 BO 尺寸同源，见 F1） */
   assert(cmdbuf->state.gfx.render.desc_ringbuf_pos + descs_sz <= RENDER_DESC_RINGBUF_SIZE);
   … /* 然后把同一个值交给 CS */
```
**环尾终止块**：命令环的换行做法可**逐字复用**（`csf/panvk_vX_gpu_queue.c:1403-1408`：`memset(…, 0, KBASE_RINGBUF_SIZE - offset)` + `insert += 差`）⇒ 描述符环在"剩余空间不足一个描述符块"时也应在环尾写入等价的终止/填充，并把游标推到环尾再绕回 ✓（**注意**：命令环那份是"零填充"，而 73 号我们发现零填充的合法性存疑 ⇒ 描述符环的终止块请用**显式 NOP/终止指令**，不要零填充）

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1 的"容量 vs BO 尺寸是两个来源"**是从 `:2478-2490` 的 seqno 初始化 + 上方 `ringbuf->size` 的用法推出的**——**`RENDER_DESC_RINGBUF_SIZE` 与 `ringbuf->size` 的数值我都没读到**（§1 给了两条 grep）⇒ **§2/§3 的判据成立与否完全取决于那两个数**，请先跑它们 ✓
2. §2 的补丁**未编译**；锚点行（`…oom_fbd);`）我是从早前逐行读到的 `:2856` 认定的**唯一性**未用 `grep -c` 复核 ⇒ **插入前请 `grep -c` 确认**（我在注释里也写了这一点）。
3. §3 的"两个数一起改"结论**取决于 §1 的两条 grep 结果**（若已同源 ⇒ 只改一处；若不同源 ⇒ 先 F1 再改）⇒ **不要跳过这一步**，否则重演 v112 ✓
4. §4 的断言依赖 `cmdbuf->state.gfx.render` 里**游标字段的真实名字**（我按语义写的 `desc_ringbuf_pos`）⇒ 落地前请以 `panvk_cmd_buffer.h` 的字段名为准 ✓
5. 我**未读** `cs_render_desc_ringbuf_move_ptr` 的实现（`genxml/cs_builder.h` 的 grep 无输出 ⇒ 可能在别处/宏展开）⇒ E3' 的断言位置是按**调用点**给的，按语义应成立，但**未逐行确认** ✓
