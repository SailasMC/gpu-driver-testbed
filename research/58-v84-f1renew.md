# 58 — v84：F1「续租失败不得清零计数」落地复核（回传 + 退避）

日期：2026-10-06 · 树 `/root/zenithblue/work/mesa`（未提交） · 构建目录 `/root/zenithblue/build/android-v4`
v84 = **task-3 报告 57 的「改动 1 + 改动 3(诊断) + 改动 2」合并落地**（源码由 Lead 写，本报告只做复核与判读设计）
本会话：**只读复核 + 只写本文件与 `/root/v84work/` 笔记**；未改源码、未编译、未碰设备、未 `rm`

> 记账提示：**共享任务板上的 `task-4` 不存在**（`team_task_get task-4` → `team task "task-4" not found`），因此本文件按 Lead 消息里的要求执行，但**无法 claim/complete 任务**。请 Lead 需要记账时补建任务。

---

## 0. 一句话

v84 = 把"跳过续租"从**不可见且会被抹掉**变成**可见且会退避重试**：`kbase_renew_tiler_heap()` 增加 `bool *renewed` 出参、跳过时打 `postponed`、调用点**只在 `renewed` 时清零**、跳过时退避 `interval/4`。
**我复核通过**：diff 与提案语义一致（+32/−5，3 处）；`.so` = `61d89bc1b0b44b30…`（20053936 B）；两 APK 内嵌驱动同 sha；两套分片重组**逐字节**等于 APK；`postponed` 字符串确在 `.so` 内；两次重编对象集合同为 **5 个**。
**发现 2 处与提案的差异（均不影响功能，但影响日志判读）**：① 提案的"改动 3"（把 `logi` 挪到结论之后并加 `-> renewed/postponed`）**未应用** ⇒ `tiler heap renewal (uAPI …)` 现在计的是**进入分支次数**而不是续租次数；② `postponed` 行里的 `old ctx` 打印的是**当前在用**的 heap ctx，不是被推迟销毁的**退休** ctx（后者目前任何日志都不打印）。

---

## 1. 本轮边界（诚实声明）

| 做的事 | 没做的事 |
|---|---|
| 逐行复核 v84 vs `.bak-v84-1791221286`（+32/−5，3 处）并与 57 号提案逐条对照 | **没有改任何源码**；未编译；未打包 |
| 独立复核 `.so`/两 APK/APK 内嵌驱动/16 个分片（sha256 + `cmp` 逐字节） | **没有操作手机**：未安装、未取 logcat、未看画面 |
| 复核两次重编日志（对象集合、告警）+ `kbase_create_group` 调用点 + `strings` 验证 | 未跑 pandecode；未做像素对照 |
| 写本报告 + `/root/v84work/` 笔记 | 未碰 `/root/mesa`、`/root/MobileGL`；未 `git checkout/stash/reset`；未 `rm` |

---

## 2. 改动逐行 diff（3 处，+32/−5）

### 2.1 函数头与"跳过"回传（`:3292` / `:3312-3326`）

```diff
@@ -3289,7 +3289,7 @@
 static VkResult
-kbase_renew_tiler_heap(struct panvk_gpu_queue *queue)
+kbase_renew_tiler_heap(struct panvk_gpu_queue *queue, bool *renewed)

@@ -3309,8 +3309,21 @@
-   if (!kbase_try_destroy_retired_heap(queue))
+   if (!kbase_try_destroy_retired_heap(queue)) {
+      /* F1/v84: 上一次的代际切换还没退休（退休槽仍可能被固件引用）⇒ 本次跳过。
+       * 关键：必须把"跳过"回传给调用点。旧代码两条路径都 return VK_SUCCESS，
+       * 调用点于是无条件清零计数 ⇒ 跳过被抹掉、续租可能长期甚至永久失效
+       * （实测：一局里 begin=9832 / end=9828，4 次开了没结束）。 */
+      mesa_loge("kbase: CKPT host heap renew #%u postponed (old ctx 0x%" PRIx64
+                "), vt %" PRIu64 "/%" PRIu64 ", frag %" PRIu64 "/%" PRIu64,
+                renew_gen, (uint64_t)tiler_heap->context.dev_addr,
+                queue->subqueues[PANVK_SUBQUEUE_VERTEX_TILER].kbase.emitted_jobs,
+                queue->kbase_retired_heap.vt_jobs,
+                queue->subqueues[PANVK_SUBQUEUE_FRAGMENT].kbase.emitted_jobs,
+                queue->kbase_retired_heap.frag_jobs);
+      *renewed = false;
       return VK_SUCCESS;
+   }
```

### 2.2 成功路径回传（`:3354`）

```diff
@@ -3338,6 +3351,7 @@
    mesa_loge("kbase: CKPT host heap renew #%u end (new ctx 0x%" PRIx64 ")",
              renew_gen, (uint64_t)tiler_heap->context.dev_addr);
+   *renewed = true;
    return VK_SUCCESS;
 }
```

### 2.3 调用点：只在成功时清零 + 跳过退避（`:3946-3965`）

```diff
@@ -3929,13 +3943,26 @@
-      result = kbase_renew_tiler_heap(queue);
+      bool renewed = false;
+      result = kbase_renew_tiler_heap(queue, &renewed);
       if (result != VK_SUCCESS)
          return vk_queue_set_lost(&queue->vk,
                                   "kbase: tiler heap renewal failed");

-      queue->kbase_tiler_submit_count = 0;
-      queue->kbase_tiler_work_count = 0;
+      if (renewed) {
+         queue->kbase_tiler_submit_count = 0;
+         queue->kbase_tiler_work_count = 0;
+      } else {
+         /* F1/v84: 本轮被推迟 ⇒ 绝不能清零计数（旧行为会让跳过不可见、并把续租
+          * 静默关掉）。但也绝不能原地不动：计数停在阈值上会导致**每次提交**都进
+          * 这个分支、每次做一遍全图形排水 ⇒ 把提交线程拖死。所以把计数压到
+          * interval - backoff，即每 backoff 次提交才重试一次。 */
+         const uint32_t interval = kbase_tiler_heap_renew_interval();
+         const uint32_t backoff = MAX2(1u, interval / 4);
+
+         queue->kbase_tiler_submit_count =
+            interval > backoff ? interval - backoff : 0;
+      }
    }
```

**行号台账（我实测）**：`:3292` 新签名 · `:3317` `postponed` 日志 · `:3324` `*renewed = false` · `:3354` `*renewed = true` · `:3946` `bool renewed = false` · `:3947` 调用 · `:3953` 成功清零 · `:3961` `backoff = MAX2(1u, interval/4)` · `:3964` `interval > backoff ? interval - backoff : 0`。

**语义核对（与 57 号 §2.2/§2.3 对照）**：`interval - MIN2(backoff, interval)`（我的写法）与 `interval > backoff ? interval - backoff : 0`（Lead 的写法）**完全等价**：当 `backoff ≥ interval` 时两者都给 0；否则都给 `interval - backoff`。⇒ 退避在 `interval=1`（每次提交重试）与 `interval=UINT32_MAX`（env=0，实际永不重试）两个端点上行为都正确，且 `interval - backoff` 不会下溢（`interval`、`backoff` 都是 `uint32_t`，`backoff ≤ interval` 保证无回绕）。

---

## 3. 与 57 号提案的差异说明

### 3.1 为什么"改动 1 + 改动 2 + 改动 3"可以合并成一次落地（Lead 的做法：对的）

三者在 57 号里是**同一个 F1 语义修复的三个面**，拆开落地会产生**不可判读的中间态**：
- 只做改动 1（回传单不消费）：`renewed` 出参没人用，行为与旧代码**完全一样**（照样无条件清零）⇒ 一次上机得不到任何结论。
- 做改动 1+2 但不做退避：跳过时计数停在阈值 ⇒ **每个提交都做一次全图形排水** ⇒ 帧时间崩坏会把"续租是否修好"这个信号淹没在性能噪声里（我在 57 号 §2.3 把这一条列为"必须有退避"的原因）。
- ⇒ **必须一起落地**，且合并后 logcat 上有一条**精确会计恒等式**可判（见 §6）：`begin = end + postponed`。这条恒等式只有"回传 + 分别打印"同时到位才成立。

### 3.2 为什么**不**并 F:2200 的 (A)/(B)（Lead 的做法：也对）

- `emitted_jobs = 0;` 在 `:2200`（`kbase_submit_init_subqueues()` 内）。(A) 要在那里顺带 `TERM` 退休 ctx，其**前提**是"走到该点时队列组已终止"——这是个**独立于 F1 语义**的前提，需要单独审计。
- 我复核了 Lead 的"低频"判断并**修正了行号**：`kbase_create_group()` 定义在 `:2126`，**全树唯一调用点 = `:4256`**（Lead 消息里的 `:4229` 是 v84 补丁前的行号；v84 在文件前部净增了行数 ⇒ 调用点后移）。⇒ 确实是**队列创建路径（低频）**，不并进来符合单变量纪律。
- 另：若并进来，一旦出问题，就无法区分是"F1 回传/退避"还是"提前 TERM"（后者正是 `:3255-3272` 注释里 0xc0 CSG fatal 那一族）⇒ 分开是对的。

### 3.3 差异 ①：提案的"改动 3"（纯诊断）**未应用**

- 现状：`mesa_logi("kbase: tiler heap renewal (uAPI %u.%u, submits %u, renew interval %u)")` **仍在** `kbase_renew_tiler_heap()` **之前**打印（`:3939-3944`），文本里**没有** `-> renewed/postponed` 后缀，`submits %u` 打的是**触发时的计数**（≈ interval），不是退避后的新值。
- **影响（必须写进判读表，否则会读错）**：
  - `count(end)` = **真正完成的续租次数**（唯一可信的"换堆次数"）。
  - `count(begin)` = `count(logi)` = **进入续租分支的次数**（含被推迟的）——**不是**续租次数。v83/v82 那局的 `begin=9832` 也是这个含义。
  - `count(postponed)` = 被推迟次数 ⇒ **`begin = end + postponed` 是本轮的核心不变量**。
- 建议（若 Lead 想再省一次日志改动）：不必改——`postponed` 行已经把"跳过 + 哪一侧没推进"讲清楚了；把 `logi` 当作"进入分支"的技术指标即可。**但不要**用 `logi` 行数当"续租频率"。

### 3.4 差异 ②：`postponed` 行的 `old ctx` 是**当前在用**的 ctx，不是退休 ctx

- 代码打印 `tiler_heap->context.dev_addr`（与同一次 `begin` 行**同一个值**），而被推迟销毁的是 `queue->kbase_retired_heap.ctx`。
- 后果：**退休 ctx 的地址目前任何日志都不打印** ⇒ 无法把"某次 `end`"与"当时被销毁的退休 ctx"对上号。功能无影响；若将来要追"是谁卡住了销毁"，把 `postponed` 行的 `old ctx` 改成 `queue->kbase_retired_heap.ctx`（一行）即可。**属建议，不是缺陷。**

---

## 4. 确定性校验（两次重编）

- 日志：`/root/v84work/v84_buildA.log`、`v84_buildB.log`（各 2057 B）
- 两次均为 **12 步**：`[1]` 生成 `src/git_sha1.h` + **5 个编译**（`csf/panvk_vX_gpu_queue.c` 的 v10/v11/v12/v13/v14）+ 5 个静态库链接 + 1 个共享库链接 = 12；**对象集合相同、顺序不同**（与 v81/v82 的结论一致）。
- 只编 5 个对象是对的：v84 只改了 **csf** 的 `panvk_vX_gpu_queue.c`。
- **告警（预存在，非 v84 引入）**：`csf/panvk_vX_gpu_queue.c:1786:7: warning: label followed by a declaration is a C23 extension [-Wc23-extensions]`（`kbase_wait_continue:` 标签后紧跟 `int64_t now = …`）。我核对 `.bak-v84-1791221286` 的同一位置**存在同样构造** ⇒ 与本次改动无关；仅记录（它位于**等待/超时**路径，若将来换更严的编译器选项会变成错误）。
- **诚实标注**：两份日志**不含 sha 行**；"两次 sha 相同"来自 Lead 的执行记录，日志只能佐证**对象集合一致**。本会话独立复核的是**当前**产物哈希（§5）。

---

## 5. 产物与切分复核

### 5.1 产物

| 产物 | 字节 | sha256 |
|---|---|---|
| `/root/final/libvulkan_panfrost_v84.so` | **20053936** | `61d89bc1b0b44b30cf7ed5d3a493fe69dbc26950f3eab6a86b472c24dcfe9d5d` |
| 构建树 `.so`（同一哈希） | 20053936 | 同上 ✅ |
| `/root/final/mgl-panvk-v84.apk`（diag） | 10220079 | `22a59a3696976e0d5c17b7d0eceb26f6dcbd3a757e17e97aea6bdbe51e7c1085` |
| `/root/final/mgl-panvk-v84-nodiag.apk` | 10220079 | `2decc5dbaa2cfcf022b8a58e477cf6515503e5256cdcc637f5b426c74b41bc91` |
| 上一版对照 `libvulkan_panfrost_v82.so` | 20053424 | `bd61a11da408e90c61504bc91da1225e25082dc3d9e0bdb684e238612acd47a2` |

**APK 内嵌 `lib/arm64-v8a/libvulkan_freedreno.so`：两版均 20053936 B、sha256 = `61d89bc1b0b44b30…` ✅**（与 `.so` 一致）。

| ZIP 条目 | v84 | v84-nodiag |
|---|---|---|
| `AndroidManifest.xml` | 3576 B · `fa8268902e8322c2fcd27439301a111286fc4cdca133bac694dac8fb49c958ce` | 3568 B · `6af935f244feac658cf98f1f1ca94f13adae48ec0a3a7325b7a24da8269d49a1` |
| `resources.arsc` | 40 B · `1fa3cb291285348ec1b33c85e7317d989467707f4791c4e5302f2d312d1e18c8` | 同左 |
| `lib/arm64-v8a/libMobileGL.so` | 16956584 B · `72919c73a7e07630f329bb4ad9605c606a9aac9e2fc6596683ee7b9f9c76848b` | 同左 |
| `lib/arm64-v8a/libvulkan_freedreno.so` | 20053936 B · `61d89bc1b0b44b30…` | 同左 |
| `classes.dex` | 1328 B · `6bd3abde2c53506f1b54bb88a8069c3cc394c2fb08440e4ca475cbf8fd64f4ad` | 同左 |
| `META-INF/*`（.SF/.RSA/MANIFEST.MF） | 620/1337/493 B（两版不同 = 签名块） | — |

⇒ 两版差异只在 manifest + 签名块；`libMobileGL.so`/`classes.dex`/`resources.arsc` 与 v79–v82 **逐字节相同** ⇒ v82→v84 只有驱动在变。

**`strings` 独立验证**：`strings -a libvulkan_panfrost_v84.so | grep -c postponed` = **1**，且字符串为
`kbase: CKPT host heap renew #%u postponed (old ctx 0x%lx), vt %lu/%lu, frag %lu/%lu`
⇒ ① 新日志确实进了码；② `%lu` 证明 `PRIu64` 修好（Lead 提到的 `-Werror,-Wformat` 踩坑已解）。

### 5.2 切分复核（16 片，独立复算）

切法同前：前 7 片各 1277510 B、末片 1277509 B（两套相同）。

| 套 | `cat … \| sha256sum` | 与 APK 比对 | `cmp` 逐字节 |
|---|---|---|---|
| diag `v84p8_00..07` | `22a59a3696976e0d5c17b7d0eceb26f6dcbd3a757e17e97aea6bdbe51e7c1085` | = `mgl-panvk-v84.apk` ✅ | ✅ 相同 |
| nodiag `v84n8_00..07` | `2decc5dbaa2cfcf022b8a58e477cf6515503e5256cdcc637f5b426c74b41bc91` | = `mgl-panvk-v84-nodiag.apk` ✅ | ✅ 相同 |

每片 sha256：

| 片 | sha256 | 片 | sha256 |
|---|---|---|---|
| `v84p8_00` | `e00d147d95a67048c1480bd47d9ce60c9c70fbf9b30a6bb441cd10da45c9ae17` | `v84n8_00` | `3f7bda4832afc734065983202ee70b22405f866a3110644c9501f543e00c0123` |
| `v84p8_01` | `faec391e19e5894c1a21d4974438b562152ccbde5875c87a4815a7051aa615a5` | `v84n8_01` | `dc0ff2bd60227ed3c1e3a8acd4d8d642c6544ae68b91877095137ce2eb694fc7` |
| `v84p8_02` | `d3544eb941bf68ecc2e2d5688180fb3345f56a5d31ba3bbc0f2f545b59a3514b` | `v84n8_02` | `f7c554f57736eb575253ea8c0b841c38ff1c7b9aa3946314b4f13b872aa94f32` |
| `v84p8_03` | `ec2a1d9590a54c84fb1fd66d0a94806c43d57105ebdbed517bc25dcba886efdf` | `v84n8_03` | `318c56f5db53311e25cf857755f6e8077a1baa96765b29847a266d75708c3b51` |
| `v84p8_04` | `03b1b9e92816d696a4a961d6a5d0695dd695ba8e0f1e931919248f7ae9252723` | `v84n8_04` | `2288c3b50917329664a263e2cb83a2f5332d58bd436e1869409a64db945c37a6` |
| `v84p8_05` | `8a39487f26b609a332dbe77105f5983fffd66fd425f8623b795458b78dce22c4` | `v84n8_05` | `937445ae245ee106ef08db3fe28ba1313497bc89ef56dc39bf33a18e9f56eb76` |
| `v84p8_06` | `0e29b5860263b628c5068c4a3500efb1614c9128e9a474bc0bff02ba1ebe7dfe` | `v84n8_06` | `0ab0b09773379dfa2eabdd617733a905a727ad58b70fcad3350731fae488422f` |
| `v84p8_07` | `5f0ce6d87ecbec9f8a0582d3f053ef8c75e013cf82b26cee2acfa57d78a901f4` | `v84n8_07` | `1946c2d66d9248ecb6e6f39f3363e481e259ff3ba389ab22a4b5cc2e7d9fb996` |

> 两套之间只有 4 片哈希相同（`…01/02/03` 等）——正常：manifest 长度不同（3576 vs 3568 B）导致分片边界错位，仅重合区段相同。

---

## 6. 上机判读表（照 57 号 §2.6 的四条判据）

### 6.1 前置闸门（先确认仪器，再判画面/性能）

```bash
# 四条计数（mesa_loge 始终输出）：
adb logcat -d | grep -c "CKPT host heap renew #.*begin"
adb logcat -d | grep -c "CKPT host heap renew #.*end"
adb logcat -d | grep -c "CKPT host heap renew #.*postponed"
adb logcat -d | grep -c "kbase: tiler heap renewal (uAPI"
```

**核心不变量：`begin == end + postponed`**（三行都在 `kbase_renew_tiler_heap()` 的同一条路径上：`begin` 在入口、`postponed` 在跳过分支、`end` 在成功分支）。
- 若 `begin > end + postponed` ⇒ 差额 = `kbase_kmod_csf_tiler_heap_create()` **失败**次数（该路径 `panvk_errorf` → 调用点 `vk_queue_set_lost` ⇒ 会伴随 `Failed to renew the kbase tiler heap` 与设备丢失）。⇒ 这一条本身就是"引入新卡死"的早期指征。
- `count(logi) == count(begin)` 应当成立（两者差一行、同一分支）；若不等 ⇒ 日志被截断（logcat 环形缓冲），计数不可用。

### 6.2 四档判读

| # | 档位 | 观察 | 读法与下一步 |
|---|---|---|---|
| 1 | **补丁生效（正常）** | `begin == end + postponed`；`postponed` 行形如 `… postponed (old ctx 0x…), vt a/b, frag c/d`，且 `a/b`（或 `c/d`）能看出**是哪一侧没推进**；健康时 `begin ≈ 图形提交数 / 32` | 这就是期望形态。**`postponed > 0` 本身不是故障**（它是被正确记录的合法状态）：v82 那局有 4 次；若 v84 局仍有少量 postponed 但堆仍定期续租（`end` 持续增长）⇒ F1 的"静默抹掉"已修好，进入观察 F6 |
| 2 | **退避没生效** | `begin` 行数 ≈ **图形提交数**（即 `logi`/`begin` 每提交一次），或 `begin` 数与帧数同量级暴增；伴随帧时间明显变差 | 退避写错或未进包：核对 `:3961` 的 `MAX2(1u, interval/4)` 与 `:3964` 的三目；核对 `PANVK_KBASE_HEAP_RENEW_INTERVAL`（`static` 缓存 ⇒ **改 env 必须重启进程**）。期望：推迟期间 begin 频率 = **每 8 次图形提交**（`interval=32`、`backoff=8`） |
| 3 | **F6 命中（永久/长期失效）** | 连续多条 `postponed`，且 `vt a/b` / `frag c/d` 的**当前值长期不越过快照**（`a` 长期 ≤ `b`，尤其跨越 `kbase: bound subqueue …`（mesa_logd，组重建）之后仍如此） | 说明 `emitted_jobs` 被重置（`:2200`）或某一侧长期不推进 ⇒ 退避只是"每 8 提交白排一次水"⇒ 需要 57 号 §2.4 的 **(A)/(B)**；此时 v84 的代价是**排水频率 4×**（有意取舍，见 §7 风险） |
| 4 | **引入新卡死** | 新增 `kbase: CS error 0x…` / `CSG fatal` / `kbase: timeout on subqueue` / `Failed to renew the kbase tiler heap` / 看门狗 | 与 v82 同场景基线比较：本补丁**不改** TERM 条件、**不改**排水时机、**不改**描述符写法 ⇒ 无因果路径应新增卡死；唯一行为变化是"推迟时更早重试（8 提交 vs 32 提交）"⇒ 只会**更早**换堆。若仍新增 ⇒ 优先怀疑 `create` 失败（看 `begin - end - postponed` 差额）而不是补丁语义 |

画面（楔形）判读**与 v84 无关**：v84 只改续租状态机，不改变堆切换的时机语义之外的东西；楔形的判决留给 v83（env=0）与后续堆/描述符线（见报告 57 §3.5）。

---

## 7. 风险与证伪方法

| 风险 | 说明 | 证伪/验证 |
|---|---|---|
| **推迟期间排水频率 4×** | 旧行为：推迟后计数清零 ⇒ 32 次提交后再试；v84：计数压到 `interval - backoff` ⇒ **8 次提交后**再试 ⇒ 病态路径（F6 长期命中）下排水次数最多 4× | 数 `begin` 频率（§6.2 档 2/3）；若 F6 命中则必须尽快做 :2200 的 (A)/(B) |
| **`interval` 很小（如 =1）时退避退化为每提交重试** | `backoff = MAX2(1, 1/4)=1` ⇒ `interval > backoff` 为假 ⇒ 计数 = 0 ⇒ 下一提交再试。与"每提交续租"的语义一致，无额外退化 | 仅在做加压实验（`=1`）时出现，属预期 |
| **`env=0` 与 v84 的交互** | `interval = UINT32_MAX` ⇒ 续租分支永不进入 ⇒ 与 v84 无关（v83 用的就是这个） | `begin ≈ 0` 即证明 env 生效（**若仍有 begin ⇒ env 没生效，别解读结果**） |
| **日志判读陷阱** | `logi` 行在续租**之前**打印 ⇒ 它计"进入分支"而非"续租"（§3.3）；`postponed` 的 `old ctx` 是当前 ctx 而非退休 ctx（§3.4） | 用 `end` 数当续租次数；用 `begin = end + postponed` 当不变量 |

---

## 8. 回滚（v84 → v82 状态）

```bash
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf
cp -a panvk_vX_gpu_queue.c.bak-v84-1791221286 panvk_vX_gpu_queue.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
ninja -j2 -C /root/zenithblue/build/android-v4
sha256sum /root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so
#   期望 = v82 的驱动：bd61a11da408e90c61504bc91da1225e25082dc3d9e0bdb684e238612acd47a2 (20053424 B)
```
说明：v83 是**纯 env**（无源码改动），v84 是 v82 之后**唯一**的源码改动 ⇒ 回滚 `.bak-v84-1791221286` 即回到 v82 的驱动哈希（**不会**回滚 v82 的 `fn_addr` 修复，那个在 `csf/panvk_vX_cmd_draw.c`）。打包用 Lead 自己的 `pack_v82*.sh`（输出 `/root/final/mgl-panvk-v82*.apk`）。

---

## 9. 未验证 / 限制（诚实清单）

1. **真机未验证**：v84 未安装、未跑画面、未取 logcat ⇒ §6 的四档全部是**预期读法**，不是结论。
2. **两次重编 sha 相同**来自 Lead；A/B 日志无 sha 行（§4）。
3. 我未核对 v84 是否**只**改了这三处（我做的是一次 `diff -u` 对 `.bak-v84-1791221286`，+32/−5，与提案一致；但 `.bak` 本身是 Lead 生成的"改动前"快照，若它被误生成则对比失真——置信度高但非零）。
4. §6.1 的会计恒等式 `begin = end + postponed` 依赖"三条日志都在同一函数路径上"；若 logcat 环形缓冲截断，计数不可用。
5. 未验证 `%lu` 在**所有** `PRIu64` 目标平台上等价（本机 aarch64 LP64 一致；仅记录）。
6. **`task-4` 在共享任务板上不存在** ⇒ 本文件无法通过 claim/complete 记账（见开头提示）。
7. 未做 `:2200` 的 (A)/(B) 前提复核（"走到该点时队列组确已终止"）——那是下一步的独立审计项。

---

## 10. 补充：方法学更正（来自 Lead，2026-10-06）

- 本树 = 基线 `5a07217f034` + **未提交**工作区改动；本项目**从不 commit** 上游补丁 ⇒ **`git merge-base --is-ancestor` / `git log <base>..origin/main` 不能用来判"代码缺失"**，只能用来**列候选提交**；判缺口**必须比对 hunk 的代码行本身**。
- **对本报告的影响：无**。§2–§8 的结论全部来自 `diff -u`（v84 ↔ `.bak-v84-1791221286`）与实测产物哈希，**未使用任何 ancestry 判据**。
- 该更正已在 **报告 59** 系统执行（21 条上游提交逐条"代码级在否"三态判定），并抓到两个方向的反例：`fbb4993c5d7` 不在我们的任何提交里但**代码在**（v76 手工 port）；`22ddbe3067c` **不在 main** 但**代码也在**。
- 任务板状态：**`task-4` 与 `task-5` 均未出现在共享任务板上**（本报告与报告 59 按 Lead 消息要求交付，无法 claim/complete 记账）。
