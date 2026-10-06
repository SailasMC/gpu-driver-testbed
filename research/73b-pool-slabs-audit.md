# 73b — 对象归属修正后的靶子：**三个 64 KiB `WB_MMAP` 池**（重点：descriptor pool）+ 双常量审计 + 正确 site 取法

日期：2026-10-06 · 只读分析 · 配套：`73-ring-bo-size-desync.md`（双常量）、`72`（越界算式）
**已采纳你的两条更正**：① ring 由**内核**按 `:3114-3124` 的 `ringbuf_size = 64*1024` 分配（**不在我们的 priv-BO 表**）⇒ 我 73 号把"三个连续 64 KiB BO"当 ring 的归属**作废**；② v112 的越界是我 (a) 实验的副作用（已还原为 v113）✓

---

## 1. 你要的"正确 site 取法"（一次定死对象身份）

`__builtin_return_address(0)` 返回的是**已重定位的绝对 PC**；减去**被调函数**的地址既不是文件偏移、也不保证非负（你看到的 `-38520` 就是这么来的）。**两种正确写法（任选其一）**：

```c
#define _GNU_SOURCE
#include <dlfcn.h>

void *ra = __builtin_return_address(0);
Dl_info di;
if (dladdr(ra, &di)) {
   /* 取法 A：文件偏移 ⇒ 服务端 addr2line -e <该 .so> 0x<off> */
   uintptr_t file_off = (uintptr_t)ra - (uintptr_t)di.dli_fbase;
   /* 取法 B：符号名+偏移（最快；该 .so 带 -g 且未 strip） */
   uintptr_t sym_off  = di.dli_saddr ? (uintptr_t)ra - (uintptr_t)di.dli_saddr : 0;
   mesa_logi("v113 bo: va=[…] site_off=0x%zx site_fn=%s+0x%zx",
             (size_t)file_off, di.dli_sname ? di.dli_sname : "?", (size_t)sym_off);
}
```
- **`dladdr` 只对"已加载模块里的地址"有效**；调用点若在 `.so` 内 ⇒ 一定命中 ✓
- 若 `dli_fbase` 为 NULL（极少见），退化为"打裸 `ra`"，服务端用 `addr2line -e /root/final/libvulkan_panfrost_v113.so 0x<pc - 该 .so 的加载基址>`（基址可从 `/proc/<pid>/maps` 取）。
- **同时建议打两个补充字段**（各一行，成本 0）：`queue` 指针（区分"第几个队列/第几次初始化"）与一个**静态计数器**（第几个同 site 分配）⇒ 一次运行就能解释你说的"3 连续 + 1 孤立"。

---

## 2. 三个 `slab_size = 64 * 1024` 的池是谁（`csf/panvk_vX_cmd_buffer.c:1078-1112`，逐行）

| 池 | label | slab_size | create_flags | 装什么 |
|---|---|---|---|---|
| `cmdbuf->cs_pool` | **"Command buffer CS pool"** | 64 KiB | **`WB_MMAP`** | **命令流（CS）chunk**：每条命令流都从这里切 |
| `cmdbuf->desc_pool` | **"Command buffer descriptor pool"** | 64 KiB | **`WB_MMAP`** | **描述符**：管线/描述符集/顶点属性/tiler 与 render 描述符等 |
| `cmdbuf->tls_pool` | "TLS pool" | 64 KiB | **`NO_MMAP`** | 着色器 TLS |

（三者都由 `panvk_pool_init(&cmdbuf->…, device, &pool->…_bo_pool, …)` 从**设备级 BO 池**取 slab）

**关键推论**：
- 你观测到的是**三个连续 + 一个孤立**、全部 `flags=0x40 (WB_MMAP)`、全部 64 KiB ⇒ **它们只可能是 `cs_pool` / `desc_pool` 的 slab**（`tls_pool` 是 `NO_MMAP`，flag 会带 `0x04`）✓
- **`desc_pool` 是头号靶子**，理由三条：
  1. draw 时 GPU 用 **LSU 读**的正是**描述符**（顶点属性、TILER/RENDER 描述符、push constants 等）——与"**READ** + CSF LSU"完全对上；
  2. 越界读的形态"**紧贴 slab 末尾之后**"最自然的解释是**某个描述符数组/描述符游标被用超了一个条目**（数组长度、`pos`、`count` 算大 1），而不是"随机野指针"；
  3. 本树在 `csf/panvk_vX_cmd_draw.c:4700-4710` 有"**chunks not released prematurely**"的注释——即开发者已知**描述符 chunk 的使用期/释放**是敏感区；三处 slab 里它最可能被"用超"。
- `cs_pool` 是第二嫌疑（命令流被固件**取指**读；但取指故障应报 `EXECUTE`，而我们是 `READ` ⇒ 优先级低于 desc）✓

---

## 3. 双常量审计（本轮最有复用价值的产出）

**定义**：同一个"缓冲区尺寸"在**两处独立出现**——一处用于**分配/告诉别人**，另一处用于**回绕/取模/边界算术**。两者一旦漂移 ⇒ **确定性越界**（v112 就是这么被造出来的）。

| # | 量 | 分配/告知侧 | 算术/回绕侧 | 风险 |
|---|---|---|---|---|
| **1** | **ring 大小** | `csf/panvk_vX_gpu_queue.c:3114/3119/3124` `drm_panthor_queue_create{ .ringbuf_size = 64*1024 }`（**内核持有**） | `:67 #define KBASE_RINGBUF_SIZE (64*1024)` → `:265 :438 :440 :456 :1329 :1409 :1916 :1991 :1993` 的 `% KBASE_RINGBUF_SIZE` | **已爆**（v112）⇒ **必须用同一个符号或 static_assert 绑死** |
| **2** | **CS tracebuf 大小** | `:38-39 MIN/DEFAULT_CS_TRACEBUF_SIZE (512 KiB / 2 MiB)`（经 `KBASE_IOCTL_CS_TRACEBUF_INIT` 告知内核） | 驱动侧 `pos`/读写游标（**是否回绕？**若线性则无此项） | 低（若无回绕）——**需一眼确认** |
| **3** | **desc tracebuf 大小** | `:36-37 MIN/DEFAULT_DESC_TRACEBUF_SIZE (128 KiB / 2 MiB)` | 同上 | 低（同上） |
| **4** | **render desc ringbuf** | `queue->render_desc_ringbuf` 的**创建尺寸**（`calc_render_descs_size()` 一族） | ctx 里的 `desc_ringbuf.ptr/pos`、以及 `:782-786` 快照打印 | **中**（若创建尺寸与 `pos` 回绕用的尺寸不同源） |
| **5** | cmdbuf 三个池的 `slab_size = 64*1024`（`:1081/1092/1104`） | 池的 slab 尺寸（**不回绕**） | — | 不属"双常量"，但**是本次越界的对象候选**（§2） |

**审计方法（可复用到别处）**：
```bash
grep -rnE "% *[A-Z_]+(_SIZE|SIZE|_BYTES)|% *[a-z_]+\.size|% *KBASE|% *DEFAULT" src/panfrost/vulkan/
grep -rnE "^#define .*(_SIZE|_BYTES|RINGBUF|TRACEBUF)" src/panfrost/vulkan/ | sort
# 然后对每个「算术侧符号」反查：它是否也在「分配/告知侧」出现过（必须同源）
```

---

## 4. 最小修复候选 + 你 5 分钟可验证的实验

| # | 实验/修复 | 改动 | 判据 |
|---|---|---|---|
| **E1（先做，单变量）** | **把 `desc_pool` 的 slab 从 64 KiB 改成 256 KiB**（`csf/panvk_vX_cmd_buffer.c:1092`，**1 行**） | 一次重编 | **故障地址是否随之移动/消失** ⇒ 移动 ⇒ **对象 = desc pool 的 slab**（越界读被"推远"）；不变 ⇒ 换 E2 |
| **E2** | 同上但改 `cs_pool`（`:1081`） | 1 行 | 移动 ⇒ 对象 = CS pool |
| **E3（真正的修复，需 E1/E2 定位后）** | **在 pool 的 slab 末尾留一段不可访问的守卫**（例如 slab 尺寸取整后留 1 页空隙，或让 `panvk_pool` 在新建 slab 时记录"最后合法字节"并在 DEBUG 下断言所有描述符游标 `<= end`） | 中 | 从此**越界读会被 MMU 立即抓住**（而不是读进相邻分配）⇒ 便于长期防回归 |
| **F-B（零风险，建议与 E1 同版）** | **双常量绑死**：`:3114/3119/3124` 改用同一个符号（或加 `STATIC_ASSERT(ringbuf_size == KBASE_RINGBUF_SIZE)`）；并在启动时打一行两者数值 | **0** | 防止 v112 那类"实验自伤"**再次发生**（这条我认为必须上） |
| **F-C（零风险）** | 在 `:3041` 打**实际 BO 尺寸**、在 `panvk_pool` 建 slab 时打 `slab va/size`（限流） | 0 | 让"对象是谁/边界在哪"在任何实验中**一眼可见**（也是 E1/E2 的读数来源） |

**推荐顺序**：**F-B + F-C（零风险，先上）→ E1（1 行，5 分钟）→ 视结果 E2/E3。**

---

## 5. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§2 的"三个连续 64 KiB 必是 cs/desc 池"依据是**flag 组合**（`WB_MMAP` = `0x40`，而 `tls_pool` 是 `NO_MMAP`）+ 三处 `slab_size` ⇒ **强推断，非直接证据**（E1/E2 与 F-C 会给出直接证据）。
2. **"desc_pool 是头号靶子"是排序，不是结论**：我没有把故障地址与**某个具体描述符游标**对上（那需要 E1/E2 的地址移动 + F-C 的 slab 边界打印）。
3. §3 的审计**只覆盖了"取模"形态的双常量**；还有一类**"两次独立算出同一个尺寸"**（不写成 `%`，例如 `calc_render_descs_size()` 与 `desc_ringbuf.pos` 的上限比较）**我没做穷举** ⇒ 第 4 项标为"中"而非"已确认"。
4. `MIN/DEFAULT_*_TRACEBUF_SIZE` 是否参与回绕**我未核对**（若纯线性则无风险）——一眼 `grep` 即可定。
5. 我**未读** `panvk_pool.c`（本轮 grep 无输出，说明该文件路径/名字与我的假设不同）⇒ "slab 如何分配、是否已有守卫"**未核实**；E3 的具体写法需要先读该文件。
6. 若 E1/E2 都"地址不变"，则"越界对象=池 slab"这一支**被否证**，应回到 v109 的三连续 BO **究竟属于哪次分配**（用 §1 的正确 site 取法一次定死）。
