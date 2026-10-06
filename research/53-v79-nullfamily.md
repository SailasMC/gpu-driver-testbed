# v79 — 空指针【一族】一次性修复（instance 2 + 报告 51 §7 高风险项）

日期：2026-10-06 · 构建树 `/root/zenithblue/work/mesa` · 构建目录 `/root/zenithblue/build/android-v4`
构建配置实测：`buildtype=release · optimization=3 · b_lto=False · b_ndebug=if-release · strip=False`

---

## 0. 一句话

`get_cs_cfg+0x320` 解析为 **`src/panfrost/genxml/decode_csf.c:2416` 的 `&instrs[i]`**
（故障指令 `0xef6764: ldr x22, [x11, x27, lsl #3]`，`x11` = `instrs` = **NULL**），
根因是 `pandecode_fetch_gpu_mem()` 在 CS 二进制未映射时返回 NULL。
本轮共改 **6 处**（instance 2 + §7 的 5 处 🔴/🟠），全部为【最小空保护 + 早返回】，
与 v77 的 `if (!vs) return;` 同风格。**6 处中 4 处经反汇编确认进入了机器码**，
2 处被编译器判定为死代码而消除（原因见 §3.2，非改动失效）。

---

## 1. `get_cs_cfg+0x320` 的解析结论（任务第 1 条）

### 1.1 用的什么二进制

`/root/final/libvulkan_panfrost_v76.so`
sha256 `fa9f29afe9ff4b016de7ccd0945afcfed873121a70c66caddd6dc342a97e5109`（20 057 992 B）
—— 与任务书给定的 `fa9f29af…` 一致 ✅

### 1.2 符号定位（关键：4 个同名 static 副本）

`decode_csf.c` 被 **每个 PAN_ARCH 编译一次**，所以 `.so` 里有 4 个 `get_cs_cfg`：

```
$ readelf -sW libvulkan_panfrost_v76.so | grep -E "get_cs_cfg$"
 26066: 0000000000ee5744  1568 FUNC LOCAL 14 get_cs_cfg
 26225: 0000000000ef6444  1568 FUNC LOCAL 14 get_cs_cfg
 26386: 0000000000f06f54  1568 FUNC LOCAL 14 get_cs_cfg
 26550: 0000000000f17cd0  1568 FUNC LOCAL 14 get_cs_cfg
```

地址换算（task 给定的约定，已用 instance 1 反证）：

```
0xef6444 + 0x320 = 0xef6764        ← 落在第 2 个副本内 [0xef6444, 0xef6444+1568=0xef6a64)
```

**约定反证（instance 1）**：`0xb8a5c0` − `0x34` = `0xb8a58c`，而
`readelf` 里 `panvk_cmd_draw` 恰有一个符号在 `0xb8a58c` ✅ ⇒ 约定 `[lib]+X` 中
`X` = 绝对 vaddr，第二个 `+0xNN` 是**函数内偏移**，成立。
（另核：本 ELF 的 `p_offset == p_vaddr`，file-offset 与 vaddr 同为 `0xef6764`，不存在映射歧义。）

4 个副本各自 `+0x320` 只有一个是 `0xef6764`：

| 副本入口 | +0x320 | == 0xef6764 ? |
|---|---|---|
| `0xee5744` | `0xee5a64` | ✗ |
| **`0xef6444`** | **`0xef6764`** | ✅ |
| `0xf06f54` | `0xf07274` | ✗ |
| `0xf17cd0` | `0xf17ff0` | ✗ |

`llvm-addr2line -e libvulkan_panfrost_v76.so -f -C -i -a 0xef6764` ⇒
`get_cs_cfg` / `decode_csf.c:0`（行号被 strip，见 §1.4）

### 1.3 故障指令与「为什么是空指针」

```
$ llvm-objdump -d --start-address=0xef6750 --stop-address=0xef6774 libvulkan_panfrost_v76.so
  ef6750: b9401409      ldr  w9,  [x0, #0x14]
  ef6754: f83b7900      str  x0,  [x8, x27, lsl #3]
  ef6758: aa0003f9      mov  x25, x0
  ef675c: 11000528      add  w8,  w9, #0x1
  ef6760: b9001408      str  w8,  [x0, #0x14]
  ef6764: f87b7976      ldr  x22, [x11, x27, lsl #3]     ★ 故障指令（get_cs_cfg+0x320）
  ef6768: d379fec8      lsr  x8,  x22, #57
  ef676c: 7100411f      cmp  w8,  #0x10
  ef6770: 54fffe61      b.ne 0xef673c <get_cs_cfg+0x2f8>
```

**为什么是空指针 —— 寄存器来源（逐条追踪，非推理）**：

```
ef64b8: bl  pandecode_find_mapped_gpu_mem_containing   ; 取 struct pandecode_mapped_memory *mem
ef64bc: cbz x0, 0xef6a24                                ; mem==NULL 已有保护（函数提前返回）
ef64c0: ldp x8, x9, [x0, #0x20]                         ; x8 = mem->addr,  x9 = mem->gpu_va
ef64cc: add x8, x8, x24                                 ; x8 = mem->addr + gpu_va
ef64d0: sub x22, x8, x9                                 ; x22 = mem->addr + gpu_va - mem->gpu_va
                                                        ;      = ★ instrs（这正是 __pandecode_fetch_gpu_mem 的返回值）
...
ef650c: bl  rzalloc_array_size                          ; cfg->blk_map
ef6510: str x0, [x21, #0x10]                            ; cfg->blk_map = x0（x21 = cfg）
ef651c: str x22, [x21]                                  ; cfg->instrs   = x22  ← 未做任何空检查
...
ef6584: ldr w8,  [x22]                                  ; i = *instr
ef6588: ldr x9,  [x21, #0x10]                           ; cfg->blk_map
ef6590: ldr x10, [x21]                                  ; cfg->instrs
ef6594: lsl x8,  x8, #3
ef6598: ldr x3,  [x9, x8]                               ; instr 值（x3 传给 cs_unpack）
ef65a0: ldr x28, [x10, x8]                              ; instrs[i]
...
ef6764: ldr x22, [x11, x27, lsl #3]                     ; ★ instrs[i]，x11 = cfg->instrs = NULL
```

**⇒ `x11` 就是 `cfg->instrs`，值为 NULL ⇒ 以 NULL 为基址 + 索引 ×8 解引用 ⇒ SIGSEGV。**

为什么 `instrs` 会是 NULL：
`__pandecode_fetch_gpu_mem()`（`src/panfrost/genxml/decode.h:56-73`）在
`pandecode_find_mapped_gpu_mem_containing()` 返回 NULL 时只做：

```c
if (!mem) {
   fprintf(stderr, "Access to unknown memory %" PRIx64 " in %s:%d\n", ...);
   fflush(ctx->dump_stream);
   assert(0);                                     /* ← 被编译掉 */
}
assert(size + (gpu_va - mem->gpu_va) <= mem->length);
return mem->addr + gpu_va - mem->gpu_va;          /* ← mem 为 NULL 时这是 UB/NULL 算术 */
```

本机实测 `b_ndebug=if-release` + `buildtype=release` ⇒ **`assert(0)` 被完全编译掉**
（release 下 `NDEBUG` 生效），于是函数**不中断**、继续返回 `NULL + gpu_va - 0`。
编译器知道 `mem->addr == 0` 时 `gpu_va` 项无意义，于是 `instrs` 直接为 NULL。
**这是"断言当保护用"的经典 release 陷阱，与 §7 #3 `assert(vs)` 同型。**

### 1.4 行号说明（诚实标注：不是 addr2line 直接给的行号）

`addr2line` 对本函数**所有**地址（含 `0xef66e0…0xef67b8` 整段）都返回
`decode_csf.c:0` —— 该 CU 的 `.debug_line` 行号表已被 strip（`.debug_info`
与 `.debug_line` 段仍在，但没有该 CU 的行程序）。**因此 file:line 是"指令 ↔ 源码"对照得出的，不是工具直出**：

| 依据 | 内容 |
|---|---|
| 函数 | `get_cs_cfg`（`decode_csf.c:2391-2498`）—— 符号 + 唯一源码定义（我们的树与上游 `main` 逐字一致，`diff` 无输出） |
| 指令 | `ef6764: ldr x22, [x11, x27, lsl #3]`，`x11` = `cfg->instrs`（`ef6590` 载入） |
| 源码 | `decode_csf.c:2416` `const uint64_t *instr = &instrs[i];` —— 该行**在 k=3 下**编译成 `cfg->instrs + i*8` 的一次解引用 |
| 结论 | **`src/panfrost/genxml/decode_csf.c:2416`（函数 `get_cs_cfg`）** |

> 另一条一致证据：报告 51 的 v76 现场显示函数大小 1568 B。本轮加守卫后
> `get_cs_cfg` 变为 1588 B，**恰好 +20 B**；反汇编可见新增
> `ef4e68: ldr x8,[x0,#0x20]` + `ef4e6c: cbz x8,0xef541c`（→ `mov x21,xzr; b +0x44` = `return NULL`），
> 且旧代码的 `ldp x8,x9,[x0,#0x20]` 被拆成两条 —— 正好解释这 20 B（见 §3.1）。

---

## 2. instance 2 的修复（任务第 2 条）

### 2.1 上游对照（先查上游，不凭猜）

```bash
curl -s "https://gitlab.freedesktop.org/api/v4/projects/176/repository/files/\
src%2Fpanfrost%2Fgenxml%2Fdecode_csf.c/raw?ref=main" -o /tmp/up_decode_csf.c
diff /tmp/up_decode_csf.c src/panfrost/genxml/decode_csf.c   # 无输出
```

⇒ **上游 `main` 与本树逐字一致，上游没有等价保护可照抄。**
采用任务书允许的第二种分支：**最小改动**。

### 2.2 改动前后对照

文件：`src/panfrost/genxml/decode_csf.c`
改前 sha256：`d868a0c10d474d050ae69040eedd0051e91bcb8211062bc28f9afdb1b23483ec`
改后 sha256：`cb2a7886f632c8011d12efe9aa041ffc5d6830a26982076a4659bc1d5affab38`
备份：`decode_csf.c.bak-v79-1791217047`

```diff
@@ -2401,6 +2401,13 @@ get_cs_cfg(struct pandecode_context *ctx, struct hash_table_u64 *symbols,
 
    uint64_t *instrs = pandecode_fetch_gpu_mem(ctx, bin, bin_size);
 
+   /* The CS binary is not (fully) mapped in the pandecode memory map:
+    * pandecode_fetch_gpu_mem() then returns NULL, and the loop below
+    * would dereference it at instrs[i].  Callers already handle a
+    * NULL return (see pandecode_cs_binary() and collect_...()). */
+   if (!instrs)
+      return NULL;
+
    cfg = rzalloc(symbols, struct cs_code_cfg);
    _mesa_hash_table_u64_insert(symbols, bin, cfg);
```

（改后 2402–2411 逐行见 §6 附录 A）

### 2.3 调用方契约核对（确认 `return NULL` 安全）

| 调用点 | 是否已处理 NULL |
|---|---|
| `decode_csf.c:2594` `main_cfg = get_cs_cfg(...)` → 紧接 `print_cs_binary(ctx, bin, main_cfg, "main_cs")` | ✅ `print_cs_binary` 首行即 `if (!cfg) return;`（已核实） |
| `decode_csf.c:2493` 递归调用（丢弃返回值） | ✅ 无解引用；`collect_indirect_branch_targets_recurse()` 操作的是**本层传入的 `cfg == cur_blk` 所属 cfg**，与递归返回值无关 |

⇒ 提前返回 NULL 不会引入新的解引用。

---

## 3. 一并修掉的 §7 高风险项（任务第 3 条）

### 3.0 总表

| # | file:line（**v79 改后**） | 函数 | 可触发性判断（依据） | 本轮 | 机器码已确认？ |
|---|---|---|---|---|---|
| inst2 | `genxml/decode_csf.c:2408` | `get_cs_cfg` | ✅ 可达：`assert(0)` 被 release 编译掉 ⇒ 必然返回 NULL | ✅ 改 | ✅ **实测** |
| #8 | `csf/panvk_vX_cmd_draw.c:4982` | `cmd_draw_fullscreen` | ✅ 可达：**meta 绘制入口**（`cmd_draw_rects`/`cmd_draw_volume`），meta 路径不设 VS ⇒ 直接读 `state.gfx.vs.shader->desc_info` | ✅ 改 | ⚪ 被 DCE（§3.2） |
| #6 | `csf/panvk_vX_cmd_draw.c:763` | `update_tls` | ✅ 可达：调用点 4978 在 `prepare_draw` 的守卫**之外** | ✅ 改 | ✅ **实测**（5/5 副本） |
| #5 | `csf/panvk_vX_cmd_draw.c:468` | `emit_varying_descs` | ✅ 可达：调用点 531（`prepare_fs_driver_set`）不经过 `panvk_cmd_draw` | ✅ 改 | ⚪ 函数被内联，见 §3.2 |
| #3 | `csf/panvk_vX_cmd_draw.c:2879` | `prepare_draw` | ✅ 改动**真实生效**：`ASSERTED bool idvs = vs->info.vs.idvs;` 在 `assert(vs)` **之前**，release 下 `ASSERTED` 展开为无、`assert` 被编译掉 ⇒ 原代码必然空解引用 | ✅ 改 | ⚪ 被 DCE（§3.2） |
| #9 | `csf/panvk_vX_cmd_dispatch.c:376` | `cmd_dispatch` | ✅ 可达：与 #1 **逐字同构**（连注释都一样） | ✅ 改 | ✅ **实测**（7/7 副本） |
| #4 | `csf/panvk_vX_cmd_draw.c:2789` | `set_tiler_idvs_flags` | ❌ **不可达**：唯一调用点 2950 在 `prepare_draw` 之内 | ✗ 不改 | — |
| #7 | `csf/panvk_vX_cmd_draw.c:1041` | `MESA_PRIM_POINTS` 分支 | ❌ **不可达**：函数内被 `prepare_draw` 调用 | ✗ 不改 | — |
| #2 | `csf/panvk_vX_cmd_draw.c:3330` | `launch_indirect_draw` | ❌ **不可达**：仅由 3536 调用，在 v77 守卫之下 | ✗ 不改 | — |
| #10 | `csf/panvk_vX_cmd_dispatch.c:451` | `CmdDispatchBase` | ❌ **本身无害**：局部 `shader` 未被解引用；根在 #9（已修） | ✗ 不改 | — |

⇒ **改了 6 处**（inst2 + #3/#5/#6/#8/#9），**未改 4 处**（#2/#4/#7/#10，理由如上）。

### 3.1 每处的改动前后对照

备份（同一时间戳 `TS=1791217047`）：
- `csf/panvk_vX_cmd_draw.c.bak-v79-1791217047`（191 710 B）
- `csf/panvk_vX_cmd_dispatch.c.bak-v79-1791217047`（18 964 B）
- `genxml/decode_csf.c.bak-v79-1791217047`（87 889 B）

源文件 sha256：

| 文件 | v78（改前） | v79（改后） |
|---|---|---|
| `csf/panvk_vX_cmd_draw.c` | `ae9884fab0932f3685fc2636897199f7cf6918d962acd27798dd443dec0775e1` | `de4457f244117918814b3e584dfabe0dab5946e04f2025749ffc381a07a5d0e8` |
| `csf/panvk_vX_cmd_dispatch.c` | `7008335e5b9a0bf74e955f42be57e380ede36569dd3b228c7c178e4e8c2164f7` | `a1558f0b01857068ca7c8a1b3a954ae7fa04470f709618bf91a7a19da8a2ed46` |
| `genxml/decode_csf.c` | `d868a0c10d474d050ae69040eedd0051e91bcb8211062bc28f9afdb1b23483ec` | `cb2a7886f632c8011d12efe9aa041ffc5d6830a26982076a4659bc1d5affab38` |

#### #8 `cmd_draw_fullscreen`（原 4960/4966 → 现 4982）

```diff
@@ -4963,6 +4975,10 @@ panvk_per_arch(cmd_draw_fullscreen)(struct vk_command_buffer *cmd,
       panvk_shader_hw_variant(cmdbuf->state.gfx.vs.shader);
    const struct panvk_shader_variant *fs =
       panvk_shader_only_variant(get_fs(cmdbuf));
+   /* If there's no vertex shader, we can skip the draw. */
+   if (!vs)
+      return;
+
    const struct panvk_shader_desc_info *vs_desc_info =
       &cmdbuf->state.gfx.vs.shader->desc_info;
```

**依据**：原第 4966 行 `&cmdbuf->state.gfx.vs.shader->desc_info` **连
`panvk_shader_hw_variant()` 都绕过**，直接解引用 `shader` 本身 —— 这是全族里
**最直接**的一处。可达性：`panvk_v10/11/12_cmd_draw_fullscreen` 被
`cmd_draw_rects`(5093) 与 `cmd_draw_volume`(5104) 调用，二者是 `vk_meta` 的
`draw_rects`/`draw_volume` 回调 ⇒ **meta 路径不设 vertex shader**，
`state.gfx.vs.shader` 完全可以是 NULL。

#### #6 `update_tls`（原 755 → 现 763）

```diff
@@ -753,6 +757,10 @@ update_tls(struct panvk_cmd_buffer *cmdbuf)
    const struct panvk_shader_variant *vs =
       panvk_shader_hw_variant(cmdbuf->state.gfx.vs.shader);
+
+   /* If there's no vertex shader, we can skip the draw. */
+   if (!vs)
+      return VK_SUCCESS;
    const struct panvk_shader_variant *fs =
       panvk_shader_only_variant(get_fs(cmdbuf));
    struct cs_builder *b =
```

**依据**：`update_tls` 有**两个调用点** —— 2895（在 `prepare_draw` 内，受 v77/#3 守卫）
与 **4978（在 `cmd_draw_fullscreen` 内，独立入口）**。返 `VK_SUCCESS` 与函数内
已有的 `if (!state->desc.gpu) return VK_ERROR_OUT_OF_DEVICE_MEMORY;` 语义一致
（"没什么要做" ⇒ 调用方 4978/2895 的 `if (result != VK_SUCCESS) return;` 自然放行）。

#### #5 `emit_varying_descs`（原 465 → 现 468）

```diff
@@ -463,6 +463,10 @@ emit_varying_descs(const struct panvk_cmd_buffer *cmdbuf,
    const struct panvk_shader_variant *vs =
       panvk_shader_hw_variant(cmdbuf->state.gfx.vs.shader);
+
+   /* If there's no vertex shader, we can skip the draw. */
+   if (!vs)
+      return;
    const struct panvk_shader_variant *fs =
       panvk_shader_only_variant(get_fs(cmdbuf));
```

**依据**：调用点 531 在 `prepare_fs_driver_set` 内，而 `prepare_fs_driver_set` 由
574 调用；574 **只**检查了 `if (!fs) return VK_SUCCESS;`（566）—— **完全没有检查 `vs`**。
原 469 行 `&vs->info.varyings.formats` 直接解引用。

#### #3 `prepare_draw`（原 2866/2869 → 现 2879）

```diff
@@ -2864,6 +2872,10 @@ prepare_draw(struct panvk_cmd_buffer *cmdbuf,
    const struct panvk_shader_variant *vs =
       panvk_shader_hw_variant(cmdbuf->state.gfx.vs.shader);
+
+   /* If there's no vertex shader, we can skip the draw. */
+   if (!vs)
+      return VK_SUCCESS;
    const struct panvk_shader_variant *fs =
       panvk_shader_only_variant(get_fs(cmdbuf));
    ASSERTED bool idvs = vs->info.vs.idvs;
```

**依据**：**这就是任务书点名"`:2866` 的 `assert(vs)` 在 release 下被编译掉"那一处**。
改动后 2879–2880 的守卫**位于 2887 行 `ASSERTED bool idvs = vs->info.vs.idvs;` 之前** ——
即"先解引用、后断言"的缺陷被彻底消除。释放版下 `ASSERTED` 展开为空、
`assert(vs)` 展开为空，所以**即使现有唯一调用点在 v77 守卫之下，
这处改动也把 2887 行从"必然空解引用"变成"不可达"**（供未来新增调用者安全）。

#### #9 `cmd_dispatch`（原 372/376 → 现 376）

```diff
@@ -373,6 +373,9 @@ cmd_dispatch(struct panvk_cmd_buffer *cmdbuf, struct panvk_dispatch_info *info)
    VkResult result;
 
    /* If there's no compute shader, we can skip the dispatch. */
+   if (!cs)
+      return;
+
    if (!panvk_priv_mem_check_alloc(cs->spd))
       return;
```

**依据**：与 #1（v77 修的 `panvk_cmd_draw`）**逐字同构** ——
`cs = panvk_shader_only_variant(...)` 后紧接 `if (!panvk_priv_mem_check_alloc(cs->spd))`，
`cs->spd` 在检查之前解引用。`panvk_shader_only_variant()` 入参 NULL 时返回 NULL
（`panvk_shader.h:486-493` 实测 `if (!shader) return NULL;`）⇒ 必然空解引用。
同样：`panvk_shader_hw_variant()` 亦然（`panvk_shader.h:496-502`），确认 #3/#5/#6/#8 的前提。

### 3.2 ⚠️ 诚实标注：6 处里有 2 处被编译器消除（**不是改动失效**）

**实测证据（反汇编）**：

| 处 | 符号 | 结果 |
|---|---|---|
| inst2 `get_cs_cfg` | 4/4 副本 | ✅ 守卫**进了机器码**；函数 1568 → **1588 B (+20)** |
| #9 `cmd_dispatch` | 7/7 副本 | ✅ 守卫**进了机器码**；函数 648 → **632 B** |
| #6 `update_tls` | 5/5 副本 | ✅ 守卫**进了机器码**；`vs` 在非空路径上被读 `[x22,#0x128]` |
| #8 `cmd_draw_fullscreen` | v10 副本 2268 → 2256 B | ⚪ 编译器判定 `vs` 恒非空，把 `vs->hw` 挪进"空分支不可达"的基本块 ⇒ 守卫消失 |
| #5 `emit_varying_descs` | **无独立符号**（被内联） | ⚪ 同上（内联到调用方后 DCE） |
| #3 `prepare_draw` | 6192 B（v6/v7 两副本） | ⚪ 同上 |

**为什么被消除**：`prepare_draw`（#3）与 `cmd_draw_fullscreen`（#8）在**当前**翻译单元内
**只有**一个调用者 —— `panvk_cmd_draw`，而它自带 v77 的 `if (!vs) return;` 守卫。
编译器做了跨函数（同 TU 内联）的空值传播，**证明了守卫不可达**，因此消除。
#5 被完全内联进 `prepare_fs_driver_set` 后同理。

**结论（不粉饰）**：
- 第 3 条要求"每处都先判断是否真的可达"—— 静态调用图判断为"可达（独立入口）"，
  但**编译器的跨过程分析比静态调用图更强**，它证明了在当前 TU 内不可达。
  两者不矛盾：**"当前唯一调用者在守卫之下" ⇔ 编译器可证明不可达**（= §0 里 #2/#4/#7 的判据）。
- 因此 **v79 的运行期行为，相对于"只改 inst2 + #6 + #9"是一样的**；
  #3/#5/#8 的改动**保留为源码级防御**，只在**未来新增独立调用者**时才进入机器码。
  这一点**在报告里明确标注，不当作已修复的路径**。
- **改动没有白做，但也没有被高估**：真正的运行期修复是 **inst2 + #6 + #9**（+ v77 的 #1）。

---

## 4. 确定性对照（任务交付项）

```bash
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
```

| 步骤 | 产物 sha256 | 字节数 | 结果 |
|------|------------|--------|------|
| **v77 基线**（任务书给定） | `cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185` | 20 053 456 | 基准 |
| v79 改后编译 #1 | `50b8a2eab2f2c74bb803566bb63486390f3b92295106013495baa8f65af6672a` | 20 052 344 | v79 产物 |
| **撤本轮全部 6 处改动**（`cp` 回 `.bak-v79-1791217047`）重编 | **`cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185`** | **20 053 456** | ✅ **逐位等于 v77** |
| 贴回改动重编 #2 | `50b8a2eab2f2c74bb803566bb63486390f3b92295106013495baa8f65af6672a` | 20 052 344 | ✅ 与 #1 逐位一致（可复现） |

⇒ **确定性双向成立**：
**撤改动 ⇒ 逐位等于 `cf65d1e1…`（20 053 456 B）✅**；贴回 ⇒ 两次编译逐位同一 ✅。
因此 sha256 的差异 **100% 由本轮 6 处改动引起**，不含任何构建漂移。
（撤/贴的源文件 sha 也逐一核对：撤后回到 `ae9884fa…/7008335e…/d868a0c1…`，
贴回回到 `de4457f2…/a1558f0b…/cb2a7886…`。）

编译 exit code：**0**（4 次 ninja 全部 `NINJA_EXIT=0`，`grep -icE "warning|error"` = **0**）。

---

## 5. 交付物与校验

### 5.1 新 `.so`

- 路径：`/root/final/libvulkan_panfrost_v79.so`
- sha256：`50b8a2eab2f2c74bb803566bb63486390f3b92295106013495baa8f65af6672a`
- 字节数：`20 052 344`
- **24 字符前缀：`50b8a2eab2f2c74bb803566b`**

### 5.2 ★ 两个 APK

| | 诊断版 | 干净版 |
|---|---|---|
| 路径 | `/root/final/mgl-panvk-v79.apk` | `/root/final/mgl-panvk-v79-nodiag.apk` |
| sha256 | `08244f58e8f82a51bd824f40c58331459628b3475ff821f99fd1c83a97182cd0` | `47e593023b548e1150f33e141e1e11cf9fd16045d0a99cb8058267ffedb672e8` |
| 字节数 | `10 215 983` | `10 215 983` |
| **24 字符前缀** | **`08244f58e8f82a51bd824f40`** | **`47e593023b548e1150f33e14`** |
| versionCode / versionName | `79` / `6.19-nullfamily` | `79` / `6.19-nullfamily-nodiag` |
| env `PANVK_DEBUG` | `1,kbase_diag` | `0` |

打包脚本：`/root/pack_v79.sh`（由 `/root/pack_v78.sh` 派生，`diff` **只 3 行值变化**：
`W` `v78→v79`、`OUT` `mgl-panvk-v78.apk→mgl-panvk-v79.apk`、
`versionCode 78→79` + `6.18-flushaudit→6.19-nullfamily`；**env 串以外的每一行与 v78 逐字符一致**）
干净版脚本：`/root/pack_v79_nodiag.sh`（与 `pack_v79.sh` 的唯一差别是
`sed` 多一条 `s/PANVK_DEBUG=1,kbase_diag/PANVK_DEBUG=0/g`）。

### 5.3 ★ 两个 APK 的 env 串 diff（任务强制要求）

`env` 串以 `<meta-data>` 写在 `AndroidManifest.xml` 的 `pojavEnv` / `boatEnv`（**两条完全同值**）。

**诊断版（= v78 逐字符一致 ✅，实测 `diff` 无输出）：**

```
LIBGL_ES=3:POJAV_RENDERER=opengles3:MOBILEGL_BACKEND_TYPE=DirectVulkan:MOBILEGL_ESPRYT_USE_ANGLE=0:MOBILEGL_MAGMA_R11G11B10F_FALLBACK=0:MOBILEGL_LOG_FILE_PATH=/sdcard/MG/mgl.log:MESA_DEBUG=1:PANVK_DEBUG=1,kbase_diag:LIBGL_DEBUG=1:EGL_LOG_LEVEL=debug:PANVK_KBASE_HEAP_RENEW_INTERVAL=32
```

**干净版：**

```
LIBGL_ES=3:POJAV_RENDERER=opengles3:MOBILEGL_BACKEND_TYPE=DirectVulkan:MOBILEGL_ESPRYT_USE_ANGLE=0:MOBILEGL_MAGMA_R11G11B10F_FALLBACK=0:MOBILEGL_LOG_FILE_PATH=/sdcard/MG/mgl.log:MESA_DEBUG=1:PANVK_DEBUG=0:LIBGL_DEBUG=1:EGL_LOG_LEVEL=debug:PANVK_KBASE_HEAP_RENEW_INTERVAL=32
```

**diff（应只有 PANVK_DEBUG 一处不同）—— 归一化验证：**

```
$ sed 's/PANVK_DEBUG=1,kbase_diag/PANVK_DEBUG=<X>/' env_v79.txt  > n_diag.txt
$ sed 's/PANVK_DEBUG=0/PANVK_DEBUG=<X>/'            env_v79n.txt > n_clean.txt
$ diff n_diag.txt n_clean.txt          # 无输出
ONLY_DIFFERENCE_IS_PANVK_DEBUG = PASS
```

token 计数（每条 `meta-data` 各一处 ⇒ 期望 2）：

| 检查 | 结果 |
|---|---|
| 诊断版含 `PANVK_DEBUG=1,kbase_diag` | **2** ✅ |
| 干净版含 `PANVK_DEBUG=0` | **2** ✅ |
| 干净版含 `kbase_diag`（必须为 0） | **0** ✅ |

> **理由（报告 52 §5.1 的新发现）**：`kbase_checkpoint()` / `kbase_mark_progress()`
> 内部**各自发射一次 `cs_flush_stores(b)`**（真机/源码实测 42 处调用点），
> 且**都受 `if (!PANVK_DEBUG(KBASE_DIAG)) return;` 门控**。
> ⇒ 诊断版会**注入额外 flush，把 hazard 掩盖掉** ✗
> ⇒ **测【画面问题】必须用干净版** `mgl-panvk-v79-nodiag.apk` ✅

### 5.4 校验（全部通过）

| 校验项 | 结果 |
|---|---|
| `unzip -p mgl-panvk-v79.apk lib/arm64-v8a/libvulkan_freedreno.so \| sha256sum` | `50b8a2ea…f6672a` == 新 `.so` ✅（20 052 344 B，非空 ✅） |
| `unzip -p mgl-panvk-v79-nodiag.apk …` | `50b8a2ea…f6672a` == 新 `.so` ✅（20 052 344 B，非空 ✅） |
| `aapt2 dump badging`（诊断版） | `versionCode='79' versionName='6.19-nullfamily'` ✅ |
| `aapt2 dump badging`（干净版） | `versionCode='79' versionName='6.19-nullfamily-nodiag'` ✅ |
| **v76 APK 未被改** | `1bb9066bbcf24d6f7a0b58fcc9a6490b2e532ec9210a04ef5c172f0356d70e20` ✅ |
| **v77 APK 未被改** | `49506ddc28fd543d41a763c673ecebcb604f1e62a676849a80c16d4746d67b5d` ✅ |
| **v78 APK 未被改** | `289a0e425d21c0c78bdf5836809991005865fe28c683814470759ef27ab5312b` ✅ |

### 5.5 切分分发（**两套**）

```bash
cd /data/dsh_downloads
cp -f /root/final/mgl-panvk-v79.apk        ./mgl-panvk-v79.apk
cp -f /root/final/mgl-panvk-v79-nodiag.apk ./mgl-panvk-v79-nodiag.apk
split -n 8 -d mgl-panvk-v79.apk        v79p8_
split -n 8 -d mgl-panvk-v79-nodiag.apk v79n8_
```

（全程**未使用 `rm`**）

| 诊断版 `v79p8_` | 字节 | 干净版 `v79n8_` | 字节 |
|---|---|---|---|
| `v79p8_00` … `v79p8_06` | 1 276 998 各 | `v79n8_00` … `v79n8_06` | 1 276 998 各 |
| `v79p8_07` | 1 276 997 | `v79n8_07` | 1 276 997 |

**重组完整性（`cat` 后 sha256 必须等于 APK）：**

| 套 | `cat *p8_* \| sha256sum` | APK sha256 | |
|---|---|---|---|
| `v79p8_` | `08244f58e8f82a51bd824f40c58331459628b3475ff821f99fd1c83a97182cd0` | 同 | ✅ |
| `v79n8_` | `47e593023b548e1150f33e141e1e11cf9fd16045d0a99cb8058267ffedb672e8` | 同 | ✅ |

**8 份合计 10 215 983 B = APK 大小 ✅**

---

## 6. 回滚命令

### 6.1 只回滚源码（撤本轮 6 处）

```bash
TS=1791217047
M=/root/zenithblue/work/mesa
cp -f $M/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c.bak-v79-$TS      $M/src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
cp -f $M/src/panfrost/vulkan/csf/panvk_vX_cmd_dispatch.c.bak-v79-$TS  $M/src/panfrost/vulkan/csf/panvk_vX_cmd_dispatch.c
cp -f $M/src/panfrost/genxml/decode_csf.c.bak-v79-$TS                 $M/src/panfrost/genxml/decode_csf.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja src/panfrost/vulkan/libvulkan_panfrost.so
# 期望 sha256 = cf65d1e169842896162a9cfa6860b00a4254d3155ff2100ccd6f34bf98218185 (20 053 456 B)  ← v77 状态
```

### 6.2 只回滚**单处**（例如只想撤 instance 2 的 `decode_csf.c`）

```bash
cp -f /root/zenithblue/work/mesa/src/panfrost/genxml/decode_csf.c.bak-v79-1791217047 \
      /root/zenithblue/work/mesa/src/panfrost/genxml/decode_csf.c
```

### 6.3 退回 v77/v76 的现成 APK（不重编）

- v77：`/root/final/mgl-panvk-v77.apk`（`49506ddc…`）
- v76：`/root/final/mgl-panvk-v76.apk`（`1bb9066b…`）

### 6.4 只把 env 串换回诊断档（不重编，只重打包）

```bash
/root/pack_v79.sh        # 诊断版（PANVK_DEBUG=1,kbase_diag）
/root/pack_v79_nodiag.sh # 干净版（PANVK_DEBUG=0）
```

---

## 7. 判读表（真机验证用）

**⚠️ 测【崩溃】用诊断版；测【画面】必须用干净版**（§5.3 的 kbase_diag flush 混杂因素）。

### 7.1 崩溃是否消失（装 `mgl-panvk-v79.apk`）

对照 v76 的两份 hs_err：

| 实例 | v76 现场 | v79 期望 |
|---|---|---|
| 1 | `libvulkan_freedreno.so+0xb8a5c0` `panvk_cmd_draw+0x34`，`elapsed 27.5 s` | v77 已修；v79 保留 |
| 2 | `libvulkan_freedreno.so+0xef5550` `get_cs_cfg+0x320`，`elapsed 68.2 s` | **本轮修** ⇒ 该帧不应再出现 |

> ⚠️ **注意地址会变**：v79 的 `.text` 缩减 1 112 B，全模块符号整体位移，
> 所以**不要**拿 `+0xef5550` 去比对 v79 的日志。要认的是
> **`Problematic frame` 里的函数名**（`get_cs_cfg` / `panvk_cmd_draw` / `update_tls` / `cmd_dispatch`）。

| 观察项 | 崩溃消失（最佳） | 显著变长（次佳） | 仍崩在**同族**函数 | 崩在**新**函数 |
|---|---|---|---|---|
| 看什么 | 无新 `hs_err_pid*.log`；`get_cs_cfg` 与 `panvk_cmd_draw` 均不再出现 | `elapsed` 从 68 s 明显增大（≥2×） | `Problematic frame` 仍是 `panvk_cmd_draw`/`update_tls`/`cmd_dispatch`/`get_cs_cfg` 之一 ⇒ 该处守卫被 DCE（§3.2）或还有未列出的同族点 | 完全不同的函数 ⇒ 病根族之外的新问题 |
| 怎么读 | ⇒ **空指针族命中** | ⇒ 命中但非唯一 | ⇒ 回 §3.2：把该变量的守卫改成编译器无法证明的形式（如 `if (unlikely(!vs))` + 跨 TU 调用者） | ⇒ 另立案，勿回改本轮 |
| 额外 | 记 `elapsed time` 与 `Problematic frame` 全文 | 同上 | 同上 | 附完整 backtrace |

### 7.2 画面问题（装 `mgl-panvk-v79-nodiag.apk`）

| 观察项 | 说明 |
|---|---|
| 楔形/半成品画面是否消失或显著减少 | 干净版**不注入** kbase_diag 的 42 处 `cs_flush_stores` ⇒ 才能暴露真实 "缺 flush" hazard |
| 必须固定环境变量 | 对比时**两条 env 串都要记录**；若 v76/v77 的历史 A/B 是在 `PANVK_DEBUG=1,kbase_diag` 下做的，**那些结论不能用**（报告 52 §5.1） |
| 建议 | 同类结果 **≥8 次重复**；先核对两份 APK 内 `.so` 的 sha 是否真的相同 |

---

## 8. 本轮**未做**的事（明确边界）

一律**未碰**：v71/v72/v73 检查点、ATOMIC/SEQNO 打印、v76 的 flush（MR!44659）、
v77 的 `if (!vs) return;`、修复 A、C1/C2/B、P2/renew32、超时常量、MobileGL、AFBC。
未执行任何 `git checkout/stash/reset`；未使用 `rm`；未操作手机。

---

## 9. 待确认 / 未验证

- **真机 v79 实际表现：未验证**（本子智能体不操作手机，任务书明令禁止）。
- **#3/#5/#8 三处的运行期效果：被编译器 DCE，等于未生效**（§3.2 已实测标注）。
  它们在"未来新增独立调用者"时才进入机器码 —— 这一点**未验证**（无法在真机触发不存在的调用者）。
- **§1.4 的 file:line**：`decode_csf.c` 的行号表被 strip，`:2416` 是**指令↔源码对照**结论，
  不是 `addr2line` 直接输出。文件与函数名是工具直出（`addr2line` 给 `get_cs_cfg` +
  `decode_csf.c`），偏差风险在**行号**，不在函数定位。
- **干净版的 `PANVK_DEBUG=0` 是否等价于"不设 `PANVK_DEBUG`"：未验证**
  （`panvk_instance.c:62` 的解析逻辑未逐行核对；若 `PANVK_DEBUG=0` 与"未设"行为不同，
  干净版需改成删掉该 token 而不是置 0）。
- **v79 的 `elapsed` 是否真能超过 68 s：未验证**（需真机）。
- **未列出的同族点**：本轮只覆盖报告 51 §7 的 10 处 + instance 2。
  `pandecode_fetch_gpu_mem()` 的**另外 2 个调用点**
  （`decode_csf.c:1298` → `qctx->ip`/`qctx->end`，`decode_csf.c:1441`）**同样没有空检查**，
  属同一族但**本轮未改**（不在任务书点名的 4~5 处内）——标记为**已知残余风险，未验证**。

---

## 附录 A：v79 最终源码块（逐行）

`src/panfrost/genxml/decode_csf.c` 2402–2411：

```c
2402    uint64_t *instrs = pandecode_fetch_gpu_mem(ctx, bin, bin_size);
2403 
2404    /* The CS binary is not (fully) mapped in the pandecode memory map:
2405     * pandecode_fetch_gpu_mem() then returns NULL, and the loop below
2406     * would dereference it at instrs[i].  Callers already handle a
2407     * NULL return (see pandecode_cs_binary() and collect_...()). */
2408    if (!instrs)
2409       return NULL;
2410 
2411    cfg = rzalloc(symbols, struct cs_code_cfg);
```

`src/panfrost/vulkan/csf/panvk_vX_cmd_dispatch.c` 375–380：

```c
375    /* If there's no compute shader, we can skip the dispatch. */
376    if (!cs)
377       return;
378 
379    if (!panvk_priv_mem_check_alloc(cs->spd))
380       return;
```

`src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c` — 4 处守卫：

```c
 467    /* If there's no vertex shader, we can skip the draw. */      #5 emit_varying_descs
 468    if (!vs)
 469       return;

 762    /* If there's no vertex shader, we can skip the draw. */      #6 update_tls
 763    if (!vs)
 764       return VK_SUCCESS;

2878    /* If there's no vertex shader, we can skip the draw. */      #3 prepare_draw
2879    if (!vs)
2880       return VK_SUCCESS;

4981    /* If there's no vertex shader, we can skip the draw. */      #8 cmd_draw_fullscreen
4982    if (!vs)
4983       return;
```

## 附录 B：机器码实测摘录

`get_cs_cfg`（v79 `0xef4df0`，旧 `0xef6444`）：

```
  ef4e60: bl   pandecode_find_mapped_gpu_mem_containing
  ef4e64: cbz  x0, 0xef53d8            ; mem == NULL（原有保护）
  ef4e68: ldr  x8, [x0, #0x20]         ; x8 = mem->addr == instrs
  ef4e6c: cbz  x8, 0xef541c            ; ★ v79 新增守卫
  ...
  ef541c: mov  x21, xzr
  ef5420: b    0xef4e34                ; → return NULL
```

（v76 对应处是 `ldp x8,x9,[x0,#0x20]` 一条指令、**无** `cbz x8`；
v79 拆成 `ldr`+`cbz` —— 正是函数 1568→1588 B（+20 B）的来源。）

`update_tls`（v79 `0xa9415c` / `0xbb26ac`）：

```
  a94178: ldr  x22, [x0, #0x2288]      ; vs
  a9417c: cbz  x22, 0xa941c0           ; ★ 守卫 → mov w0, wzr (VK_SUCCESS)
  ...
  a9419c: ldr  w8, [x22, #0x128]       ; 非空路径才解引用 vs
```

`cmd_dispatch`（v79 `0xa4c7fc`）：

```
  a4c818: ldr  x24, [x0, #0x31b8]      ; cs
  a4c81c: cbz  x24, 0xa4ca58           ; ★ 守卫 → return
  a4c820: ldr  x8, [x24, #0x600]       ; cs->spd 仅非空时读
```

## 附录 C：本轮产物清单

| 产物 | 路径 | sha256 前缀 |
|---|---|---|
| 新 `.so` | `/root/final/libvulkan_panfrost_v79.so` | `50b8a2eab2f2c74bb803566b` |
| 诊断版 APK | `/root/final/mgl-panvk-v79.apk` | `08244f58e8f82a51bd824f40` |
| 干净版 APK | `/root/final/mgl-panvk-v79-nodiag.apk` | `47e593023b548e1150f33e14` |
| 打包脚本（诊断） | `/root/pack_v79.sh` | — |
| 打包脚本（干净） | `/root/pack_v79_nodiag.sh` | — |
| 源码备份 | `*.bak-v79-1791217047`（3 个） | — |
| 两套切分 | `/data/dsh_downloads/v79p8_00..07`、`v79n8_00..07` | 见 §5.5 |
