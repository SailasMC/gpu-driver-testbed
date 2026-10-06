# 49 — v75：完成写从「system-scope 原子加」换成「绝对 STORE64 + LS 刷新」

单变量轮。行为面**只有一个**变量：**权威完成写（subqueue seqno cell + 0）的写法**。
v71/v72/v73 的检查点与 ATOMIC/SEQNO 打印、修复 A、C1/C2/B、超时常量、MobileGL、
`IO_COHERENT` —— **一律未动** ✓。

---

## 0. 一页结论与产物指纹

| 项 | 值 |
|---|---|
| 源码（v74，改前） | `panvk_vX_gpu_queue.c` sha256 `48da562136c2b89f1b044a96fd336aad7d60e7886ba49f7fe06a86ae0098d2db` |
| 源码（v75，改后） | 同文件 sha256 `ba64903b3fc3f07682c4b1671cf31d5d8110f6667e2ed78d937932c4ee6c19fb`（181 132 B → 185 664 B） |
| v74 驱动（基准，未动） | `/root/final/libvulkan_panfrost_v74.so` sha256 `1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56`，**20 055 024 B** |
| **v75 驱动** | `/root/final/libvulkan_panfrost_v75.so` sha256 `f7a0b0b903cd58220ad1def4d3186b530b6b2d76d10d9e18fe2b0fed76c8cec4`，**20 057 480 B** |
| **v75 APK** | `/root/final/mgl-panvk-v75.apk` sha256 `a108c3356418524290d84917701d31bb61f88f4c13666f48eef3c84337f5f19c`，10 220 079 B |
| 打包脚本 | `/root/pack_v75.sh`（由 `/root/pack_v74.sh` sed 派生） |
| 应用脚本 / 补丁 | `/root/v75_apply.py`（带锚点断言，可重复执行）、`/root/v75_seqno_store.patch`（6 534 B，仅供阅读） |
| 源码备份 | `src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c.bak-1791214365`（= v74 逐字节） |

编译：`export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH` +
`ninja -j4 -C /root/zenithblue/build/android-v4` ⇒ **4 次构建全部 exit 0**，警告只有 v74 既有的
`-Wc23-extensions`（`label followed by a declaration`，`:1870`；v73/v74 构建日志里同样是这一条，
一次构建 5 份 TU 各 1 条）。**无 error。**

---

## 1. 改动点（file:line，行号全部实测自 v75 源码 `ba64903b…`）

| # | 位置 | 内容 |
|---|---|---|
| 1 | `src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c:831-859` | v75 说明注释块（完成写为什么换） |
| 2 | 同文件 **`:860-864`** | **新开关** `kbase_seqno_store_enabled()` → `debug_get_bool_option("PANVK_KBASE_SEQNO_STORE", true)` |
| 3 | 同文件 **`:866-877`** | **新开关** `kbase_defer_diag_enabled()` → `debug_get_bool_option("PANVK_KBASE_DEFER_DIAG", false)` |
| 4 | 同文件 **`:1247-1248`** | `kbase_log_defer_diag(...)` 调用被 `if (kbase_defer_diag_enabled())` 包住（静音 v74 的 `kbase: DEFER` 四行刷屏；函数体 `:913` 原样保留） |
| 5 | 同文件 `:1581-1585` | 既有"sync 之后不再发射"不变量注释补一句 v75 note（**纯注释**，只对 `=0` 分支成立） |
| 6 | 同文件 **`:1638-1670`** | **★ 唯一行为改动**：完成写按开关二选一 |

`:1638-1670` 的实际代码：

```c
   if (kbase_seqno_store_enabled()) {                 /* :1638  默认分支 */
      ...
      cs_move64_to(&b, val64, target_seqno);          /* :1662 */
      cs_store64(&b, val64, addr64, 0);               /* :1663  绝对写 cell+0 */
      cs_wait_slot(&b, SB_ID(LS));                    /* :1664  LS 刷新 */
   } else {
      cs_move64_to(&b, val64, 1);                     /* :1667  = v74 原文 */
      cs_sync64_add(&b, true, MALI_CS_SYNC_SCOPE_SYSTEM, val64, addr64,
                    cs_defer(0, SB_ID(DEFERRED_SYNC))); /* :1668-1669 = v74 原文 */
   }
```

> 注：v74 树里那些日志/注释字符串写的 `panvk_vX_gpu_queue.c:1357` 是**旧行号**（v74 源里发射点
> 其实在 `:1584-1586`）。本报告一律给实测行号，**没有**去改那些历史字符串（避免引入无关改动）。

---

## 2. 新旧完成写：逐条差异

寄存器约定（本函数内，`:1477-1492`）：`reg = csif_info->cs_reg_count - 4`；
`addr64 = {reg, reg+1}`，`val32 = {reg+2}`，`val64 = {reg+2, reg+3}`。四者就是 ring entry
唯一允许改写的 FW-unpreserved 寄存器。`seqno_addr = kbase_subqueue_seqno_dev_addr(queue, subqueue)`
（`:1493`），`target_seqno = subq->kbase.emitted_jobs + 1`（`:1494`）——**发射时已知 ✓**。

| 步骤 | v74（`=0` 分支 / 原行为） | v75（默认分支） | 是否相同 |
|---|---|---|---|
| 地址寄存器 | `addr64`（`:1586` 由 `cs_move64_to(addr64, seqno_addr)` 装载，其后无改写） | 同一个 `addr64`，**不重装**（沿用 v74 完全相同的活性假设） | **相同** |
| 装载写值 | `cs_move64_to(val64, 1)`（加数 1） | `cs_move64_to(val64, target_seqno)`（**绝对值**） | 不同（值语义） |
| 目标地址 | `cell + 0`（`addr64` 偏移 0） | `cell + 0`（`addr64` 偏移 0） | **相同** |
| 写指令 | `cs_sync64_add(SCOPE_SYSTEM, …)` = **1 条 SYNC_ADD64，RMW 原子加** | `cs_store64(…)` = **1 条 STORE_MULTIPLE（mask 0b11，64 bit），绝对写，无 RMW** | **不同（核心变量）** |
| 等待/刷新 | 延迟发射：`cs_defer(wait_mask 0x0000, signal_slot SB_ID(DEFERRED_SYNC)=1)`；发射后**本 wrapper 不再发任何指令** | **`cs_wait_slot(SB_ID(LS))`** ＝ `cs_wait_slots(BIT(0))` = 1 条 WAIT | 不同（发射模型） |
| 检查点 | `WRAP_BEFORE_SYNC_ADD`（id 42）在写之前 | **同一个 id 42，同一位置** | **相同** |
| 之后 | `cs_error_barrier()` → `cs_end()` | 同 | **相同** |
| cell 初值/终值语义 | `kbase_init_seqnos()` 处 `memset(cpu, 0, 4096)`（`:1308`）+ 每 entry `+1` ⇒ 终值 = 已退休的最大 target | 每 entry **覆盖**写 `emitted_jobs+1`，子队列内 entry 顺序执行 ⇒ 终值 = 已退休的最大 target | **语义等价 ✓** |
| 发射的 ring entry 长度 | 基准 | **+1 条指令（+8 B）** | 见下 |

**指令数增量（分析值，非真机测量）**：
`cs_wait_slots()` 无论 mask 多少都只发 **1 条 WAIT**（`cs_builder.h:935-951`）；
`cs_move64_to()` 在 `imm < 2^48` 时都走 **1 条 move48**（`cs_builder.h:1481-1490`），
`1` 与 `target_seqno` 同一条路径 ⇒ 指令数不变；`cs_store64()` = 1 条 STORE_MULTIPLE
（`cs_builder.h:2159-2163` → `cs_store()` `:2133-2149`）。
⇒ 净变化 = **−1（SYNC_ADD64）+1（STORE_MULTIPLE）+1（WAIT）= +1 条 = +8 B**。
上限 `kbase_ring_job_max_size()` = **512 B**（非 diag）/ **768 B**（`PANVK_DEBUG=kbase_diag`，
`:61 / :69 / :73`）。附加好处：这条 WAIT 命中 LS slot 时会清 `pending_stores`
（`cs_builder.h:947-950`），与检查点用的是同一条 store-flush 纪律。

**为什么它不会变成新的挂点**：STORE_MULTIPLE **不是** deferred op，它只占 LS 记分板；
同一个 wrapper 在本改动**之前**就已经在 `:1595` 前后反复等同一个 LS slot
（`kbase_wrapper_checkpoint()` 的 `cs_wait_slot(b, SB_ID(LS))`，`:1346`；LS 完成副本 `:1606-1607`），
且 `conf.ls_sb_slot = SB_ID(LS) = 0`（`:1417`，`panvk_cmd_buffer.h:367/377`）。所以这条等待
在真机上已被 v71/v72 跑通过 ✓。

---

## 3. env 开关用法

| 变量 | 缺省 | 取值 | 效果 |
|---|---|---|---|
| `PANVK_KBASE_SEQNO_STORE` | **1（未设即新写法）** | 未设 / `1` / `y`/`yes`/`t`/`true` → **绝对 STORE64 + LS 等待（v75）**；`0` / `n`/`no`/`f`/`false` → **v74 的 SYNC_ADD64，逐指令相同**；其它值 → 回落缺省(true) | 完成写二选一 |
| `PANVK_KBASE_DEFER_DIAG` | **0（静音）** | `1` → 恢复 v74 的 `kbase: DEFER …` 四行打印；其它/未设 → 不打印 | 只影响日志 |

解析规则取自本树自己的 `debug_parse_bool_option()`（`src/util/u_debug.c:108-140`：NULL→缺省、
`"0"`→false、`"1"`→true、未知串→缺省）。**缺省值写在代码里**（`:863` `… , true`；
`:876` `… , false`），**没有写进 APK 的 env 串** ✓。

**APK 内 env 串与 v74 逐字符一致 ✓**（`aapt2 dump xmltree` 逐行比对，
`pojavEnv`/`boatEnv` 两行的 `android:value` diff 为**空**；两 APK manifest 的整体 diff
**只有** versionCode/versionName 两行）：

```
LIBGL_ES=3:POJAV_RENDERER=opengles3:MOBILEGL_BACKEND_TYPE=DirectVulkan:MOBILEGL_ESPRYT_USE_ANGLE=0:MOBILEGL_MAGMA_R11G11B10F_FALLBACK=0:MOBILEGL_LOG_FILE_PATH=/sdcard/MG/mgl.log:MESA_DEBUG=1:PANVK_DEBUG=1,kbase_diag:LIBGL_DEBUG=1:EGL_LOG_LEVEL=debug:PANVK_KBASE_HEAP_RENEW_INTERVAL=32
```

**一次上机做 A/B**：
- 装 v75 APK、env **不动** ⇒ 新写法（默认）。
- 想在同一支二进制上回到 v74 行为：在启动 env 里追加 `:PANVK_KBASE_SEQNO_STORE=0`
  （该串用 `:` 分隔，直接追加到 `pojavEnv` / `boatEnv` 末尾即可），重启应用。
- 想看 v74 那四行 DEFER 打印：追加 `:PANVK_KBASE_DEFER_DIAG=1`。

---

## 4. 确定性对照（★必查项）

命令固定为 `export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH`
+ `ninja -j4 -C /root/zenithblue/build/android-v4`；日志 `v75_build1/2/3.log`、`v75_ctrl_build.log`。

| # | 源码状态 | 源码 sha256（前 8） | ninja exit | `.so` sha256 | 字节数 |
|---|---|---|---|---|---|
| 1 | v75（应用补丁） | `ba64903b` | **0** | `f7a0b0b903cd58220ad1def4d3186b530b6b2d76d10d9e18fe2b0fed76c8cec4` | 20 057 480 |
| 2 | **同一份源码重编**（touch → 重编该 TU + 重链接） | `ba64903b` | **0** | **`f7a0b0b9…c8cec4`（同上，逐位相同 ✓）** | 20 057 480 |
| 3 | **撤掉本轮全部改动（含新增开关）** | `48da5621`（= v74 逐字节） | **0** | **`1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56`** | **20 055 024** |
| 4 | 贴回补丁（`v75_apply.py` 再跑一次，4 处锚点断言全通过） | `ba64903b` | **0** | `f7a0b0b9…c8cec4`（与 #1 一致 ✓） | 20 057 480 |

⇒ **「撤改动后是否逐位等于 1df10436…」= 是** ✓（sha256 **与**字节数同时相等，
不是仅前 16/24 字符相符）。
⇒ 新 `.so` 二次重编可复现 ✓（#1 与 #2/#4 三次构建同一 sha256）。
⇒ 另证新代码确已编入：`strings` 中 `PANVK_KBASE_SEQNO_STORE` / `PANVK_KBASE_DEFER_DIAG`
在 v75 `.so` 各出现 1 次，在 v74 `.so` 出现 **0** 次。

---

## 5. APK 与分发校验

| 校验 | 结果 |
|---|---|
| `unzip -p mgl-panvk-v75.apk lib/arm64-v8a/libvulkan_freedreno.so \| sha256sum` | `f7a0b0b903cd58220ad1def4d3186b530b6b2d76d10d9e18fe2b0fed76c8cec4` ✓ 非空 |
| 该条目字节数 | 20 057 480 ✓（= 源 `.so` 逐字节） |
| `aapt2 dump badging` | `versionCode='75' versionName='6.15-seqno-store'` ✓ |
| env 串 parity | v74 vs v75 `pojavEnv`/`boatEnv` 的 value diff 为空 ✓；整 manifest diff 仅 version* 两行 |
| v72/v73/v74 APK 未被改 | `mgl-panvk-v72.apk 37169cc4…`、`-v73.apk e5b3fb86…`、`-v74.apk 74b173ce…`，打包前后 sha256 完全一致 ✓ |
| 切分 | `/data/dsh_downloads/v75p8_00..07`（`_00`..`_06` 各 1 277 510 B，`_07` = 1 277 509 B，合计 10 220 079 B） |
| 切分重组 | `cat v75p8_0* \| sha256sum` = `a108c3356418524290d84917701d31bb61f88f4c13666f48eef3c84337f5f19c` = APK sha256 ✓ |
| 报告要求的 24 字符 | `a108c3356418524290d84917` |

（全部操作**未使用 `rm`** ✓，未 `git checkout/stash/reset` ✓，未操作手机 ✓。）

---

## 6. 回滚命令

**A. 运行时回滚（最快，无需重编，同一支 APK）**
```
在启动 env 追加：PANVK_KBASE_SEQNO_STORE=0
（附加到 pojavEnv / boatEnv 末尾，前面加 ":"）⇒ 完成写回到 v74 的 SYNC_ADD64，逐指令相同
```

**B. 源码/构建回滚到 v74（本轮改动全部撤掉，含新增开关）**
```
cp -f /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c.bak-1791214365 \
      /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
ninja -j4 -C /root/zenithblue/build/android-v4
# 期望：exit 0，
# libvulkan_panfrost.so = 1df104361201b0a3a22cad1019b98cd0d24d526517c6ff7885cd69ce80704a56，20 055 024 B
```
**C. 重新贴回 v75 补丁**
```
python3 /root/v75_apply.py     # 自带锚点断言；源码 sha256 应变成 ba64903b…
ninja -j4 -C /root/zenithblue/build/android-v4   # 期望 f7a0b0b9…c8cec4 / 20 057 480 B
```
**D. 驱动侧回滚**：直接装回 `/root/final/mgl-panvk-v74.apk`（sha256 `74b173ce…`，本轮未改动）。

---

## 7. 判读表（真机读数 → 结论）

对照基线（v74 那一次决定性快照）：`cell+0 = 701`、expected target `702`、
`CKPT timeout snapshot subqueue 1 word 0xc0de0010 => id 16 (FRAG_BEFORE_TILING_WAIT)`、
无 CS fault / 无 OOM / 无 MMU 异常。

| 新写法下的观测 | 判读 |
|---|---|
| ★ **不再卡**：同一场景跑通，`cell+0 == target_seqno`，不再出现超时快照 | **定论**：唯一的卡点就是那条 system-scope 原子 `SYNC_ADD64` 本身不落地（v74 已排除 CS fault / OOM / MMU 异常，且 dc civac + dsb sy 后仍读到 701 ⇒ 不是陈旧副本）；同一 cell 上普通 STORE64 + LS 刷新可替代它 ⇒ 采纳该完成写，本轮目标达成 |
| **仍卡在同一处**：超时快照里 `cell+0` 仍 = target−1，`CKPT` 仍读 **id 42**（WRAP_BEFORE_SYNC_ADD） | 说明卡点**不是**"原子 vs 普通 store"的差别：连普通 STORE64 也写不进去 ⇒ 卡点在**这条写之前**（`:1595` 的 all-slots WAIT，或 `WRAP_AFTER_WAIT` 之后的路径），或在该地址所在 BO 的映射/可见性契约上。下一步先看 `CKPT` 是 **45**（已过 all-slots WAIT）还是 **44**（卡在 WAIT 里） |
| 仍卡但 `CKPT` 落在 **44 / 45** 之间或变成 45 | 精确落在"all-slots WAIT 已过、完成写未生效"的窗口 ⇒ 指向 LS / 内存可见性与 kbase 通知契约，而非原子能力；再看 `ls_copy`/marks 与 CQS / CSF_EVENT 通知是否到达 |
| `cell+0` 到了 target，但进程仍超时 | 完成写这条链已通；剩下的是 **CPU 侧等待/通知**（`kbase_wait_sync_targets()` / CQS / `ppoll`），与本次单变量无关，另开一轮 |
| 出现**新的** CS fault / MMU 异常 / OOM（v74 三者皆无） | 新写法引入了新问题（例如 STORE_MULTIPLE 与 `cs_error_barrier` 的排序，或 ring entry 长度越界触发 `:1682` 的 assert）⇒ 按新读数定位，不要归因到旧结论 |
| **A/B 对照**：`PANVK_KBASE_SEQNO_STORE=0` 复现 v74 的卡点，`=1`（默认）通过 | **单变量因果成立**：卡点确由这条完成写引起、并被这条替换消除；v74 的读数不是环境抖动 |

判读时请一并核对：`ATOMIC` / `SEQNO` / `CKPT` 打印与 v74 **同格式**（本轮未动）；
默认分支下**不再出现** `kbase: DEFER …` 四行是本轮的静音（预期 ✓），要看就加
`PANVK_KBASE_DEFER_DIAG=1`。

---

## 8. 未验证 / 边界（如实标注）

1. **新写法的真机行为未验证** —— 本轮禁止操作手机，只交付二进制与"构建级 + 静态级"证据，
   第 7 节的判读留待下一轮上机。
2. **ring entry 的绝对字节数未测量**：只给出 **+8 B** 的增量（分析值，依据 `cs_builder.h` 的
   `cs_wait_slots` / `cs_move64_to` / `cs_store` 三条发射规则）与 **512 B / 768 B** 上限；
   若真越界，`:1682` 的 `assert(entry_size <= kbase_ring_job_max_size())` 会当场 abort ——
   属于"立刻可见的硬失败"，不会静默劣化。
3. **研究报告 48 未在本机**：`/root/research/` 只到 `47-v74-defer-diag.md`，无 `48-*`
   （已 `find /root -maxdepth 4 -name '48-*'` 确认为空）。本报告对 48 的引用
   （`cs_defer(0, SB_ID(DEFERRED_SYNC))` 的 `wait_mask=0` ⇒ `signal_slot` 被强制 0
   ⇒ 不是延迟发射）**转引自本轮任务书**，未能在本机复读原文。
4. **APK 签名**用既有 `/root/dsh-driver.keystore`（别名 `dshdriver`，pass `android`），
   与 v74 同源；`classes.dex` 与 `libMobileGL.so` 取自 `/root/v54/`，与 v74 相同（`pack_v75.sh`
   由 `pack_v74.sh` 仅改 4 处派生，见 §0 的脚本 diff 说明）。
5. 构建机为 **x86_64**（`Linux RainYun-6uOcEx7k 6.12.38+deb13-amd64`，Debian 13），
   交叉编译到 Android arm64；"两次重编逐位一致"只在本机这套构建配置下成立，
   换构建机需另行验证。
