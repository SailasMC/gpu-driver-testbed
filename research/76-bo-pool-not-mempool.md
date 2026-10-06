# 76 — 按你的新框架：设备级 mempool **不是**那个 64 KiB slab；E1/E2 之所以无效的原因找到了

日期：2026-10-06 · 只读分析 · 配套：`75c-two-bos-overlap.md`、`73b-pool-slabs-audit.md`
**先接受你的方法论要点**：`tri` 不做 tracing ⇒ `panvk_utrace_capture_data`/`cmd_copy_data` **从未执行** ⇒ **那个 utrace 堆 BO 只是"占 VA 布局"** ⇒ **故障对象 = 紧随其后分配的那块** ✓ ⇒ **我 74/75 追的"capture 消费者"是错的靶子**，以本轮为准 ✓（也顺带解释了"堆耗尽"与"关诊断"都不影响 ✓）

---

## 1. 你问的第 1 点：`dev->mempools` 清单（**已核到 file:line**）

`panvk_device.h:84-88`：
```c
   struct {
      struct panvk_pool rw;        /* 缓存 */
      struct panvk_pool rw_nc;     /* 非缓存 */
      struct panvk_pool exec;      /* 可执行（shader） */
   } mempools;
```
`panvk_vX_device.c:73-111`（`panvk_device_init_mempools()`，**device init 期**）：

| 池 | label | **slab_size** | create_flags | init 行 |
|---|---|---|---|---|
| `mempools.rw` | Device RW cached memory pool | **16 KiB** | `WB_MMAP`(0x40) | `:79`（props）/`:86`（init） |
| `mempools.rw_nc` | Device RW uncached memory pool | **16 KiB** | `GPU_UNCACHED`(0x20) | `:91`/`:98` |
| `mempools.exec` | Device executable memory pool (shaders) | **16 KiB** | `EXECUTABLE\|WB_MMAP`(0x41) | `:103`/`:110` |

⇒ **重要否定结果**：**三个设备级 mempool 的 slab 都是 16 KiB** ⇒ **你观测到的那个 64 KiB / `flags=0x40` 的 slab 不可能来自任何 `dev->mempools.*`** ✗（你第 1 点的假设**不成立**——但方向对了一半，见 §2）

---

## 2. 那 64 KiB slab 是谁 + **为什么 E1/E2 完全不生效**（本轮关键）

**它来自"命令池的 BO 池"**（backing pools）。证据链：
- 你 v115 的 `site_off=0x9c8270` = **`panvk_pool_alloc_backing`** ⇒ **这是通用 slab 分配器**，谁调用它取决于**哪个 pool 缺 slab**；
- `csf/panvk_vX_cmd_buffer.c:1091/1109/1121` 显示：**cmdbuf 的三个池是"消费者池"**，它们的 slab **从 `pool->cs_bo_pool` / `pool->desc_bo_pool` / `pool->tls_bo_pool` 取**：
```c
   panvk_pool_init(&cmdbuf->cs_pool,   device, &pool->cs_bo_pool,   NULL,  &cs_pool_props);
   panvk_pool_init(&cmdbuf->desc_pool, device, &pool->desc_bo_pool, NULL,  &desc_pool_props);
   panvk_pool_init(&cmdbuf->tls_pool,  device, &pool->tls_bo_pool,  &pool->tls_big_bo_pool, &tls_pool_props);
```
- **E1/E2 改的是"消费者池"的 `slab_size`（`csf/panvk_vX_cmd_buffer.c:1081/1092`）——那是"向 BO 池请求多大"**，而 **BO 池自己的 `slab_size`（决定真实 BO 尺寸 = 你看到的 64 KiB）从来没被改过** ⇒ **所以故障地址纹丝不动** ✓✓✓
⇒ **⇒ 真正的单变量在这两处之一**（用这条命令定位，5 秒）：
```bash
grep -rn "cs_bo_pool\|desc_bo_pool\|tls_bo_pool" \
  /root/zenithblue/work/mesa/src/panfrost/vulkan/ | grep -i "pool_init\|slab"
```
它们应在 **`panvk_cmd_pool_create()`**（`csf/panvk_vX_cmd_buffer.c` 或 `panvk_cmd_buffer.c`）里以 `panvk_pool_init(&pool->cs_bo_pool, device, NULL, NULL, &props)` 的形式初始化，`props.slab_size` 就是**决定 64 KiB BO 的那个数**。

**⇒ 下一个实验（5 分钟，单变量）**：把**BO 池**的 `slab_size` 从 `64*1024` 改成 `256*1024`（**改 BO 池，不要改 `:1081/:1092` 的消费者池**）⇒
- **故障地址随之移动（或消失）** ⇒ **对象 = 该 BO 池的 slab** ⇒ 锁定 ✓✓
- **不动** ⇒ 那个 64 KiB slab 属于别的 pool ⇒ 走 §3 的"打标签"一次定死（比猜快得多）。

---

## 3. 更省的一条：**直接打 pool 的 label**（一次运行就定死对象）

`panvk_pool_alloc_backing` 是**所有 pool 的共用路径**，而每个 `panvk_pool_properties` 都带 **`.label`**（`cs_pool_props.label = "Command buffer CS pool"` 等）⇒ **在 `panvk_pool_alloc_backing`（你已有的 v109 打印旁）加一个字段**：
```c
   mesa_logi("v118 slab: va=[0x%" PRIx64 ", 0x%" PRIx64 ") size=0x%" PRIx64
             " label=\"%s\" pool=%p site_off=0x%zx",
             va, va + size, size, pool->props.label, (void *)pool, site_off);
```
⇒ **一行就能说出"`0x5fffe07000` 那块 64 KiB 到底是谁的"** —— 比逐个改 slab 尺寸试要快，而且**不需要重编多版**（一次重编，然后任何 slab 都自带身份）✓ **这是我推荐先做的**（与 §2 的实验不冲突：先打标签，再改那一个 slab）。

---

## 4. 那块 slab 里装什么 / 谁会被"走多一格"（你第 2 点）

按"命令池的 BO 池"这一归属，它承载的是**命令缓冲的命令流/描述符 chunk**；draw 时 GPU 按**数组/游标**走的候选：
| 候选 | 为什么可能走多一格 |
|---|---|
| **render/tiler 描述符数组**（`render_desc_ringbuf` 的 chunk 链） | 本树在 `csf/panvk_vX_cmd_draw.c:4700-4710` 就写着 "chunks not released prematurely" ⇒ 描述符 chunk 的**数量/生命周期**是敏感区 |
| **顶点属性/描述符集数组** | 数量由 pipeline 布局算出，**算大 1** 就会读第 N+1 个（正好越过 slab 末尾） |
| **attachment/FBD 数组** | v95 的 fb 对账已证明 extent 正确，但**数组条目数**未被同样审计过 |
⇒ **判据**：等 §3 的 label 定死是哪个池后，再在那个池的消费者侧（`panvk_pool_alloc` 的调用方）加一条 **DEBUG 断言：`ptr + len <= slab_end`**（或把 `len` 打出来与 slab 余量比）⇒ **一次运行就能抓到"走多一格"的那个循环**。

---

## 5. 两条小事的确认

1. **"一次运行出现两个 utrace BO（256 MiB + 1 MiB）"**：**两者 VA 区间重叠**（256 MiB 覆盖到 `0x5fffe00000`，1 MiB 从 `0x5fffd1f000` 开始）⇒ **同一 GPU VA 空间不可能重叠** ⇒ **那两行不是同一次运行/同一进程**（或同一进程内**两个 device**）✓
   而 `get_utrace_clone_mem_size()`（`csf/panvk_vX_utrace.c:176-178`）**不是** `DEBUG_GET_ONCE_*`——它每次调用都 `debug_get_num_option`（`os_get_option` 只缓存 env 字符串本身）⇒ **同一进程内该值不会变** ⇒ **⇒ 你的 env A/B 效力没问题** ✓（只需在打印里加 `PID + dev 指针 + 读到的值` 即可永久消除歧义）
2. **以本轮框架为准** ✓：`故障 = slab 末尾 + 0x7000 = 下一个分配槽`（`0x7000` 是分配器的每分配间隔）；我此前"堆末尾 + 0x1E000"与"1:1"两种说法**作废** ✓

---

## 6. 未验证 / 限制（诚实清单）

1. 本轮**未做设备实验**；§1 的设备级 mempool 清单**逐行核实**（`panvk_device.h:85-88` + `panvk_vX_device.c:73-111`），**这是本轮最硬的否定结果**。
2. **BO 池的确切 `panvk_pool_init` 位置我没能定位**（grep 未命中，说明它的写法与我的假设不同，例如经由 `panvk_cmd_pool` 的数组/宏）⇒ §2 给了**定位命令**，具体的 `file:line` 需要那一条 grep 的输出；**因此 §2 的 diff 我只能给"改哪个数"，不能给行号**（不猜）。
3. §3 的 label 插桩**依赖 `panvk_pool_alloc_backing` 能拿到 `pool` 指针**（通常可以）；若拿不到，退化为打印 `site_off + 尺寸`并靠 §2 的实验排除。
4. §4 的三个候选是**排序**，没有把故障地址与某个具体数组游标对位（需要 §3/§2 先定对象）。
5. 所有改动**未编译、未上机**。
