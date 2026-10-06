# 35 — DSH 状态悬浮窗 APK（com.dsh.overlay.status）

从零构建的极小 Android 悬浮窗状态牌：把 `/sdcard/MG/status.txt` 的内容以一个小卡片显示在屏幕上，
让用户随时看到 AI 当前在做什么，同时尽量不挡屏幕。

## 1. 交付物与校验

| 项 | 值 |
|---|---|
| APK | `/root/final/dsh-status-overlay.apk` |
| 大小 | 25200 字节（约 25 KB） |
| sha256 | `545d517b9af5eee6a21ddcbb5f4bdcaee6d2879cb5c0f48e13fead93ef5de491` |
| 包名 | `com.dsh.overlay.status` |
| 应用名 | DSH 状态 |
| versionCode / versionName | 1 / 1.0 |
| minSdk / targetSdk | 26 / 34 |
| 语言 | Java（无 Kotlin，无 Gradle，无 AndroidX） |
| 签名 | /root/dsh-driver.keystore，alias `dshdriver`，v2+v3 通过 |
| 证书 SHA-256 | EB:A5:09:50:17:E1:B5:26:6E:4A:60:AB:A4:81:96:DD:40:56:BB:32:8E:68:02:62:CE:01:BD:E2:DE:09:E5:65 |

构建工具链（服务器实测）：`/opt/android-sdk/build-tools/35.0.0/{aapt2,d8,zipalign,apksigner}`、
`/opt/android-sdk/platforms/android-34/android.jar`、javac 25（`--release 17`）。
源码与中间产物：`/root/overlay/`（`src/`、`res/`、`AndroidManifest.xml`、`build/`）。

构建/校验命令（可重放）：
```bash
cd /root/overlay
BT=/opt/android-sdk/build-tools/35.0.0; AJ=/opt/android-sdk/platforms/android-34/android.jar
$BT/aapt2 compile --dir res -o build/res.zip
$BT/aapt2 link -o build/base.apk -I $AJ --manifest AndroidManifest.xml --java build/gen \
  --min-sdk-version 26 --target-sdk-version 34 --version-code 1 --version-name 1.0 build/res.zip
javac --release 17 -encoding UTF-8 -cp $AJ -d build/classes $(find src build/gen -name '*.java')
$BT/d8 --release --min-api 26 --lib $AJ --output build/dex $(find build/classes -name '*.class')
cp build/base.apk build/app-unsigned.apk && (cd build/dex && zip -q -X ../app-unsigned.apk classes.dex)
$BT/zipalign -f -p 4 build/app-unsigned.apk build/app-aligned.apk
$BT/apksigner sign --ks /root/dsh-driver.keystore --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias dshdriver --v1-signing-enabled true --v2-signing-enabled true \
  --out build/dsh-status-overlay.apk build/app-aligned.apk
```

实测校验结果：
- `apksigner verify --verbose`：**Verifies**；v2 scheme = true，v3 scheme = true（v1 = false，minSdk 26 ≥ 24 时系统只用 v2，属正常）。
- `unzip -l`：含 `classes.dex`(21120) + `AndroidManifest.xml` + `resources.arsc` + 3 个 `res/drawable/*.xml`。
- `aapt2 dump badging`：`package: name='com.dsh.overlay.status' versionCode='1' versionName='1.0'`、
  `minSdkVersion:'26'`、`targetSdkVersion:'34'`、`application-label:'DSH 状态'`、
  `launchable-activity: com.dsh.overlay.status.MainActivity`。
- `aapt2 dump permissions`：SYSTEM_ALERT_WINDOW / FOREGROUND_SERVICE / FOREGROUND_SERVICE_SPECIAL_USE / POST_NOTIFICATIONS。

## 2. 如何安装

前置：手机与服务器之间不需要连接，直接把 APK 拷到手机（数据线 / 文件管理器 / 任意传输方式），
或在手机上用文件管理器点开。

1. 拷贝 `dsh-status-overlay.apk` 到手机存储。
2. 用文件管理器点开安装；若系统提示「未知来源」，在弹窗里允许该文件管理器「安装未知应用」。
3. 安装后桌面出现「DSH 状态」图标。（本任务未在真机安装，属未验证：见第 7 节。）

## 3. 如何给「显示在其他应用上层」授权（只需一次）

1. 打开应用「DSH 状态」。
2. 首屏会显示「权限：未授权 ✗」和一个按钮 **① 授予「显示在其他应用上层」权限**，点击它。
3. 系统跳到本应用的「显示在其他应用上层 / 悬浮窗」开关页，把开关打开，按返回键回到应用。
4. 回到应用即显示「权限：已授权 ✓」，此时出现 **② 启动悬浮窗** 与 **停止悬浮窗** 两个按钮，点击启动。
5. 之后每次开机/重开：直接打开应用 →「② 启动悬浮窗」即可，权限不用再给。

替代路径（按钮打不开系统页时）：设置 → 应用 → DSH 状态 → 「显示在其他应用上层」（部分 ROM 在
「应用管理 → 特殊权限 → 悬浮窗」）。

Android 13+ 首次启动会顺带请求通知权限（用于前台服务通知），拒绝也不影响悬浮窗工作，只是通知不显示。

## 4. status.txt 格式约定

- 路径固定 `/sdcard/MG/status.txt`（= `/storage/emulated/0/MG/status.txt`），**UTF-8 纯文本**，1~8 行。
- 每行一条；空行会被忽略；超过 8 行只取前 8 行。
- **第 1 行 = 当前任务**（也决定左侧色条颜色与通知内容）。折叠态只显示这一行。
- 建议写法（示例）：

```
⏳ 正在编译 dsh-status-overlay
- aapt2 link 完成
- javac 3 个类完成
- d8 -> classes.dex
```

- 目录不存在或文件读不到 ⇒ 卡片显示 `(等待状态…)`（灰色），不会崩溃。
- 刷新频率 1 秒（`Handler.postDelayed` 1000 ms）；文件内容没变则不重绘。

## 5. 状态色条映射（看第 1 行）

| 第 1 行包含 | 颜色 |
|---|---|
| `✅` 或 `成功` | 绿 #4CAF50 |
| `❌` 或 `失败` 或 `错` | 红 #F44336 |
| `⚠`（含 ⚠️） | 黄 #FFC107 |
| `⏳` 或 `正在` 或 `跑` | 蓝 #2196F3 |
| 其它 | 灰 #9E9E9E |

判定顺序即上表顺序（先绿、再红、再黄、再蓝，最后灰）。

## 6. 悬浮窗行为

- 窗口类型 `TYPE_APPLICATION_OVERLAY`；flag = `FLAG_NOT_FOCUSABLE | FLAG_NOT_TOUCH_MODAL | FLAG_LAYOUT_NO_LIMITS`，
  **没有** `FLAG_NOT_TOUCHABLE`（牌子自己可拖），除牌子矩形以外的屏幕区域触摸事件照常下发给下面的应用。
- 外观：深色半透明 `#99000000`（60% 不透明），圆角 12dp，内边距 8dp，字 11sp、紧凑行距、单行省略号截断，
  左侧 3dp 竖色条（左圆角）。
- 位置：默认左上角 x=16dp、y=64dp（避开状态栏）；按住拖动，松手后写 `SharedPreferences("dsh_status")`，下次启动恢复；
  松手时会把位置夹回屏幕内（至少留 48dp 可抓取区）。
- 尺寸上限：宽 ≤ 屏宽 70%、高 ≤ 屏高 25%（两者都在运行时按真实分辨率算，并据此限制可见行数）。
- 折叠 = 1 行；单击切换展开 = 前 ≤ 6 行（第 1 行始终是当前任务行）。
- 长按 ≥ 600 ms = 隐藏（从 WindowManager 移除），60 秒后由 `postDelayed` 自动恢复；期间通知文案变为「已隐藏 60 秒 · …」。
- 前台服务 `OverlayService`：`startForeground` + 通知渠道 `dsh_status`（IMPORTANCE_LOW，常驻、不响铃），
  通知标题「DSH 状态」、正文 = 状态第 1 行；manifest 声明 `foregroundServiceType="specialUse"` +
  `PROPERTY_SPECIAL_USE_FGS_SUBTYPE`。

## 7. 已知限制 / 未验证项（如实标注）

1. **未在真机安装运行**：本任务纪律禁止操作手机（不 adb、不安装、不 input）。以上「已验证」仅指
   构建与静态校验（apksigner / aapt2 / unzip）；**运行时行为（卡片外观、拖动、通知）未经真机验证**。
2. 需要用户手动给一次「显示在其他应用上层」；应用无法自行授予。
3. `/sdcard/MG/status.txt` 需要有 App 可读权限：Android 10+ 分区存储下 `/sdcard` 根目录读取通常仍可读
   （本应用不申请存储权限，靠 `File` 直读），个别 ROM 若收紧到读不到，卡片会停在 `(等待状态…)`——**未验证**。
4. 展开态显示的是**前 ≤6 行**（第 1 行 = 当前任务）。规格写「最近 ≤ 6 行」；若希望改成「尾部最近 6 行」，
   只需改 `OverlayService.render()` 里取 `ls` 子区间的一行。
5. 长按隐藏期间卡片被移除，位置仍在；60 秒自动回来（进程若被杀则不回来，重开应用点「启动悬浮窗」）。
6. 部分国产 ROM 需要额外把应用加入「自启动/后台白名单」，否则前台服务可能被清掉——常见坑，未验证。
7. 通知小图标为矢量图（API 26+ 渲染正常）；某些 ROM 的通知栏小图标显示为纯色剪影，属系统行为。
8. 屏幕尺寸取 `getDefaultDisplay().getRealSize()`（已废弃 API，因兼容性优先），折叠屏/分屏下 70%/25% 上限
   按物理屏计算，可能与当前窗口不完全一致——未验证。

## 8. 与需求逐条对照

| # | 需求 | 实现 |
|---|---|---|
| 1 | 包名/应用名/版本/SDK/Java | 见第 1 节表格 |
| 2 | 纯文件状态源，1 s 轮询，读不到显示 `(等待状态…)` | `StatusReader.read()` + `OverlayService.pollRunnable`(1000 ms)，无 socket/广播/无障碍 |
| 3 | 悬浮窗类型/flag/外观/位置/拖动记忆/尺寸上限/单击/长按 | 见第 6 节 |
| 4 | 权限流程 + 前台服务 | `MainActivity`(`canDrawOverlays` / `ACTION_MANAGE_OVERLAY_PERMISSION` / 两个按钮 / 前 3 行预览) + `OverlayService` |
| 5 | 色条映射 | 见第 5 节 |
| 6 | aapt2/javac/d8/zipalign/apksigner 构建与校验 | 见第 1 节 |
| 7 | 交付路径 + 本报告 | 见第 1 节 |
