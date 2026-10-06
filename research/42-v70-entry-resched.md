# v70 — ENTRY 原始字节 dump + 超时瞬间一次 RESCHED（诊断级，无行为修复）

- 任务：v70 = v69 树 + 两组**诊断**改动。不做行为修复；不动修复 A、C1/C2/B、超时常量、
  wrapper 发射序列、任何 `SB_*` 写入、MobileGL。
- 唯一改动文件：`/root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c`
  （其余源文件 mtime 均为本次会话之前；见 §5 的逐位对照证明）
- 改动规模：`diff` = **+209 / −1** 行（那 1 行是 `return vk_queue_set_lost(...)` 因块插入而被重排的上下文行，
  语义未变）
- v70 源文件 sha256：`4dc5e3a7dd365d3545e6526ba2c16a8590fc03968de8779a80df805857c66686`
  （冻结副本 `/root/v70-final-panvk_vX_gpu_queue.c`，同一哈希）

---

## 1. 打点位置（file:line，v70 最终行号）

文件 = `src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c`

| 行 | 内容 |
|---|---|
| 321–421 | v70 新增注释块 + 两个静态助手 |
| **357** | `kbase_cs_opcode_is_wait()` — 等待类/普通类判定 |
| **377** | `kbase_log_entry_bytes()` — 16 字节原始 dump + 首指令解码 |
| **402** | `kbase: ENTRY … 16 raw bytes:` 十六进制 dump 日志 |
| **414** | `kbase: ENTRY … first instr … opcode … wait_mask … => 类` 日志 |
| **1399** | timeout 块内 v70 段起点 |
| **1414** | ENTRY dump：`extract` 指针处 |
| **1418** | ENTRY dump：`last_job_offset`（entry 头）处（与 extract 不同才打） |
| **1421** | `kbase: ENTRY … geometry:` —— last_job_offset / entry 尺寸 / extract 在 entry 内的偏移 / insert / active |
| **1440–1501** | RESCHED 块 |
| **1459** | `kick_ret = kbase_kmod_csf_queue_kick(dev->kmod.dev, subq->kbase.ringbuf_dev);` ← **唯一一次** |
| **1469** | `os_time_sleep(200 * 1000);`（延迟 200 ms 再读） |
| **1477** | `kbase: RESCHED …` 统一前缀日志（pre / ioctl us + rc / post-immediate / +200 ms / insert / target） |
| **1491** | 结论字符串分支（kick 失败 / extract 前进 / 不动） |
| 1503 | 既有 `return vk_queue_set_lost(... "kbase: timeout on subqueue %u")` —— v70 段插在它**之前、既有全部快照日志之后** |

参考点：既有 publish kick 在 **1175**；`ringbuf_cpu` 映射在 **1636**。

**位置选择的理由**：v70 段放在该 timeout 分支的**末尾**（既有快照/`kbase_log_subqueue_state`
与 pandecode 全部打印之后）。因此 v69 既有的证据行在 v70 中**内容与顺序完全不变**，v69↔v70 日志可直接对读；
RESCHED 的 kick 与 200 ms 等待也不会污染快照里读到的 extract。

---

## 2. CPU 映射来源与理由（不猜）

`grep -n "ringbuf_cpu" panvk_vX_gpu_queue.c`：

- **1636**：`subq->kbase.ringbuf_cpu = pan_kmod_bo_mmap(dev->kmod.dev, subq->kbase.ringbuf_bo);`
  —— `kbase_init_subqueue()` 里唯一赋值点，映射的正是这个子队列的 ring BO（1465 附近还把它登记为
  `"kbase_ringbuf"`）。
- 该指针本就是本文件读写 ring 的那一个：`kbase_subqueue_emit_job()` 用它写每一条 entry
  （`memset`/cache clean/`cs_buffer.cpu`），`kbase_ring_qword()`（229）也已经在既有的 timeout 快照里
  用它读 ring 字节。

所以 ENTRY 用的是**已有的、CPU 可读的 ring 映射**：没有新 mmap、没有新 ioctl、没有靠猜的指针。

**一致性处理**：ring 只由 CPU 写，entry 在 kick 前经 `kbase_cache_clean_range()`
（`kbase_subqueue_emit_job()`）清洗；故 CPU 直接 load 这些字节是自洽的。因此代码只做两次
volatile 64-bit load，**故意不做 invalidate**（对 CPU-dirty 行做 invalidate 会把 CPU 数据丢掉）。
GPU 不写 ring，故不存在需要 invalidate 的情形。

---

## 3. 解码规则与判据

- **opcode = 首 qword 的最高字节 bits[63:56]**：`src/panfrost/genxml/v12.xml` 里每个
  `CS <INSTR>` 结构都带 `<field name="Opcode" size="8" start="56" type="CS Opcode"/>`；
  这正是本树既有的 `kbase_stream_opcode()`（239：`qword >> 56`）用的规则。
  名称取自**生成的、按架构编译的** `mali_cs_opcode_as_str()`
  （`build/android-v4/src/panfrost/genxml/v12_pack.h:1329`）——不自己维护名字表。
- **wait mask = bits[31:16]**：`v12.xml` 的 `CS WAIT` / `CS SYNC_WAIT32` / `CS SYNC_WAIT64` /
  `CS SET_EXCEPTION_HANDLER` 等均为 `<field name="Wait Mask" size="16" start="16"/>`。
- **架构确认**：G720 = `PAN_PROD_ID(12,8,0)`（`src/panfrost/model/pan_model.c:111`
  `FIFTHGEN_MODEL(PAN_PROD_ID(12,8,0), 4, "G720", …)`）→ 实际编入的是 v12 pack 头；
  同一份 `panvk_vX_gpu_queue.c` 会为 v10..v14 各编一次，而 `mali_cs_opcode_as_str()` 由
  `gen_macros.h` 只包含匹配架构的那一个 pack 头（`gen_macros.h:29-70`），故名字永远与当前架构一致。
  助手里的 `PROGRESS_WAIT` 分支按各架构实测可用性用 `#if PAN_ARCH <= 13` 包住
  （v14 的 opcode 表已无 `PROGRESS_WAIT`，若不加会在编译该架构时报未定义）。

**ENTRY 判据（审计给的判据）**：
`WAIT(3)` / `SYNC_WAIT32(39)` / `SYNC_WAIT64(53)` / `PROGRESS_WAIT(24)`，**或** wait_mask ≠ 0
⇒ `WAIT-CLASS`（被取指到的这条指令自身就在等 scoreboard）；否则 `NORMAL-CLASS`
（执行/拷贝/同步类，不可能是一条“停住不走的等待指令”）。

---

## 4. 交付物与哈希

### 4.1 驱动 `.so`
`/root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so`
（冻结副本 `/root/v70-libvulkan_panfrost.so`，同一哈希）

```
sha256 6610c2a78791a54c360e58d5893a7e74d60837239b53faadbb6e0af7d2db7003
size   20 031 368 B
```
`strings` 自证：`kbase: ENTRY subqueue` ×3、`kbase: RESCHED subqueue` ×1、
`| kick ioctl %ld us rc %d |` ×1。

### 4.2 APK
`/root/final/mgl-panvk-v70.apk`
```
sha256 f9af2fed1dd68bed1f8a44a5b8e275c8b1a8b096bd75dea72883259b47a030b9
size   10 203 695 B
```
由 `/root/pack_v69.sh` 按任务给定形式派生（`sed -e 's/v69/v70/g'
-e 's/versionCode="69"/versionCode="70"/g' -e 's/6\.9-slowcall-diag/6.10-entry-resched/g'`
→ `/root/pack_v70.sh`），源 `.so` 用默认路径即 v70 的 `.so`。

### 4.3 载荷校验（独立复核，不依赖打包脚本自述）

| 项 | 值 | 判定 |
|---|---|---|
| `unzip -p … lib/arm64-v8a/libvulkan_freedreno.so \| sha256sum` | `6610c2a78791a54c360e58d5893a7e74d60837239b53faadbb6e0af7d2db7003` | == 新 `.so`，非空 ✓ |
| 该载荷字节数 | 20 031 368 | 与 `.so` 一致 ✓ |
| `libMobileGL.so` | `72919c73a7e07630f329bb4ad9605c606a9aac9e2fc6596683ee7b9f9c76848b` / 16 956 584 B | == `/root/v54` 载荷 ✓ |
| `classes.dex` | `6bd3abde2c53506f1b54bb88a8069c3cc394c2fb08440e4ca475cbf8fd64f4ad` / 1 328 B | == `/root/v54` ✓ |
| `aapt2 dump badging` | `versionCode='70' versionName='6.10-entry-resched'`，minSdk 26 / targetSdk 34 | ✓ |
| v70 manifest vs **v69** manifest（`aapt2 dump xmltree` 全量 diff） | 只差 `versionCode` 与 `versionName` 两行 | **env 串与 v69 逐字符一致** ✓ |
| env 自检 | `PANVK_KBASE_HEAP_RENEW_INTERVAL`×2、`kbase_diag`×2、`MESA_VK_WSI_HEADLESS_SWAPCHAIN`×0 | ✓ |
| v69 APK | `174cafc5b4000d7eef3e1ae826dd97ee3676ed7895cbc8828a796c8d7f6c0286`，mtime 17:12 未变 | 未被改 ✓ |
| v68 APK | `29c793dff14705322bc0f7e9d1a7006041f1f295018e1ae8d207b1a39af62522`，mtime 16:15 未变 | 未被改 ✓ |

---

## 5. 确定性对照（本任务的核心证据）

- **v69 驱动基线**：`unzip -p /root/final/mgl-panvk-v69.apk lib/arm64-v8a/libvulkan_freedreno.so`
  = `f2e98dcb8a75c971c2bd697fa2421826d04195ca677f2bda0ed06d7b1d5cdac6`（与任务给出的
  `f2e98dcb8a75c971…` 一致），**20 022 000 B**。
- **A) 撤掉本轮改动重编**：`cp panvk_vX_gpu_queue.c.bak-v70-1791192708 panvk_vX_gpu_queue.c`
  → `ninja src/panfrost/vulkan/libvulkan_panfrost.so`（exit 0）
  → `sha256 f2e98dcb8a75c971c2bd697fa2421826d04195ca677f2bda0ed06d7b1d5cdac6`，20 022 000 B，
  `cmp`（与 v69 APK 载荷）**逐位相同 YES**。
- **B) 贴回**：`cp /root/v70-final-panvk_vX_gpu_queue.c panvk_vX_gpu_queue.c` → `ninja`（exit 0）
  → `6610c2a78791a54c360e58d5893a7e74d60837239b53faadbb6e0af7d2db7003`，20 031 368 B。
  **二次/三次重编得到同一哈希**（同一冻结源重编 2 次、且注释期一次重编均为同一 `.so`；注释改动后
  codegen 不变已由 `cmp` 证实），即**可复现**。
- 结论：本轮的全部二进制 delta 只能来自 `panvk_vX_gpu_queue.c` 的这 +209 行；
  A 方向逐位等于 v69 ⇒ 没有夹带任何其它改动。

---

## 6. 回滚命令

```bash
# 回滚到 v69（驱动侧，逐位等于 f2e98dcb…）
cd /root/zenithblue/work/mesa/src/panfrost/vulkan/csf
cp -f panvk_vX_gpu_queue.c.bak-v70-1791192708 panvk_vX_gpu_queue.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4
ninja src/panfrost/vulkan/libvulkan_panfrost.so     # => f2e98dcb… / 20 022 000 B

# 重新应用 v70（用冻结副本，不要重跑 /root/v70-patch.py：
# 该脚本按锚点插入，在已打补丁的树上会二次插入）
cp -f /root/v70-final-panvk_vX_gpu_queue.c \
      /root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
ninja src/panfrost/vulkan/libvulkan_panfrost.so     # => 6610c2a7… / 20 031 368 B

# APK：重新打包前必须让 /root/v70 无残留 base.apk（脚本自身会拒绝陈旧 zip entry）
mv /root/v70 /root/v70-attempt$(date +%s) 2>/dev/null || true
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
/root/pack_v70.sh                                    # => /root/final/mgl-panvk-v70.apk
# 完整源码 diff：/root/v70.diff（对 .bak-v70-1791192708）
```

本轮**未**使用 `git checkout/stash/reset`；未 `rm -rf` 任何既有目录（旧的 `/root/v70` 打包中间目录
用 `mv` 让位，保留为 `/root/v70-attempt1`）；改前已 `cp` 备份。

---

## 7. 判读表

### 7.1 `kbase: ENTRY` —— 被取指到的那条指令

先看 geometry 行：`extract` 在 entry 内的偏移（`+N B into the last entry`）。
- `N == 0`：firmware 连这条 entry 的第一条取指都没发出（最接近“从未被派发”）。
- `N > 0`：取指已进入 entry 内部（v69 的实例是 `extract 86912 / last_job_offset 21248`，
  `KBASE_RINGBUF_SIZE = 64 KiB` ⇒ `+128 B`，即 entry 的前 128 B 已被取过）。

再看首指令解码：

| ENTRY 首指令 | 含义 | 对审计假设的作用 |
|---|---|---|
| **WAIT-CLASS**（opcode ∈ {WAIT, SYNC_WAIT32, SYNC_WAIT64, PROGRESS_WAIT} 或 `wait_mask ≠ 0`） | 取指指针恰好停在一条**自身会阻塞**的指令上；`wait_mask` 的置位直接给出它在等哪些 scoreboard slot | **支持“取指后挂在指令上”**：至少证明取指推进到了该处，且停点是一条等待指令；把等待对象缩小到该 mask。此时“从未被派发”仍可能，但“派发过且卡在等待”成为最经济的解释 |
| **NORMAL-CLASS**（MOVE32/48、STORE/LOAD_MULTIPLE、REQ_RESOURCE、CALL、FLUSH_CACHE2、SYNC_ADD/SET、HEAP_OPERATION、ERROR_BARRIER…，且 `wait_mask == 0`） | 取指指针停在不阻塞的指令上 | **“取指后挂在这条指令上”不成立**；CS_EXTRACT 只是冻结在某处而 firmware 不再推进 ⇒ 明显偏向**调度层未继续服务该子队列**（doorbell 丢失 / group 挂起 / 优先级饿死） |
| 16 字节全 0（NOP 填充） | 取指落在了 entry 的 64 B 对齐填充上 | entry 已到尾/取指越界，按 entry 尺寸与 `last_job_size` 判读；同样不支持“等待类”解释 |

### 7.2 `kbase: RESCHED` —— 超时瞬间强制 kick 一次

| 观察 | 含义 |
|---|---|
| `rc != 0` | kick ioctl 本身失败 ⇒ **本次 RESCHED 无结论**（这也是必须打印 rc 的原因；`int kbase_kmod_csf_queue_kick(struct pan_kmod_dev *, uint64_t ringbuf_va)`，`src/panfrost/lib/kmod/kbase_kmod.h:54`） |
| `rc == 0` 且 **+200 ms extract（或 seqno）前进** | **调度层假死被证实**：一次 doorbell/kick 就足以让 firmware 继续推进 ⇒ 之前该队列确实没被调度服务（落回“doorbell 丢失/group 挂起/优先级饿死”这一族） |
| `rc == 0` 且 **+200 ms extract 与 seqno 都完全不动** | **调度层假死被削弱**：不是“一次 kick 就能救回来”的状态。结合 ENTRY：若 ENTRY 为 WAIT-CLASS ⇒ 更像 CS 停在 scoreboard 等待（等待的那个 signal 没人发）；若 ENTRY 为 NORMAL-CLASS 且 kick 无效 ⇒ 需要外部的 group/优先级视角才能定论（本工具链无 root，标**未验证**） |
| post-immediate 与 +200 ms 不同 | 说明推进发生在 200 ms 窗口内而非 ioctl 同步路径上（正常，kick 只是通知） |

---

## 8. 未验证 / 已知偏差（如实标注）

1. **未在设备上运行**：本任务明令禁止操作手机（不 adb / 不安装 / 不 input）。因此 ENTRY / RESCHED 的
   **实际输出未验证**，§7 的判读表是设计意图与判据约定，不是实测结论。
2. RESCHED 的 200 ms 窗口内若存在**其它并发提交**，extract 前进也可能来自它们；本模型为同步提交
   （`kbase_subqueue_wait_seqno` 阻塞等待），理论上无并发提交，但**未验证**。
3. `os_time_sleep(200 * 1000)` 会在超时路径上多停 200 ms（仅超时那一次，超时本已是 10 s 级），
   不改变正常路径行为；对整体卡死时长的影响**未验证**。
4. `pack_v70.sh` 自检里 `grep -c "PANVK_KBASE_PROGRESS_CMDBUF_START" panvk_vX_cmd_buffer.c` 打印 **2**
   而脚本注释写 “must be 1”：该文件在本次会话中**未被修改**（mtime 16:15:22，早于本会话；
   两处命中 = 905 注释 + 920 代码）。这是 v69 树自身的既有偏差 / 脚本陈旧注释，与 v70 无关。
5. 编译告警仅 1 类且为既有的 `-Wc23-extensions`（`int64_t now = os_time_get_nano();` 位于 label 之后，
   1281 行，v69 即存在），无新增告警；`ninja` exit 0。
