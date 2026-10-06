# 90 — GPU 驱动测试台 App 代码审查（首次上机前）

审查对象：`app/`（= Lead 所述 `/root/appsrc/`）· **只读审查，未改任何源码**
本机路径：`/storage/emulated/0/Mali驱动项目/app/`
**⚠️ 写入位置说明**：本次会话的 SSH 通道已断开（无活动连接、无已保存资源），无法写 `/root/research/`；报告写在**本机工作区** `research/90-app-code-review.md`，请 Lead 自行拷到 `/root/research/`。

**标注约定**：`[逐行核实]` = 我读了该行并按 Vulkan/Android 规范逐条核对；`[推断]` = 按经验判断，需上机确认。
**整体判断**：能编译、机制正确（loader-less ICD 路线与 `tools/vkicd_probe.c` 同源）、**但有 3 处会在真机上"第一次跑就不对"的硬伤**（A1/A2/C4），以及 1 处会让**驱动被冤枉**的判定逻辑（B1/B4）。建议按 A→B→C 顺序修完再上机。

---

## A. 会崩 / 会卡（必须先修）

### A1 `[逐行核实]` 命令缓冲重复 `vkBeginCommandBuffer`，而命令池没有 `RESET_COMMAND_BUFFER_BIT` —— **bench 循环里每秒上百次**
- **位置**：`jni/gputest.c:175`（建池，`flags` 未设）；使用点 `:221`（readback）、`:311`（tri）、`:384/:393`（fill）、`:423/:433`（blit）、`:464`（draw）
- **为什么出问题**：没有 `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` 时，**已 record 的命令缓冲不能再次 begin**（VUID-vkBeginCommandBuffer-commandBuffer-00049）；必须先 `vkResetCommandBuffer`（也要求该标志）或整池 reset。真机上表现为：部分驱动直接返回错误（被你的 `fails++` 吞掉、看着像"驱动不稳"），部分驱动**未定义行为/崩**。
- **修法**（两处，照抄）：
```c
/* :175 */
VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,   /* ← 加这一行 */
    .queueFamilyIndex = c->qfam };
```
并把各处 `begin(t->cb, &bi);` 改为检查返回值（`readback:221` 现在**完全忽略**返回值，随后 `end` 一个未进入 recording 状态的 CB 又是一次违规）：
```c
if (begin(t->cb, &bi) != VK_SUCCESS) return 0;
```
（`RESET_COMMAND_BUFFER_BIT` 已足够让重复 begin 合法，无需显式 ResetCommandBuffer；但显式 `vkResetCommandBuffer(t->cb, 0)` 更稳。）

### A2 `[逐行核实]` `dlclose` 驱动 .so 时设备/队列/对象仍存活，且同进程会切换驱动
- **位置**：`jni/gputest.c:106` `ctx_close`（只有 `dlclose`，没有任何 `vkDestroy*`/`vkDeviceWaitIdle`）；调用点 `:241`（smoke）、`:350`（tri）、`:550`（bench）、`:593`（benchAll）
- **为什么出问题**：① 驱动内部线程/延时回调可能仍在执行其代码段 ⇒ 卸载即崩；② 系统 loader **从不**卸载 ICD，正是为此；③ 同一进程 `dlopen` 第二份驱动（A/B 对比是这 App 的核心用法）会与第一份的全局状态/符号冲突；④ 卸载后重新 `dlopen` 同一 .so 可能拿到残留状态。你 Java 侧"换个驱动再点一次"就会走到这里。
- **修法**（最小、且与你的用法一致——**不要卸载**）：
```c
static void ctx_close(Ctx *c) {
    /* 不 dlclose：① 驱动可能仍有线程/回调引用其代码段；② loader 也从不卸载 ICD；
     * ③ 同进程要切换驱动做 A/B 时，卸载/重载是经典崩溃源。让进程退出时由内核回收。 */
    c->h = NULL;
}
```
若将来一定要卸载：先 `vkDeviceWaitIdle(dev)` → 销毁全部对象 → `vkDestroyDevice` → `vkDestroyInstance`，且**同一 .so 只 dlopen 一次并缓存 handle**。

### A3 `[逐行核实]` `stageDriver` 每次都覆盖同一个 `files/drv.so`（就地覆写已 mmap 的文件）
- **位置**：`src/com/dsh/gputest/MainActivity.java:270`
- **为什么出问题**：若上一次 `dlopen` 的映射仍活着（采用 A2 的修法后**一定**活着），**就地覆写一个已被 mmap 的文件**会让正在执行的代码页变脏 ⇒ 崩/随机错误。Linux 上"写已映射的可执行文件"是未定义行为。
- **修法**：按内容哈希命名 + 原子改名 + 已存在即复用：
```java
private String stageDriver(String src) {
    if (src == null) return null;
    try {
        String tag = sha16(new File(src));
        File dst = new File(getFilesDir(), "drv-" + tag + ".so");
        if (dst.exists() && dst.length() > 0) return dst.getAbsolutePath();   /* 复用，绝不覆写 */
        File tmp = new File(getFilesDir(), "drv-" + tag + ".tmp");
        try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        if (!tmp.renameTo(dst)) { tmp.delete(); return null; }
        dst.setReadable(true, true);
        return dst.getAbsolutePath();
    } catch (Throwable t) { log("  ✗ 暂存驱动失败: " + t); return null; }
}
```
（不再需要 `setExecutable`：`dlopen` 只要求可读；执行位在 app 私有目录无影响。）

### A4 `[逐行核实]` `fullLog`（StringBuilder）跨线程写入、与导出并发
- **位置**：`MainActivity.java:63`（字段）、`:391-395`（`log()` 从 worker 线程调用，见 `:286-295`/`:308-317`）、`:342`（`exportLog` 里 `toString()`）
- **为什么出问题**：`StringBuilder` 非线程安全；native 测试在 `new Thread(...)` 里跑，`log()` 又被多个线程调用 ⇒ 并发 append/toString 可能抛 `ArrayIndexOutOfBoundsException` 或产生错乱日志（错乱日志会让你误读测试结论）。
- **修法**：
```java
private void log(String s) {
    final String line = new SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(new Date()) + "  " + s;
    synchronized (fullLog) { fullLog.append(line).append('\n'); }
    ui.post(() -> { if (logView != null) logView.append(line + "\n"); });
}
// exportLog 里：
String dump; synchronized (fullLog) { dump = fullLog.toString(); }
os.write(dump.getBytes("UTF-8"));
```

---

## B. 会错 / 会让驱动被冤枉（Vulkan 正确性）

### B1 `[逐行核实]` `bench_blit` 的两个 layout 转换 barrier **完全没有 accessMask** —— 你猜的这条是真的
- **位置**：`jni/gputest.c:415-421`（`bs[0]`、`bs[1]` 都只有 `oldLayout/newLayout`，`srcAccessMask`/`dstAccessMask` 均为 0）
- **为什么出问题**：从 `UNDEFINED` 转换到 `TRANSFER_SRC/DST_OPTIMAL` 时，**必须**给出 `dstAccessMask`（`TRANSFER_READ`/`TRANSFER_WRITE`）才能让后续 copy 的读/写与 layout 转换建立可用性；缺失时验证层报 `SYNC-HAZARD`，真机上可能读到未初始化数据（表现为"blit 全黑/花"）⇒ **错误会算在驱动头上**。
- **修法**（照抄替换 `bs` 定义）：
```c
VkImageMemoryBarrier bs[2] = {
    { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,                                  /* old = UNDEFINED，无需 src */
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = a->img, .subresourceRange = rg },
    { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = b->img, .subresourceRange = rg } };
```
（`bench_fill:379-385` 的 `b0` **是对的**：`srcAccessMask` 省略（=0，old=UNDEFINED）、`dstAccessMask=TRANSFER_WRITE`、`srcStage=TOP_OF_PIPE→dstStage=TRANSFER` ✓ `[逐行核实]`）

### B2 `[逐行核实]` bench 循环内同一图像连续写（fill：每轮 8 次 clear 同一图；blit：每轮 4 次拷进同一目标）
- **位置**：`:395`（fill，`per=8`）、`:434-435`（blit，`per=4`）
- **判断**：**跨轮次没问题**——你每轮都 `submit_wait`（fence 等待 + reset），轮与轮之间有完整同步 ✓；**同一轮内**多次写同一图像属 WAW，规范上仍需 barrier 才算合法（验证层会报 hazard）。因为写的是同一内容/同一区域，**结果通常不错**，所以定级为"会错（验证层报错会误导判断）"而不是"结果错"。
- **修法（不改变测吞吐的性质）**：在每轮的 `for` 之前加一次 `VkMemoryBarrier`（transfer→transfer，`MEMORY_WRITE|MEMORY_READ`），或在同一轮内每 `per` 次后加一次。给最小版：
```c
VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT };
/* 每轮 begin 之后、首个 clear/copy 之前： */
bar(t->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
```

### B3 `[逐行核实]` `readback` 假定图像已在 `TRANSFER_SRC_OPTIMAL`，自己不做转换
- **位置**：`:223`（`cp(..., VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ...)`）
- **现状**：tri 路径恰好成立（render pass `finalLayout = TRANSFER_SRC_OPTIMAL`，`:160` ✓）；但 `readback` 是公共函数，若将来用于 `bench_fill` 之后的图像（处于 `TRANSFER_DST_OPTIMAL`）即违规 ⇒ 会误报"回读失败"。
- **修法**（最小可用：给 `Target` 加 layout 跟踪）：
```c
/* Target 结构体加： */ VkImageLayout curLayout;   /* 初值 VK_IMAGE_LAYOUT_UNDEFINED */
/* readback 开头插 barrier： */
VkImageMemoryBarrier to_src = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    .oldLayout = t->curLayout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = t->img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
VkMemoryBarrier mb = { ... };   /* 见 B2 */
bar(t->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
    0, 1, &mb, 0, NULL, 1, &to_src);
t->curLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
```
（各路径也要相应更新 `curLayout`：tri/draw 的 render pass 结束 ⇒ `TRANSFER_SRC_OPTIMAL`；fill ⇒ `TRANSFER_DST_OPTIMAL`；blit 的 b ⇒ `TRANSFER_DST_OPTIMAL`。）

### B4 `[逐行核实]` `nativeTri` 的判定会误判（**这是"会冤枉驱动"的最大一处**）
- **位置**：`:328-344`（采样点假设 + `red > 0 && other == 0` ⇒ "绘制正确"）
- **两个问题**：
  1. **没要求 `black > 0`** ⇒ 若清屏色变成红、或三角形铺满整屏（恰恰是渲染 bug 的典型形态），"全红画面"会被判成 **正确** ✗；
  2. **`[12]` 的采样点 `(W/2, H*0.75)` 建立在"三角形覆盖左下大三角"的假设上**（`:330-331` 注释）；若 `shaders.h` 里 `tri.vert` 的顶点位置不是这样，就会打印黑点并判"无绘制输出" ⇒ 冤枉驱动。
- **修法（改为"按实测包围盒判定"，不再依赖假设）**：
```c
long red = 0, black = 0, other = 0;
int minx = (int)t.W, miny = (int)t.H, maxx = -1, maxy = -1;
for (uint32_t y = 0; y < t.H; y++)
  for (uint32_t x = 0; x < t.W; x++) {
    unsigned char *q = p + ((size_t)y * t.W + x) * 4;
    if (q[0] > 200 && q[1] < 60 && q[2] < 60) { red++; if ((int)x<minx)minx=(int)x; if((int)x>maxx)maxx=(int)x;
                                                if ((int)y<miny)miny=(int)y; if((int)y>maxy)maxy=(int)y; }
    else if (q[0] < 40 && q[1] < 40 && q[2] < 40) black++;
    else other++;
  }
long total = (long)t.W * t.H;
LOG("[12] 红像素: %ld (%.1f%%) 包围盒=(%d,%d)-(%d,%d)  黑=%ld  其它=%ld\n",
    red, 100.0*red/total, minx, miny, maxx, maxy, black, other);
LOG("\n== 判定: %s ==\n",
    (red > 0 && black > 0 && other == 0) ? "绘制正确（纯红三角形 + 纯黑背景）" :
    (red > 0 && other == 0)              ? "只有红/黑两色但缺背景 —— 可疑（请贴图）" :
    (red > 0)                            ? "有输出但存在异常像素（可能正是渲染 bug）" :
                                           "无绘制输出");
```
（256×256 = 65536 px，全量扫描代价可忽略；顺便把中心点 `[12]` 保留作为参考值。）

### B5 `[逐行核实]` `pick_mem` 的两级回退可能给出**不满足 HOST_VISIBLE** 的类型；无匹配时返回 0
- **位置**：`:115-124`（第二级"只要 bits 命中就返回"；`return 0`）；调用点 `:144`（图像，要 DEVICE_LOCAL）、`:215-216`（回读缓冲，要 HOST_VISIBLE|HOST_COHERENT）
- **为什么出问题**：回读缓冲可能拿到 DEVICE_LOCAL-only 类型 ⇒ `vkMapMemory` 失败 ⇒ 日志写"映射回读缓冲失败"，看起来像驱动问题；`return 0` 在"bits 不含 bit0"时让 `vkAllocateMemory` 必然失败，同样误导。
- **修法**：
```c
static uint32_t pick_mem(Ctx *c, uint32_t bits, VkMemoryPropertyFlags want, const char *tag) {
    PFN_vkGetPhysicalDeviceMemoryProperties gm = (PFN_vkGetPhysicalDeviceMemoryProperties)
        c->gipa(NULL, "vkGetPhysicalDeviceMemoryProperties");
    if (!gm) { LOG("  X %s: 拿不到内存属性\n", tag); return UINT32_MAX; }
    VkPhysicalDeviceMemoryProperties mp; gm(c->pd, &mp);
    int32_t any = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        if (any < 0) any = (int32_t)i;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) {
            LOG("  内存类型 %u (flags=0x%x) 满足 0x%x  [%s]\n", i, mp.memoryTypes[i].propertyFlags, want, tag);
            return i;
        }
    }
    if (any >= 0) { LOG("  ! %s 无理想类型(0x%x)，退化到 %d (flags=0x%x)\n",
                        tag, want, any, mp.memoryTypes[any].propertyFlags); return (uint32_t)any; }
    LOG("  X %s 没有任何可用内存类型 (bits=0x%x)\n", tag, bits);
    return UINT32_MAX;
}
```
调用方：`if (idx == UINT32_MAX) return 0;`（把失败原因说清楚，而不是让 `vkAllocateMemory` 报一个含糊错误）。

### B6 `[逐行核实]` 队列族：未检查 `queueCount`；找不到图形族仍用 `qfam=0`
- **位置**：`:86-89`（`if (qfs[i].queueFlags & GRAPHICS) { qfam=i; found=1; }`，未看 `queueCount`）、`:89` 日志"否(退化)"，但后续**照旧**用 `qfam` 建管线（`:96`、`:301`）
- **为什么出问题**：① 图形族 `queueCount == 0` 时 `vkGetDeviceQueue(...,0)` 拿到无效队列 ⇒ 后续崩/错；② 没有图形族时（纯 compute 驱动、或被裁剪的 ICD），管线创建必失败，但日志只说"退化" ⇒ 结论会被误读成"驱动画不出来"。
- **修法**：
```c
c->qfam = 0; int found = 0;
for (uint32_t i = 0; i < nq; i++)
    if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfs[i].queueCount > 0) { c->qfam = i; found = 1; break; }
LOG("[5] 队列族 %u 个；选用 #%u (图形=%s, queueCount=%u)\n", nq, c->qfam,
    found ? "是" : "否", found ? qfs[c->qfam].queueCount : 0);
if (!found) { LOG("X 该驱动没有可用的图形队列族 —— 图形测试无意义\n"); return 0; }
```
（若要保留"退化可跑非图形测试"，就把 `found` 存进 `Ctx`，让 `nativeTri`/`bench_draw` 明确跳过并说明原因。）

### B7 `[逐行核实]` `codeSize = _len * 4` 的约定 —— **核对为正确** ✅
`jni/shaders.h:42` `tri_vert_spv_len = 284`、`:56` `tri_frag_spv_len = 88`，而两个数组是 `static const uint32_t tri_vert_spv[]`（`:4`/`:43`）⇒ `_len` 是 **uint32 字数**，`*4` 得 1136/352 字节 ✓ 与"38 行/13 行 × 每行 8 字"量级吻合 ✓ **不要改这里**。
**建议加保险**（防有人手改数组忘改长度）：
```c
_Static_assert(sizeof(tri_vert_spv)/4 == 284, "tri_vert_spv_len 与数组不一致");
_Static_assert(sizeof(tri_frag_spv)/4 ==  88, "tri_frag_spv_len 与数组不一致");
```

### B8 `[逐行核实]` `submit_wait` 在等待失败后仍然 `ResetFences`，且不区分超时与 DEVICE_LOST
- **位置**：`:190-194`
- **为什么出问题**：`wf` 返回 `VK_TIMEOUT(-2)`/`VK_ERROR_DEVICE_LOST(-4)` 后仍 reset fence；**-4 与 -2 是完全不同的结论**（"驱动崩了" vs "太慢/卡住"），而当前日志只写"失败/超时" ⇒ 把最关键的证据糊掉了。另外超时后那次提交仍在飞行，下一轮再用同一个命令缓冲就会撞 A1。
- **修法**：
```c
r = wf(c->dev, 1, &t->fence, VK_TRUE, 3000000000ull);
if (r == VK_SUCCESS) {
    rf(c->dev, 1, &t->fence);
} else {
    LOG("  ! fence 等待返回 %d（%s）\n", r,
        r == VK_TIMEOUT ? "超时：GPU 未在 3s 内完成" :
        r == VK_ERROR_DEVICE_LOST ? "DEVICE_LOST：驱动/GPU 已失联（重要证据，请保存日志）" : "其它错误");
    /* 不 reset：让 fence 状态如实保留，便于现场判断 */
}
return r;
```

### B9 `[逐行核实]` `bench_draw` 的 `pixels_per_op` 语义错误 ⇒ 报告里的 "Mpixel/s" 会误导
- **位置**：`:461`（`st->pixels_per_op = W*H`），报告处 `:364-366`
- **为什么**：draw 模式实际统计的是**三角形个数**，但 `bench_report` 会把它乘上 `W*H` 打印成像素吞吐 ⇒ 数字虚高且无意义（会被人拿去和 fill 比）。
- **修法**：draw 模式把 `st->pixels_per_op = 0`（`bench_report` 里 `if (pixels_per_op > 0)` 就不会打印像素行 ✓，`:364`），并在日志里显式说明"draw 口径 = 三角形/s"。

### B10 `[逐行核实]` 失败时仍打印吞吐数字 ⇒ 会被截图误读
- **位置**：`:534-539`/`:543-544`（先 `bench_report(...)` 再判 `ok`）、`:546`
- **修法**：把 `bench_report` 调用移到 `ok` 判断之后，或在 `bench_report` 里传入 `ok` 并在为假时先打一行 `（本次有失败，下列数字仅供参考/不可用）`。

---

## C. 会误导（结论/功能层面）

### C1 `[逐行核实]` `/sdcard` 扫描在 targetSdk 34 上**必然为空**，而 UI 声称能扫；`ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION` **在代码里根本不存在**
- **位置**：`MainActivity.java:137-149`（扫 `/sdcard/Mali驱动项目/驱动`、`/sdcard/Mali驱动项目/.probe`、`/sdcard/Download`）；`AndroidManifest.xml:7,9,10`（minSdk 26 / targetSdk 34，声明了 `READ/WRITE_EXTERNAL_STORAGE`，**没有** `MANAGE_EXTERNAL_STORAGE`）
- **为什么出问题**：API 30+ 分区存储 ⇒ 直接访问 `/sdcard/<任意目录>` 被拒，`listFiles()` 返回 `null` ⇒ 你代码里 `if (fs == null) continue;` **静默跳过** ⇒ 用户看到"共 0 个候选"，会以为是"设备上没有驱动"。`WRITE_EXTERNAL_STORAGE` 在 30+ 被忽略；`READ_EXTERNAL_STORAGE` 在 33+ 对非媒体目录无效。
- **修法（推荐 SAF；若要全盘访问则按第二段）**：
```java
/* 方案 A（推荐，零权限）：让用户选目录，用 SAF 遍历 */
private static final int REQ_TREE = 1002;
private void pickDriverTree() {
    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
    i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
    try { startActivityForResult(i, REQ_TREE); } catch (Throwable t) { log("  ✗ " + t); }
}
/* onActivityResult 里：DocumentFile.fromTreeUri(this, uri).listFiles() 挑 .so，再把 uri 拷到 filesDir */
```
```xml
<!-- 方案 B：manifest 加 -->
<uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE" />
```
```java
/* 方案 B：扫描前检查并跳转设置页 */
if (android.os.Build.VERSION.SDK_INT >= 30 && !android.os.Environment.isExternalStorageManager()) {
    log("  ! 需要「所有文件访问」权限，正在跳转设置…");
    try { startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:" + getPackageName()))); } catch (Throwable t) { log("  ✗ " + t); }
    return;
}
```
**另外**：把"目录不存在 / 无权限 / 目录里没有 .so"**分开打印**（现在是三者都静默）✓
**并且**：`getExternalFilesDir(null)/drivers`（`:139`）**不需要任何权限**，建议在 UI 上直接告诉用户："把 .so 放到 `Android/data/com.dsh.gputest/files/drivers/`" ——这是**最可靠的喂驱动方式** ✓

### C2 `[逐行核实]` APK 提取"挑最大 .so"会挑错（而且挑错了看不出来）
- **位置**：`MainActivity.java:227-229`
- **为什么**：驱动 APK 里若还有更大的无关 .so，或同时含 `arm64-v8a` 与 `armeabi-v7a`（v7a 的那个未必更小），就会提取错的那一个 ⇒ `dlopen` 失败 ⇒ 日志写"不是可加载 ICD" ⇒ **看起来像驱动坏了** ✗
- **修法**（ABI + 名字双重打分，并把候选全打印）：
```java
ZipEntry best = null; long bestScore = -1; StringBuilder cand = new StringBuilder();
java.util.Enumeration<? extends ZipEntry> e = z.entries();
while (e.hasMoreElements()) {
    ZipEntry en = e.nextElement();
    String n = en.getName(); String low = n.toLowerCase(Locale.ROOT);
    if (!low.startsWith("lib/") || !low.endsWith(".so")) continue;
    boolean arm64 = low.startsWith("lib/arm64-v8a/");
    boolean looksIcd = low.contains("vulkan") || low.contains("panfrost")
                    || low.contains("freedreno") || low.contains("icd") || low.contains("mali");
    long score = (arm64 ? 1L << 40 : 0) + (looksIcd ? 1L << 39 : 0) + en.getSize();
    cand.append("\n      · ").append(n).append("  ").append(en.getSize())
        .append(arm64 ? "  [arm64]" : "  [非arm64]").append(looksIcd ? " [像ICD]" : "");
    if (score > bestScore) { bestScore = score; best = en; }
}
log("    APK 内 .so 候选:" + cand);
```
（`[非arm64]` 的候选即便更大也不会被选中 ✓；日志里能看到全部候选 ⇒ 挑错立刻可查。）

### C3 `[逐行核实]` `installApk` 与 `exportLog` 用 `file://` URI ⇒ 在 targetSdk 34 上**必然失败**
- **位置**：`:256`（`Uri.fromFile(apk)` + `FLAG_GRANT_READ_URI_PERMISSION`）、`:346`（`EXTRA_STREAM` 用 `Uri.fromFile`）
- **为什么**：API 24+ 对 `file://` URI 抛 `FileUriExposedException`；你 catch 住了 ⇒ **不崩，但"安装 APK"和"分享日志"这两个功能永远不会成功**，只在日志里留一行。
- **修法（二选一）**：
  - **省事版（推荐，与 C1 的 SAF 一致）**：导出日志改用 SAF：
```java
Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
i.addCategory(Intent.CATEGORY_OPENABLE);
i.setType("text/plain");
i.putExtra(Intent.EXTRA_TITLE, "gputest-" + new SimpleDateFormat("MMdd-HHmmss", Locale.ROOT).format(new Date()) + ".log");
startActivityForResult(i, REQ_SAVE_LOG);
/* onActivityResult 里用 getContentResolver().openOutputStream(uri) 写入 */
```
  - **FileProvider 版**：需要 manifest 加 `<provider>` + `res/xml/file_paths.xml`，**并且 `build_app.sh` 的 `aapt2 link` 必须加 `-R res/`**（现在完全没有资源，见 D3）⇒ 改三处，成本更高。
  - 安装 APK 同理：用 `ACTION_VIEW` + `FileProvider` content URI（或让用户用文件管理器安装，并把"替代方案"从 catch 里提到正常日志里）。

### C4 `[逐行核实]` ★`sha16` **必然返回 "?"**（`md.digest()` 在循环里被反复调用）
- **位置**：`MainActivity.java:353-364`，关键行 `:361`
```java
for (int i = 0; i < 8; i++) sb.append(String.format("%02x", md.digest()[i]));
```
- **为什么**：`MessageDigest.digest()` **返回摘要并重置**；第二次调用返回**空数组** ⇒ `md.digest()[1]` 抛 `ArrayIndexOutOfBoundsException` ⇒ 被 `catch (Throwable) { return "?"; }` 吞掉 ⇒ **每次测试头部打印的 `sha256(16)=?` 永远是 `?`** ✗（而它正是"这个 .so 到底是哪个版本"的唯一标识 ⇒ 日志的可用性大打折扣）
- **修法**：
```java
byte[] d = md.digest();
for (int i = 0; i < 8 && i < d.length; i++) sb.append(String.format("%02x", d[i]));
```
**注意**：`sha16` 还被 `stageDriver`（我的 A3 修法）用作文件名标签 ⇒ 修好它同时修好驱动缓存命名 ✓

### C5 `[逐行核实]` `findSoIn` 的 "名字含 vulkan 就返回" 可能选到非 ICD 的 `libvulkan.so`
- **位置**：`MainActivity.java:176`
- **修法**：与 C2 同口径——优先 `panfrost|freedreno|icd|adreno`，其次 `vulkan`，且**必须 arm64**（`nativeLibraryDir` 只有一个 ABI，可直接取，但插件包里可能有多个 .so）：
```java
File best = null; int bestRank = -1;
for (File f : fs) {
    String n = f.getName().toLowerCase(Locale.ROOT);
    if (!n.endsWith(".so")) continue;
    int rank = (n.contains("panfrost") || n.contains("freedreno") || n.contains("adreno")) ? 3
             : n.contains("icd") ? 2 : n.contains("vulkan") ? 1 : 0;
    if (rank > bestRank || (rank == bestRank && (best == null || f.length() > best.length()))) {
        bestRank = rank; best = f;
    }
}
```

### C6 `[逐行核实]` `readback` 之后从不 `vkUnmapMemory`，且**所有 Vulkan 对象从不销毁** ⇒ 反复点按钮会累积泄漏
- **位置**：`:226`（map 后不 unmap）；整个文件没有一处 `vkDestroy*`/`vkFreeMemory`/`vkFreeCommandBuffers`
- **判断**：**"短进程内可接受"在本 App 里不成立** —— Java 侧允许你反复点 ④⑤⑥⑩（同一进程、同一驱动、甚至不同驱动）⇒ 每次泄漏 ≈ 图像 4 MiB（1024²×4）+ 回读缓冲 4 MiB + 若干对象；点十几次就可能 OOM 或撞驱动内存上限，**而且会把"驱动问题"与"我们自己泄漏"混在一起** ✗
- **最小修法**：加一个 `target_free()`，在每次 native 入口结束前调用（顶层对象销毁顺序：fence → pool(cb 随池) → fb/view/rp → buffer/mem → image/mem）：
```c
static void target_free(Ctx *c, Target *t) {
    PFN_vkDestroyFence df = (PFN_vkDestroyFence) c->gdpa(c->dev, "vkDestroyFence");
    PFN_vkDestroyCommandPool dp = (PFN_vkDestroyCommandPool) c->gdpa(c->dev, "vkDestroyCommandPool");
    PFN_vkDestroyFramebuffer dfb = (PFN_vkDestroyFramebuffer) c->gdpa(c->dev, "vkDestroyFramebuffer");
    PFN_vkDestroyRenderPass drp = (PFN_vkDestroyRenderPass) c->gdpa(c->dev, "vkDestroyRenderPass");
    PFN_vkDestroyImageView dv = (PFN_vkDestroyImageView) c->gdpa(c->dev, "vkDestroyImageView");
    PFN_vkFreeMemory fm = (PFN_vkFreeMemory) c->gdpa(c->dev, "vkFreeMemory");
    PFN_vkDestroyBuffer db = (PFN_vkDestroyBuffer) c->gdpa(c->dev, "vkDestroyBuffer");
    PFN_vkDestroyImage di = (PFN_vkDestroyImage) c->gdpa(c->dev, "vkDestroyImage");
    PFN_vkUnmapMemory um = (PFN_vkUnmapMemory) c->gdpa(c->dev, "vkUnmapMemory");
    PFN_vkDeviceWaitIdle wi = (PFN_vkDeviceWaitIdle) c->gdpa(c->dev, "vkDeviceWaitIdle");
    if (wi) wi(c->dev);                       /* 保证没有在飞的提交 */
    if (um && t->bmem) um(c->dev, t->bmem);
    if (db && t->buf) db(c->dev, t->buf, NULL);
    if (fm && t->bmem) fm(c->dev, t->bmem, NULL);
    if (df && t->fence) df(c->dev, t->fence, NULL);
    if (dp && t->pool) dp(c->dev, t->pool, NULL);
    if (dfb && t->fb) dfb(c->dev, t->fb, NULL);
    if (drp && t->rp) drp(c->dev, t->rp, NULL);
    if (dv && t->view) dv(c->dev, t->view, NULL);
    if (di && t->img) di(c->dev, t->img, NULL);
    if (fm && t->mem) fm(c->dev, t->mem, NULL);
    t->img = t->view = t->rp = t->fb = t->pool = t->cb = t->fence = t->buf = NULL;
    t->mem = t->bmem = NULL;
}
```
**若不想大改**：至少在 Java 侧每次测试前提示"建议先杀掉 App"（`:284` 附近），并**不要** dlclose（A2）✓

### C7 `[推断]` 日志截断可能切在多字节 UTF-8 中间
- **位置**：`jni/gputest.c:15-16`（`LOG` 宏在 128 KiB 处截断）、`:230`（`NewStringUTF(g_log)`）
- **风险**：`NewStringUTF` 收到非法 modified-UTF-8 是未定义行为（可能 abort）。当前日志量看样子到不了 128 KiB，属低概率 ⇒ 若想稳妥，截断时回退到最后一个完整 UTF-8 序列边界（或把上限提到 512 KiB 并同样处理）✓
- 另外 `LOG` 宏本身的算术**是对的**（`[逐行核实]`：`_n < remaining ? _n : remaining-1`，且 `snprintf` 返回"将要写的长度"）✓

### C8 `[推断]` "三角形覆盖左下大三角"是假设（与 `shaders.h` 的实际顶点需核对）
- **位置**：`:328-331`；我没有反汇编 SPIR-V 去确认顶点位置 ⇒ 标为推断。按 B4 改成包围盒判定后，这条自动失效 ✓

---

## D. 仅风格 / 工程性

- **D1** `[逐行核实]` `pickFile(String type, int req)`（`:186-193`）**忽略了 `type`**（硬编码 `*/*`）⇒ 参数没用、误导读者；要么用它 `i.setType(type)`，要么删掉形参。
- **D2** `[逐行核实]` `finish(JNIEnv*, path, jso)`（`gputest.c:230`）两个参数没用；且 `ReleaseStringUTFChars(env, jso, path)` 在 `path` 可能为 NULL（`GetStringUTFChars` 失败）时未判空（`:242/:351/:551/:594`）⇒ 加 `if (path)`。
- **D3** `[逐行核实]` `build_app.sh`：
  - `aapt2 link` **没有 `-R res/`、`--java`、`-A assets`** ⇒ 目前无资源可用；**若采纳 C3 的 FileProvider 方案必须加 `-R res/`**，否则运行时找不到 provider ✓
  - 建议显式加 `--min-sdk-version 26 --target-sdk-version 34`（与 `AndroidManifest.xml:7` 和 `d8 --min-api 26` 对齐；现在靠 manifest 隐式一致 ✓ 但缺 `<uses-sdk>` 时会默认 1）
  - `.so` 用 `ZIP_DEFLATED` 写入（`:42-47`），与默认 `extractNativeLibs=true` **兼容** ✓（安装时解压到 `nativeLibraryDir`，`System.loadLibrary` 正常）；**但**若将来有人设 `extractNativeLibs="false"`，必须改为 **`ZIP_STORED` + 4 KiB 页对齐**（`zipalign -p 4` 已做对齐 ✓ 但压缩会让它失效）⇒ 建议现在就把 .so 改成不压缩：
```python
z.write(so, so, compress_type=zipfile.ZIP_STORED)   # 更快加载、且未来兼容 extractNativeLibs=false
```
  - `javac ... 2>&1 | grep -v "^Note:" || true` 会把 javac 的报错也过滤掉 ⇒ 失败时几乎看不到原因（靠下一步 `[ -d "$W/classes/com" ]` 兜住 ✓ 但体验差）⇒ 改为 `set -o pipefail` + `tee "$W/javac.log"`。
  - Java 里写 "v0.2"、manifest 是 `versionName 0.1`、产物名 `GPUTest-0.1.apk` ⇒ 版本号不一致（风格）。
- **D4** `[逐行核实]` Java 按钮编号顺序是 ①②③④⑤⑥⑥b⑥c⑩⑦⑧⑨（`:80-97`）⇒ 用户按编号找会迷路；建议重排为 ①②③④⑤⑥⑦⑧⑨⑩（把"全流程"放最后）。
- **D5** `[逐行核实]` `scanDrivers()` 在 `onCreate` 里同步调用（`:113`），其中 `pm.getInstalledPackages(...)` 在**主线程**跑 ⇒ 冷启动可能卡几百毫秒（ANR 风险）⇒ 挪到 worker 线程（`log()` 已线程安全化的话）。
- **D6** `[逐行核实]` `copyUriToCache`（`:203-213`）没判 `openInputStream` 返回 null（`:206`）⇒ 靠外层 catch 兜住（不崩，但错误信息是 NPE，读起来像代码 bug）⇒ 加 `if (in == null) { log("  ✗ 无法打开该 URI"); return null; }`。

---

## E. 首次上机验收清单（按顺序点，逐步对照）

> 前置：`adb install -r GPUTest-0.1.apk`（或从 `/data/dsh_downloads/` 拿包）；**每次测试前建议 `adb shell am force-stop com.dsh.gputest`**（规避 C6/A2 的累积效应）。
> **修完 A1/A3/A4/C4 再开始**，否则第 4 步之后的结论不可信。

| 步 | 操作 | 应看到 | 出现什么就是错的 |
|---|---|---|---|
| 1 | 打开 App | 标题 + 说明 + 自动扫描结果；底部日志显示"启动。点「① 扫描驱动」开始" | 直接闪退 ⇒ 看 `adb logcat | grep -E "gputest|DEBUG"`；若崩在 `System.loadLibrary` ⇒ `extractNativeLibs`/ABI 问题（D3） |
| 2 | 点 ① | 列出插件包驱动（若装了驱动插件 APK）；`共 N 个候选` | `共 0 个候选` 时**先看是不是 C1**（`/sdcard` 被分区存储挡住 = 预期内，不是没驱动）⇒ 改用第 3 步 |
| 3 | 点 ② 选驱动 APK | `APK: N 字节` → `APK 内 .so 候选:` 列表 → `✓ 提取 xxx.so` → `sha256(16)=<8字节>` | **`sha256(16)=?` ⇒ C4 没修** ✗；提取到的不是 `libvulkan_*` ⇒ C2 |
| 4 | 点 ④ 驱动自测 | `[1] dlopen 成功` → `[2] negotiate=有 gipa=有` → `[3] vkCreateInstance → 0 OK` → `[4] 物理设备数: 1`（含驱动版本/厂商）→ `[5] 队列族 …` → `[6] vkCreateDevice → 0 OK` → `[7] 设备与队列就绪` → `== 判定: 驱动可用 ==` | `[1]` 失败且 `dlerror` 含 `cannot locate symbol`/`not found` ⇒ .so 依赖或 ABI 不对；`[3]`/`[6]` 失败 ⇒ 该 ICD 与"裸调 ICD"路线不兼容（**这本身是重要结论**，请留日志） |
| 5 | 点 ⑤ 三角形 | `[8]/[9]` 着色器模块 `→ 0` → `[10] 图形管线 → 0 OK` → `[11] 提交并等待 fence → 0 完成` → `[12] 红像素 … 包围盒=(…)` → `== 判定: 绘制正确 ==` | `[11]` 为 `-4 DEVICE_LOST` ⇒ **驱动崩（最有价值的现场，立刻导出日志）**；`[11]` 为 `-2` ⇒ 超时/卡死；`红=0` ⇒ 无输出（真问题）或采样假设错（C8/B4 未修时无法区分）；`其它>0` ⇒ 渲染异常像素（正是要找的 bug） |
| 6 | 点 ⑥ fill（5 s） | `时长≈5.0s 提交=N 操作=8N 失败=0` + `吞吐=… Mpixel/s` + `分数` | `失败>0` ⇒ 先修 A1 再看；`提交` 只有个位数 ⇒ 每次提交都在等 3 s 超时（B8）⇒ 数据无效 |
| 7 | 点 ⑥b blit | 同上 + `带宽=… GB/s` | 结果全黑/花 ⇒ 先确认 B1 的 accessMask 已补（否则会冤枉驱动） |
| 8 | 点 ⑥c draw | `… 三角形/s` + 分数 | 若同时打印了 "Mpixel/s" ⇒ B9 未修（数字无意义） |
| 9 | 点 ⑩ 全流程 | 依次冒出 ①②③ 三段 + `汇总` + `综合分` + 口径说明 | 任一段 `(失败)` ⇒ 该段数字不计分（口径已是透明的 ✓）；**全流程跑完进程仍存活** ⇒ A1/A2 至少没当场炸 |
| 10 | 点 ⑧ 导出/分享 | 日志文件写到 `Android/data/com.dsh.gputest/files/`；分享面板弹出 | 只有一行 `✗ ...FileUriExposedException` ⇒ C3 未修（用 `adb pull /sdcard/Android/data/com.dsh.gputest/files/` 取） |
| 11 | 点 ⑦ 拉起启动器 | `✓ 发现: <包名>` → `→ 已拉起` | 找不到 ⇒ `QUERY_ALL_PACKAGES` 已声明 ✓，若仍找不到说明包名不含 zalith/fcl/foldcraft/pojav ⇒ 加白名单 |
| 12 | 换另一个驱动重复 4–9（A/B 对比） | 每次都从 `[1] dlopen` 开始、互不影响 | 第二次开始异常（崩/结果错乱）⇒ A2（dlclose）+ A3（覆写同一 `drv.so`）几乎必然是原因 |

---

## F. 附带：驱动线 `pan_kmod_vm_bind` 插桩 diff（可直接应用）

> 目的：一次运行点名那个 72 KiB 对象（`ops[i].map.bo`，**不是** `ops[i].bo`）。**头文件安全性**：不引 `<dlfcn.h>`；不含任何 `.c`-only 依赖。

```diff
--- a/src/panfrost/lib/kmod/pan_kmod.h
+++ b/src/panfrost/lib/kmod/pan_kmod.h
@@ static inline VkResult
 pan_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                  struct pan_kmod_vm_op *ops, uint32_t op_count)
 {
+   /* v129: 在提交前记录"va.start 是否仍是 AUTO_VA 哨兵" —— 用于区分
+    * "内核分配的 VA" 与 "我们自己算的 VA"，这是重叠/越界类问题的关键一位。 */
+   bool was_auto[8];
+   const uint32_t nauto = op_count < 8 ? op_count : 8;
+   for (uint32_t i = 0; i < nauto; i++)
+      was_auto[i] = (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP &&
+                     ops[i].va.start == PAN_KMOD_VM_MAP_AUTO_VA);
    ...
    /* 你 v126 已加的逐 op 打印，替换为： */
-            mesa_logi("... ret=%d type=%d va=[0x%" PRIx64 ", 0x%" PRIx64 ") ...",
-                      ret, (int)ops[i].type, ops[i].va.start,
-                      ops[i].va.start + ops[i].va.size);
+            mesa_logi("vb: vm=%p ret=%d type=%d was_auto=%d va=[0x%" PRIx64
+                      ", 0x%" PRIx64 ") size=0x%" PRIx64 " bo=%p bo_size=0x%" PRIx64,
+                      (void *)vm, ret, (int)ops[i].type,
+                      i < nauto ? was_auto[i] : false,
+                      ops[i].va.start, ops[i].va.start + ops[i].va.size,
+                      ops[i].va.size,
+                      (void *)ops[i].map.bo,
+                      ops[i].map.bo ? (uint64_t)ops[i].map.bo->size : 0);
```
**落地前三个检查（都在同一头文件里，一眼可见）**：
1. `ops[i].map.bo` —— 对 MAP op 有效（`pan_kmod_vm_op` 的 `map` 成员；**顶层没有 `bo`** ✓ 你已经踩过）。
2. `ops[i].map.bo->size` —— 若结构字段名不是 `size`，改用 `pan_kmod_bo_size(ops[i].map.bo)`；**注意该 helper 必须在本 inline 之前已声明**，否则用 `->size`。
3. `bool`/`PRIx64`/`mesa_logi` —— 均由你现有打印证明可用 ✓；`was_auto[8]` 的边界用 `nauto` 显式夹住（不依赖 `ARRAY_SIZE` 是否可见）。
**判读**：`bo_size=0x11000` 而 `size=0x12000` ⇒ 绑定比 BO 多一页；`was_auto` 有 1 有 0（同一 `vm`）⇒ 两个 VA 分配器各自为政（重叠的根因）；`vm` 不同 ⇒ 那些区间根本不是重叠 ✓

---

## G. 本轮"逐行核实 / 推断"对照

- **[逐行核实]**（我读了该行并按规范核对）：A1–A4、B1–B10、C1–C6、C7 的 `LOG` 宏算术、C8 之外的项、D2/D3/D4/D5/D6、**B7 的 `_len*4` 约定（核对为正确）**、manifest 的 minSdk/targetSdk/权限/`debuggable`。
- **[推断]**（需上机或需读 SPIR-V 才能定）：C8（三角形几何假设）、C7（截断切 UTF-8 的实际概率）、以及"某驱动在 A1 下会崩还是会返回错误"（取决于实现）。
- **未做**：真机运行、`adb logcat`、SPIR-V 反汇编（`shaders.h` 的顶点位置）、以及 `tools/vkicd_probe.c` 的逐行对照（Lead 说可对照，但本次 SSH 断开，我读的是本机 `app/` 副本）。
