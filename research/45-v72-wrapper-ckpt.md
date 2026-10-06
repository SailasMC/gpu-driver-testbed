# 45 · v72 — 把 v71 的检查点机制扩展到 ring wrapper 侧（纯诊断）

树 `/root/zenithblue/work/mesa` · 构建目录 `/root/zenithblue/build/android-v4` ·
工具链 `export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH`
（`/opt/android-sdk/ndk/27.3.13750724` 是它的符号链接，readlink -f 实测同一棵树）。

## 1. 摘要

v71 只能看到 **被调用流内部** 的检查点，因此真机读数停在 `0xc0de0018`（24 = CMDBUF_DONE）时，
无法区分「CALL 没返回」和「CALL 返回了、卡在 wrapper 的完成尾巴上」。v72 在
`kbase_subqueue_emit_job()` 的 wrapper 指令序列里，用**完全相同**的机制（同一个
`MAGIC | id` 常量 STORE、同一个 seqno cell + 56 的 32 位字、
`PANVK_DEBUG(KBASE_DIAG)` 门控、同一条 store→LS wait 纪律）插入 5 个 wrapper 检查点，
并且**刻意覆盖** callee 的最后一个检查点 —— 因为该字记录的是「最后写下的检查点」，
覆盖之后「读到的还是 24」本身就证明 CALL 没有返回。

**行为语义零改动**：5 个检查点全部在 `if (PANVK_DEBUG(KBASE_DIAG))` 之内，纯 STORE +
既有 LS 纪律，不引入任何新的等待、不改控制流、不改任何既有检查点语义、不改修复 A/C1/C2/B、
不改超时常量、不改 `SB_*` 写入、不碰 MobileGL。**diag 门关闭时，发出的 ring 字节与 v71 逐条相同**
（见 §7 的构造性论证：唯一被改动的非门控值 `kbase_ring_job_max_size()` 在门关闭时返回 512，
与 v71 的常量逐位相等）。

## 2. 新增 ID 表（id → 名字 → 文件:行）

| id | 名字 | 发射点 file:line | 相对位置的语义 |
|----|------|------------------|----------------|
| 40 | `WRAP_BEFORE_CALL` | `csf/panvk_vX_gpu_queue.c:1187-1190`（枚举 `panvk_cmd_buffer.h:171`） | 检查点字被清零之后、cache flush 与 CALL 之前 |
| 41 | `WRAP_AFTER_CALL` | `csf/panvk_vX_gpu_queue.c:1221-1224`（枚举 `:172`） | CALL 返回后（post-CALL 面包屑之后）★最关键之一 |
| 42 | `WRAP_BEFORE_SYNC_ADD` | `csf/panvk_vX_gpu_queue.c:1286-1289`（枚举 `:173`） | 延迟 SYNC_ADD64 之前（wrapper 可观测段的最后一条）★ |
| 44 | `WRAP_BEFORE_WAIT` | `csf/panvk_vX_gpu_queue.c:1243-1246`（枚举 `:174`） | `cs_wait_slots(all_mask)` 之前 |
| 45 | `WRAP_AFTER_WAIT` | `csf/panvk_vX_gpu_queue.c:1248-1251`（枚举 `:175`） | `cs_wait_slots(all_mask)` 之后 |

辅助件：
* `kbase_wrapper_checkpoint()` 定义：`csf/panvk_vX_gpu_queue.c:996`（发射体，4 条指令：
  `MOVE64 addr64=seqno_addr` → `MOVE32 val32=MAGIC|id` → `STORE32 @cell+56` → `WAIT SB_ID(LS)`）。
* 名字表 `kbase_checkpoint_name()` 同步新增 5 个 case：`csf/panvk_vX_gpu_queue.c:630-634`
  （与枚举一一对应，无遗漏无多余）。
* 枚举新增：`csf/panvk_cmd_buffer.h:171-175`；说明性注释 `csf/panvk_cmd_buffer.h:113-124`。
* **1..39 未动**（`panvk_cmd_buffer.h:116-167` 逐字保留）。

## 3. wrapper 内的实际发射顺序（指令流顺序，与 id 数值序不同）

```
40 WRAP_BEFORE_CALL
   [cache flush]  [CALL]                <- v71 已有，未改
41 WRAP_AFTER_CALL
   [MOVE64 addr64 = seqno_addr]
44 WRAP_BEFORE_WAIT
   cs_wait_slots(all_mask)              <- wrapper 自己的「等全部 scoreboard」WAIT
45 WRAP_AFTER_WAIT
   [post-wait 面包屑块 / LS copy]        <- v71 已有，未改
42 WRAP_BEFORE_SYNC_ADD
   cs_move64_to(val64,1)
   cs_sync64_add(... cs_defer(0, SB_ID(DEFERRED_SYNC)))   <- 延迟完成写
   cs_error_barrier()                   <- 无寄存器
   cs_end()
```

## 4. 刻意省略的 ID：43 / 46 / 47（未定义、未发射）

任务建议的 43 `WRAP_AFTER_SYNC_ADD`、46 `WRAP_BEFORE_ERROR_BARRIER`、
47 `WRAP_AFTER_ERROR_BARRIER` **一律不发射、也不在枚举里定义**。两条**各自独立成立**的理由：

1. **会破坏完成写**：延迟的 SYNC_ADD64 在 in-flight 期间读取它的操作数寄存器
   （`addr64` = seqno cell → 寄存器 14/15，`val64` = 加数 1 → 寄存器 16/17）。这 4 个寄存器
   正是 ring entry 唯一允许改写的 FW-unpreserved 寄存器；而一次检查点 STORE 至少要占用
   其中 2 个（地址 + 数据，`CS STORE_MULTIPLE` 的数据**只能来自寄存器**，`genxml/v11.xml:787-794`
   没有 immediate 数据域 —— 已核对），所以在那之后插 STORE 必然踩掉 in-flight 完成写的操作数。
   这正是 v71 既有不变式（`panvk_vX_gpu_queue.c:1234-1237`「Nothing is emitted after that
   sync operation … so its operands cannot be clobbered」）要保护的东西，v72 保持它成立。
2. **即使写了也读不到**：LS 操作按程序序退休，排在延迟 SYNC_ADD64 之后的 store 只有在该
   sync add **已经执行完**之后才落盘 —— 也就是任务不再挂起的时候。挂起现场该字仍会读成 42。

因此「完成计数到底写没写」不需要新 ID：**硬件 seqno（cell + 0）本身就能回答**，
v71 超时日志已经在打印（`seqno` 到没到 `target_seqno`）。见 §10。

## 5. REPLACE 语义（刻意覆盖 callee 的 CMDBUF_DONE）

该 32 位字是「最后写下的检查点」，不是历史。wrapper 在 CALL 返回之后才写 41，
所以 41 会覆盖 callee 最后写的 24（CMDBUF_DONE）。这正是想要的性质：

* 读到 **1..39** ⇒ 字是**被调用流**最后写的 ⇒ CALL 没有返回（v71 现场 sq0 = 24 就属于这一类）；
* 读到 **40..45** ⇒ 字是**wrapper**最后写的 ⇒ CALL 已返回或根本没进去。

流侧（1..39）与 wrapper 侧（40..45）**ID 空间互不相交**，因此单看读数就知道「最后写下的是谁」。

## 6. 复用而非重造（不改动清单）

* 同一字：`KBASE_SEQNO_CHECKPOINT_OFFSET 56`（`panvk_vX_gpu_queue.c:78`）+
  `static_assert`（`:79`）保持；`PANVK_KBASE_CKPT_MAGIC 0xc0de0000u` 复用
  （`panvk_cmd_buffer.h:125`，未改）。
* 同一门控与纪律：`PANVK_DEBUG(KBASE_DIAG)` + STORE→`cs_wait_slot(SB_ID(LS))`。
* 未改：v71 检查点语义（1..39 及其发射点）、修复 A、C1/C2/B、`KBASE_WAIT_TIMEOUT_NS`
  超时常量、`SB_*` 写入、MobileGL、任何行为逻辑。

## 7. diag 专用容量上限（唯一的非纯诊断改动，且对非 diag 路径逐位无害）

新增 `KBASE_RING_JOB_MAX_SIZE_DIAG 768` 与 `kbase_ring_job_max_size()`
（`panvk_vX_gpu_queue.c:61-75`），替换了原来 5 处直接使用 `KBASE_RING_JOB_MAX_SIZE` 的地方
（`:1016` `required`、`:1018`/`:1056` 跨环判定、`:1068` `.capacity`、`:1304` `assert`）。

理由：v66/v71 的 diag wrapper 已把最宽子队列的 entry 抬到 ≈320 B，5 个新检查点再各加 4 条
指令（`MOVE64`+`MOVE32`+`STORE32`+`WAIT`，seqno 地址 ≥2^48 时 `cs_move64_to` 多一条 ⇒ 每条
32–40 B），即 +160…200 B ⇒ ≈480…520 B。旧常量 512 只剩零余量，为免 `cs_is_valid()` 失败
（诊断轮直接不可用）与 `assert` 触发，diag 轮预留 768 B。

**非 diag 路径逐位不变**：`kbase_ring_job_max_size()` 在门关闭时返回 512，与 v71 常量相等 ⇒
reserve/跨环/capacity/assert 全部取到与 v71 相同的值；5 个发射点都在
`if (PANVK_DEBUG(KBASE_DIAG))` 内 ⇒ **门关闭时发出的 ring 字节序列与 v71 逐条相同**。
（该等价性为构造性论证，未在设备上实测 —— 本轮禁止操作手机。）

## 8. 确定性对照（全部实测）

| 步骤 | 命令 | 结果 |
|------|------|------|
| 起点 | `sha256sum …/libvulkan_panfrost.so` | `27c3cf2fc5a920eb…` = v71 APK 载荷、20 040 968 B |
| 打补丁后重编 | `ninja -j2 -C build/android-v4` | **exit 0**，`0d03a6f2f7c09b82…`（20 044 944 B） |
| **撤掉本轮改动重编** | `cp -f *.bak-v72-1791209500 → 原文件` 后 `ninja … libvulkan_panfrost.so` | **退出 0，`27c3cf2fc5a920eb67e508ae3c91887ea5f0741b70e5de9b5b0e76428681ebab` —— 与 v71 逐位相等 ✓**（源文件哈希亦回到 `3d9ebca2…` / `45bfa6fa…`） |
| 再贴回重编 | `sh /root/v72_apply.sh` + `ninja …` | **退出 0，`0d03a6f2f7c09b820604d768eeaccd62020b05b9cd7eb2c7d17f198a7ddf731f` —— 二次重编可复现 ✓** |

## 9. APK 与分发校验

派生命令：`/root/pack_v72.sh`（由 `/root/pack_v71.sh` 逐字符派生，diff 只有 3 处：
`W=/root/v71→/root/v72`、`OUT=…v71.apk→…v72.apk`、manifest 的
`versionCode 71→72` + `6.11-checkpoints→6.12-wrap-ckpt`（源取自 `/root/v71/AndroidManifest.xml`）；
另把 `rm -rf $W;` 去掉 —— 系统拦截 rm，且 `/root/v72` 本就不存在，`mkdir -p` 等价）。
env/MobileGL/classes.dex/keystore/工具链串与 v71 **逐字符一致**。

* `unzip -p /root/final/mgl-panvk-v72.apk lib/arm64-v8a/libvulkan_freedreno.so | sha256sum`
  = `0d03a6f2f7c09b820604d768eeaccd62020b05b9cd7eb2c7d17f198a7ddf731f`，20 044 944 B（非空 ✓，等于新 `.so` ✓）
* `aapt2 dump badging` → `versionCode='72' versionName='6.12-wrap-ckpt'` ✓
* APK 全量 `sha256 = 37169cc4b10423854843ba7429583a249f1c8e5dcc35194a5a44a11d091af6e4`，10 211 887 B
* **未被动过**：`mgl-panvk-v70.apk`（`f9af2fed…`，10 203 695 B）、`mgl-panvk-v71.apk`
  （`918928cc…`，10 211 887 B；重新解包其载荷仍为 `27c3cf2f…` ✓）
* 载荷一致性：`libMobileGL.so` = `72919c73…`（v71 = v72）、`classes.dex` = `6bd3abde…`（v71 = v72）
* HTTP 分发：`/data/dsh_downloads/mgl-panvk-v72.apk` 已复制；`split -n 8 -d … v72p8_` 得到
  `v72p8_00..v72p8_07`（各 1 276 486 B，最后一片 1 276 485 B；`cat v72p8_0*` 重组的 sha256
  与源 APK 逐位相等 ✓）。sha256 前缀 = `37169cc4b10423854843ba74`

## 10. 判读表（**每个新 ID 命中意味着什么**）

读法：`grep -E 'CKPT|timeout on subqueue' cap.txt`。字值 = `0xc0de0000 | id`。

| 读到的 id | 名字 | 含义（该子队列的 ring entry 走到哪一步） |
|-----------|------|------------------------------------------|
| `0x0`（0） | — | 连 40 都没写：wrapper 序言（清零那一步之后）没走完，entry 基本没被执行 —— 优先怀疑 kick/entry 根本没有被 CS 取到 |
| **`0x28`（40）** | WRAP_BEFORE_CALL | 走到了 **flush/CALL 之前**。若 callee 的流侧 id 从未出现 ⇒（CALL 没进去 / 被调用流没有写任何检查点）；结合 sq1/sq2 一起看可判定是否「卡在 CALL 之前」 |
| **`0x29`（41）** | WRAP_AFTER_CALL | ★**CALL 已经返回**。即 callee 的指令流跑完了（或已退出），控制权回到 wrapper ⇒ **卡点在 wrapper 的完成尾巴**（all-slot WAIT 或延迟 SYNC_ADD64）。这一条直接回答 v71 悬而未决的问题 |
| **`0x2c`（44）** | WRAP_BEFORE_WAIT | 卡在 `cs_wait_slots(all_mask)` 里 ⇒ 它在等的某个 scoreboard 槽永远没人 signal（「等别的队列」型死锁；与 sq1/sq2 的 BARRIER_BEFORE_QUEUE_WAIT 互为镜像） |
| `0x2d`（45） | WRAP_AFTER_WAIT | 全槽 WAIT 已退休，但还没走到/没退休完成写（40/41 已过，42 未到；窗口很窄：只包住 post-wait 面包屑块） |
| **`0x2a`（42）** | WRAP_BEFORE_SYNC_ADD | ★★**wrapper 已经走到延迟 SYNC_ADD64 之前**：`seqno` 若仍未到 `target_seqno` ⇒ **完成计数没有被写下**，卡点精确落在 `SYNC_ADD64`（或其延迟退休）上。**这条正是「指令流跑完但 ring entry 不完成」的机理硬证据** |
| `0x18`（24） | CMDBUF_DONE（流侧） | **CALL 没有返回**：字还是 callee 最后写的 ⇒ 挂在被调用流内部 / CALL 返回路径上（= v71 现场 sq0 的读数）。与 41 构成互斥判定 |
| 其他 `0x01..0x27` | 流侧 1..39 | 挂在被调用流内部对应位置（v71 已有的判读法不变）。**注意**：sq1/sq2 读到的 27 = BARRIER_BEFORE_QUEUE_WAIT 属于这一类 |
| `0x2b`/`0x2e`/`0x2f`（43/46/47） | 未定义 | v72 **不会**产生；若出现，说明驱动不是这一版 |

交叉核对：`word==41 或 42` 且 `seqno < target` ⇒ 卡点在「CALL 返回/退休」这一段（本轮假设 ✓）；
`word==42` 且 `seqno` 已推进 ⇒ 完成写其实成功（则该子队列的 timeout 另有原因）。

## 11. 回滚 / 重放命令

```sh
# 回滚到 v71（源代码 + 产物）
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/
cp -f panvk_vX_gpu_queue.c.bak-v72-1791209500 panvk_vX_gpu_queue.c
cp -f panvk_cmd_buffer.h.bak-v72-1791209500  panvk_cmd_buffer.h
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:/root/zenithblue/build/host-tools/bin:$PATH
ninja -j2 -C /root/zenithblue/build/android-v4 src/panfrost/vulkan/libvulkan_panfrost.so
# 期望 27c3cf2fc5a920eb67e508ae3c91887ea5f0741b70e5de9b5b0e76428681ebab

# 重新贴回 v72
sh /root/v72_apply.sh          # = python3 /root/v72_patch.py && python3 /root/v72_fix1.py
ninja -j2 -C /root/zenithblue/build/android-v4 src/panfrost/vulkan/libvulkan_panfrost.so
# 期望 0d03a6f2f7c09b820604d768eeaccd62020b05b9cd7eb2c7d17f198a7ddf731f
bash /root/pack_v72.sh         # 重新打包 /root/final/mgl-panvk-v72.apk
```

源文件哈希：补丁前 `panvk_vX_gpu_queue.c` `3d9ebca2427efadf…`、`panvk_cmd_buffer.h`
`45bfa6fa5a621101…`；v72 `17a9ea310eeb72db…`、`2756104deb3707a9…`。
注：`/root/v72_fix1.py` 是必需的第二步（`v72_patch.py` 的枚举注释插在 `*/` 之后，
必须补一个 `*` 续行把注释块接回，否则 `-Wimplicit-int` 报错 —— 实测过）。

## 12. 未验证项（如实标注）

1. **未在设备上运行**：本轮禁止操作手机，因此新 ID 的实际命中情况、以及 §10 判读表
   的现场验证，都要等装机后一轮。
2. diag wrapper entry 的实际字节数（估算 480…520 B ≤ 768 B）**未实测**；
   768 是宽松上界，`assert`/`cs_is_valid()` 不会因此触发。
3. §7 的「门关闭时发出的 ring 字节与 v71 逐条相同」是构造性论证，非设备实测。
