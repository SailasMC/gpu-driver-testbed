# 41 — v69 = 当前树（v68）+ 慢调用看门狗 + 诊断 D 无快照可读（编译 + 打包已完成）

**日期**：2026-10-05 17:07–17:14
**任务**：在 v68 树上加两组**纯诊断**改动（① `vkWaitForFences` / `vkAcquireNextImageKHR` ≥2 s 慢调用日志；
② 诊断 D 的 barrier wait mask 在没有 timeout 快照时也能读到），增量重编 ⇒ 打 v69 APK；
并做"撤改动 ⇒ 逐位等于 v68 驱动 `dcda738f…`"的确定性对照。
**结论**：✅ 全部完成。3 次构建 **exit 0**；**撤改动后逐位等于 v68 驱动**；
v69 `.so` 二次重编 **逐位可复现**；APK 载荷 = 新 `.so`（`cmp` 逐字节通过，非空）；
manifest 与 v68 APK **只差 versionCode/versionName**（env 串逐字符一致）。
**未操作手机**（无 adb / 无安装 / 无 input）。行为语义未改（见 §6）。

---

## 1. 交付物与指纹

| 产物 | 路径 | sha256 | 字节数 |
|---|---|---|---|
| **v69 新驱动 `.so`**（构建树当前态） | `/root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so` | `f2e98dcb8a75c971c2bd697fa2421826d04195ca677f2bda0ed06d7b1d5cdac6` | 20 022 000 |
| **v69 APK**（交付） | `/root/final/mgl-panvk-v69.apk` | `174cafc5b4000d7eef3e1ae826dd97ee3676ed7895cbc8828a796c8d7f6c0286` | 10 195 503 |
| v69 APK 内载荷 `lib/arm64-v8a/libvulkan_freedreno.so` | （APK 内） | `f2e98dcb…1d5cdac6` = 新 `.so` ✓ | 20 022 000（非空 ✓） |
| v69 首编基线 | `/root/v69-build1.so` | `f2e98dcb…1d5cdac6` | 20 022 000 |
| v69 二次重编 | `/root/v69-build2.so` | `f2e98dcb…1d5cdac6`（`cmp` = **BIT-IDENTICAL to build1**） | 20 022 000 |
| **对照 `.so`（撤改动后重编）** | `/root/v69-control-revert-equals-v68.so` | `dcda738feee42e91d61411ad240351ebf94ea876cc893d80ed56ead74ba013f0` = **v68 驱动** ✓ | 20 016 896 |
| 打包脚本 | `/root/pack_v69.sh` | `d5d1cbd1be0e61a45b349eb84bb66d5f7e168cb56fa43b4c7af774103015b315` | — |

* `pack_v69.sh` = `pack_v68.sh` 经
  `sed -e 's/v68/v69/g' -e 's/versionCode="68"/versionCode="69"/g' -e 's/6\.8-fixA-diagD/6.9-slowcall-diag/g'` 派生，
  末尾追加 v69 变更说明注释。
* **v68 / v67 APK 未被触碰**（打包前后同值同 mtime）：
  `mgl-panvk-v68.apk` sha256 `29c793df…af62522`、10 195 503 B、mtime 16:15；
  `mgl-panvk-v67.apk` sha256 `1cc838f5…18c822`、10 195 503 B、mtime 15:47。
* v69 源码 4 份的快照保存在 `/root/v69-edits/`（重贴用），回滚备份见 §5。

---

## 2. 打点位置（file:line，均为**当前树最终状态**）与选取理由

### 打点 A —— `vkWaitForFences`（含多 fence 形式）
**文件**：`/root/zenithblue/work/mesa/src/vulkan/runtime/vk_fence.c`
* 宏 `VK_WAITFENCES_SLOWCALL_MS 2000`（**:256**）/ `VK_WAITFENCES_SLOWCALL_RET(res)`（**:257–:277**）
* 计时起点 **:288**（`const int64_t vk_slowcall_t0 = os_time_get_nano();`，紧跟 `VK_FROM_HANDLE`）
* 改造的 4 个返回点：**:294**（DEVICE_LOST）、**:297**（fenceCount==0）、**:322**（device_status）、**:324**（正常返回）
* `#undef` **:326–:327**

**为什么是这个点**：先 grep 定位（不是猜）：`grep -rn 'WaitForFences' src/panfrost/` **零命中** ⇒ panvk 未覆盖该入口。
生成的分发表 `panvk_entrypoints.h:510` 只声明弱符号 `panvk_WaitForFences`，panvk 没有定义它，
因此实际实现就是公共 runtime 的 **`vk_common_WaitForFences`（本函数）**。
**"多 fence 形式"在本树不是另一个函数**：`vkWaitForFences` 只有这一个实现，`fenceCount > 1` 就是它自己，
所以单点即覆盖两种形式（`waitAll` / `fenceCount` / `timeout` 也一并打印，无需再找第二处）。
守卫 `vk_device_is_lost` / `fenceCount == 0` 的早返回也在计时范围内。

**打印内容**（仅在耗时 ≥ 2000 ms 时输出）：
```
kbase: SLOWCALL vkWaitForFences device 0x… fenceCount N waitAll W timeout 0x…LL -> R after T ms
kbase: SLOWCALL vkWaitForFences fence[i] 0x…            （最多前 8 个 fence 句柄）
```
**关于"该 fence 关联队列的最后已知 submit/seqno"**：在**本层取不到**，故按任务允许的降级只打耗时 + 句柄（+ 计数/超时/返回值）。
理由是确定性的、不是猜测：`struct vk_sync { const struct vk_sync_type *type; enum vk_sync_flags flags; }`
（`src/vulkan/runtime/vk_sync.h:325`）**不含任何驱动私有载荷**，而 `struct kbase_cpu_sync` 定义在
`panvk_physical_device.c` 内部、runtime 层无法 `container_of`，跨层包含 panvk 头会污染所有共用该 runtime 的驱动。
⇒ 该字段改由**打点 C**（驱动层、同一个 2 s 阈值）补上。

### 打点 B —— `vkAcquireNextImageKHR`（Android WSI 路径）
**文件**：`/root/zenithblue/work/mesa/src/vulkan/wsi/wsi_common.c`
* `#include "util/log.h"` 新增 **:31**
* 宏 `WSI_ACQUIRE_SLOWCALL_MS 2000` / `WSI_ACQUIRE_SLOWCALL_RET(res)` **:2280（注释）/ :2286、:2287（两个宏）**
* 计时起点 **:2309**
* 改造的 4 个返回点：**:2314**（acquire 失败/OUT_OF_DATE）、**:2325**（semaphore 信号失败）、**:2333**（fence 信号失败）、**:2339**（正常）
* `#undef` **:2341–:2342**

**为什么是这个点**：grep 定位出的 WSI 调用链（全部实测，非推测）：
`wsi_AcquireNextImageKHR`（**:2196–:2197**，仅组装 `VkAcquireNextImageInfoKHR`）
→ `device->dispatch_table.AcquireNextImage2KHR`（**:2215**，生成表把该槽绑到弱符号 `wsi_AcquireNextImage2KHR`，见构建目录
`src/vulkan/wsi/wsi_common_entrypoints.h:699/:895`）
→ `wsi_AcquireNextImage2KHR`（**:2345**，3 行转发）
→ **`wsi_common_acquire_next_image2`（**:2300–:2301**）** ⇒ acquire 的**全部实际工作**都在这里：
`swapchain->acquire_next_image()` 与随后的 semaphore/fence 信号。因此按要求在**实际实现处**打点
（而不是打在 3 行转发器上），单点覆盖 `vkAcquireNextImageKHR` 与 `vkAcquireNextImage2KHR` 两个入口。
Android 具体 swapchain 方法是 `panvk_android_swapchain_acquire_next_image`（`src/panfrost/vulkan/panvk_wsi.c:222`，
由 `:349` 绑定），它本身立即返回 `VK_SUCCESS`，所以真正的阻塞只可能出现在本函数覆盖的这段里。

**打印内容**（≥2000 ms）：
```
kbase: SLOWCALL vkAcquireNextImageKHR device 0x… swapchain 0x… timeout 0x…LL -> R after T ms
```

### 打点 C —— 驱动层 kbase sync 等待（补上 fence→seqno）
**文件**：`/root/zenithblue/work/mesa/src/panfrost/vulkan/panvk_physical_device.c`
* `#include "util/log.h"` 新增 **:27**
* 宏 `KBASE_SLOWCALL_MS 2000` / `KBASE_SLOWCALL_RET(res)` **:821–:859**
* `kbase_cpu_sync_wait_many()` 定义 **:860–:895**：计时起点 **:867**；改造 5 个返回点 **:876 / :878 / :887 / :889 / :892**；`#undef` **:896–:897**

**为什么加这一点**：`kbase_cpu_sync_wait_many` 是 `kbase_cpu_sync_type`（**:1099** `.wait_many = kbase_cpu_sync_wait_many`）
的 `wait_many` 实现，即 **fence/semaphore 载荷真正阻塞的地方**；`struct kbase_cpu_sync` 就定义在本文件，
`pending_data` / `targets[]`（`PANVK_KBASE_SYNC_TARGET_COUNT = 3`，见 `panvk_physical_device.h:108`）
正是队列提交时 `panvk_kbase_sync_set_pending(signal->sync, submit->queue, kbase_wait_sync_targets, submit->kbase_target_seqnos)`
（`csf/panvk_vX_gpu_queue.c:3205`）写进去的 **target seqno**。
⇒ 打点 A 打不出的"fence 关联 seqno"，由本点在**同一 2 s 阈值**下补出：
```
kbase: SLOWCALL kbase-sync wait device 0x… waits N wait_any W flags 0x… -> R after T ms
kbase: SLOWCALL   wait[i] sync 0x… state S targets A/B/C wait_fn 0x… data 0x…
```
（`data` 对队列 fence 就是 `struct panvk_gpu_queue *`，`wait_fn` = `kbase_wait_sync_targets`；两者都是裸指针，
用于和 timeout 快照/`PANVK_KBASE_PROGRESS` 日志对齐。**只读 POD，不加锁**——诊断不得扰动等待。）

### 打点 D —— 诊断 D（barrier wait mask）无需 timeout 快照即可读
**文件**：`/root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c`（`kbase_subqueue_wait_seqno`，定义 **:1076**）
* **(D.1) 等待返回时打印**：完成判定块 **:1136–:1151**（原 `break;` 改为带诊断的块；打印在 **:1146–:1149**）
  ```
  kbase: DIAG wait subqueue U returned after T ms, last barrier wait mask 0xMMMMMMMM
  ```
  仅当 `PANVK_DEBUG(KBASE_DIAG)` 且本次等待 **≥1 s** 时输出（见 §6 关于"无条件"的说明）。
* **(D.2) 周期采样携带 mask**：**:1194–:1207**。原 v66 周期采样阈值 **2000 ms → 1000 ms**，
  并在同一行追加 `last barrier wait mask 0xMMMMMMMM`：
  ```
  kbase: DIAG wait subqueue U t=T ms: extract … seqno X/Y … progress 0x… insert … last barrier wait mask 0xMMMMMMMM
  ```
* v68 在 timeout 快照里的那行 **:558 保持原样未动**（`kbase_log_callee_diag`）。

**为什么这样实现**：D 的 mask 是**GPU 侧** `panvk_per_arch(kbase_record_wait_mask)`（`csf/panvk_vX_cmd_buffer.c:601`、
STORE 到 seqno cell offset 52）写的，"barrier 等待返回"**没有主机侧事件**可挂钩；主机唯一能稳定观察到的
"等待返回"就是 `kbase_subqueue_wait_seqno` 的返回。所以：周期采样（每秒一次，含正在卡住的那次）
+ 等待返回一次，二者都输出 mask。**没有动 offset 52 的写入、没有动 `cs_cmd`/barrier 发射序列。**

---

## 3. 构建（3 次，全部 exit 0）

命令固定为：
```
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja src/panfrost/vulkan/libvulkan_panfrost.so
```

| # | 树状态 | ninja | 结果 `.so` sha256 | 字节数 | 时间 |
|---|---|---|---|---|---|
| 1 | v68 + A/B/C/D | **exit 0**（17/17） | `f2e98dcb…1d5cdac6` | 20 022 000 | 4.2 s |
| 2 | 撤掉 A/B/C/D（= v68 源码，md5 逐字节还原） | **exit 0** | **`dcda738feee42e91d61411ad240351ebf94ea876cc893d80ed56ead74ba013f0`** | **20 016 896** | — |
| 3 | 贴回 A/B/C/D | **exit 0** | `f2e98dcb…1d5cdac6`（`cmp` = **BIT-IDENTICAL to build 1**） | 20 022 000 | — |

### 3.1 确定性对照（硬要求，已满足）
* 撤改动方式：`cp -f <file>.bak-v69-1791191447 <file>`（4 个文件）。
* 撤后**源码 md5 逐字节回到改前值**：
  `vk_fence.c adf9c11ff107e5ad927c6b9e22c068c5`、`wsi_common.c d3e6530b56c7ac2219b5bc7f03b93438`、
  `panvk_physical_device.c 6d4ad3382653ed41d56e8c90407ba3a9`、`csf/panvk_vX_gpu_queue.c e95f4de556734b732db492da42384de7`
  （后者 = 报告 40 记录的 v68 值 ✓）。
* 重编后 **逐位等于 v68 驱动 `dcda738f…ba013f0`（20 016 896 B）** ⇒ **无残留** ✓
  （`git_sha1.h` 重生成未改变产物，build1/build3 一致已证）。
* 贴回后二次重编 **逐位等于首次构建** ⇒ v69 `.so` 可复现 ✓。

### 3.2 编译告警
v69 与对照构建**各 5 条、内容完全相同**，只有行号位移（`gpu_queue.c:1178` vs 对照 `:1165`）：
`warning: label followed by a declaration is a C23 extension`（`kbase_wait_continue:` 标签后接声明）——
**v67/v68 原树就有，非本次引入**。
`vk_fence.c` / `wsi_common.c` / `panvk_physical_device.c` **零告警**。

---

## 4. APK 校验（打包 exit 0，脚本打印 `PACK_DONE`）

```
unzip -p /root/final/mgl-panvk-v69.apk lib/arm64-v8a/libvulkan_freedreno.so | sha256sum
  → f2e98dcb8a75c971c2bd697fa2421826d04195ca677f2bda0ed06d7b1d5cdac6   （== 新 .so ✓）
  → 字节数 20 022 000（非空 ✓）；解包后与 /root/v69-build2.so `cmp` 逐字节相同 ✓
  → 载荷内 `kbase: SLOWCALL` 串计数 = 5 ✓
  → file: ELF 64-bit LSB shared object, ARM aarch64, for Android 35, NDK r27c
```
* `aapt2 dump badging` → `versionCode='69'  versionName='6.9-slowcall-diag'`，package `com.dsh.plugin.driver.g720` ✓
* `aapt2 dump xmltree` 与 **v68 APK** 的 dump（`/root/research/32-work/32-v68-manifest.txt`）
  `diff` **只有 versionCode/versionName 两行** ⇒ `pojavEnv` / `boatEnv` 两串与 v68 **逐字符一致** ✓
  （`PANVK_DEBUG=1,kbase_diag` 2 处、`PANVK_KBASE_HEAP_RENEW_INTERVAL=32` 2 处、
  `MESA_VK_WSI_HEADLESS_SWAPCHAIN` = 0；脚本另与 v64 dump 对照，差异仍只是 version* + 追加的 `kbase_diag`）
* 载荷完整性（其余条目 = 原物）：`libMobileGL.so` 16 956 584 B sha256 `72919c73…9c76848b` ✓；
  `classes.dex` 1 328 B sha256 `6bd3abde…d64f4ad` ✓
* `apksigner verify` 通过 ✓；v68/v67 APK sha256+mtime 前后一致 ✓

---

## 5. 回滚（服务器本机执行；回滚后即 v68 驱动）

```bash
TS=1791191447
M=/root/zenithblue/work/mesa
for f in src/vulkan/runtime/vk_fence.c \
         src/vulkan/wsi/wsi_common.c \
         src/panfrost/vulkan/panvk_physical_device.c \
         src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c; do
  cp -f "$M/$f.bak-v69-$TS" "$M/$f"
done
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja src/panfrost/vulkan/libvulkan_panfrost.so
# 期望 sha256 = dcda738feee42e91d61411ad240351ebf94ea876cc893d80ed56ead74ba013f0（20 016 896 B）
```
* 重贴 v69（不重跑 python）：`cp -f /root/v69-edits/<下划线化的相对路径> $M/<相对路径>`（4 个文件），再 ninja。
* 备份：`<file>.bak-v69-1791191447`（4 份，均在树内，未 `rm`）。
* 未使用 `git checkout/stash/reset`，未 `rm -rf` 任何既有目录。

---

## 6. 真机判读表（SLOWCALL 出现 vs 不出现）

阈值：SLOWCALL 全部 **≥2000 ms** 才打印 ⇒ **正常跑图应零输出**；DIAG(mask) 为 **≥1000 ms**（且需 `PANVK_DEBUG=kbase_diag`，v69 APK 已带）。

| 真机看到什么（grep `kbase: SLOWCALL` / `kbase: DIAG`） | 说明什么 |
|---|---|
| **无任何 SLOWCALL**，冻结照旧 | 阻塞点**不在** `vkWaitForFences`(含多 fence) / `vkAcquireNextImageKHR` / kbase sync `wait_many` 三处。下一步优先怀疑：`vkQueueSubmit` 等 image-acquired 信号量、**`vkQueuePresentKHR`**（注意 `panvk_wsi.c:271` 的 `panvk_android_swapchain_queue_present` 里就有一次 `DeviceWaitIdle`！）、或用户态互斥/内存分配。建议 v70 在 QueueSubmit/Present/DeviceWaitIdle 加同款看门狗。 |
| 冻结前只有 `kbase: SLOWCALL vkWaitForFences … after T ms`，**紧接着**有 `kbase: SLOWCALL kbase-sync wait …` 且其 `wait[i] … targets A/B/C` | 阻塞在 fence 等待，且是 **kbase 队列 seqno** 等待。`targets` 就是没退休的 target seqno，与 `kbase: DIAG wait subqueue U … seqno X/Y` 对齐即可定位是哪个 subqueue 卡住。 |
| 出现 `kbase: SLOWCALL vkWaitForFences …`，但**没有**对应的 `kbase-sync wait` 行，且**没有** v66/v68 的 10 s timeout 快照 | 阻塞在 fence 入口里，但**不在队列 seqno 等待**（因此 10 s 看门狗看不到）⇒ 落在 `kbase_cpu_sync_wait_one` 的**状态机等待**或 `kbase_sync_file_wait_func`（`poll()` on sync_file）上。这正是 v68 "静默冻结 + 看门狗不响"的最可能形态。 |
| 出现 `kbase: SLOWCALL vkAcquireNextImageKHR … swapchain 0x… after T ms` | 阻塞在 **WSI acquire**（MGL 帧节奏/图像获取），与我们的提交路径无关。看 T 值即可区分"慢"与"完全卡死"。 |
| 出现 `kbase: DIAG wait subqueue U returned after T ms, last barrier wait mask 0xMMMMMMMM`（T≥1 s） | 一次**队列等待**耗时 ≥1 s 后返回：`M` 是**最后写下的 barrier wait mask**。`0x0003` = LS + DEFERRED_SYNC；`0xfff8` = iteration slots 3..15。若这次之后恢复正常，说明是"卡了一下又回来了"。 |
| 每 1 秒重复 `kbase: DIAG wait subqueue U t=… seqno X/Y … last barrier wait mask 0xMMMMMMMM` | 队列在**持续卡住**，每秒一次 mask 读数 ⇒ **无需 timeout 快照**（这正是本轮 D 改动的目标）。若 10 s 到点后再出现 v68 的完整快照，两者应一致。 |
| 出现 `kbase: SLOWCALL kbase-sync wait … wait[i] … state 1 (PENDING)` + `wait_fn` 指向 `kbase_wait_sync_targets` | fence 是队列 fence 且仍 PENDING；与 target seqno 一起给出"欠了多少"。 |
| 出现 `state 2/3`（WAITING/其它）或 `wait_fn 0x…` **不是** `kbase_wait_sync_targets` | 该 sync 不是队列 fence（例如 WSI semaphore / 外部 sync_file）⇒ 阻塞与 GPU seqno 无关。 |

**判读要点**：`SLOWCALL` 出现 ⇒ 能**指名**阻塞调用与时长；`SLOWCALL` **不出现**而仍冻结 ⇒ **排除**了这三个入口，
必须扩大打点面（首要嫌疑 `vkQueuePresentKHR` → `DeviceWaitIdle`）。

---

## 7. 明确未验证 / 诚实标注

1. **未做真机验证**（任务禁止操作手机）：§6 的判读表是**预测**，不是实测结论。
2. **打点 A 打不出 fence→队列/seqno**（原因见 §2-A）；该字段由打点 C 在驱动层补出。任务允许此降级 ✓。
3. **D 的"无条件打印"按任务括号内的"至少等待超过 1 秒时打印"实现**：真的"每次 barrier 等待都打印"会在
   每帧产生数千行（每个 command buffer 多次 barrier 等待），故取 ≥1 s 门槛；周期 1 s 采样 + 等待返回各一次。
   offset 52 的 GPU 侧 STORE（v68 的 D）**一字未改**。
4. **若阻塞是 WSI semaphore 等待**，打点 C 能报耗时与 `targets`，但**无法**在那一层打印 barrier mask
   （mask 存在某个 kbase queue 的 seqno cell 里，而 `struct kbase_cpu_sync` 没有指回队列；
   `struct panvk_device` 也没有队列表）。此时 mask 只能在 `kbase_subqueue_wait_seqno` 里读到。
5. **打点 C 对 `struct kbase_cpu_sync` 的字段是无锁只读**（POD），刻意不加 `ks->mutex`，以免扰动等待；
   极端并发下打印值可能与瞬时值有出入——只影响日志，不影响任何判定。
6. **打点 D 的打印受 `PANVK_DEBUG(KBASE_DIAG)` 守卫**（与 v68 的 D 一致）；v69 APK 的 `PANVK_DEBUG=1,kbase_diag` 已开，
   真机可见；SLOWCALL 三点**不依赖任何环境变量**。
7. **`pack_v68.sh` 自带标注有误（非本轮引入）**：脚本第 6 个 source-state marker 写
   `v68 head marker emission (must be 1)`，实际打印 **2**。已核实 `csf/panvk_vX_cmd_buffer.c`
   **本轮未被触碰**（md5 `77e28cb3858814bafc99217119f3dc43` = 报告 40 的 v68 值；
   与 `.bak-v68-1791188038` 的差异是 v68 的 D 补丁本身），且该字符串本来就出现两次
   （注释 `:905` + 代码 `:920`）。⇒ 是脚本标注过时，v68 时同样为 2。
8. 本轮只动 4 个文件；`find` 确认树内无其它 `.c/.h` 在 17:0x 被修改；
   MobileGL、`/root/MobileGL`、`/root/mesa` 一律未动；未改修复 A、C1/C2/B、超时常量、wrapper 发射序列、`SB_*` 写入。
