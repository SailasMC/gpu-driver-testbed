# 66b — 剪切探测器：探针改造已编译交付 + 剩余剪切源穷举 + gralloc 真实 stride 的读法

日期：2026-10-06 · 交付物在 `/root/research/probe10/`（**未触碰 mesa 源码树**）

---

## 1. 交付物（路径 / sha256 / 构建方式）

| 项 | 值 |
|---|---|
| 二进制 | **`/root/research/probe10/panvk_wsi_probe`** |
| sha256 | **`c10556e911c31712bd12cd35393b601b957d68110a0d60db4d5dbe64e31f7d44`** |
| 大小 / 类型 | 289728 B · `ELF 64-bit LSB pie executable, ARM aarch64 … for Android 35`（NDK r27c，带 debug_info） |
| 源码 | `panvk_wsi_probe.c`（原文件备份为 **`panvk_wsi_probe.c.pre-shear`**） |
| 改动脚本（可复核/可重放） | `patch_shear.py`（12 处锚点，每处断言唯一）· `fix_anchor.py` · `fix_devfns.py` |
| 构建 | `bash /root/research/probe10/build.sh`（原始命令未改；`HAVE_TRIANGLE` 仍启用） |
| 源码 sha256 | `128bca9fe293301923f1cd6c18981bc1cb2dfe2c005eb306af162ac05c0c8169`（打补丁、尚未加 DEV_FNS 时；加 `vkCmdCopyBufferToImage` 后字节数 76576→76607） |
| 编译警告 | 2 条，均无害：① `-Wdangling-else`（我的 `if/else` 无花括号，语义正确）；② `shear_shift_matches` 未被引用（我保留的备用助手） |

**改动清单（逐项）**
1. `W`/`H` 由 `#define 64` **改为运行期变量**：`#define W g_w` / `#define H g_h` + `static int g_w = 64, g_h = 64;` —— **已核实全文件 22 处 `W`/`H` 全部出现在运行期表达式里**（extent/viewport/readback 尺寸/`AImageReader_new`），**没有**任何 `W`/`H` 出现在常量表达式或数组维度里 ⇒ 该改法安全，`all` 模式与既有断言不受影响（默认仍是 64×64）。
2. 新增 `--width=N --height=N`（`g_w`/`g_h`）；banner 增加 `geometry=WxH shear=0/1`。
3. 新增确定性图案 `shear_px(x,y) = 0xff000000 | ((x*7) ^ (y*131) ^ ((x>>3)*29) ^ (y>>2)) & 0xffffff`：**逐像素不同、相邻行不相关** ⇒ **平移 1 像素即大面积不匹配**；同时统计 **R/B 交换后**的匹配数（沿用原探针对 gralloc ABGR/RGBA 的处理）。
4. 新增 `check_shear_pixels[_strided]()`：逐字节精确比对 + **首处不匹配 (x,y)** + **逐行最佳水平位移**（搜索 ±16 px，打印 row 0/1/2/3/8/16/…/H-1）+ **斜率拟合** + **换算行距差 `ΔB/row = slope × 4`**；通过判据：`exact == total` 或 `swapped == total`。
5. **复用既有管线**（关键设计）：`clear_copy_readback()` 里用 `g_shear` 分流 —— 改为 `vkCmdCopyBufferToImage`（图案上传）+ 回读后调 `check_shear_pixels`；`make_host_buffer` 的 usage 增加 `TRANSFER_SRC`（加一位，不影响旧行为）。
   ⇒ 于是 **`--mode=shear`（普通/optimal 图像路径）与 `--mode=shear-ahb`（AHB 外部导入路径）自动成立**，无需另写导入代码。
6. **`--mode=shear-win`**：在 `swapchain_steps()` 的清屏处改为上传图案到 swapchain image，并在 `AImageReader` 读回处改用 `check_shear_pixels_strided(data, AImage_getPlaneRowStride(...))` —— **这正是"写侧 pitch（`win_buf.stride*4`）vs 读侧 pitch（`AImage_getPlaneRowStride`）"的对账**。
   （暂存缓冲故意泄漏：它在 submit/present 序列中，必须在拷贝之后仍存活；探针短命，泄漏比追踪生命周期更安全 —— 已写在注释里。）

## 2. 建议调用命令行

```bash
# 推上去（改名以免和旧探针混淆）
adb push /root/research/probe10/panvk_wsi_probe /data/local/tmp/probe10_shear
adb shell chmod 755 /data/local/tmp/probe10_shear

# 0) 回归确认：旧行为没坏（必须仍 PIXEL PASS）
adb shell "MESA_LOG_LEVEL=info /data/local/tmp/probe10_shear --icd=/data/local/tmp/libvulkan_panfrost.so --mode=render --width=64 --height=64"
# 1) 真尺寸、普通图像路径（驱动 optimal tiling）
adb shell "… --mode=shear --width=2376 --height=1080"
# 2) 真尺寸、AHB 外部导入路径（≈ 游戏的渲染目标那条链）
adb shell "… --mode=shear-ahb --width=2376 --height=1080"
# 3) 真尺寸、窗口/交换链/present 路径（≈ 游戏的呈现那条链）★最关键
adb shell "… --mode=shear-win --width=2376 --height=1080 --fmt=0x1"
```
（把 `…` 换成 `MESA_LOG_LEVEL=info /data/local/tmp/probe10_shear --icd=/data/local/tmp/libvulkan_panfrost.so`；`--fmt=0x1` = `AIMAGE_FORMAT_RGBA_8888`，与游戏一致。若想看**没有** 3000 µs settle 时的差异，另跑一版带/不带 settle 的对照。）

## 3. 我预计的正确输出（你该看什么）

**全对时**（`2376×1080 = 2 566 080` 像素）：
```
  [shear] shear pattern 2376x1080: exact 2566080/2566080 (100.000%), R/B-swapped 0/2566080
  [shear] SHEAR PASS (mismatch=0/2566080, exact match)
```
**判读要点（按信息量）**
1. **`exact == total`** ⇒ 该路径**无剪切、无 pitch 失配**（这一条就能把对应层清出嫌疑）；
2. **`first mismatch at (x,y)`**：若 `y ≈ 0` 且 x 很小 ⇒ 整幅偏移（基址/偏移问题）；若 `y` 很大才出现 ⇒ 逐行累积（**剪切/pitch 差**）；
3. **逐行 best shift 表**：`row 0` 与 `row 1079` 的差值 ÷ 1079 = **斜率（px/行）**；**`ΔB/row = slope × 4`** 直接给出**行距差字节数**（例如斜率 0.3 ⇒ Δ≈1.2 B/row；若斜率是 1.0 ⇒ Δ=4 B/row，即整整一行差一个像素）；
4. **三种模式的分工**：`shear` FAIL ⇒ 驱动 optimal 路径；`shear`/`ahb` PASS 而 **`shear-win` FAIL** ⇒ **WSI 的 CPU 拷贝或窗口缓冲 pitch 解释**（写侧 `win_buf.stride*4` vs 读侧 `AImage_getPlaneRowStride`）；三者全 PASS ⇒ **驱动与 WSI 在该尺寸下清白 ⇒ 火力转 MobileGL/应用侧**（这正是你想要的"一刀切开"）；
5. `R/B-swapped == total` ⇒ 只是通道序（gralloc ABGR vs RGBA），**不是**剪切，探针按 PASS 处理。

## 4. 剩余能造成"逐行剪切"的量（穷举 + file:line + 量级）

| # | 候选 | file:line | 量级 / 判据 |
|---|---|---|---|
| **D1** | **GPU 生效行距 ≠ 显式 `rowPitch`** | `panvk_image.c:538`（`.wsi_row_pitch_B = pPlaneLayouts[].rowPitch = 9536`）→ `pan_image_layout_init()` → **`pan_mod.c:904-947`（linear handler）**：`use_explicit_layout` 时**逐字赋值** `slayout->tiled_or_linear.row_stride_B = layout_constraints->wsi_row_pitch_B`，只有**输入**的对齐校验（不符则 `return false` 让建 image 失败）→ `pan_desc.c:259 *row_stride = slayout->tiled_or_linear.row_stride_B` → **`pan_desc.c:318 cfg.row_stride = row_stride`** | **没有第二处对齐/改写**（我逐行核过 linear handler）。9536 = 64×149 ⇒ 64 B 对齐，**校验必过**。⚠️ 仅核了 linear；AFBC/AFRC 各有自己的对齐（我们 modifier=0 不走）；`has_zs_ext`/CRC 影响 **fbd 尺寸**，**不改 `row_stride`** ⇒ **D1 量级 = 0**（除非走了非 linear 路径） |
| **D2** | **写侧按 `win_buf.stride` 像素解、读侧（SF/AImageReader）按字节解**（或反之） | 写：`panvk_wsi.c:285-286`（`y * win_buf.stride * 4`）；读：`AImage_getPlaneRowStride` | **量级 = 4×**（若单位误判）⇒ 会表现为**极强**剪切/条纹，不是 0.3 px/行 ⇒ **多半不是它**，但 `shear-win` 会**直接量出来** |
| **D3** | **写侧 pitch 与 SF 实际消费 pitch 不同**（gralloc 给窗口缓冲的行距 ≠ `lock` 回报的 `stride`，或 SF 按自己的对齐消费） | 写：`panvk_wsi.c:285`；SF 在进程外 | **量级 = 差值本身**（可能 1~32 B/row）⇒ **最符合 0.3 px/行（≈1.2 B/row）** 的候选 ⇒ **`shear-win` 的 `win_buf.stride*4` vs `AImage_getPlaneRowStride` 对账就是为它设计的** |
| **D4** | **显示端按"比例"采样**（SF 认为的宽度/尺寸与我们写入的不同） | 进程外 | 等效斜率恒定、与内容无关 ⇒ 用 E1（往窗口缓冲写合成图案）或 `shear-win` 判 |
| **D5** | **应用侧自身缩放/投影**（MobileGL 渲染分辨率 ≠ 呈现分辨率） | 不在本树 | `shear`/`shear-ahb`/`shear-win` **全 PASS** 而游戏仍有面板 ⇒ 指向它 |
| **D6** | **读回的是"未写完"的图**（M7'/M9 族，时间型） | `panvk_wsi.c:260-308`（present 只靠 11–25 µs 的 idle） | 斜率**随帧变化**（不是恒定的 0.3）⇒ 用连拍帧斜率是否稳定来区分 |

**⇒ 结论**：在 D1 被代码链压到 0 之后，**"恒定小斜率"最可能是 D3（窗口缓冲的写侧/读侧 pitch 不一致）**，其次是 D4（比例型）；**D6 是"斜率不稳定"时才成立**。三种 `shear` 模式的组合正好把 D3/D4（win FAIL）与 D5（全 PASS）分开。

## 5. 如何**不依赖 `lockPlanes`** 读出 gralloc 的真实 stride（你要的办法）

- **原理**：`AHardwareBuffer` 的公开 API 没有布局字段（`describe()` 的 `stride` 是"像素行距"、`lockPlanes()` 给的是"字节行距"，两者都可能与 gralloc **内部**的 format layout 不同）。唯一权威来源是 **mapper 的 `getMetadata`**：`IMapper.getMetadata(handle, GET_FORMAT_LAYOUT, &blob)`，blob 里是 `PlaneLayoutComponent` 列表，**其中就包含 `ROW_STRIDE_IN_BYTES`**（与 `lockPlanes` 的 `rowStride` 是两条独立来源）。
- **探针里现成的入口**：`mode_mapper()`（`panvk_wsi_probe.c:1224-1330`，你已经在跑、并且已经 dump 出 metadata blob 结构）+ `AHardwareBuffer_getNativeHandle`（探针已 dlsym，`:127-129`）。
- **落地步骤（3 步）**：
  1. 在 `mode_mapper()` 里**再加一次 `getMetadata`**，metadata 类型取 **format layout**（不是当前的 dataspace/其它），把 blob 原样 hexdump（探针已有 `hexdump()`，`:1212`）；
  2. 用 NDK 头里的类型常量解析：`grep -rn "ROW_STRIDE_IN_BYTES\|PlaneLayoutComponentType" /opt/android-ndk-r27c/**/graphics/common/*.h` ⇒ 按该 enum 的值把 blob 里的条目解出来；
  3. **三方对账**（这就是"真实 stride"）：`getMetadata → ROW_STRIDE_IN_BYTES` **vs** `describe().stride * 4` **vs** `lockPlanes().rowStride`。**若前两者与第三者不同 ⇒ D3 类剪切源确认**，并且差值就是 `ΔB/row`（对应斜率的 4 倍）。
- **补充（零代码）**：把同一个 AHB 交给一个**纯显示**路径（例如经 `AImageReader`/`SurfaceControl` 只做显示不做拷贝），用"竖线图案"看是否被剪切 —— 这与 `shear-win` 等价但更直接地隔离 SF。

## 6. 未验证 / 限制（诚实清单）

1. 二进制**只在服务器上编译通过**（`BUILD OK`），**未在设备上运行过**：探针的 **shear 路径是否真的 PASS**（含 `vkCmdCopyBufferToImage` 是否被驱动接受、`AImage_getPlaneRowStride` 的语义）需要你上机验证。
2. `--width/--height` 的宏改造经**文本层面核实**（无常量/数组维度使用），但**未做全路径运行**验证（`all` 模式我只保证默认值下行为不变；不同尺寸下 `AImageReader_new` 等是否有上限未测）。
3. `shear-win` 的暂存缓冲**故意泄漏**（见 §1.6），且 `-Wdangling-else` 警告来自 `clear_copy_readback` 的分支写法（语义正确）。
4. D1 的"无第二处改写"**只对 linear handler 逐行核过**；若设备上实际走了 AFBC/tiled（与"modifier=0"矛盾）则需重核。
5. §5 的 `getMetadata` **类型常量值我未查**（不猜）⇒ 给出的是 grep 命令与三步流程；`mode_mapper()` 现有实现里 metadata 类型的取值需要你按该 enum 修正。
6. 我未修改 `build.sh`（除 DEV_FNS 那一处源码改动外，构建方式与原来完全一致）。
