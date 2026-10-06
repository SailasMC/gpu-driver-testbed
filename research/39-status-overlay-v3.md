# 39 — DSH 状态悬浮窗 v3：第一行「加载中」动效标记

**结论**：在 `com.dsh.overlay.status` 第一行（当前任务行）行首加入了一个 **13dp 不确定模式 `ProgressBar`**：
忙态（第一行首个非空白字符是 `⏳`）持续转圈、颜色跟随状态色；空闲态（`✅`/`❌`/`⚠️`/无标记/读不到文件）显示**静态小圆点**，不动画。
版本 `versionCode 3 / versionName 1.2-loading-spin`，**沿用同一 keystore**，证书指纹与 v1/v2 完全一致 ⇒ 可直接覆盖安装。

---

## 1. 交付物与校验

| 项 | 值 |
|---|---|
| APK | `/root/final/dsh-status-overlay-v3.apk` |
| 大小 | **25200 字节** |
| sha256 | `5ce07b8b40c58d89c915704c2daed96b4950532164761bb2e8e64d677ce789aa`（前缀 **`5ce07b8b`**） |
| 包名 | `com.dsh.overlay.status`（与 v1/v2 一致，未改） |
| 应用名 | DSH 状态 |
| versionCode / versionName | **3** / **1.2-loading-spin** |
| minSdk / targetSdk | 26 / 34 |
| classes.dex | 26292 字节，sha256 `139484ab8f2c49d5fc4f4e698623e0c55f0743cb152c4ddb7175c1a9c6ad5e13` |
| 签名 | `/root/dsh-driver.keystore`，alias `dshdriver`，v2+v3 scheme 通过 |
| 证书 SHA-256 | `eba5095017e1b5266e4a60aba48196dd4056bb328e680262ce01bde2de09e565` |
| 报告 | 本文件 |

**字节可复现**：连续 3 次独立执行 `build-v3.sh`，退出码均 0、均打印 `ALL CHECKS PASSED`、sha256 三次完全相同
（唯一带变量的 zip 条目 mtime 已被固定为 `2026-01-01 00:00`）。所以上面这个 sha256 可以被任何人重放验证。

> **附带解释一个容易被误判的现象**：v1 / v2 / v3 三个 APK 的文件大小**都是 25200 字节**。
> 这不是「打错了/覆盖了」，实测已排除：
> - `cmp -s v2 v3` ⇒ 不同；sha256 不同；`classes.dex` 不同（24016 → 26292 字节，sha256 不同）；`AndroidManifest.xml` 不同（3388 → 3392）。
> - zip 内**压缩后**总字节 v2=17745 / v3=18956（差 1211）。
> - 原因：apksigner 会把 **APK Signing Block 补齐到 4096 字节整页**（实测三个包的 signing block 都是 4096 字节，
>   中央目录都从偏移 24576 开始）。v3 多出来的 1211 字节被同一页里的空隙吸收，于是总大小没变。

---

## 2. 改了什么（类 / 行）

改动**只涉及 2 个文件**；`MainActivity.java` **零改动**（diff 为空）；`res/`、`AndroidManifest.xml` 除版本号外零改动。

### 2.1 `src/com/dsh/overlay/status/StatusReader.java`（247 → 287 行，+37 / −0）

| 行 | 内容 |
|---|---|
| 33–37 | 类注释补 v3 说明 |
| **43** | `static final char BUSY_CHAR = '\u23F3';` — 忙态字形 |
| **233–266** | 新增 `// busy state (v3)` 段 + **`static boolean isBusy(String firstLine)`**（唯一新增逻辑） |

`StatusReader` 的候选路径解析 / 读取 / 配色**一行未动**。

### 2.2 `src/com/dsh/overlay/status/OverlayService.java`（452 → 587 行，+126 / −6）

| 行 | 内容 |
|---|---|
| 11 / 14 / 15 | 新增 import `ColorStateList` / `PorterDuff` / `Drawable` |
| 29 / 31 | 新增 import `FrameLayout` / `ProgressBar` |
| 40–48 | 类注释补 v3 说明 |
| **81–86** | 新增字段：`firstRow` / `markSlot` / `spin` / `dot` / `markBusy` / `markColor` |
| 99–102 | 新增尺寸字段：`markPx` / `dotPx` / `maxFirstLineWidthPx` |
| **233–234** | `markPx = dp(13)`、`dotPx = dp(7)` |
| **244** | `maxFirstLineWidthPx = max(maxLineWidthPx − markPx − gapPx)`，保证 70% 上限不变 |
| **329–364** | `buildCard()`：第一行改为 `firstRow = [13dp markSlot][6dp][line1]`，`spin`/`dot` 创建并叠放进 `markSlot`；**第 2..n 行原样加入 `linesBox`**（不含第 1 行，所以没有多出一行） |
| 371–379 | 触摸监听补挂到 `firstRow/markSlot/spin/dot`（整块牌子仍可拖动/点击/长按） |
| **409–417** | 新增 `setDotColor(int)`：`GradientDrawable.OVAL` 静态圆点 |
| **428–451** | 新增 `applyLoadingMark(boolean busyNow, int color)`：忙/闲切换 + 上色 |
| **476–515** | `render()` 重写第 1 行分支；末尾调用 `applyLoadingMark(StatusReader.isBusy(firstLine), color)`（第 509 行） |

**未改动（逐项 grep 复核，见第 4 节 V8）**：`#99000000` 背景、12dp 圆角、3dp 色条、8dp 内边距、11sp、
两行布局、0.70×0.25 上限、每秒轮询、拖动/位置记忆/单击折叠/长按隐藏 60s、
`FLAG_NOT_FOCUSABLE|FLAG_NOT_TOUCH_MODAL|FLAG_LAYOUT_NO_LIMITS`（**代码中无 `FLAG_NOT_TOUCHABLE`**）、
候选路径顺序（私有目录 → media → `/sdcard/MG`）、包名 `com.dsh.overlay.status`。

---

## 3. 动效是怎么实现的

**用的就是 `ProgressBar` 的内建不确定动画，没有自绘、没有 `AnimationDrawable`、没有 `RotateAnimation`。**

```java
// OverlayService.java:338   —— 等价于 XML 的 style="?android:attr/progressBarStyleSmall"
spin = new ProgressBar(this, null, android.R.attr.progressBarStyleSmall);
spin.setIndeterminate(true);                                        // 不确定模式：无限循环
spin.setLayoutParams(new FrameLayout.LayoutParams(markPx, markPx, Gravity.CENTER));  // 强制 13dp 方形
spin.setVisibility(View.GONE);                                      // 初始空闲
```

- **尺寸**：`markPx = dp(13)`，`ProgressBar` 被 LayoutParams 钉死成 13dp×13dp（规格要求 12~14dp）。
- **持续动画**：`setIndeterminate(true)` 让 `ProgressBar` 自带的不确定动画无限循环，**不需要我们写任何帧循环**。
- **不阻塞布局**：不确定动画完全跑在 drawable 自己的帧回调里，不触发 `requestLayout`；且 `render()` 只在
  文件内容**变化**时才被调用（沿用 v2 的 `if (!ls.equals(current))`），每秒轮询不会干扰动画。
- **切换防抖**：`applyLoadingMark` 首行做了 `if (markBusy == busyNow && markColor == color) return;`
  ⇒ 状态没变时**一个 View 都不碰**，既不会重启动画，也不会造成无谓重排。
- **固定槽位不跳动**：`markSlot` 是固定 13dp 的 `FrameLayout`，`spin`(13dp) 与 `dot`(7dp, 居中) 叠放其中，
  忙/闲切换只改 `Visibility` ⇒ **文字位置绝不左右跳动**。第 1 行文字 `maxWidth` 预先减去 `13dp+6dp`，
  所以「标记 + 6dp + 文字」总宽仍 ≤ 原来的 70% 屏宽上限。
- **颜色跟随状态色**：
  ```java
  // OverlayService.java:438  主路径
  spin.setIndeterminateTintList(ColorStateList.valueOf(color));
  spin.setIndeterminateTintMode(PorterDuff.Mode.SRC_IN);
  // OverlayService.java:443  兜底（个别 ROM 上 tint 对 animated-rotate drawable 不生效）
  Drawable d = spin.getIndeterminateDrawable();
  if (d != null) d.setColorFilter(color, PorterDuff.Mode.SRC_IN);
  ```
  `color` 就是 v1/v2 原有的 `StatusReader.colorFor(第一行)`：忙态 → 蓝 `#2196F3`；`✅`→绿、`❌`→红、`⚠️`→黄、
  无标记/读不到 → 灰 `#9E9E9E`。双保险保证「至少主题色/绿」不会退化成看不见的深色圈。
- **垂直居中**：`firstRow.setGravity(Gravity.CENTER_VERTICAL)` ⇒ 标记与首行文字垂直居中对齐（规格 3）。
- **行首、不新开一行**：标记是 `firstRow` 的第 0 个子 View，`firstRow` 是 `linesBox` 的第 0 个子 View；
  `linesBox` 里第 1 行文字不再重复添加 ⇒ **总行数与 v2 完全相同**。
- **空闲态是纯静态**：`dot` 只是一个 `GradientDrawable.OVAL` 背景的裸 `View`，代码里**没有任何**
  `AnimationDrawable` / `RotateAnimation` / `startAnimation` / `ObjectAnimator` / `ValueAnimator`（V7-f 已核）。

**忙态判定**（`StatusReader.isBusy`，第 245 行）：取第一行，跳过前导空白（含 **NBSP U+00A0 / BOM U+FEFF /
全角空格 U+3000**），再看该字符是否等于 `U+23F3`。所以 `"  ⏳ 编译中"` 是忙态，而 `"done ⏳"`、`"✅ 完成"`、
`"(等待状态…)"`、`null`、空串都不是。

---

## 4. 验证结果（全部实跑，日志 `vfy-v3/RESULTS.txt`）

| # | 检查 | 结果 |
|---|---|---|
| V1 | `apksigner verify --verbose --print-certs` | **Verifies**；v2 scheme=true，v3 scheme=true（v1=false 属正常，minSdk 26） |
| V2 | `aapt2 dump badging` | `package: name='com.dsh.overlay.status' versionCode='3' versionName='1.2-loading-spin'`；minSdk 26 / targetSdk 34；label `DSH 状态`；4 条权限与 v2 相同 |
| V3 | **证书指纹 v1/v2/v3 对比** | v1 = v2 = v3 = `eba5095017e1b5266e4a60aba48196dd4056bb328e680262ce01bde2de09e565` ⇒ **与 v2 完全一致，可覆盖安装** |
| V4 | `unzip -l` | 9 项，含 **`classes.dex` (26292)** + AndroidManifest.xml + resources.arsc + 3 个 drawable + 3 个签名文件 |
| V5 | sha256 / 大小 | v3 = `5ce07b8b…789aa` / 25200 B；v1 仍 `545d517b…`、v2 仍 `e65478d4…`（**未被改动**） |
| V6 | **对成品 `classes.dex` 做 dexdump 反汇编** | `Landroid/widget/ProgressBar;.<init>:(Context;AttributeSet;I)V` 调用点存在；`setIndeterminate` / `setIndeterminateTintList` / `setIndeterminateTintMode` / `getIndeterminateDrawable` / `setVisibility` 各 1~3 次；`applyLoadingMark`/`setDotColor`/`isBusy` 均已编译进 dex |
| V7 | **静态源码检查（规格 6）** | (a) 忙态确实 `new ProgressBar(this, null, android.R.attr.progressBarStyleSmall)` ✓ (b) `setIndeterminate(true)` ✓ (c) **忙态分支内 `spin.setVisibility(View.VISIBLE)`** ✓ (d) 忙态分支内**没有** `dot.setVisibility(VISIBLE)` ✓ (e) 空闲分支 `spin GONE` + `setDotColor` + `dot VISIBLE` ✓ (f) 圆点是 `OVAL`、无任何动画 API ✓ (g) 忙态来自第一行首个非空白字符 ✓ (h) 标记在 `firstRow` 内、`linesBox` 第 0 项 ✓ (i) 13dp/7dp/6dp ✓ (j) tint + colorFilter 双保险 ✓ |
| V8 | **未改动项复核** | `0x99000000` ✓、`setCornerRadius(dp(12))` ✓、`barPx = dp(3)` ✓、`setPadding(padPx,…)` ✓、`COMPLEX_UNIT_SP,11f` ✓、`POLL_MS=1000L` ✓、`0.70f/0.25f` ✓、三条 flag ✓、**代码中无 `FLAG_NOT_TOUCHABLE`** ✓、`HIDE_MS=60000L`/`LONG_PRESS_MS=600L` ✓、候选路径三条 ✓、包名 ✓ |
| V9 | 冻结件未被触碰 | `/root/final/dsh-status-overlay.apk`（25200B, 15:18）、`-v2.apk`（25200B, 15:37）mtime 未变；`mgl-panvk-*.apk` 未变 |
| V10 | v3 确实不同于 v2 | `cmp` 不同；`classes.dex` 24016→26292；manifest 3388→3392 |
| **L** | **`vfy-v3/run-busy-test.sh`：真源码 JVM 逻辑测试** | **pass=29 fail=0**：8 个忙态用例（含前导空格/Tab/NBSP/BOM/全角空格）、10 个空闲用例（✅/❌/⚠️/纯文本/`(等待状态…)`/null/空串/全空白/`⏳` 不在首位）、6 个配色用例、5 个「文件内容 → 标记状态」端到端用例全部 PASS |

复现：
```bash
bash /root/overlay/build-v3.sh                 # 构建 + V1..V10 全部校验，期望 exit 0 且打印 ALL CHECKS PASSED
bash /root/overlay/vfy-v3/run-busy-test.sh     # 忙/闲判定逻辑测试，期望 pass=29 fail=0
```
源码 v2 → v3 的完整 diff：`/root/overlay/vfy-v3/diff-StatusReader.txt`、`diff-OverlayService.txt`。
v2 源码已备份在 `/root/overlay/src-v2-backup/`（未删任何东西）。

---

## 5. 真机上应看到什么（判据）

覆盖安装（同签名，直接装即可，**不会**要求卸载）→ 打开「DSH 状态」→ 启动悬浮窗。然后：

**判据 A — 忙态有转圈**
把状态文件第一行写成以 `⏳` 开头，例如
`echo '⏳ 正在编译 v3' > /sdcard/Android/data/com.dsh.overlay.status/files/status.txt`
⇒ 卡片**第一行行首**出现一个**持续旋转的小圆圈**（约 13dp，蓝色 `#2196F3`），圈与文字间距约 6dp，
颜色与左侧 3dp 色条一致。**转圈必须一直在转**，不是转一下就停。

**判据 B — 空闲态是静态点、不动**
把第一行改成 `✅ 编译完成`（或 `❌ …` / `⚠️ …`，或整行没有标记）
⇒ 行首的转圈**消失**，变成同一位置的**静态实心小圆点**（✅绿 / ❌红 / ⚠️黄 / 无标记灰），
**肉眼观察 5 秒必须是完全静止的**，且色条颜色同步变化。

**判据 C — 位置/布局没有变**
- 标记在**第一行行首**，**没有把内容挤到第二行**（折叠态仍然只有一行）。
- 忙 ↔ 闲来回切换时，**文字不会左右跳动**（固定 13dp 槽位）。
- 单击仍然折叠/展开（展开后第 2 行起无标记，只有第一行有）；长按 0.6s 隐藏、60s 后自动回来；
  拖动仍然可移动、位置被记住。
- 卡片整体仍然 ≤ 屏宽 70% / 屏高 25%，深色 `#99000000` + 12dp 圆角 + 左侧 3dp 色条 + 8dp 内边距 + 11sp 字。
- 牌子以外区域的触摸**照常穿透**到下面的应用（没有 `FLAG_NOT_TOUCHABLE` 被误加，也没有反过来挡住屏幕）。

**判据 D — 版本与签名**
设置 → 应用 → DSH 状态：版本号应显示 `1.2-loading-spin`（versionCode 3）。
若系统提示「应用未安装 / 签名不一致」，说明装错了包 —— 本包的证书 SHA-256 必须是
`eba5095017e1b5266e4a60aba48196dd4056bb328e680262ce01bde2de09e565`（与已装 v2 相同）。

**判据 E — 读不到文件也不崩**
`/sdcard/…` 三个候选都读不到时，卡片显示 `(等待状态…)`，行首是**灰色静态点**（`(等待状态…)` 不以 `⏳` 开头），
进程不崩、每秒继续重试。

---

## 6. 未验证 / 已知限制（如实标注）

1. **真机运行未验证**：本任务纪律禁止操作手机（不 adb / 不安装 / 不 input）。第 5 节的判据是**预期行为**，
   来自静态检查 + dex 反汇编 + JVM 逻辑测试，**不是**真机实测结果。
2. **「静态小圆点」的对应关系**：v1/v2 源码里**并没有**一个专门的圆点 View（grep 过 `dot`/`●`/`•` 均无命中）；
   用户看到的「静态小圆点」实为第一行文字自带的**行首状态字形**（`⏳`/`✅`/`❌`/`⚠️`）。
   v3 采用规格允许的「**补充**」方案：**文字内容一字不改**，在它**前面**新增一个专用标记位。
   因此忙态行会同时看到「转圈 + `⏳` 字形」。若希望变成「**替代**」（隐藏文字里的 `⏳`），
   只需在 `render()` 里对 `ls.get(0)` 做一次 `isBusy` 时去掉首个 `⏳`，属未实施的可选项。
3. `ProgressBar` 走 `?android:attr/progressBarStyleSmall`，个别 ROM 上 `setIndeterminateTintList` 对
   `animated-rotate` drawable 可能不生效；已加 `drawable.setColorFilter(SRC_IN)` 兜底，但**兜底本身未在真机验证**。
4. 全部为 `try/catch` 包裹的防御式调用，最坏情况是转圈显示为系统主题色而不是状态色，**不会**导致崩溃或布局错乱。
5. 隐藏 60s 期间卡片被移出 WindowManager，`ProgressBar` 动画随之停止；恢复后 `attach() → render()` 会重新
   置位可见性，动画自动重开 —— **该行为依赖 `ProgressBar` 的 attach 自动起播，未在真机验证**。

---

## 7. 关键路径

| 用途 | 路径 |
|---|---|
| 交付 APK（v3） | `/root/final/dsh-status-overlay-v3.apk` |
| 本报告 | `/root/research/39-status-overlay-v3.md` |
| v3 源码 | `/root/overlay/src/com/dsh/overlay/status/{StatusReader,OverlayService,MainActivity}.java` |
| v2 源码备份 | `/root/overlay/src-v2-backup/` |
| 构建脚本 | `/root/overlay/build-v3.sh` |
| 逻辑测试 | `/root/overlay/vfy-v3/run-busy-test.sh` |
| 全部校验日志 | `/root/overlay/vfy-v3/RESULTS.txt` |
| v2→v3 diff | `/root/overlay/vfy-v3/diff-{StatusReader,OverlayService,MainActivity}.txt` |
| dex 反汇编 | `/root/overlay/vfy-v3/dexdump-v3.txt` |
| 保留未动 | `/root/final/dsh-status-overlay.apk`(v1)、`dsh-status-overlay-v2.apk`、`mgl-panvk-*.apk` |
