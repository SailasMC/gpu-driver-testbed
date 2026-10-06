# 40 — v68 = v67 + 修复 A + 诊断 D（编译 + 打包已完成）

**日期**：2026-10-05 16:13–16:16
**任务**：把 33 号报告 §8 的 **候选 A（修复）** 与 **诊断 D** 应用到当前 v67 树，增量重编 ⇒ 打 v68 APK；
并做"撤改动 ⇒ 逐位等于 v67 驱动 c134b54e…"的确定性对照。
**结论**：✅ 全部完成。3 次构建 **exit 0**；撤改动后 **逐位等于 v67 驱动**；v68 `.so` 二次重编 **逐位可复现**；
APK 载荷 = 新 `.so`（逐字节 cmp 通过，非空）；manifest 与 v67 **只差 versionCode/versionName**。
**B/C 未应用**（严格按要求）。

---

## 1. 交付物与指纹

| 产物 | 路径 | sha256 | 字节数 |
|---|---|---|---|
| 新驱动 `.so` | `/root/zenithblue/build/android-v4/src/panfrost/vulkan/libvulkan_panfrost.so` | `dcda738feee42e91d61411ad240351ebf94ea876cc893d80ed56ead74ba013f0` | 20 016 896 |
| **v68 APK**（交付） | `/root/final/mgl-panvk-v68.apk` | `29c793dff14705322bc0f7e9d1a7006041f1f295018e1ae8d207b1a39af62522` | 10 195 503 |
| v68 APK 内载荷 `lib/arm64-v8a/libvulkan_freedreno.so` | （APK 内） | `dcda738f…ba013f0` = 新 `.so` ✓ | 20 016 896（非空 ✓） |
| 对照 `.so`（撤改动后重编） | `/root/v68-control-revert-equals-v67.so` | `c134b54ef40b6068f9f7085b6fc87f030642b5fa041ef57ec7c7a4108a6aa734` = **v67 驱动** ✓ | 20 020 032 |
| v68 首次构建 `.so`（可复现基线） | `/root/v68-A-D-build1.so` | `dcda738f…ba013f0` | 20 016 896 |

其它指纹：
* `A.patch`（从报告 §8.4 逐字提取）= `/root/patches-v68/A.patch`，sha256 `2ddd8a267e90b075ee812de934efa3fbe1d9846b77c438b51fd1186891ad0f86`
* `D.patch`（从报告 §8.5 逐字提取）= `/root/patches-v68/D.patch`，sha256 `4c9270490ac1afbe2f590ddf45a6ed95f6954a800b8e73e31f6f681acdf42a84`
* 打包脚本 `/root/pack_v68.sh`，sha256 `66443b99b1de05366242450075c834f95e12581f6d533c20eb1b061c84dd66db`
  （= `pack_v67.sh` 经 `sed -e 's/v67/v68/g' -e 's/versionCode="67"/versionCode="68"/g' -e 's/6\.7-marker-diag/6.8-fixA-diagD/g'`，末尾追加 v68 变更说明注释）
* v67 APK **未被触碰**：`/root/final/mgl-panvk-v67.apk` sha256 `1cc838f545c4d95e7f92816fd0a46228d6440b3f9d6e9f3fd1baee964318c822`（打包前后同值），10 195 503 B，mtime 仍为 15:47。

---

## 2. 应用了什么（file:line，均为**当前树**最终状态）

### diff A —— 删掉 wrapper ring entry 里剩余的 4 条 `SET_STATE`
文件：`/root/zenithblue/work/mesa/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c`
* 新注释块起始 **:833**（`/* v68: no SET_STATE here. … */`），`#if PAN_ARCH >= 11` 分支内 4 条
  `cs_set_state_imm32(SB_SEL_ENDPOINT / SB_MASK_WAIT / SB_SEL_OTHER / SB_SEL_DEFERRED)` 已删除；
  `#else` 的 arch<11 分支（`cs_set_scoreboard_entry(SB_ITER(0), SB_ID(LS))`）未动。
* 验证：`sed -n '805,860p' … | grep -c cs_set_state_imm32` = **0**（wrapper 内已无 SET_STATE）；
  init 流的 `cs_set_state_imm32(…SB_MASK_STREAM…)` 仍为 **1**（未被误删）。

### diff D —— barrier 等待前把 wait mask STORE 进 seqno cell offset 52，并在 timeout 快照打印
1. `…/csf/panvk_vX_cmd_buffer.c`
   * 新函数 `panvk_per_arch(kbase_record_wait_mask)` 定义 **:601–:635**（`static void` + `PANVK_DEBUG(KBASE_DIAG)` 守卫 +
     `kbase_node_path[0]` 守卫 + `cs_load64_to`→`cs_move32_to`→`cs_store32(b, value, addr, 52)`→`cs_flush_stores`）；
   * 调用点 **:688–:689**（`emit_barrier_insert_waits()` 内，`cs_wait_slots(b, deps.src[i].wait_sb_mask)` 之前，
     紧跟 `kbase_mark_progress(FRAG_BEFORE_TILING_WAIT)`）。
2. `…/csf/panvk_vX_gpu_queue.c`
   * 超时快照打印块 **:549–:560**（`mesa_loge("kbase: DIAG %s subqueue %u last barrier wait mask 0x%08x", …, *(uint32_t*)(cell_bytes + 52))`，
     读前 `kbase_cache_invalidate_range(cell_bytes, kbase_seqno_stride())`），位于 `kbase_log_callee_diag()` 的
     "2. The decisive question" 之前。
* **offset 52 空闲性已复核**（补强报告结论）：
  `kbase_seqno_stride() = ALIGN_POT(sizeof(struct panvk_cs_sync64)=16, 64) = 64`（`gpu_queue.c:202-205`）；
  已占用仅为 `LS_COPY@16`、`MARK_PRE_CALL@24`、`MARK_POST_CALL@32`、`MARK_POST_WAIT@40`、`STREAM_PROGRESS@48`(32 bit)
  ⇒ 0..51 占满，52..63 空闲 ✓（全树 grep 只出现这 5 个 offset）。

### 应用过程（可复现命令）
```
cd /root/zenithblue/work/mesa
# 先 dry-run（两份都 0 offset / 0 fuzz / 0 reject，exit 0）
patch -p1 --dry-run --batch --forward < /root/patches-v68/A.patch
patch -p1 --dry-run --batch --forward < /root/patches-v68/D.patch
# 备份
cp -f src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c  src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c.bak-v68-1791188038
cp -f src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c.bak-v68-1791188038
# 应用（顺序 A 然后 D，两份 patch 都动 gpu_queue.c）
patch -p1 --batch --forward < /root/patches-v68/A.patch
patch -p1 --batch --forward < /root/patches-v68/D.patch
```
应用后 md5：`gpu_queue.c = e95f4de556734b732db492da42384de7`、`cmd_buffer.c = 77e28cb3858814bafc99217119f3dc43`、
`cmd_draw.c = 74162c0b34eda580eecb9edd336205af`（**未变** ✓，B/C 未动）。

---

## 3. 构建（3 次，全部 exit 0）

命令固定为：
```
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja src/panfrost/vulkan/libvulkan_panfrost.so
```

| # | 树状态 | ninja | 结果 `.so` sha256 | 字节数 |
|---|---|---|---|---|
| 1 | v67 + A + D | **exit 0**（17/17，real 6.5 s） | `dcda738f…ba013f0` | 20 016 896 |
| 2 | 撤掉 A + D（= v67） | **exit 0** | **`c134b54ef40b6068f9f7085b6fc87f030642b5fa041ef57ec7c7a4108a6aa734`** | **20 020 032** |
| 3 | 贴回 A + D | **exit 0** | `dcda738f…ba013f0`（`cmp` = **BIT-IDENTICAL to build 1**） | 20 016 896 |

* 每次只重编 2 个源文件 × 各 arch 变体（v6/v7/v10..v14 的 `libpanvk_vX.a`）+ 链接，属**增量**编译；无 meson 重配置。
* 编译告警：`panvk_vX_gpu_queue.c:1165`（control 构建为 `:1146`）`label followed by a declaration is a C23 extension`
  —— **v67 原树就有**（build 2 纯 v67 源码同样告警），非本次引入；`cmd_buffer.c` **零告警**。

### 3.1 确定性对照（硬要求，已满足）
* 撤改动方式：`patch -p1 -R --batch --forward < D.patch` 然后 `< A.patch`（逆序）。
* 撤后**源码逐字节**等于改前备份：`cmp` 两份 `.bak-v68-1791188038` 均返回 0（
  md5 回到 `cb96a62abad4fe82aedc32e6b336176d` / `a9b69c84ac4553ef236f205d7f5b3924`）。
* 重编后 **逐位等于 v67 驱动** `c134b54ef40b6068f9f7085b6fc87f030642b5fa041ef57ec7c7a4108a6aa734`（20 020 032 B）
  ⇒ **无残留** ✓（`git_sha1.h` 重生成也未改变产物，见 build 2 与 build 1/3 的一致性）。
* 贴回后二次重编 **逐位等于首次构建** ⇒ v68 `.so` 可复现 ✓。

---

## 4. APK 校验（打包 exit 0，`PACK_DONE`）

```
unzip -p /root/final/mgl-panvk-v68.apk lib/arm64-v8a/libvulkan_freedreno.so | sha256sum
  → dcda738feee42e91d61411ad240351ebf94ea876cc893d80ed56ead74ba013f0   （== 新 .so ✓）
  → 字节数 20 016 896（非空 ✓）；解包后与构建树 .so `cmp` 逐字节相同 ✓
  → file: ELF 64-bit LSB shared object, ARM aarch64, for Android 35, NDK r27c, BuildID 7e1f2c86…b198
```
载荷完整性（其余条目 = v67/v64 原物）：
* `libMobileGL.so` 16 956 584 B，sha256 `72919c73a7e07630f329bb4ad9605c606a9aac9e2fc6596683ee7b9f9c76848b`（= `/root/v54/…`）✓
* `classes.dex` 1 328 B，sha256 `6bd3abde2c53506f1b54bb88a8069c3cc394c2fb08440e4ca475cbf8fd64f4ad`（= `/root/v54/classes.dex`）✓
* 载荷 CRC 确已改变（`libvulkan_freedreno.so` CRC-32 `34b76439` → `1a08fd2c`），压缩尺寸 4 319 860 → 4 320 225。

manifest：
* `aapt2 dump badging` → `versionCode='68' versionName='6.8-fixA-diagD'`，package `com.dsh.plugin.driver.g720` ✓
* `aapt2 dump xmltree` 与 **v67** 的 dump（`/root/research/32-work/32-v67-manifest.txt`）
  `diff` 只出现 **versionCode/versionName 两行** ⇒ `pojavEnv`/`boatEnv` 两串与 v67 **逐字符一致** ✓
  （`PANVK_DEBUG=1,kbase_diag`、`PANVK_KBASE_HEAP_RENEW_INTERVAL=32` 各 2 处；`MESA_VK_WSI_HEADLESS_SWAPCHAIN` = 0）
* 与 v64 dump 的差异仍是"仅 version* + 追加的 `kbase_diag`"（脚本内建对照）✓
* 附带产物：`/root/final/mgl-panvk-v68.apk.idsig`（apksigner v4，与 v67 同规则）。

> 为什么 v67/v68 APK **总字节数相同**（10 195 503）：apksigner 的签名块填充对齐所致——
> v68 的条目数据合计比 v67 **多 360 B**（.so 压缩后 +365，manifest/RSA/MANIFEST.MF 各 -1~-2），
> 对齐填充相应少了 360 B。载荷 sha/CRC 不同，可证内容确实已更新（非同一文件）。

---

## 5. 回滚（任选一条；都在服务器本机执行）

**v68 → v67（撤掉 A+D），已验证逐位等于 c134b54e…**
```
cd /root/zenithblue/work/mesa
patch -p1 -R --batch --forward < /root/patches-v68/D.patch
patch -p1 -R --batch --forward < /root/patches-v68/A.patch
# 或直接还原备份：
# cp -f src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c.bak-v68-1791188038  src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c
# cp -f src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c.bak-v68-1791188038 src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja src/panfrost/vulkan/libvulkan_panfrost.so
# 期望 sha256 = c134b54ef40b6068f9f7085b6fc87f030642b5fa041ef57ec7c7a4108a6aa734（20 020 032 B）
```
**v67 → v68（重新贴回）**
```
cd /root/zenithblue/work/mesa
patch -p1 --batch --forward < /root/patches-v68/A.patch
patch -p1 --batch --forward < /root/patches-v68/D.patch
export PATH=/opt/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
cd /root/zenithblue/build/android-v4 && ninja src/panfrost/vulkan/libvulkan_panfrost.so
# 期望 sha256 = dcda738feee42e91d61411ad240351ebf94ea876cc893d80ed56ead74ba013f0（20 016 896 B）
```
**重新打包**：`rm -f /root/v68/base.apk /root/v68/aligned.apk && bash /root/pack_v68.sh`
（`pack_v68.sh` 有 `set -e`，且若 `$W/base.apk` 已存在会**拒绝**执行以防陈旧 zip 条目——重打包前必须先删这两个中间文件。）

---

## 6. 纪律遵守情况
* ✅ 未用 `git checkout/stash/reset`；未 `rm -rf` 任何既有目录（仅删除 `pack_v68.sh` 自建的中间文件 `base.apk`/`aligned.apk`）。
* ✅ 未改 `/root/mesa`、`/root/MobileGL`（`find … -newermt "2026-10-05 15:50"` 为空）；未改 `/root/final` 下其它既有产物；
  `mgl-panvk-v67.apk` 及 `.idsig` sha256 前后一致。
* ✅ 未操作手机（无 adb / 安装 / input）。
* ✅ 只应用 A 与 D；**B、C 未应用**（`cmd_draw.c` md5 与改前一致）。

---

## 7. 未验证 / 待真机确认（**不得当成已验证**）
1. 【未验证·需真机】候选 A 的**生效判据**"ring entry 变短"：无 diag 时 VT/FRAG `152→136 B`、COMPUTE `136→120 B`；
   带 kbase_diag（v68 即此种）`288→272` / `272→256 B`。本机只做静态构建，**未运行任何作业**，
   该字节数差异**未在设备上观测**（`.so` 总体比 v67 小 3 136 B 与删除 4 条 SET_STATE + 新增诊断 STORE 方向一致，但不构成判据）。
2. 【未验证】诊断 D 打印的实际 mask 值（`0xfff8` / `0x0003` / `0x0001` / 未打印）——**必须真机读日志**；
   本报告只证明该打印/STORE 已进入二进制（源码位置 + 构建成功），**未**在运行中取到值。
3. 【未验证】offset 52 在**运行时**是否被 kbase 固件/其他地方写过（源码层面全树只用 0..51；固件侧写入未见证据，但未实测）。
4. 【未验证】v68 是否解决 v67 的冻结签名（VT 只有 bit0 / FRAG 停在 barrier 等待）——需真机对照日志。
5. 【未验证】诊断 D 的判定分档（`0xfff8`=迭代槽 3..15 / `0x0003`=LS+DEFERRED_SYNC / `0x0001`=LS）来自报告 33 §8.5 的**位图推断**，本机未做运行时确认；VT 侧 C2 卡点本次**未加新标记**，仍按 §8.2 的“首个未标记等待”定案。
