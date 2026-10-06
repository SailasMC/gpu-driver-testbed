# 32 · v66：零风险诊断升级 —— CS 寄存器进 timeout 快照 + kbase_diag 位图

**任务**：只做诊断升级，不改行为。目标：**一次运行就能判定「固件到底有没有进入被 CALL 的那条流」**。
**结论一句话**：现场（14:47:55–14:48:16 这一次运行）**是 v65 载荷**，签名与 v54 以来逐字一致（三子队列 extract 各自精确停在 CALL：VT/FRAG +104 B、COMPUTE +88 B），v65 的删行把 entry 从 20 条缩到 19 条但**没有改变失败类别**；本轮把 `CS_STATUS_*`（含 `CS_STATUS_WAIT_SB_MASK`）/`BLOCKED_REASON`/`CS_FAULT`/`CS_HEAP_*` 与 **callee 侧进度位图**打进 timeout 快照，并让 **bit0 = CMDBUF_START 成为被调用流的第一条指令** ⇒ 固件是否入流从「推断」变成「读数」。撤改动重编**逐位等于 v65 `b9952f75…`（20006016 B）**，已实测 ✓。

---

## 1. 现场确认（`/sdcard/MG/cap.txt`，文件 9 244 509 B / 64 319 行，含且仅含一次运行）

### 1.1 这一次运行是 **v65 载荷**（不是 v64）——算术闭合证明

| 判据 | v64（报告 31 §3.3 实测） | 本次现场（14:47 运行） | 一致？ |
|---|---|---|---|
| VT/FRAG entry 未填充字节 | 160 B（20 条） | **152 B（19 条）** | = v65 预测「160→152」✓ |
| COMPUTE entry | 144 B（18 条） | **136 B（17 条）** | = v65 预测「144→136」✓ |
| extract − last_job_offset（VT/FRAG） | +112（= 14×8，CALL 在 idx14） | **+104（= 13×8，CALL 在 idx13）** | = v65 预测「CALL 由 +112 移到 +104」✓ |
| extract − last_job_offset（COMPUTE） | +96（= 12×8） | **+88（= 11×8）** | = v65 预测「+96→+88」✓ |

三条算术与报告 31 §3.3 的预测**逐位吻合** ⇒ 本次运行跑的是 v65 驱动（wrapper 已不再重写 `SB_MASK_STREAM`），而**失败签名完全没变**：仍然精确停在 CALL 上。**v65 ✗ 无效，得到确认**（报告 31 的「廉价验证标记」生效）。

### 1.2 数值（PID 27047，唯一一次运行）

| 项 | 数值 |
|---|---|
| 驱动加载（`W/MESA No gralloc hwmodule`） | 14:47:55.645 |
| 交换链 / 帧流 | `vkshim SurfaceCaps: min=2 max=4 cur=2376x1080` ×208，首帧 14:47:55.648，末真帧 14:48:06.231（≈10.6 s，≈19.6 fps） |
| `tiler heap renewal` | **10 次**（14:48:01.962 … 14:48:06.039），每次 `uAPI 1.21, submits 32, renew interval 32` ⇒ `PANVK_KBASE_HEAP_RENEW_INTERVAL=32` 生效 ✓ |
| 等待起点（超时 − 10 s） | ≈14:48:06.24（= 末次续期之后） |
| **看门狗超时** | **14:48:16.237**（`E/MESA`），存活 = 加载→超时 **20.59 s** |
| 挂起原文（subqueue 0） | `seqno 332, ls_copy 0, target 333, marks pre/post-call/post-wait 0x0/0x0/0x0, stream progress 0x0, insert 63936, extract 63848, active 0, error 0x0, jobs 333` |
| ring[0..3] | `0x1c00000000000003 / 0x1c00000900000008 / 0x1c00000100000000 / 0x1c00000200000001`（4× SET_STATE，与 v64 相同） |
| last job @63744 | entry **152/192 B**，stream `0x5fdef70000`/4488，flush 391，extract offset **63744+104** |
| subqueue 1 | extract 63848（+104），stream `0x5ff9fcb000`/4640，entry 152/192 |
| subqueue 2（COMPUTE） | extract 63832（**+88**），stream `0x5ff9ef0000`/2320，entry 136/192 |
| queue syncobj 0/1/2 | seqno 4959 / 9928 / 5371，error 0x0（三个都**没有**推进到目标） |
| DEVICE_LOST | 14:48:16.238 `vkQueuePresentKHR`（VulkanRenderer.cpp:13001）、14:48:16.245 `vkAcquireNextImageKHR`（:13067）、14:48:16.256 `vkWaitForFences -4` |
| 其它 | 无 CS fault、无 `tiler heap OOM`、`active 0`、`error 0x0`、`Access denied finding property "vendor.mesa.vk.abort.on.device.loss"` |

> 与 v64（报告 31：33.7 s / 31.6 s，503 帧 ≈29 fps）相比，v65 这次只有 20.6 s / 208 帧 ≈19.6 fps。**差异存在但不构成结论**（场景与调度随机性未控），本轮不据此判 v65 有害。

### 1.3 为什么 marks / progress 全是 0x0（本轮之前无法判定入流的原因）

* v64/v65 的 pojavEnv 是 `PANVK_DEBUG=1`；本树 `parse_debug_string()`（`src/util/u_debug.c:449-472`）按 `", \n"` 切分并**要求 token 精确等于选项名**，`"1"` 不匹配任何项 ⇒ **v64/v65 的 `PANVK_DEBUG=1` 实际什么都没打开**（`panvk_debug == 0`）。
* 于是 wrapper 的 `marks` 与 PanVK 的 `PANVK_KBASE_PROGRESS_*` 位图**根本没有被发射**，快照只能打印 0 ⇒ 「固件有没有进入被 CALL 的流」在此之前无法从用户态读数，只能推断。

---

## 2. 判据链：用户态到底能读到什么（先查实，再动手）

被 CALL 的流是否被进入，本质是「固件 CS 的状态」。可观测面盘点（全部查实于本树）：

| 想读的东西 | 能否读到 | 依据 |
|---|---|---|
| CS 取指进度 `CS_EXTRACT` / `CS_ACTIVE` | ✔ 已读 | `include/drm-uapi/mali_kbase_ioctl.h:398-401`（user_io page1/page2）＋ `mali_kbase_csf_registers.h` 的 `CS_USER_OUTPUT_BLOCK`：`CS_EXTRACT_LO 0x0000 / HI 0x0004 / CS_ACTIVE 0x0008` |
| `CS_STATUS*` / `CS_ACK` / `CS_FAULT` / `CS_HEAP_*` | ⚠ **只能读页2 的“镜像”**：`CS_KERNEL_OUTPUT_BLOCK_BASE = 0x0000`（`mali_kbase_csf_registers.h:40-42`），所以字段偏移就是 `CS_STATUS_CMD_PTR_LO 0x0040`、`CS_STATUS_WAIT 0x0048`、`CS_STATUS_REQ_RESOURCE 0x004C`、`CS_STATUS_SCOREBOARDS 0x005C`、`CS_STATUS_BLOCKED_REASON 0x0060`、`CS_FAULT 0x0080`、`CS_HEAP_VT_START 0x00C0`…；**uAPI 只承诺 0x00/0x08 两个字段**，其余是否被固件镜像进页2 **没有文档保证**（本树既有代码 `panvk_vX_gpu_queue.c:1128-1130` 已经这么读了，但那是假设，不是事实） |
| 内核 mmio 读 CS 寄存器块 | ✗ 无 root、无 debugfs | 本树 kbase 后端使用的 ioctl 全集里没有任何 CS 寄存器读（`KBASE_IOCTL_CS_QUEUE_*`/`TILER_HEAP_*`/`KCPU_*`…）；`KBASE_IOCTL_CS_CPU_QUEUE_DUMP`（nr 53）在 uAPI 里存在但后端未实现，且需要固件侧触发 dump 程序，**无法在无设备验证的情况下安全启用**（本轮不动） |
| 固件**自己**写进内存的面包屑 | ✔ **最可靠** | (a) wrapper 的 `marks`（KBASE_DIAG）；(b) **被调用流内部**的 `PANVK_KBASE_PROGRESS_*` 位图（`panvk_vX_cmd_buffer.c:44-82`，用 SYNC_ADD32 累加，wrapper 在每次 CALL 前把该字清零）；(c) CS 写进 subqueue ctx 的计数器（tiling/OOM 路径） |

⇒ 因此本轮用**两条互相独立的通道**回答同一个问题：
1. **无假设通道**：`PANVK_KBASE_PROGRESS_CMDBUF_START`（bit0）作为**被调用流的第一条指令**——只有固件真的把控制权转进 callee 才可能置位；
2. **寄存器通道**：页2 镜像里的 `CS_STATUS_CMD_PTR`（固件当前 PC）等——并对它做**自证伪分类**（PC 是否落在已知的 ring entry / callee VA 区间内），一次运行就能确认或否证镜像假设。

---

## 3. 改动（两份文件，5 处；全部诊断，行为零改）

### 3.1 `src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c`（初始化主命令流时发一条面包屑）

* **:859-878**（`init_cs_builders()` 内，紧接 `cs_builder_init(b, &conf, root_cs);`）新增：

```c
      if (cmdbuf->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
         panvk_per_arch(kbase_mark_progress)(
            cmdbuf, i, PANVK_KBASE_PROGRESS_CMDBUF_START);
```

* 为什么安全：
  * `kbase_mark_progress()` 在 `PANVK_DEBUG(KBASE_DIAG)` 关闭时**第一行就 return**（`panvk_vX_cmd_buffer.c:53`）⇒ 对照构建行为不变；
  * **只对 PRIMARY**：secondary 的流会被 `cs_call()` 内联进 primary（`panvk_vX_cmd_buffer.c:1157`），标记 secondary 会改变「谁被内联」，所以不标；
  * **不改变提交集合**：`finish_cs()` 对每个子队列**无条件**发 `WAIT/FLUSH_CACHE/ERROR_BARRIER`（`panvk_vX_cmd_buffer.c:302-312`），`cs_is_empty()` 在提交时**本来就是 false**，所以「加一条头指令」不会让原本被跳过的子队列变成被提交；
  * 该位置是 `cs_root_chunk_gpu_addr()` 的第一条指令，而 wrapper 的 CALL 目标正是它（`panvk_vX_gpu_queue.c:2896` 用 `cs_root_chunk_gpu_addr(b)` 作 `stream_addr`）。
* `init_cs_builders()` 两个调用点（`panvk_vX_cmd_buffer.c:919` reset、`:1010` create）都在 `vk_command_buffer_init()` 之后，`cmdbuf->vk.level` 已就绪。
* 以上行号均为 **v66 树内**行号（插入代码之后 v65 的行号会相应下移）。

### 3.2 `src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c`（timeout 快照升级）

| 处 | 行号 | 内容 |
|---|---|---|
| B | **:326-676** | 新增寄存器偏移表 + 4 个解码/打印函数：`kbase_blocked_reason_name()`、`kbase_progress_bit_name()`、`kbase_classify_pc()`、`kbase_log_callee_progress()`、`kbase_log_callee_diag()` |
| C | **:677-678** | `kbase_log_subqueue_state()` 内调用 `kbase_log_callee_diag(...)` ⇒ timeout 快照对 3 个子队列各打一组读数 |
| D | **:1086** | `int64_t last_diag_sample = start;` |
| E | **:1163-1172** | 等待循环内每 2 s 采样一次固件面包屑（纯读；健康提交微秒级完成，只在已经卡住时才触发） |

打印内容（每条都带 `DIAG` 前缀，便于 `grep 'DIAG'`）：

```
kbase: DIAG timeout snapshot subqueue N marks: pre_call 0x… post_call 0x… post_wait 0x… ls_copy … (结论串)
kbase: DIAG … callee progress bit B (NAME) SET          ← 每个置位一行
kbase: DIAG … => firmware EXECUTED instructions inside the called stream…
kbase: DIAG … callee ctx: last_error … desc_ringbuf.ptr … td_count/layer_count … tiler_oom.counter … cond_render_flag … kbase_progress_addr …
kbase: DIAG … CS_STATUS_CMD_PTR 0x… (+Δ B into INSIDE the last ring entry / INSIDE the called stream / outside…)
kbase: DIAG … CS_STATUS_WAIT 0x… (SB_MASK 0x…, SB_SOURCE …, SYNC_WAIT …, SYNC_COND …, PROGRESS_WAIT …, PROTM_PEND …) BLOCKED_REASON 0x… (NAME) SCOREBOARDS … REQ_RESOURCE …
kbase: DIAG … wait_sync_ptr … wait_sync_value … CS_FAULT … CS_FATAL … FAULT_INFO …
kbase: DIAG … CS_HEAP vt_start … vt_end … frag_end … heap_addr …
kbase: DIAG … raw page2 qwords @0x40 … @0x48 … @0x50 … @0x58 … @0x60 …   ← 可自证伪
```

寄存器名/偏移全部取自本树 `src/panfrost/lib/kmod/mali_kbase_csf_registers.h`（该文件 `CS_KERNEL_OUTPUT_BLOCK_BASE = 0x0000`、`CS_USER_OUTPUT_BLOCK_BASE = 0x0000`），**没有凭记忆编造**；README 式注释就写在代码里（`:326-355`）。

### 3.3 明确**不做**的三件事（都在报告里留痕，避免下一轮重复踩）

1. **不动 wrapper 的发射序列**（除诊断块外）：v65 已证伪 SB_MASK_STREAM 假设，本轮不加猜测性改动。
2. **不做 trampoline/probe CALL**：会改热路径且引入额外 CALL/RET。
3. **不实现 `KBASE_IOCTL_CS_CPU_QUEUE_DUMP`**：需要未文档化的 dump 程序编码 + 设备实测，属猜测性改行为。

---

## 4. 编译与确定性对照（全部 exit 0）

```
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja
```

| 步骤 | ninja exit | 产物 sha256 / 字节 | 结论 |
|---|---|---|---|
| ① v66（改动后） | **0** | `24bd7ccdca0e0402bd06d0674a6af5fd406f534650f119fde8a220091f0bbfe4` / 20 018 480 B | 新驱动 |
| ② **撤改动**（`.bak-v66-1791183743` 覆盖回源）重编 | **0** | **`b9952f750c6e452c71bb1b7b368f7c4909aff9f91717628cb89031c7b9d1eeb0` / 20 006 016 B** | **逐位等于 v65 ✓✓** |
| ③ 再应用改动重编 | **0** | `24bd7ccd…` / 20 018 480 B | 与 ① 逐位相等 ✓（可复现） |

* 源文件 sha256：`panvk_vX_gpu_queue.c` = `d7a59a2f6b8c2128eeb41dca3bd638c29d3f028de8497a7f014caf7a435d536d`（138 986 B，v65 为 7aa4414d…/123 748 B）、`panvk_vX_cmd_buffer.c` = `e092f7b64ec6c83fefabc1d4b459718524468660a63d017eaa67c9d4aa51275f`（50 380 B）。
* 唯一告警：`panvk_vX_gpu_queue.c:1146: warning: label followed by a declaration is a C23 extension`（既有 `kbase_wait_continue:` 标签，**非本轮引入**；行号随插入代码下移）。
* 日志：`/root/research/32-work/build-v66.log`、`build-control-v65.log`、`build-v66-reapply.log`。

---

## 5. APK `/root/final/mgl-panvk-v66.apk`

* versionCode **66**，versionName `6.6-kbase-diag-regs`，包名 `com.dsh.plugin.driver.g720`（同 v64）
* APK：**10 195 503 B**，sha256 **`3e02218b5bcb6f2a3f65bfcf0b62ab21563b7dcaa25bf6ef663badd02264bf0f`**
* 打包脚本 `/root/pack_v66.sh`，日志 `/root/research/32-work/pack-v66.log`

| 载荷 | APK 内字节 | APK 内 sha256 == 源 |
|---|---|---|
| `lib/arm64-v8a/libvulkan_freedreno.so` | 20 018 480 | `24bd7ccdca0e0402bd06d0674a6af5fd406f534650f119fde8a220091f0bbfe4` == 构建产物 ✓ **非空** ✓ |
| `lib/arm64-v8a/libMobileGL.so` | 16 956 584 | `72919c73a7e07630f329bb4ad9605c606a9aac9e2fc6596683ee7b9f9c76848b` == `/root/v54/…` ✓（v64 载荷） |
| `classes.dex` | 1 328 | `6bd3abde2c53506f1b54bb88a8069c3cc394c2fb08440e4ca475cbf8fd64f4ad` == `/root/v54/classes.dex` ✓ |

* `aapt2 dump xmltree` 与 v64 对比 ⇒ **只有 4 行不同**：`versionCode 64→66`、`versionName`，以及 pojavEnv/boatEnv 两行各多 `,kbase_diag`（其余逐字符一致，含 `line=` 行号）。
* **诊断 env（本轮唯一 env 变更）**：`PANVK_DEBUG=1` → `PANVK_DEBUG=1,kbase_diag`（pojavEnv 与 boatEnv 同步；`PANVK_KBASE_HEAP_RENEW_INTERVAL=32` **保留** ✓，`MESA_VK_WSI_HEADLESS_SWAPCHAIN` 计数 0 ✓）。

---

## 6. 回滚命令

```bash
# 回退到 v65 源码并重编（唯一需要撤的就是本轮 5 处诊断改动）
cd /root/zenithblue/work/mesa
cp -f src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c.bak-v66-1791183743 \
      src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
cp -f src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c.bak-v66-1791183743 \
      src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja     # 期望 b9952f75…，20 006 016 B（已实测 ✓）

# 或整包回退（无需重编）
#   /root/final/mgl-panvk-v65.apk  （驱动 b9952f75…）
```

> 注意：磁盘上现在**已经是 v66 源码状态**（第 ③ 步重编产物 = `24bd7ccd…`）。若要留在 v65，请执行上面的 `cp` + `ninja`。

---

## 7. ★ 判读表：v66 一次运行该怎么读（`grep -E 'DIAG|timeout on subqueue' cap.txt`）

### 7.1 通道 1 —— 被调用流是否被进入（**无假设，最硬**）

`stream progress` 这一字由 wrapper 在**每次 CALL 前清零**（`cs_move64_to(val32,0)+cs_store32`，`panvk_vX_gpu_queue.c:900-908`，CALL 在 `:924`），此后由**被调用流内部的指令**用 SYNC_ADD32 累加（`panvk_vX_cmd_buffer.c:78-81`）。

| 读数 | 判定 |
|---|---|
| **bit0 `CMDBUF_START` = 1** | **固件确实把控制权转进了被 CALL 的流**（本轮新增的就是这条头指令）⇒ 「H1：卡在流内部」。再按位定位深度 |
| `COMPUTE_ENTER`(bit1) 及其后位 | compute 子队列：dispatch 路径已执行到该阶段 |
| `FINISH_BEFORE_WAIT`(28)/`AFTER_WAIT`(29)/`CMDBUF_DONE`(30) | 流已走到尾部（收尾/等槽/完成阶段） |
| **progress == 0 且 `pre_call` 有、`post_call` 无** | 固件执行到了 CALL 之前（含清零），但**没有被调用流的任何一条写内存指令退役** ⇒ 「卡在 CALL 上」（或 CALL 转移后、在其第一条写指令之前就阻塞）。与 7.2 合判可完全分开这两种 |
| progress == 0 且 **marks 全 0** | 诊断没生效（env 没传进 / 非 kbase 路径）——不是结论，先查 env |

### 7.2 通道 2 —— wrapper marks 阶梯（KBASE_DIAG）

| marks | 判定 |
|---|---|
| `pre_call` 置位、`post_call` 0 | 已退役到 CALL 之前的所有指令（含清零/等待），**CALL 没有返回** |
| `pre_call`+`post_call` 置位、`post_wait` 0 | CALL 返回了，卡在 CALL 之后的全槽等待（`cs_wait_slots(all_mask)`） |
| 三个都置位、`ls_copy` 0 | 完成复制没写 ⇒ 卡在最终 `SYNC_ADD64` 之前 |
| `ls_copy == target` | ring entry 已完成（问题在完成通知侧，不是取指侧） |

### 7.3 通道 3 —— `CS_STATUS_CMD_PTR`（固件 PC）分类：**同时用来证伪镜像假设**

| 读数 | 判定 |
|---|---|
| PC ∈ `[ringbuf_dev+last_job_offset, +last_job_size)` | 固件 PC 在**最后一条 ring entry 内**（Δ 会打出）：Δ≈CALL 偏移 ⇒ 卡在 CALL 指令本体；Δ 更小 ⇒ 卡在更早的指令（与 extract 对照即可） |
| PC ∈ `[last_stream_addr, +last_stream_size)` | **固件在执行被 CALL 的流**（Δ 给出发内偏移）⇒ 与 bit0 互相印证，H1 铁证 |
| PC == 0 或落在两个区间之外（含 0xdeadbeef 类垃圾） | **页2 不镜像 `CS_KERNEL_OUTPUT_BLOCK`**：镜像假设被否证（也解释了为什么既有 `CS_FAULT@0x80` 一直读到 0）⇒ 只采用通道 1/2，后续不要再依赖 0x40/0x48/0x60 |

### 7.4 通道 4 —— `CS_STATUS_WAIT` / `BLOCKED_REASON` / `SCOREBOARDS` / `CS_HEAP_*`（仅当 7.3 自证成立时使用）

| 读数 | 判定 |
|---|---|
| `SB_MASK != 0` | 该 CS 正在等这些 scoreboard 槽没到（低 16 位 = 槽位图）；`SB_SOURCE` 指示来源 |
| `SYNC_WAIT=1` + `wait_sync_ptr/value` | 卡在 syncobj 等待：把 `wait_sync_ptr` 与 queue syncobj / semaphore 的 GPU VA 对照 |
| `PROGRESS_WAIT=1` / `PROTM_PEND=1` | 等进度（老架构专用）/ 等保护模式挂起 |
| `BLOCKED_REASON` = `FLUSH`(6) | 卡在 flush 未完成（与 `FLUSH_CACHE2`+`WAIT(IMM_FLUSH)` 有关） |
| = `WAIT`(1) / `SYNC_WAIT`(3) / `RESOURCE`(5) / `DEFERRED`(4) / `UNBLOCKED`(0) | 分别：等槽 / 等 syncobj / 等资源（heap 等）/ 等 deferred 操作 / 没阻塞（则在跑） |
| `SCOREBOARDS != 0` | 仍有未取走的 scoreboard 信号（配合 SB_MASK 看是谁） |
| `CS_HEAP vt_start` 与 `vt_end` 差距大且不收敛 | 顶点/分块作业没有被消费 ⇒ tiler heap 侧卡住（与 P2/C2 相关） |
| `CS_HEAP_ADDRESS` 与 `queue->tiler_heap.context.dev_addr` 不符 | 固件看到的 heap 上下文与实际提交的不一致（世代切换残留） |

### 7.5 通道 5 —— 2 s 采样（区分「冻结」与「在动」）

| 采样序列 | 判定 |
|---|---|
| extract / seqno / ls_copy / marks **一字节不动**（10 s 内） | CS **冻结**（不是慢）：固件在这条 entry 上彻底停住 |
| extract 前进、seqno 不前进 | 固件在取指/执行但完成写没发生 |
| marks/progress 前进 | 固件在动，问题在完成/等待判定侧 |

### 7.6 对「当前签名」的预期判定

现场 3 个子队列都停在 CALL（VT/FRAG +104、COMPUTE +88），且 `active 0 / error 0 /` 无 fault。v66 一次运行后：

* 若 VT 的 **bit0 = 1**（大概率若问题真在流内）⇒ 直接锁定「CALL 成功、卡在被调用流内部」，并可用 7.3 的 Δ、7.4 的 `SB_MASK`/`BLOCKED_REASON` 继续定位到**具体等待对象**（这是 v54 以来第一次能拿到「在等哪个槽」的硬读数）。
* 若 VT 的 **bit0 = 0 而 marks 的 `pre_call` 已置位** ⇒ CALL 没有完成转移 ⇒ 嫌疑收敛到 `CALL` 指令/CS 调用栈/流切换状态机本身（而不是流内部逻辑）。
* 若 VT 的 bit0 = 0 且 **compute 的 bit1（COMPUTE_ENTER）= 1** ⇒ 同一套 wrapper 对 compute 是能入流的 ⇒ 差异在**哪条流/哪个子队列**，可用对照收敛（这是「一次运行」能拿到的额外信息）。

### 7.7 必须知道的副作用（开 kbase_diag 之后现场会长得不一样）

* **ring entry 变长**：wrapper 诊断块 = pre-CALL 8 条 + post-CALL 5 条 + post-wait 7 条 = **+20 条 = +160 B**（`cs_move64_to` 对 ≥2^48 的立即数用 2 条，见 `src/panfrost/genxml/cs_builder.h:1481-1490`）。
  ⇒ VT/FRAG entry ≈ **312 B**、COMPUTE ≈ **296 B**（64 B 对齐后 **320 B**）；`KBASE_RING_JOB_MAX_SIZE = 512`（`:61`）⇒ **不溢出** ✓，但 64 KB ring 由 ~341 job/圈 变成 ~204 job/圈，`insert` 步进不再恒为 192。
  ⇒ **不要再用「extract − last_job_offset == +104/+112」当判据**；改用 marks/bit0（同一行 `last ring[0..15]` 仍会把 CALL 位置原样打出来，可人工复核）。
* **热路径变慢/扰动**：每条 entry 多 20 条指令 + 2 次 LS 等待；流内 marker 每个 dispatch/draw 段各多一次 SYNC_ADD32 + flush。诊断轮可接受，但**不要把 v66 当作性能基线**，也不要用它的存活秒数与 v64/v65 直接比。
* **行为不变的部分**：提交集合、CALL 目标、内存布局、同步契约、`SB_MASK_STREAM`（v65 状态）、C2 与 P2 全部保留；对照实验已逐位证明「关掉 diag 时编译产物 = v65」。

---

## 8. 下一轮建议（一次运行即可收网）

1. 安装 `/root/final/mgl-panvk-v66.apk`（versionCode 66），跑一次 Minecraft 到挂起，抓 `cap.txt`。
2. `grep -E 'DIAG|timeout on subqueue|DEVICE_LOST' cap.txt`：先看 **bit0**（入流与否），再看 **CS_STATUS_CMD_PTR** 的 Δ（在 ring 内还是流内）与 **SB_MASK / BLOCKED_REASON**（在等谁）。
3. 按 §7.3 判定镜像假设真伪：
   * 真 ⇒ 后续每轮都白拿「固件 PC + 等待对象」；
   * 假 ⇒ 删掉 0x40/0x48/0x60 读数（保留 raw dump 一行做回归），只依赖 bit0/marks。
4. 如果 bit0 = 1：用 7.4 的 `SB_MASK` 定位到**具体 scoreboard 槽**，与 PanVK 的槽分配（`dev->csf.sb.all_iters_mask` / `SB_WAIT_ITER`）对照，直接指向下一个「契约不符」点。
5. 如果 bit0 = 0：把注意力放到 CALL 本身（CS 调用栈/流切换），可用「临时把 CALL 目标换成一个 2 条指令的探针流」做一次 A/B（属行为改动，需单独一轮授权）。
