package com.dsh.gputest;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.text.method.ScrollingMovementMethod;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * GPU 驱动测试台 v2.0 —— 六个页签：
 *   [概览] 设备/GPU/驱动概要 + 综合分（大字号）+ 快捷操作
 *   [驱动] 扫描（插件包 / 本地 .so）+ 从 APK 提取 + 会话式安装 + 系统驱动对照
 *   [测试] ICD 冒烟 · 三角形像素校验 · 能力清单（nativeCaps）
 *   [画面] 实时渲染画面（nativeRenderInit/Frame/Blit/Stats/Stop）
 *   [跑分] fill / blit / draw / 一键全流程 / A-B 对比（分数超大字号）
 *   [日志] 实时日志 + 读取启动器日志 + 导出分享 + 清空
 *
 * 硬约束：仅平台 API（无 androidx / 无 Gradle / 无新增 XML 资源）；javac --release 11 + d8；
 * 原生方法签名一字不改；只用 Java 8 及以前的库 API（Java 9+ 的 String.repeat/isBlank/List.of
 * 等 d8 不脱糖，API 26 上会 NoSuchMethodError）。
 */
public class MainActivity extends Activity {

    static { System.loadLibrary("gputest"); }

    /* ---------------- 原生接口（签名与 jni/gputest.c 严格一致，勿改） ---------------- */
    public native String nativeIcdSmoke(String soPath);
    public native String nativeTri(String soPath);
    public native String nativeBench(String soPath, int seconds, String mode);
    public native String nativeBenchAll(String soPath, int seconds);
    public native String nativeRenderInit(String soPath, int w, int h);
    public native String nativeRenderFrame(int frameIndex);
    public native String nativeRenderBlit(Bitmap bmp);
    public native String nativeRenderStats();
    public native void   nativeRenderStop();
    public native String nativeCaps(String soPath);
    public native String nativeLastStep();   /* 看门狗：返回"当前卡在哪一步" */
    public native String nativeClassify(String soPath);  /* 真 dlopen+dlsym 分类：ICD / 垫片 / 渲染器 / 打不开 */
    /* v5.1 压力测试（原生尚未接入时这些调用会抛 UnsatisfiedLinkError ⇒ 本页只提示，不崩 ✓） */
    public native String nativeStressInit(String soPath, int w, int h);   /* v5.2：必须带驱动路径（3 参） */
    public native void   nativeStressClearColor(int r, int g, int b);     /* v5.2 原生导出：jint,jint,jint */
    public native String nativeStressFrame(int triCount);
    public native String nativeStressBlit(Bitmap bmp);
    public native void   nativeStressStop();
    /* v7.0 光影（多 pass 光照）：配置/摘要立即可用；Init/Frame/Blit/Stop 待原生接入 ⇒ 未接入降级不崩 ✓ */
    public native String nativeLitConfig(String key, int value);   /* (jstring,jint) */
    public native String nativeLitSettings();                      /* () */
    public native String nativeLitInit(String soPath, int w, int h);/* (jstring,jint,jint) */
    public native String nativeLitFrame();                          /* () */
    public native int    nativeMini3DInit(String soPath, int w, int h);   /* v9.11 迷你 3D 独立路径 (jstring,jint,jint) */
    public native String nativeMini3DFrame(Bitmap bmp);                   /* v9.11 (Landroid/graphics/Bitmap;)Ljava/lang/String; */
    public native void   nativeMini3DStop();
    public native void   nativeMini3DSettings(int shadows, int pcf, int res, int bloom, int tonemap, int animate, int passes);
    public native void   nativeRtSettings(int samples, int bounce, int detail, int bloomPct, int lightMove, int mirrorPct);   /* v9.98 */                              /* v9.11 ()V */
    public native String nativeLitBlit(Bitmap bmp);                 /* (jobject) */
    public native void   nativeLitStop();                           /* () */
    /* v7.0 光追能力探测（原生已可用）。注意：**不声明**尚未导出的 RtInit/RtFrame
     * ⇒ 避免 jni_check.py 因"Java 有声明、原生无导出"拒绝构建 ✗ */
    public native String nativeRtProbe(String soPath);               /* (jstring) */
    public native String nativeRtInit(String soPath, int w, int h);   /* (jstring,jint,jint) v7.0 已导出 ✓
     * v7.4 已导出（参数个数必须一致，否则 jni_check.py 拦构建 ✓） */
    public native String nativeRtFrame();                             /* () */
    public native String nativeRtBlit(Bitmap bmp);                    /* (jobject) */
    public native String nativeRtStop();                              /* () */
    /* nativeCpuRtRender(int) / nativeCpuRtBlit(Bitmap) **仍不声明** ✗ ⇒ 等原生导出后再加 ✓ */

    /* ---------------- 配色 ---------------- */
    /* ============================ 主题系统 ============================
     * 下面这些 C_* **不是颜色值，而是调色板键**（1..17）—— 取色一律 c(key)。
     * 好处：① 全文不再有任何硬编码颜色；② 切换主题 = 换一张调色板 + 重刷已注册视图。
     * 键的顺序必须与 Theme.p[] 一致（key-1 即下标）。 */
    private static final int C_BG = 1, C_CARD = 2, C_STROKE = 3, C_TEXT = 4, C_DIM = 5,
                             C_ACCENT = 6, C_OK = 7, C_BAD = 8, C_WARN = 9, C_NEUTRAL = 10,
                             C_TAB_ON_BG = 11, C_TAB_OFF_BG = 12, C_TAB_ON_TX = 13,
                             C_BTN_BG = 14, C_LOG_BG = 15, C_LOG_TX = 16, C_RENDER_BG = 17;

    /* 扩展令牌 x[]：0=卡片圆角 1=按钮圆角 2=对话框圆角 3=表面 alpha 4=渐变亮度差 5=高光边 alpha
     * 现有三套用 DEFAULT_X（= 现状：12/12/16dp、不透明、无渐变无高光）⇒ 观感零变化 ✓ */
    private static final int[] DEFAULT_X = { 12, 12, 16, 255, 0, 0 };

    private static final class Theme {
        final String id, name; final int[] p, x;
        Theme(String id, String name, int[] p) { this(id, name, p, DEFAULT_X); }
        Theme(String id, String name, int[] p, int[] x) { this.id = id; this.name = name; this.p = p; this.x = x; }
    }

    /* 三套主题：深色（默认）/ 浅色 / 高对比（户外看数字）。
     * 浅色与高对比的三态色都按"压在各自底色上仍看得清"选过（红/绿/黄在浅底上必须够暗、在黑底上必须够亮）。*/
    private static final Theme[] THEMES = {
        /* 深色（默认，沿用原配色） */
        new Theme("dark", "深色", new int[]{
            0xFF0B0F14, 0xFF161B22, 0xFF2A313C, 0xFFE6EDF3, 0xFF9AA4B2, 0xFF7FD1FF,
            0xFF34C759, 0xFFFF453A, 0xFFFFCC00, 0xFF9AA4B2,
            0xFF1F6FEB, 0xFF1A2029, 0xFFFFFFFF, 0xFF1B2430, 0xFF0D1117, 0xFFDDE6F0, 0xFF05070A }),
        /* 浅色：白底深字；红 #B42318 / 绿 #067647 / 黄 #8A4B00 在白底上对比度 ≥5:1 */
        new Theme("light", "浅色", new int[]{
            0xFFF2F4F7, 0xFFFFFFFF, 0xFFD0D7DE, 0xFF111418, 0xFF57606A, 0xFF0B63CE,
            0xFF067647, 0xFFB42318, 0xFF8A4B00, 0xFF57606A,
            0xFF0B63CE, 0xFFE7ECF2, 0xFFFFFFFF, 0xFFF6F8FA, 0xFFFFFFFF, 0xFF111418, 0xFFE9EEF4 }),
        /* 高对比：纯黑底 + 高亮三态色（户外看数字） */
        new Theme("contrast", "高对比", new int[]{
            0xFF000000, 0xFF0A0A0A, 0xFF3A3A3A, 0xFFFFFFFF, 0xFFD0D0D0, 0xFF00E5FF,
            0xFF00FF66, 0xFFFF3B30, 0xFFFFD400, 0xFFD0D0D0,
            0xFF00E5FF, 0xFF141414, 0xFF000000, 0xFF101010, 0xFF000000, 0xFFFFFFFF, 0xFF000000 }),
        /* ===== ColorOS（第四套，用户参考 ColorOS 17：低饱和柔和 + 以蓝为锚 + 半透磨砂 + 大圆角 + 凝光渐变）=====
         * 三态色都是"压在半透白卡上 ≥4.5:1"的深色版 ✓（ok 取 #0E8A45 稳过 AA）*/
        new Theme("coloros", "ColorOS", new int[]{
            0xFFF3F6FA, 0xFFFFFFFF, 0xFFEAF0F7, 0xFFDCE4EE, 0xFF12161C, 0xFF5A6675,
            0xFF1B6EF3, 0xFF0E8A45, 0xFFC77700, 0xFFD92D20, 0xFF5A6675,
            0xFF1B6EF3, 0xFFE7EEF8, 0xFFFFFFFF, 0xFFEAF1FA, 0xFFFFFFFF, 0xFF12161C, 0xFF0A0F16 },
            new int[]{ 20, 16, 24, 230, 8, 0x1F }),        /* 卡 20dp / 按钮 16 / 弹层 24 / 表面 90% / 渐变 +8 / 高光 12% */
        /* ColorOS 深色变体：深蓝黑 + 半透 + 同一套大圆角与凝光 */
        new Theme("coloros-dark", "ColorOS 深色", new int[]{
            0xFF0A0F16, 0xFF121A24, 0xFF1A2432, 0xFF2A3644, 0xFFE9EFF7, 0xFF9AA8BA,
            0xFF4C9AFF, 0xFF38D39F, 0xFFFFC24B, 0xFFFF6B6B, 0xFF9AA8BA,
            0xFF4C9AFF, 0xFF16202C, 0xFF08111C, 0xFF18222F, 0xFF070C12, 0xFFDCE7F5, 0xFF05080D },
            new int[]{ 20, 16, 24, 235, 10, 0x22 }),
    };

    private static final String PREFS = "gputest", PREF_THEME = "theme";   /* 持久化：SharedPreferences("gputest").getString("theme", "dark") */
    private static int themeIdx = 0;
    private static int[] CUR = THEMES[0].p.clone();
    private static int[] CURX = THEMES[0].x.clone();

    private static int c(int key) {
        int i = key - 1;
        return (i >= 0 && i < CUR.length) ? CUR[i] : 0xFF888888;
    }

    private static int x(int i) { return (i >= 0 && i < CURX.length) ? CURX[i] : 0; }
    private static int withAlpha(int color, int a) { return (color & 0x00FFFFFF) | ((a & 0xFF) << 24); }
    private static int lighten(int color, int d) {
        int r = Math.min(255, ((color >> 16) & 0xFF) + d);
        int g = Math.min(255, ((color >> 8) & 0xFF) + d);
        int b = Math.min(255, (color & 0xFF) + d);
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    /* ---------------- 页签 ---------------- */
    private static final String[] TAB_NAMES = { "概览", "驱动", "测试", "画面", "跑分", "日志", "压力" };
    private static final int TAB_OVERVIEW = 0, TAB_DRIVER = 1, TAB_TEST = 2,
                             TAB_PICTURE = 3, TAB_BENCH = 4, TAB_LOG = 5, TAB_STRESS = 6;

    /* v6.1：底部导航 4 个目的地（原 7 个页签降级为页内二级分段） */
    private static final int DEST_OVERVIEW = 0, DEST_TEST = 1, DEST_RENDER = 2, DEST_RECORDS = 3, DEST_SETTINGS = 4;   /* v9.57 */
    private static final String[] DEST_NAMES  = { "概览", "测试", "画面", "记录", "设置" };   /* v9.57：设置排最后 ✓ */
    private static final String[] DEST_ICONS  = { "ic_tab_overview", "ic_tab_test", "ic_tab_render", "ic_tab_log", "ic_tab_log" };
    private static final String[] DEST_GLYPHS = { "▦", "▤", "▶", "▥", "⚙" };
    private static final int SEG_TEST_DRIVER = 0, SEG_TEST_VERIFY = 1, SEG_TEST_BENCH = 2, SEG_TEST_AB = 3;
    private static final int SEG_RENDER_LIVE = 0, SEG_RENDER_LIT = 1, SEG_RENDER_STRESS = 2;
    private static final int SEG_REC_SCORE = 0, SEG_REC_REPORT = 1, SEG_REC_LOG = 2;
    private final Button[] tabBtns = new Button[TAB_NAMES.length];
    private final View[]   tabPages = new View[TAB_NAMES.length];
    private FrameLayout content;
    private int curTab = -1;

    /* ---------------- 常驻头部 ---------------- */
    private TextView headerDriver, headerStatus;
    private LinearLayout rootV;                                   /* 主题切换时要重刷根背景 */
    private Button themeBtn;                                      /* 🎨 主题按钮（标签显示当前主题） */
    /* 主题重刷：只登记"颜色依赖主题"的视图（键），切换时按角色重上色，不动布局 */
    private final java.util.WeakHashMap<View, Integer> textKeys = new java.util.WeakHashMap<>();
    private final java.util.WeakHashMap<View, Integer> bgKeys   = new java.util.WeakHashMap<>();   /* 值 = 填充键*100 + 圆角 */
    private final java.util.WeakHashMap<Button, Boolean> tabStates = new java.util.WeakHashMap<>();

    /* ---------------- v6.1 App 骨架：App Bar + 底部导航 + FAB + 二级分段 ---------------- */
    private final View[] destPages = new View[5];
    private LinearLayout appBar, statusStrip, bottomNav;
    private TextView appTitle, driverChip, busyChip;
    private Button themeBtnTop;
    private final View[]   navItems = new View[5];
    private final View[]   navIndicators = new View[5];
    private final TextView[] navLabels = new TextView[5];
    private final View[]   navIcons = new View[5];
    private int curDest = -1;
    private int segTest = 0, segRender = 0, segRec = 0;
    private View[] hubTestPages, hubRenderPages, hubRecPages;
    private TextView[][] segPills = new TextView[3][];      /* [测试/画面/记录][段] */
    private final java.util.WeakHashMap<View, Integer> iconKeys = new java.util.WeakHashMap<>();  /* ImageView 着色 */
    private final java.util.List<Button> primaryBtns = new java.util.ArrayList<>();   /* 语义按钮：主题切换时要重刷 */
    private final java.util.List<Button> dangerBtns  = new java.util.ArrayList<>();
    private ImageView blurBackdrop;                         /* ③ 磨砂底衬（只模糊它自己的快照 ⇒ 文字不糊 ✓） */
    private TextView fab;                                   /* 每页一个主操作（FAB） */
    private Runnable fabAction;
    private TextView scoreTable;                              /* 记录→成绩 */
    private View scoreEmpty;                                  /* 记录→成绩 的空状态 */

    /* ---------------- 一键跑全部驱动（v6.3）：串行遍历 + 实时行 + 排名表 ---------------- */
    private LinearLayout rankBox, failBox;                     /* 排名表 / 失败表（自绘行） */
    private TextView rankRulesTx, rankTableTx, rankFailTx;
    private volatile boolean sweepRunning = false;
    private final java.util.List<double[]> sweepNums = new java.util.ArrayList<>();   /* {fill,blit,draw,duty} */
    private final java.util.List<String>   sweepNames = new java.util.ArrayList<>();
    private final java.util.List<Boolean>  sweepOk   = new java.util.ArrayList<>();
    private final java.util.List<String>   sweepNote = new java.util.ArrayList<>();
    private final java.util.HashMap<String, String> driverLabels = new java.util.HashMap<>();   /* path -> 显示名 */
    /* 单跑解析器的"最新结果"（sweep 直接读它 ⇒ 不再自造一套匹配 ✗） */
    private volatile double lastFill = -1, lastBlit = -1, lastDraw = -1, lastDuty = -1;
    private volatile double lastGpuMs = -1, lastWallS = -1;

    /* ---------------- 驱动列表 ---------------- */
    private RadioGroup driverGroup;          /* ① Vulkan 驱动 */
    private RadioGroup rendererGroup;        /* ② 渲染器（跑在驱动之上） */
    private LinearLayout unknownBox;         /* ③ 打不开 / 未知（仅列出） */
    private final List<String> driverPaths   = new ArrayList<>();
    private final List<String> rendererPaths = new ArrayList<>();
    private String baselinePath = null;
    private int driverCount = 0, rendererCount = 0;

    /* ---------------- 概览 / 跑分 卡片 ---------------- */
    private TextView ovScore, ovSummary, ovDevice, ovGpu, ovDrv;
    private TextView benchFill, benchBlit, benchDraw, benchScore, capsText;

    /* ---------------- 画面页 ---------------- */
    private static final int RENDER_W = 512, RENDER_H = 512;
    private ImageView renderView;
    private TextView  renderFps, renderInfo;
    private Button    renderBtn;
    private volatile boolean renderRunning = false;
    private Thread renderThread;
    private final Bitmap[] renderBufs = new Bitmap[2];   /* 乒乓：写的那张永远不在屏幕上 */
    private volatile Bitmap shownBmp;
    private int renderFrameIdx = 0;

    /* ---------------- 压力页（参考图语义：ppf 自动加量 + 统计面板 + 进展条） ---------------- */
    private static final int STRESS_W = 512, STRESS_H = 512;
    private ImageView stressView;
    private TextView stressPanel, stressProgTx, stressRgbTx, stressNote;
    private CheckBox stressAuto;
    private EditText stressPpf;
    private ProgressBar stressProg;
    private Button stressBtn;
    private final Bitmap[] stressBufs = new Bitmap[2];
    private volatile boolean stressRunning = false;
    private Thread stressThread;
    private volatile int stressTri = 0;
    private int stressCap = 200000;                     /* 进展条上限（三角形数） */
    private volatile double stressMaxFrameMs = 100.0;    /* 停止阈值：平均帧时 > 100 ms */
    private final int[] stressRgb = { 8, 8, 25 };        /* 默认清屏色，与参考图一致 */
    private String lastDeviceRaw = "";                   /* 最近一次能力清单/冒烟里的设备行 */

    /* ---------------- v7.0 光影设置面板 ---------------- */
    private static final int LIT_W = 512, LIT_H = 512;
    private int litSize = 256;   /* v9.20：默认降到 256² —— 512² 会把 GPU 打满，连系统合成器都拿不到 GPU 时间 ⇒ 整个 App 点不动 ✗ */
    private ImageView litView;
    private TextView litSummary, litStats, litPassTx, litNativeTx;   /* litNativeTx：原生返回行原样显示 ✓ */
    private SeekBar litPassBar;
    private final Bitmap[] litBufs = new Bitmap[2];
    private volatile boolean m3dRunning = false;   /* v9.11 迷你 3D 运行标志（跨线程可见 ✓）*/
    private Button m3dBtn;
    private TextView shzStatusTx;          /* v9.53 设置卡：Shizuku 状态行 */
    private volatile boolean fpsCap = false;  /* v9.53 帧率上限开关（默认不限）*/
    private volatile boolean rt3dRunning = false;   /* v9.22：硬件光追接入 3D 测试 */
    private Button rt3dBtn;
    private final java.util.concurrent.atomic.AtomicBoolean uiPending = new java.util.concurrent.atomic.AtomicBoolean(false);   /* v9.18 */                          /* v9.14：迷你 3D 按钮要有引用，才能把文案切成「■ 停止」 ✓ */
    private volatile boolean litRunning = false;
    private Thread litThread;
    private int litShadows = 1, litPcf = 3, litRes = 2048, litCubes = 36,
                litBloom = 1, litTonemap = 1, litPasses = 1, litDebug = 0, litAnimate = 1;
    /* v9.98 · 光追配置 */
    private int rtSamples = 4, rtBounce = 1, rtDetail = 1, rtBloom = 100, rtLightMove = 1, rtMirror = 97;
    /* v10.1 · 真实数据面板 */
    private String shzDataText = "";
    private TextView shzDataTx;
    /* v10.3 · 实时监视 */
    private boolean liveOn = false;
    private TextView liveTx;
    /* v10.4 · 趋势：滚动保存最近 24 次采样（约 48 秒）*/
    private final java.util.ArrayList<Float> liveHist = new java.util.ArrayList<>();
    private float liveGpuT = -1f, liveMemMB = -1f;
    /* v10.7 · 帧统计（gfxinfo）*/
    private int liveJankPct = -1, liveF50 = -1, liveG50 = -1, liveFrames = -1;

    /** 一行离散开关（pill）：键名 / 取值表 / 当前选中。 */
    private static final class LitRow {
        final String key; final int[] vals; final TextView[] pills; final String[] labels; int sel;
        LitRow(String key, int[] vals, String[] labels, TextView[] pills, int sel) {
            this.key = key; this.vals = vals; this.labels = labels; this.pills = pills; this.sel = sel;
        }
    }
    private final java.util.List<LitRow> litRows = new java.util.ArrayList<>();

    /* ---------------- v7.0 光追（大选项）：能开就真开，不能开明说 ✓ ---------------- */
    private android.widget.Switch rtSwitch;
    private TextView rtProbeTx, rtVerdictTx;
    private Button rtRunBtn;
    private LinearLayout rtCardBox;
    private int rtOn = 0;                  /* 0/1，持久化键 lit_raytracing */
    private volatile boolean rtProbing = false;
    private int rtState = 0;               /* 0=空闲 1=BLAS 已建 */
    private TextView rtInitTx;             /* nativeRtInit 返回行（原样） */
    private Button rtCpuBtn;               /* CPU 光追参考图（下版接入） */
    private ImageView rtView;              /* 光追画面（乒乓双缓冲显示，别撕裂 ✓） */
    private final Bitmap[] rtBufs = new Bitmap[2];
    private volatile boolean rtRunning = false;
    private Thread rtThread;
    private int rtFrames = 0;

    /* ---------------- 日志 ---------------- */
    private TextView logView;
    private final StringBuilder fullLog = new StringBuilder();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private static final int REQ_PICK_APK = 1001;

    /* ------------- 看门狗 -------------
     * 原生日志是"整体缓冲、函数返回时才交给 Java" ⇒ 驱动若卡死在某个调用里，
     * 界面上一行原生日志都不会有。所以：UI 线程 20 秒一跳，到点还没返回就报
     * nativeLastStep() 的结果。⚠️ 只报状态，绝不 join / 绝不打断（驱动不可中断）。 */
    private static final long WATCHDOG_MS = 20000;
    private volatile String busyTitle;
    private volatile long progressToken = 0, watchdogSeen = 0;
    private volatile long taskGen = 0;            /* B6：看门狗代际号（防"给新任务打假警报"） */

    /* ---- B5：任务互斥（并发点两下 = 两个 worker 同时进原生，而原生 g_rc/g_rt/g_rpipe 是 static 全局） ---- */
    private volatile boolean busy = false;
    private volatile String busyWhat = null;
    private final List<Button> actionButtons = new ArrayList<>();

    /* ---- A1/A4：成绩归属与失败态 ---- */
    private volatile String benchOwner = "(未跑)";
    private TextView benchNote;

    /* (c) v4.1：GPU 时间 / 墙钟 / 占空比 三个数并排 + 原生自证行 + 独立校验卡 */
    private TextView benchGpu, benchWall, benchDuty, benchProof, validNote;
    private static final java.util.regex.Pattern P_GPU_MS = java.util.regex.Pattern.compile(
            "(?:GPU\\s*时间|GPU\\s*TIME|gpu_ms)[^0-9]{0,12}([0-9]+(?:\\.[0-9]+)?)",
            java.util.regex.Pattern.CASE_INSENSITIVE);
    private static final java.util.regex.Pattern P_WALL_S = java.util.regex.Pattern.compile(
            "(?:墙钟|WALL|时长)[^0-9]{0,12}([0-9]+(?:\\.[0-9]+)?)\\s*s",
            java.util.regex.Pattern.CASE_INSENSITIVE);

    private final Runnable watchdog = new Runnable() {
        @Override public void run() {
            final long myGen = taskGen;                  /* B6：进来先记住代际 */
            final String t = busyTitle;
            if (t == null || myGen != taskGen) return;   /* 已结束 / 已被新任务取代 ⇒ 立刻退出 */
            long now = progressToken;
            if (now != watchdogSeen) {              /* 有进展 ⇒ 静默续期 */
                watchdogSeen = now;
                ui.postDelayed(this, WATCHDOG_MS);
                return;
            }
            String step;
            try { step = nativeLastStep(); } catch (Throwable e) { step = "nativeLastStep 调用异常: " + e; }
            if (step == null || step.length() == 0) step = "（原生未报告步骤）";
            log("⏱ 原生调用「" + t + "」超过 " + (WATCHDOG_MS / 1000) + " 秒无进展 —— 当前卡在：" + step);
            log("   判据：进程还在 ⇒ 驱动卡死（不可中断，只能杀 App）；界面恢复但无结果 ⇒ 另说");
            if (myGen != taskGen) return;                /* B6：报警前再确认一次代际 */
            setStatus("⚠ " + t + " 无进展 " + (WATCHDOG_MS / 1000) + "s：卡在 " + step, C_BAD);
            ui.postDelayed(this, WATCHDOG_MS);      /* 每 20 秒重复提醒 */
        }
    };

    /** 进入一个"可能卡死"的原生任务（从工作线程调用也安全）。 */
    private void beginTask(String title) {
        taskGen++;                                   /* B6：新一代 ⇒ 旧回调自动作废 */
        busyTitle = title;
        watchdogSeen = progressToken;
        ui.removeCallbacks(watchdog);
        ui.postDelayed(watchdog, WATCHDOG_MS);
    }

    private void endTask() {
        taskGen++;                                   /* B6：同样推进代际 */
        busyTitle = null;
        ui.removeCallbacks(watchdog);
    }

    /* ================= B5：任务互斥 ================= */
    private boolean claim(String what) {
        synchronized (this) {
            if (busy) {
                final String w = busyWhat;
                log("  ⚠ 忽略点击：" + what + "（已有任务在跑：" + w + "）");
                toast("已有任务在跑：" + w + "，等它结束再点");
                return false;
            }
            busy = true; busyWhat = what;
        }
        ui.post(() -> {
            for (Button b : actionButtons) if (b != null) b.setEnabled(false);
            if (busyChip != null) busyChip.setVisibility(View.VISIBLE);   /* App Bar 忙碌指示 */
        });
        return true;
    }

    private void release() {
        synchronized (this) { busy = false; busyWhat = null; }
        ui.post(() -> {
            for (Button b : actionButtons) if (b != null) b.setEnabled(true);
            if (busyChip != null) busyChip.setVisibility(View.GONE);
        });
    }

    /* =========================================================================
     *  生命周期 / 骨架
     * ========================================================================= */
    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        loadThemePref();                                    /* 主题：先读上次选的，再建界面 */

        rootV = new LinearLayout(this);
        rootV.setOrientation(LinearLayout.VERTICAL);
        rootV.setBackgroundColor(c(C_BG));

        rootV.addView(buildAppBar());                       /* A. App Bar 56dp + 状态行 24dp */

        content = new FrameLayout(this);                    /* B. 内容宿主（含 FAB 叠层） */
        rootV.addView(content, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        destPages[DEST_OVERVIEW] = safePage("概览", () -> buildOverviewPage());
        destPages[DEST_TEST]     = safePage("测试", () -> buildTestHubPage());
        destPages[DEST_RENDER]   = safePage("画面", () -> buildRenderHubPage());
        destPages[DEST_RECORDS]  = safePage("记录", () -> buildRecordsHubPage());
        destPages[DEST_SETTINGS] = safePage("设置", () -> buildSettingsPage());   /* v9.57 */
        for (View v : destPages) {
            v.setVisibility(View.GONE);
            content.addView(v, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        blurBackdrop = new ImageView(this);                 /* ③ 快照层：位置在页面之下、栏之内 ⇒ 只模糊它 */
        blurBackdrop.setScaleType(ImageView.ScaleType.FIT_XY);
        blurBackdrop.setVisibility(View.GONE);
        content.addView(blurBackdrop, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        content.addView(buildFab(), fabLp());               /* 每页一个主操作 */

        rootV.addView(buildBottomNav());                    /* C. 底部导航 64dp */

        setContentView(rootV);
        try { selectDest(DEST_OVERVIEW, 0, false); }
        catch (Throwable t) { log("  ✗ 初始页选择失败（已忽略，不影响进入）: " + t); }
        log("GPU 驱动测试台 v" + appVersion() + " 启动。");
        refreshDeviceCards();
        scanDrivers();
    }

    @Override protected void onStop() {
        /* B7：退到后台就停渲染（放 onStop 不放 onPause —— 文件选择器/授权页只触发 onPause） */
        if (renderRunning || renderThread != null) stopRender("退到后台");
        if (stressRunning || stressThread != null) stopStress("退到后台");
        if (litRunning || litThread != null) stopLit("退到后台");
        if (rtRunning || rtThread != null) stopRt("退到后台");
        super.onStop();
    }

    @Override protected void onDestroy() {
        try { nativeLitStop(); } catch (Throwable ignored) { }   /* v7.0：光影收尾 */
        try { nativeRtStop(); } catch (Throwable ignored) { }    /* v7.4：光追收尾（安全可调用 ✓） */
        /* 退出时不必等 join（进程都要没了）：置标志 + 让原生停掉，循环下一轮自然退出。 */
        renderRunning = false;
        stressRunning = false;
        try { nativeStressStop(); } catch (Throwable ignored) { }
        endTask();
        try { nativeRenderStop(); }
        catch (Throwable t) { android.util.Log.w("gputest", "nativeRenderStop 异常: " + t); }   /* D11：不再静默 */
        super.onDestroy();
    }

    /* =========================================================================
     *  页签切换
     * ========================================================================= */
    /** 旧调用点兼容：把原 7 页签索引映射成 (目的地, 二级分段)。 */
    private void switchTab(int legacy) {
        switch (legacy) {
            case TAB_OVERVIEW: selectDest(DEST_OVERVIEW, 0, true); break;
            case TAB_DRIVER:   selectDest(DEST_TEST, SEG_TEST_DRIVER, true); break;
            case TAB_TEST:     selectDest(DEST_TEST, SEG_TEST_VERIFY, true); break;
            case TAB_BENCH:    selectDest(DEST_TEST, SEG_TEST_BENCH, true); break;
            case TAB_PICTURE:  selectDest(DEST_RENDER, SEG_RENDER_LIVE, true); break;
            case TAB_STRESS:   selectDest(DEST_RENDER, SEG_RENDER_STRESS, true); break;
            case TAB_LOG:      selectDest(DEST_RECORDS, SEG_REC_LOG, true); break;
            default:           selectDest(DEST_OVERVIEW, 0, true); break;
        }
        curTab = legacy;                                     /* 兼容字段：老代码只读它做判据 */
    }

    /** 底部导航切换：先收尾离开的页面（渲染/压力线程），再切页 + 分段，内容淡入 225ms。 */
    private void selectDest(int dest, int seg, boolean animate) {
        if (dest == curDest && seg < 0) return;
        /* 离开「画面」目的地 ⇒ 停掉在跑的画面/压力任务（不打断跑分：那是另一套 claim） */
        if (curDest == DEST_RENDER && dest != DEST_RENDER) {
            if (renderRunning || renderThread != null) stopRender("切走页面");
            if (stressRunning || stressThread != null) stopStress("切走页面");
            if (litRunning || litThread != null) stopLit("切走页面");
            if (rtRunning || rtThread != null) stopRt("切走页面");
        }
        if (dest == DEST_RENDER && seg >= 0 && seg != segRender) {   /* 段内切换：停掉离开那段的任务 */
            if (segRender == SEG_RENDER_LIVE && (renderRunning || renderThread != null)) stopRender("切到别的分段");
            if (segRender == SEG_RENDER_STRESS && (stressRunning || stressThread != null)) stopStress("切到别的分段");
            if (segRender == SEG_RENDER_LIT && (litRunning || litThread != null)) stopLit("切到别的分段");
            if (segRender == SEG_RENDER_LIT && (rtRunning || rtThread != null)) stopRt("切到别的分段");
        }
        curDest = dest;
        for (int i = 0; i < destPages.length; i++)
            destPages[i].setVisibility(i == dest ? View.VISIBLE : View.GONE);
        if (appTitle != null) appTitle.setText(DEST_NAMES[dest]);
        styleBottomNav();
        updateFab();
        if (seg >= 0) {
            if (dest == DEST_TEST)    segTest = seg;
            if (dest == DEST_RENDER)  segRender = seg;
            if (dest == DEST_RECORDS) segRec = seg;
        }
        applySegments();
        refreshBackdrop(dest);                              /* ③ 切页后刷新磨砂底衬（快照 1/4 缩放 + API31 真模糊） */
        if (animate && destPages[dest] != null) {
            destPages[dest].setAlpha(0f);
            destPages[dest].animate().alpha(1f).setDuration(260)          /* ④ 260ms 流体 */
                    .setInterpolator(new android.view.animation.PathInterpolator(0.0f, 0.0f, 0.2f, 1.0f)).start();
        }
    }

    private void styleTab(Button t, boolean on) {
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(10));
        g.setColor(c(on ? C_TAB_ON_BG : C_TAB_OFF_BG));
        g.setStroke(dp(1), c(on ? C_ACCENT : C_STROKE));
        t.setBackground(g);
        t.setTextColor(c(on ? C_TAB_ON_TX : C_DIM));
        t.setTypeface(Typeface.DEFAULT, on ? Typeface.BOLD : Typeface.NORMAL);
        tabStates.put(t, on);                        /* 主题切换时按选中态重刷 */
        textKeys.put(t, on ? C_TAB_ON_TX : C_DIM);
        t.setTag(on ? "tab-on" : "tab-off");
    }

    /* =========================================================================
     *  ① 概览
     * ========================================================================= */
    private View buildOverviewPage() {
        LinearLayout col = col();
        ovScore = text("—", 44, C_ACCENT, true);
        ovScore.setGravity(Gravity.CENTER_HORIZONTAL);
        TextView scoreLbl = text("综合分（填充率 + 带宽×100 + 三角形吞吐÷1000）", 11, C_DIM, false);
        scoreLbl.setGravity(Gravity.CENTER_HORIZONTAL);
        ovSummary = text("还没有跑分记录。到「跑分」页点一个项目，或直接「一键全流程」。", 12, C_DIM, false);
        ovSummary.setPadding(0, dp(6), 0, 0);
        col.addView(card(scoreLbl, ovScore, ovSummary));

        ovDevice = text("读取中…", 12, C_TEXT, false);
        ovDevice.setLineSpacing(0, 1.2f);
        col.addView(card(section("设备"), ovDevice));

        ovGpu = text("读取中…", 12, C_TEXT, false);
        ovGpu.setLineSpacing(0, 1.2f);
        col.addView(card(section("GPU / 图形栈"), ovGpu));

        ovDrv = text("未选择驱动", 12, C_TEXT, false);
        ovDrv.setLineSpacing(0, 1.2f);
        col.addView(card(section("当前驱动"), ovDrv));

        themeBtn = actionBtn(themeLabel(), v -> cycleTheme());       /* 🎨 标签即当前主题，一眼可见 ✓ */
        /* v9.60b · 概览页只留「最常用」：主题切换 + 一键全流程 + 跑分。
         * 扫描/提取/日志这些在「测试」页有完整分组（v9.60 拆好的 4 组）⇒ 这里不再重复 ✗ */
        col.addView(card(section("快捷操作"),
                themeBtn,
                actionBtn("一键全流程自测（冒烟→三角形→三种跑分）", v -> runAll()),
                actionBtn("拉起 Minecraft 启动器", v -> launchChooser()),
                actionBtn("读取启动器日志（FCL / ZL2）", v -> { switchTab(TAB_LOG); readLauncherLogs(); })));
        return scroll(col);
    }

    private void refreshDeviceCards() {
        StringBuilder d = new StringBuilder();
        d.append("品牌/型号 : ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        d.append("Android   : ").append(Build.VERSION.RELEASE)
                .append("  (API ").append(Build.VERSION.SDK_INT).append(")\n");
        d.append("主板/硬件 : ").append(Build.BOARD).append(" / ").append(Build.HARDWARE).append('\n');
        d.append("CPU ABI   : ").append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?")
                .append("   (").append(Build.SUPPORTED_ABIS.length).append(" 个 ABI)\n");
        d.append("CPU 核数  : ").append(Runtime.getRuntime().availableProcessors());
        if (Build.VERSION.SDK_INT >= 31) {
            try { d.append("\nSoC 型号  : ").append(Build.SOC_MODEL); } catch (Throwable ignored) { }
        }
        ovDevice.setText(d.toString());

        StringBuilder g = new StringBuilder();
        String gles = "?";
        try {
            android.content.pm.FeatureInfo[] fis = getPackageManager()
                    .getPackageInfo(getPackageName(), PackageManager.GET_CONFIGURATIONS).reqFeatures;
            if (fis != null) for (android.content.pm.FeatureInfo fi : fis) {
                if (fi.name != null && fi.name.contains("opengles")) { gles = "OpenGL ES " + fi.version; break; }
            }
        } catch (Throwable ignored) { }
        try {
            android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
            g.append("屏幕      : ").append(dm.widthPixels).append('x').append(dm.heightPixels)
                    .append("  ").append(dm.densityDpi).append(" dpi\n");
            g.append("刷新率    : ").append(String.format(Locale.ROOT, "%.0f", refreshHz())).append(" Hz\n");
        } catch (Throwable ignored) { }
        g.append("GLES      : ").append(gles).append('\n');
        g.append("测试方式  : " + (isSystemDriver() ? "系统 Vulkan loader（原生）" : "直接 dlopen 驱动 .so + vk_icdGetInstanceProcAddr") + "\n");
        g.append("            " + (isSystemDriver() ? "（经系统 loader，与系统其它应用同一条路径）" : "（不经过系统 Vulkan loader）") + "\n");
        ovGpu.setText(g.toString());

        updateSelectedHeader();
    }

    private float refreshHz() {
        try {
            if (Build.VERSION.SDK_INT >= 30) return getDisplay() != null ? getDisplay().getRefreshRate() : 60f;
            return getWindowManager().getDefaultDisplay().getRefreshRate();
        } catch (Throwable t) { return 60f; }
    }

    /* =========================================================================
     *  ② 驱动
     * ========================================================================= */
    private View buildDriverPage() {
        LinearLayout col = col();
        col.addView(card(section("扫描与安装"),
                note("· 已安装的驱动插件包（读 nativeLibraryDir 里的 .so）\n"
                   + "· 本地 .so：/sdcard/Mali驱动项目/驱动、/sdcard/Download、"
                   + "Android/data/com.dsh.gputest/files/drivers\n"
                   + "· 从任意 APK 里提取（自动优先 arm64 + 像 ICD 的那个）"),
                actionBtn("扫描驱动与渲染器", v -> scanDrivers()),
                actionBtn("从 APK 提取驱动 .so", v -> pickApkForExtract()),
                actionBtn("安装驱动插件 APK", v -> pickApkForInstall())));

        /* ---- v9.60 · 按功能分组：诊断与数据 ---- */
        col.addView(card(section("诊断与数据"),
                note("需要 Stellar/Shizuku 授权（设置页可申请）。"
                   + "拿到的是 shell 级真数据：GPU 频率、温度、系统面。"),
                actionBtn("Shizuku/Stellar 探针", v -> shizukuProbe())));

        /* ---- v9.60 · 按功能分组：渲染控制 ---- */
        col.addView(card(section("渲染控制"),
                actionBtn("一键全流程自测", v -> runAll()),
                actionBtn("开始 / 停止渲染", v -> toggleRender()),
                actionBtn("开始 / 停止光影渲染", v -> toggleLit())));

        /* ---- v9.60 · 按功能分组：系统与维护 ---- */
        col.addView(card(section("系统与维护"),
                actionBtn("授权「所有文件访问」", v -> requestAllFiles()),
                actionBtn("系统驱动对照", v -> runSystemBaseline()),
                actionBtn("导出日志", v -> exportLog())));

        /* ⑤ 悬浮式搜索栏：过滤下面的驱动/渲染器列表（半透磨砂 + 1dp 描边 + 大圆角） */
        EditText search = new EditText(this);
        search.setHint("搜索驱动 / 渲染器…");
        search.setSingleLine(true);
        search.setTextSize(14);
        search.setTextColor(c(C_TEXT));
        search.setHintTextColor(c(C_DIM));
        textKeys.put(search, C_TEXT);
        search.setBackground(themedBg(C_BTN_BG, x(1)));
        search.setPadding(dp(16), dp(12), dp(16), dp(12));
        search.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s2, int a, int b, int c2) { }
            @Override public void onTextChanged(CharSequence s2, int a, int b, int c2) { }
            @Override public void afterTextChanged(android.text.Editable e) {
                String q = e == null ? "" : e.toString().trim().toLowerCase(Locale.ROOT);
                filterGroup(driverGroup, q);
                filterGroup(rendererGroup, q);
            }
        });
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.setMargins(dp(12), dp(12), dp(12), dp(4));
        search.setLayoutParams(slp);
        col.addView(search);

        col.addView(section("Vulkan 驱动（选一个用于测试）"));
        driverGroup = new RadioGroup(this);
        driverGroup.setOrientation(RadioGroup.VERTICAL);
        driverGroup.setOnCheckedChangeListener((g, id) -> updateSelectedHeader());
        col.addView(card(driverGroup,
                note("这一组是「渲染器底下的驱动」——启动器里 Vulkan 驱动器下拉的那几个。")));

        col.addView(section("渲染器（跑在 Vulkan 驱动之上）"));
        rendererGroup = new RadioGroup(this);
        rendererGroup.setOrientation(RadioGroup.VERTICAL);
        rendererGroup.setOnCheckedChangeListener((g, id) -> updateSelectedHeader());
        col.addView(card(rendererGroup,
                note("本版只把分类与命名做对（选中会记下来，头部常驻显示）。"
                   + "「渲染器 × 驱动」的组合测试是下一步：用 Vulkan 垫片把渲染器的 "
                   + "dlopen(\"libvulkan.so\") 转发到上面选中的驱动 ICD。")));

        col.addView(section("打不开 / 未知（仅列出，不参与测试）"));
        unknownBox = new LinearLayout(this);
        unknownBox.setOrientation(LinearLayout.VERTICAL);
        col.addView(card(unknownBox));
        return scroll(col);
    }

    private void scanDrivers() {
        driverPaths.clear();
        rendererPaths.clear();
        driverGroup.removeAllViews();
        /* v2.7：置顶「使用系统驱动」—— 与自定义驱动在同一个单选组里一键切换，
         * 省掉原来那个独立的「⑬ 系统驱动对照」按钮。原生侧已支持特殊路径 "system"。 */
        {
            RadioButton sys = new RadioButton(this);
            sys.setText("🖥  原生 Vulkan（系统 loader）");
            sys.setTextColor(c(C_TEXT)); textKeys.put(sys, C_TEXT);
            sys.setTag(driverPaths.size());
            driverPaths.add("system");
            driverGroup.addView(sys, 0);                 /* index 0 ⇒ 置顶 ✓ */
            if (driverPaths.size() == 1) sys.setChecked(true);
        }
        rendererGroup.removeAllViews();
        unknownBox.removeAllViews();
        driverCount = 0; rendererCount = 0;
        log("\n=== ① 扫描驱动与渲染器 ===");
        final File drop = driversDir();
        log("  喂 .so 目录（无需任何权限）：" + drop.getAbsolutePath());
        log("  分类依据：真的 dlopen + dlsym 查符号（ICD 入口 / EGL·GLES / 其它）");
        beginTask("扫描：分类中（dlopen 查符号）");
        new Thread(() -> {
            try {
                List<Cand> cands = new ArrayList<>();
                PackageManager pm = getPackageManager();

                /* (a) 已安装插件包：**元数据优先**（照抄 FCL PluginManager.buildApp 的规则）——
                 *     renderer 或 fclPlugin_V2 ⇒ 渲染器；driver ⇒ Vulkan 驱动；
                 *     FCLNativePlugin=true ⇒ 原生库；三者都没有 ⇒ 不是插件，直接忽略。
                 *     元数据能定类的**一律不 dlopen**（v2.3 的闪退根因就是 dlopen 渲染器）。 */
                try {
                    for (PackageInfo pi : pm.getInstalledPackages(PackageManager.GET_META_DATA)) {
                        ApplicationInfo ai = pi.applicationInfo;
                        if (ai == null) continue;
                        android.os.Bundle md = ai.metaData;
                        String drvName = metaStr(md, "driver");
                        String rndName = metaStr(md, "renderer");
                        boolean v2      = md != null && md.containsKey("fclPlugin_V2");
                        boolean fclP    = metaBool(md, "fclPlugin");
                        boolean nativeP = metaBool(md, "FCLNativePlugin");   /* FCL 的 key 字符串就是这个 */
                        boolean isRenderer = (rndName != null) || v2;
                        boolean isDriver   = (drvName != null);
                        if (!isRenderer && !isDriver && !nativeP) continue;  /* 不是插件 ⇒ 忽略 */

                        /* 元数据全打一行，便于核对（含 env 串） */
                        String des  = metaStr(md, "des");
                        String pEnv = metaStr(md, "pojavEnv");
                        String bEnv = metaStr(md, "boatEnv");
                        String envAll = (pEnv == null ? "" : "pojavEnv=" + pEnv + " ")
                                      + (bEnv == null ? "" : "boatEnv=" + bEnv);
                        String metaLine = "driver=" + drvName + " / renderer=" + rndName
                                + " / fclPlugin=" + fclP + " / fclPlugin_V2=" + v2
                                + " / FCLNativePlugin=" + nativeP
                                + (des == null ? "" : " / des=" + des)
                                + (envAll.length() == 0 ? "" : " / env: " + envAll);
                        pluginMeta.put(pi.packageName, metaLine);
                        if (envAll.length() > 0) pluginEnv.put(pi.packageName, envAll);
                        log("  ✓ 插件: " + pi.packageName + "  [" + pi.versionName + "]");
                        log("      元数据: " + metaLine);
                        if (envAll.length() > 0)
                            log("      自带 env(前120字): " + envAll.substring(0, Math.min(120, envAll.length())));

                        String appLabel = null;
                        try { appLabel = String.valueOf(ai.loadLabel(pm)); } catch (Throwable ignored) { }
                        File dir = ai.nativeLibraryDir == null ? null : new File(ai.nativeLibraryDir);
                        String srcTag = "插件 " + pi.packageName + (appLabel == null ? "" : "（" + appLabel + "）");

                        if (isDriver) {
                            File so = pickVulkanSo(dir);
                            String dlabel = friendlyName(pi.packageName, drvName) + " [" + pi.versionName + "]";
                            if (so != null) {
                                Cand c = new Cand(dlabel, so.getAbsolutePath(), srcTag,
                                        icdScore(so.getName().toLowerCase(Locale.ROOT)));
                                c.kind = KIND_DRIVER;
                                c.cls = "元数据 driver · 目录内 Vulkan 库 " + so.getName();
                                cands.add(c);
                            } else {
                                Cand c = new Cand(dlabel + "（目录内没找到 Vulkan 库）",
                                        dir == null ? "?" : dir.getAbsolutePath(), srcTag, 0);
                                c.kind = KIND_UNKNOWN;
                                c.cls = "元数据 driver，但 nativeLibraryDir 里没有 Vulkan 库";
                                cands.add(c);
                            }
                        }
                        if (isRenderer) {
                            String metaGl = metaStr(md, "glLib");
                            if (metaGl == null) metaGl = metaStr(md, "gl");
                            String metaEgl = metaStr(md, "eglLib");
                            if (metaEgl == null) metaEgl = metaStr(md, "egl");
                            String rlabel = friendlyName(pi.packageName, rndName) + " [" + pi.versionName + "]";
                            String glEgl = glEglOf(rndName != null ? rndName : pi.packageName,
                                    dir == null ? null : dir.getAbsolutePath(), metaGl, metaEgl);
                            String glLib = null;
                            int i0 = glEgl.indexOf("GL: "), i1 = glEgl.indexOf(" · EGL");
                            if (i0 >= 0 && i1 > i0) glLib = glEgl.substring(i0 + 4, i1).trim();
                            File so = pickRendererSo(dir, glLib);
                            Cand c = new Cand(rlabel, so == null ? (dir == null ? "?" : dir.getAbsolutePath())
                                                                 : so.getAbsolutePath(), srcTag, 0);
                            c.kind = KIND_RENDERER;
                            c.extra = glEgl;
                            c.cls = "元数据 " + (v2 ? "fclPlugin_V2" : "renderer")
                                    + (so == null ? "（目录内没有 .so）" : " · 代表库 " + so.getName());
                            cands.add(c);
                        }
                        if (nativeP && !isDriver && !isRenderer) {
                            Cand c = new Cand("原生库 " + pi.packageName + " [" + pi.versionName + "]",
                                    dir == null ? "?" : dir.getAbsolutePath(), srcTag, 0);
                            c.kind = KIND_UNKNOWN;
                            c.cls = "元数据 FCLNativePlugin（第三类，不参与测试）";
                            cands.add(c);
                        }
                    }
                } catch (Throwable t) { log("  ✗ 枚举已安装包失败: " + t); }

                /* (b) 本地目录：全部 .so 都收进来分类（不再按文件名筛掉） */
                List<File> dirs = new ArrayList<>();
                dirs.add(new File("/sdcard/Mali驱动项目/驱动"));
                dirs.add(new File("/sdcard/Mali驱动项目/.probe"));
                dirs.add(new File("/sdcard/Download"));
                dirs.add(drop);
                for (File dir : dirs) {
                    File[] fs = dir.listFiles();
                    if (fs == null) {
                        /* 「不存在」与「存在但读不到」分开报（合成一句会被误读成权限问题） */
                        log(dir.exists()
                                ? "  · 目录存在但读不到（权限 / 分区存储限制）：" + dir.getAbsolutePath()
                                : "  · 目录不存在：" + dir.getAbsolutePath());
                        continue;
                    }
                    for (File f : fs) {
                        if (!f.isFile()) continue;
                        String n = f.getName().toLowerCase(Locale.ROOT);
                        if (!n.endsWith(".so")) continue;
                        Cand bc = new Cand("[so] " + f.getName(), f.getAbsolutePath(),
                                "目录 " + dir.getName(), icdScore(n));
                        bc.needClassify = true;                 /* 裸 .so：没有元数据可依 */
                        cands.add(bc);
                    }
                }
                log("  候选共 " + cands.size() + " 个（插件走元数据，裸 .so 才回退 nativeClassify）");

                final List<Cand> grpDrv = new ArrayList<>(), grpRnd = new ArrayList<>(), grpUnk = new ArrayList<>();
                for (Cand c : cands) {
                    if (c.needClassify) {                       /* 只有裸 .so 才分类（静态读符号表） */
                        String cls;
                        try { cls = nativeClassify(c.path); } catch (Throwable t) { cls = "打不开/未知"; }
                        if (cls == null || cls.length() == 0) cls = "打不开/未知";
                        c.cls = cls;
                        c.kind = (cls.contains("ICD") || cls.contains("垫片")) ? KIND_DRIVER
                               : cls.contains("渲染器") ? KIND_RENDERER : KIND_UNKNOWN;
                        log("    · " + new File(c.path).getName() + " ⇒ " + cls + "（" + c.source + "）");
                    } else {
                        log("    · " + c.label + " ⇒ " + c.cls + "（元数据判定，未 dlopen）");
                    }
                }
                for (Cand c : cands) {
                    if (c.kind == KIND_DRIVER) grpDrv.add(c);
                    else if (c.kind == KIND_RENDERER) grpRnd.add(c);
                    else grpUnk.add(c);
                }
                java.util.Collections.sort(grpDrv, (a, b) -> b.score - a.score);          /* 像 ICD 的在前 */
                java.util.Collections.sort(grpRnd, (a, b) -> a.label.compareToIgnoreCase(b.label));
                /* 未知组：像 GL/翻译层的排后面（looksNonIcd 仅用于排序，不再用来猜分类） */
                java.util.Collections.sort(grpUnk, (a, b) ->
                        (looksNonIcd(new File(a.path).getName().toLowerCase(Locale.ROOT)) ? 1 : 0)
                      - (looksNonIcd(new File(b.path).getName().toLowerCase(Locale.ROOT)) ? 1 : 0));
                final int nd = grpDrv.size(), nr = grpRnd.size(), nu = grpUnk.size();
                ui.post(() -> {
                    for (Cand c : grpDrv) addEntry(driverGroup, driverPaths, c, "驱动");
                    for (Cand c : grpRnd) addEntry(rendererGroup, rendererPaths, c, "渲染器");
                    for (Cand c : grpUnk)          /* 分类已解决，不再加 ⚠ 猜测 */
                        unknownBox.addView(text(c.label + "  [" + c.cls + "]", 12, C_DIM, false));
                    driverCount = nd; rendererCount = nr;
                    log("  ① Vulkan 驱动 " + nd + " 个 ｜ ② 渲染器 " + nr + " 个 ｜ ③ 未知 " + nu + " 个");
                    if (nd == 0) log("  没有可用的 Vulkan 驱动 —— 可点②从 APK 提取，或把 .so 放进 "
                            + drop.getAbsolutePath());
                    refreshDeviceCards();
                    updateSelectedHeader();
                    endTask();
                });
            } catch (Throwable t) {
                log("  ✗ 扫描异常: " + t);
                ui.post(() -> { setStatus("扫描异常: " + t, C_BAD); endTask(); });
            }
        }, "gpu-scan").start();
    }

    /**
     * 列表里加一个驱动。⚠️ RadioButton 必须是 RadioGroup 的**直接子 View** ——
     * 中间嵌一层 LinearLayout 就不会被 RadioGroup 追踪（互斥、getCheckedRadioButtonId
     * 会失效）。第二行「来源 · 大小 · sha256」用 Spannable 缩到 0.78 倍 + 灰色，
     * 挂在同一个 RadioButton 文本里；元信息后台算好后回来刷新。
     */
    /** 入库：c.cls 决定它进 ①驱动 还是 ②渲染器（调用方已分好组）。元信息后台算。 */
    private RadioButton addEntry(RadioGroup group, List<String> paths, Cand c, String role) {
        paths.add(c.path);
        driverLabels.put(c.path, c.label + " ［" + shortTag(c.source) + "］");   /* ④ 同名也分得清 */
        final int idx = paths.size() - 1;

        RadioButton rb = new RadioButton(this);
        tint(rb, C_TEXT);
        rb.setTextSize(13);
        rb.setTag(idx);
        rb.setText(twoLine(c.label, (c.extra.length() > 0 ? c.extra + " · " : "")
                + c.cls + " · " + c.source + " · 计算中…"));
        group.addView(rb);
        if (paths.size() == 1) rb.setChecked(true);

        final String p = c.path, lbl = c.label, cls = c.cls, src = c.source, extra = c.extra;
        new Thread(() -> {
            long len = new File(p).length();
            String h = sha16(new File(p));
            final String meta = (extra.length() > 0 ? extra + " · " : "") + cls + " · " + src + " · "
                    + (len / 1048576) + "." + ((len % 1048576) * 10 / 1048576)
                    + " MB · sha256(16) " + h;
            ui.post(() -> rb.setText(twoLine(lbl, meta)));
        }).start();
        return rb;
    }

    /** 从 APK 提取出来的那个 .so（调用方已分类）⇒ 归到对应组并选中。 */
    private RadioButton addDriver(String label, String path, String source) {
        Cand c = new Cand(label, path, source, icdScore(new File(path).getName().toLowerCase(Locale.ROOT)));
        return addEntry(driverGroup, driverPaths, c, "驱动");
    }

    /** 第一行正常大小，第二行 0.78 倍 + 灰色（纯平台 API，不需要 XML/HTML 资源）。 */
    private CharSequence twoLine(String main, String meta) {
        android.text.SpannableString s = new android.text.SpannableString(main + "\n" + meta);
        int start = main.length() + 1;
        s.setSpan(new android.text.style.RelativeSizeSpan(0.78f), start, s.length(),
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        s.setSpan(new android.text.style.ForegroundColorSpan(C_DIM), start, s.length(),
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        return s;
    }

    /** 头部常驻：【驱动: X ｜ 渲染器: Y】（渲染器是"跑在驱动之上"的那一层）。 */
    private void updateSelectedHeader() {
        final String d = selectedPath(), r = selectedRendererPath();
        if (headerDriver != null) {                          /* App Bar 里的"驱动 chip"只放短标签 */
            headerDriver.setText("驱动: " + (d == null ? "未选择" : new File(d).getName()));
            headerDriver.setTextColor(c(d == null ? C_WARN : C_ACCENT));
        }
        StringBuilder s = new StringBuilder();
        s.append("[① Vulkan 驱动] ").append(d == null ? "未选择" : d).append('\n');
        if (d != null) {
            s.append("    大小 ").append(new File(d).length() / 1048576)
             .append(" MB · sha256(16) ").append(sha16(new File(d))).append('\n');
            s.append("    来源 ").append(d.equals(baselinePath) ? "（当前 A/B 基准）" : "列表选中").append('\n');
        }
        s.append("[② 渲染器] ").append(r == null ? "未选择" : r).append('\n');
        if (r != null) {
            s.append("    大小 ").append(new File(r).length() / 1048576)
             .append(" MB · sha256(16) ").append(sha16(new File(r))).append('\n');
            s.append("    本版仅分类显示；「渲染器 × 驱动」组合测试是下一步").append('\n');
        }
        if (ovDrv != null) ovDrv.setText(s.toString());
    }

    private String selectedPath() {
        return selectedIn(driverGroup, driverPaths);
    }

    /** ② 渲染器组的选择（本版只记录+显示，用于下一步的组合测试）。 */
    private String selectedRendererPath() {
        return selectedIn(rendererGroup, rendererPaths);
    }

    private String selectedIn(RadioGroup group, List<String> paths) {
        if (group == null) return null;
        int id = group.getCheckedRadioButtonId();
        if (id < 0) return null;
        View rb = group.findViewById(id);
        if (rb == null || !(rb.getTag() instanceof Integer)) return null;
        int idx = (Integer) rb.getTag();
        return (idx >= 0 && idx < paths.size()) ? paths.get(idx) : null;
    }

    /* ---- FCL 元数据读取（key 字符串照抄 PluginManager.kt，注意 FCLNativePlugin 不是 META_NATIVE_PLUGIN） ---- */
    private Object metaRaw(android.os.Bundle md, String key) { return md == null ? null : md.get(key); }

    private String metaStr(android.os.Bundle md, String key) {
        Object o = metaRaw(md, key);
        if (o == null) return null;
        String v = String.valueOf(o);
        return v.length() == 0 ? null : v;
    }

    private boolean metaBool(android.os.Bundle md, String key) {
        Object o = metaRaw(md, key);
        if (o instanceof Boolean) return (Boolean) o;
        if (o instanceof String) return "true".equalsIgnoreCase((String) o);
        if (o instanceof Integer) return ((Integer) o) != 0;
        return o != null;
    }

    /** 由"渲染器名 / 目录 / 插件自带库名"推出 GL 与 EGL 库（并标明来源，便于核对）。 */
    private String glEglOf(String name, String dirPath, String metaGl, String metaEgl) {
        String gl = metaGl, egl = metaEgl, src = "插件元数据";
        if (gl == null || egl == null) {
            String n = name == null ? "" : name.toLowerCase(Locale.ROOT);
            for (String[] r : FCL_RENDERERS) {
                if (n.contains(r[0].toLowerCase(Locale.ROOT))) {
                    if (gl == null)  gl  = r[1];
                    if (egl == null) egl = r[2];
                    src = "FCL 内置表";
                    break;
                }
            }
        }
        if ((gl == null || egl == null) && dirPath != null) {
            File[] fs = new File(dirPath).listFiles();
            if (fs != null) for (File f : fs) {
                String fn = f.getName();
                String lo = fn.toLowerCase(Locale.ROOT);
                if (egl == null && lo.startsWith("libegl")) egl = fn;
                if (gl == null && !lo.startsWith("libegl") && lo.endsWith(".so")) gl = fn;
            }
            if (!"FCL 内置表".equals(src)) src = "目录实际文件";
        }
        return "GL: " + (gl == null ? "—" : gl) + " · EGL: " + (egl == null ? "—" : egl) + "  [" + src + "]";
    }

    /** 驱动插件：在它的 nativeLibraryDir 里找最像 Vulkan ICD 的 .so（纯名字打分，**不 dlopen**）。 */
    private File pickVulkanSo(File dir) {
        File[] fs = dir == null ? null : dir.listFiles();
        if (fs == null) return null;
        File best = null; int bestScore = 0;
        for (File f : fs) {
            if (!f.isFile() || !f.getName().endsWith(".so")) continue;
            int sc = icdScore(f.getName().toLowerCase(Locale.ROOT));
            if (sc > bestScore) { bestScore = sc; best = f; }
        }
        return best;
    }

    /** 渲染器插件：优先它自带的 GL 库文件，否则目录里最大的 .so。 */
    private File pickRendererSo(File dir, String glLib) {
        File[] fs = dir == null ? null : dir.listFiles();
        if (fs == null) return null;
        File gl = null, big = null;
        for (File f : fs) {
            if (!f.isFile() || !f.getName().endsWith(".so")) continue;
            if (glLib != null && f.getName().equalsIgnoreCase(glLib)) gl = f;
            if (big == null || f.length() > big.length()) big = f;
        }
        return gl != null ? gl : big;
    }

    /**
     * 显示名：**优先用插件的 `driver=` 元数据**（就是用户在启动器里看到的名字，如
     * "PanVK G720 26.3" / "Turnip (Mesa 26.1.6)"）；没有元数据时按包名给出同一套名字
     * （MobileGL Magma/Espryt、MobileGLues、Kopper Zink、GL4ES、VirGLRenderer…）。
     */
    private String friendlyName(String pkg, String drvName) {
        if (drvName != null && drvName.length() > 0) return drvName;
        String p = pkg == null ? "" : pkg.toLowerCase(Locale.ROOT);
        if (p.contains("magma"))       return "MobileGL Magma";
        if (p.contains("esprty"))      return "MobileGL Espryt";
        if (p.contains("mobileglues")) return "MobileGLues";
        if (p.contains("mobilegl"))    return "MobileGL";
        if (p.contains("zink"))        return "Kopper Zink";
        if (p.contains("gl4es"))       return "GL4ES / gl4es+";
        if (p.contains("virgl"))       return "VirGLRenderer";
        if (p.contains("krypton"))     return "Krypton Wrapper";
        if (p.contains("freedreno"))   return "Freedreno (Adreno)";
        if (p.contains("turnip"))      return "Turnip";
        if (p.contains("purplevk"))    return "PurpleVK";
        if (p.contains("panvk") || p.contains("g720") || p.contains("zenith")) return "PanVK";
        return pkg == null ? "?" : pkg;
    }

    /** ICD 像不像（文件名特征打分）：只用于**分组内排序**，不再参与分类。 */
    private int icdScore(String n) {
        if (n.startsWith("libvulkan_") || n.equals("libvulkan.so")) return 100;
        if (n.contains("freedreno") || n.contains("turnip") || n.contains("panfrost")
                || n.contains("panvk") || n.contains("adreno") || n.contains("mali")) return 80;
        if (n.contains("vulkan")) return 60;
        if (n.contains("icd")) return 50;
        return 0;
    }

    /** 明确不像 Vulkan ICD 的（着色器转换 / 软件 GL / GL 翻译层）⇒ 标注而不是静默混进来。 */
    private boolean looksNonIcd(String n) {
        return n.contains("shaderconv") || n.contains("osmesa") || n.contains("mobileglues")
                || n.contains("mobilegl") || n.contains("swiftshader") || n.contains("gl4es");
    }

    /** 喂驱动目录：不需要任何权限；不存在就建（用户最省事的投放点）。 */
    private File driversDir() {
        File d;
        try {
            File ext = getExternalFilesDir(null);
            d = (ext == null) ? new File(getFilesDir(), "drivers") : new File(ext, "drivers");
        } catch (Throwable t) { d = new File(getFilesDir(), "drivers"); }
        try { if (!d.isDirectory()) d.mkdirs(); }
        catch (Throwable t) { log("  ✗ 无法创建喂驱动目录 " + d + " ：" + t); }                 /* D11：不再静默 */
        if (!d.isDirectory()) log("  ✗ 喂驱动目录不可用：" + d);
        return d;
    }

    /** 扫描候选项（先收集、按 ICD 相似度排序，再进列表）。 */
    /* 分类（照抄 FCL PluginManager.kt 的权威规则）：一个包**可以同时是驱动和渲染器**（types 是 Set） */
    private static final int KIND_DRIVER = 0, KIND_RENDERER = 1, KIND_UNKNOWN = 2;

    private static class Cand {
        final String label, path, source; final int score;
        String cls = "";                       /* 人类可读的分类标签 */
        String extra = "";                     /* 渲染器：GL/EGL 库名那一行 */
        int kind = KIND_UNKNOWN;
        boolean needClassify = false;          /* true = 裸 .so，只有它才回退 nativeClassify */
        Cand(String label, String path, String source, int score) {
            this.label = label; this.path = path; this.source = source; this.score = score;
        }
    }

    /* FCL RendererManager.kt 内置渲染器的 (GL 库, EGL 库) —— 权威值，逐字抄 */
    private static final String[][] FCL_RENDERERS = {
        { "Holy-GL4ES",      "libgl4es_114.so", "libEGL.so" },
        { "VirGLRenderer",   "libOSMesa_81.so", "libEGL.so" },
        { "VGPU",            "libvgpu.so",      "libEGL.so" },
        { "Zink",            "libglxshim.so",   "libEGL_mesa.so" },
        { "Freedreno",       "libOSMesa_8.so",  "libEGL.so" },
        { "Krypton Wrapper", "libng_gl4es.so",  "libEGL.so" },
    };

    /** 每个插件自带的 env 串（v2.3 组合测试要用：pojavEnv/boatEnv）——先存着。 */
    private final java.util.HashMap<String, String> pluginEnv = new java.util.HashMap<>();
    /** 每个插件读到的元数据摘要（日志核对用）。 */
    private final java.util.HashMap<String, String> pluginMeta = new java.util.HashMap<>();

    private void pickApkForExtract() { pickFile(REQ_PICK_APK); }
    private void pickApkForInstall() { pickFile(REQ_PICK_APK + 1); }

    private void pickFile(int req) {
        try {
            Intent i = new Intent(Intent.ACTION_GET_CONTENT);
            i.setType("*/*");
            i.addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(Intent.createChooser(i, "选择 APK 文件"), req);
        } catch (Throwable t) { log("  ✗ 打开文件选择器失败: " + t); }
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) { log("  （未选择文件）"); return; }
        Uri uri = data.getData();
        if (req == REQ_PICK_APK) extractSoFromApk(uri);
        else if (req == REQ_PICK_APK + 1) installApk(uri);
    }

    private File copyUriToCache(Uri uri, String name) {
        try {
            File dst = new File(getCacheDir(), name);
            InputStream in = getContentResolver().openInputStream(uri);
            if (in == null) { log("  ✗ 无法打开该 URI（权限或类型问题）"); return null; }
            OutputStream out = new FileOutputStream(dst);
            byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            in.close(); out.close();
            return dst;
        } catch (Throwable t) { log("  ✗ 读取文件失败: " + t); return null; }
    }

    private void extractSoFromApk(Uri uri) {
        log("\n=== ② 从 APK 提取驱动 .so ===");
        setStatus("正在读取 APK…", C_NEUTRAL);
        new Thread(() -> {
            File apk = copyUriToCache(uri, "src.apk");
            if (apk == null) { setStatus("读取 APK 失败", C_BAD); return; }
            log("  APK: " + apk.length() + " 字节");
            try (ZipFile z = new ZipFile(apk)) {
                ZipEntry best = null; long bestScore = -1;
                StringBuilder cand = new StringBuilder();
                java.util.Enumeration<? extends ZipEntry> e = z.entries();
                while (e.hasMoreElements()) {
                    ZipEntry en = e.nextElement();
                    String n = en.getName(); String low = n.toLowerCase(Locale.ROOT);
                    if (!low.startsWith("lib/") || !low.endsWith(".so")) continue;
                    boolean arm64 = low.startsWith("lib/arm64-v8a/");
                    boolean looksIcd = low.contains("vulkan") || low.contains("panfrost")
                            || low.contains("freedreno") || low.contains("icd") || low.contains("adreno");
                    long score = (arm64 ? 1L << 40 : 0) + (looksIcd ? 1L << 39 : 0) + en.getSize();
                    cand.append("\n      · ").append(n).append("  ").append(en.getSize())
                        .append(arm64 ? "  [arm64]" : "  [非arm64]")
                        .append(looksIcd ? " [像ICD]" : "");
                    if (score > bestScore) { bestScore = score; best = en; }
                }
                log("  包内 .so 候选:" + cand);
                if (best == null) { log("  ✗ 该 APK 内没有 lib/**/*.so"); setStatus("该 APK 没有 .so", C_BAD); return; }
                String extPath = null;
                try { File d = getExternalFilesDir(null); extPath = d == null ? null : d.getAbsolutePath(); }
                catch (Throwable ignored) { }
                File dir = extPath == null ? getFilesDir() : new File(extPath, "drivers");
                dir.mkdirs();
                File out = new File(dir, new File(best.getName()).getName());
                InputStream in = z.getInputStream(best);
                OutputStream os = new FileOutputStream(out);
                byte[] buf = new byte[1 << 16]; int n;
                while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                in.close(); os.close();
                log("  ✓ 提取 " + best.getName() + " (" + best.getSize() + " 字节)");
                log("    → " + out.getAbsolutePath());
                log("    sha256(16)=" + sha16(out));
                String fcls;
                try { fcls = nativeClassify(out.getAbsolutePath()); } catch (Throwable t) { fcls = "打不开/未知"; }
                if (fcls == null || fcls.length() == 0) fcls = "打不开/未知";
                log("    分类: " + fcls);
                final String op = out.getAbsolutePath(), on = out.getName(), cls2 = fcls;
                ui.post(() -> {
                    Cand c = new Cand("[提取] " + on, op, "APK 提取",
                            icdScore(on.toLowerCase(Locale.ROOT)));
                    c.cls = cls2;
                    RadioButton rb = null;
                    if (cls2.contains("ICD") || cls2.contains("垫片")) rb = addEntry(driverGroup, driverPaths, c, "驱动");
                    else if (cls2.contains("渲染器"))                  rb = addEntry(rendererGroup, rendererPaths, c, "渲染器");
                    if (rb != null) rb.setChecked(true);              /* 提取即选中 */
                    updateSelectedHeader();
                    setStatus("已提取 " + on + "（" + cls2 + "）", C_OK);
                });
            } catch (Throwable t) {
                log("  ✗ 解压失败: " + t);
                setStatus("解压失败", C_BAD);
            }
        }).start();
    }

    private android.content.IntentSender getIntentSenderForInstall() {
        Intent i = new Intent(this, MainActivity.class);
        return android.app.PendingIntent.getActivity(this, 0, i,
                android.app.PendingIntent.FLAG_IMMUTABLE).getIntentSender();
    }

    private void installApk(Uri uri) {
        log("\n=== ③ 安装 APK（会话式） ===");
        File apk = copyUriToCache(uri, "install.apk");
        if (apk == null) return;
        try {
            android.content.pm.PackageInstaller pi = getPackageManager().getPackageInstaller();
            android.content.pm.PackageInstaller.SessionParams sp =
                    new android.content.pm.PackageInstaller.SessionParams(
                            android.content.pm.PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            int sid = pi.createSession(sp);
            android.content.pm.PackageInstaller.Session sess = pi.openSession(sid);
            InputStream in = new FileInputStream(apk);
            OutputStream out = sess.openWrite("base.apk", 0, apk.length());
            byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            in.close(); out.close();
            sess.commit(getIntentSenderForInstall());
            log("  ✓ 已提交安装会话（系统会弹安装界面）");
            setStatus("已提交安装会话", C_OK);
        } catch (Throwable t) {
            log("  ✗ 会话式安装失败: " + t);
        }
    }

    /* =========================================================================
     *  ③ 测试
     * ========================================================================= */
    private View buildTestPage() {
        LinearLayout col = col();
        col.addView(card(section("单项测试"),
                note("三项互相独立：冒烟只看能不能建实例/设备；三角形会真画并逐像素校验；"
                   + "能力清单倒出设备/限制/扩展/内存堆。"),
                actionBtn("ICD 冒烟（dlopen → 协商 → 实例 → 设备 → 队列）", v -> runSmoke()),
                actionBtn("三角形绘制 + 像素校验（应出现纯红三角形）", v -> runTri()),
                actionBtn("能力清单（设备 / 限制 / 扩展 / 内存堆）", v -> runCaps())));

        capsText = text("还没跑能力清单。", 11, C_TEXT, false);
        capsText.setTypeface(Typeface.MONOSPACE);
        capsText.setLineSpacing(0, 1.15f);
        col.addView(card(section("能力清单结果（完整内容在「日志」页）"), capsText));

        /* (c)：正确性校验与跑分**分离**（学 glmark2 --validate） */
        validNote = text("还没做正确性校验。", 11, C_TEXT, false);
        validNote.setLineSpacing(0, 1.2f);
        col.addView(card(section("正确性校验（⑤，与跑分分离）"), validNote,
                note("「能显示」不等于「渲染正确」—— 校验结果只在这里，**不混进跑分卡**。")));

        col.addView(card(section("结果怎么读"),
                note("绿 = 通过（驱动可用 / 绘制正确）\n"
                   + "红 = 失败（DEVICE_LOST、无绘制输出、异常像素、提交失败）\n"
                   + "黄 = 警告（退化、跳过、超时但未崩）\n"
                   + "「失败=N」N>0 即为不稳定，那一项的分数不可用。")));
        return scroll(col);
    }

    /* =========================================================================
     *  ④ 画面（实时渲染）
     * ========================================================================= */
    private View buildPicturePage() {
        LinearLayout col = col();
        renderView = new ImageView(this);
        renderView.setAdjustViewBounds(true);
        renderView.setScaleType(ImageView.ScaleType.CENTER_INSIDE);
        renderView.setBackgroundColor(c(C_RENDER_BG));
        col.addView(card(section("实时画面（" + RENDER_W + "x" + RENDER_H + "，GPU 渲染 → Bitmap）"),
                renderView, note("原生侧每帧提交一次并等 fence 完成，因此这里的 FPS 就是这套驱动的真实上限。")));

        renderFps = text("— fps", 30, C_ACCENT, true);
        renderFps.setGravity(Gravity.CENTER_HORIZONTAL);
        renderInfo = text("未启动", 12, C_DIM, false);
        renderInfo.setGravity(Gravity.CENTER_HORIZONTAL);
        renderBtn = actionBtn("▶ 开始渲染", v -> toggleRender());
        col.addView(card(renderFps, renderInfo, renderBtn,
                note("说明：切走本页或退出 App 会自动停止渲染（按规格）。"
                   + "若初始化失败，请先确认这个驱动在「测试」页能通过冒烟。")));
        return scroll(col);
    }

    private void toggleRender() {
        if (renderRunning) stopRender("手动停止");
        else startRender();
    }

    private void startRender() {
        final String p = selectedPath();
        if (p == null) { toast("先在「驱动」页选一个驱动"); return; }
        if (renderRunning) return;
        if (!claim("画面渲染")) return;                                   /* B5：与跑分互斥 */
        renderBtn.setText("■ 停止渲染");
        tint(renderBtn, C_BAD);
        setRenderStatus("初始化中…", C_WARN);
        log("\n=== ④ 画面页：初始化渲染 ===");
        new Thread(() -> {
            String staged = stageDriver(p);
            if (staged == null) { setRenderStatus("暂存驱动失败", C_BAD); return; }
            beginTask("画面初始化");
            String out;
            try { out = nativeRenderInit(staged, RENDER_W, RENDER_H); }
            finally { endTask(); }
            logBlock("renderInit", out);
            if (out == null || !out.contains("OK 渲染就绪")) {
                ui.post(() -> { renderBtn.setText("▶ 开始渲染"); tint(renderBtn, C_TEXT); });
                setRenderStatus("初始化失败（详见日志）", C_BAD);
                release();                                                /* B5：初始化失败也要放锁 */
                return;
            }
            /* 乒乓双缓冲：写的那张永远不是屏幕上正在显示的那张，避免 UI 线程与渲染线程
             * 同时碰同一个 Bitmap 的像素（撕裂 / 未定义行为）。 */
            renderBufs[0] = Bitmap.createBitmap(RENDER_W, RENDER_H, Bitmap.Config.ARGB_8888);
            renderBufs[1] = Bitmap.createBitmap(RENDER_W, RENDER_H, Bitmap.Config.ARGB_8888);
            shownBmp = renderBufs[0];
            ui.post(() -> renderView.setImageBitmap(shownBmp));
            renderFrameIdx = 0;
            renderRunning = true;
            beginTask("画面渲染（卡住 ⇒ 驱动在提交/回读里挂住）");
            renderThread = new Thread(this::renderLoop, "gpu-render");
            renderThread.start();
            setRenderStatus("渲染中…", C_OK);
        }).start();
    }

    private void renderLoop() {
        int i = 0;
        int buf = 1;
        while (renderRunning) {
            final Bitmap target = renderBufs[buf];
            buf ^= 1;                                     /* 只写"当前没挂在 ImageView 上"的那张 */
            String e1 = nativeRenderFrame(i++);
            if (e1 != null && e1.contains("X ")) { logBlock("渲染失败", e1); break; }
            String e2 = nativeRenderBlit(target);
            if (e2 != null && e2.contains("X ")) { logBlock("回读失败", e2); break; }
            progressToken++;                               /* 看门狗：这一帧有进展 ⇒ 静默续期 */
            final int shown = i;
            ui.post(() -> {
                shownBmp = target;
                renderView.setImageBitmap(target);
                renderView.invalidate();
                renderInfo.setText("已渲染 " + shown + " 帧");
            });
            if (i % 30 == 0) {
                String st = nativeRenderStats();
                final String fps = grab(st, "平均FPS=([0-9.]+)");
                final String ms  = grab(st, "平均帧时=([0-9.]+)");
                if (fps != null) ui.post(() -> renderFps.setText(fps + " fps"));
                if (ms != null) ui.post(() -> renderInfo.setText("已渲染 " + shown + " 帧 · 平均帧时 " + ms + " ms"));
            }
        }
        final int total = i;
        endTask();
        release();          /* B5：循环因出错自行 break 时也要放锁（release 幂等） */
        ui.post(() -> { renderBtn.setText("▶ 开始渲染"); tint(renderBtn, C_TEXT); });
        log("  渲染循环结束，共 " + total + " 帧。");
    }

    /**
     * 停止渲染。⚠️ **绝不能在 UI 线程 join** —— nativeRenderFrame 里的 fence 最长等 3 秒，
     * 在 UI 线程等就是 ANR。这里只置标志，join + nativeRenderStop 全放后台线程。
     */
    private void stopRender(String why) {
        final boolean was = renderRunning;
        renderRunning = false;
        final Thread t = renderThread;
        renderThread = null;
        setRenderStatus("正在停止…", C_WARN);
        new Thread(() -> {
            if (t != null) { try { t.join(3600); } catch (InterruptedException ignored) { } }
            try { nativeRenderStop(); }
            catch (Throwable t2) { log("  ! nativeRenderStop 异常: " + t2); }               /* D11（变量名 t2：外层已有 Thread t） */
            endTask();
            release();                                                                      /* B5：画面任务结束 */
            ui.post(() -> { renderBtn.setText("▶ 开始渲染"); tint(renderBtn, C_TEXT); });
            setRenderStatus("已停止（" + why + "）", C_NEUTRAL);
            if (was) log("  画面渲染已停止：" + why);
        }, "gpu-render-stop").start();
    }

    private void setRenderStatus(String s, int color) {
        ui.post(() -> { if (renderInfo != null) { renderInfo.setText(s); tint(renderInfo, color); } });
    }

    /* =========================================================================
     *  ⑤ 跑分
     * ========================================================================= */
    private View buildBenchPage() {
        LinearLayout col = col();
        benchFill = bigNum("—");
        benchBlit = bigNum("—");
        benchDraw = bigNum("—");
        col.addView(card(section("单项跑分（每项 5 秒）"),
                actionBtn("填充率 fill", v -> runBench("fill")),
                actionBtn("拷贝带宽 blit", v -> runBench("blit")),
                actionBtn("三角形吞吐 draw", v -> runBench("draw"))));

        col.addView(card(section("最近成绩"),
                numRow("填充率", benchFill, "Mpixel/s"),
                numRow("拷贝带宽", benchBlit, "GB/s"),
                numRow("三角形吞吐", benchDraw, "个/s")));

        /* (c)：GPU 时间 / 墙钟 / 占空比 —— 与 glmark2 的 --results fps,cpu,shader 同源：
         * GPU 时间来自时间戳，墙钟含提交/驱动/呈现开销，两个数必须并排看。 */
        benchGpu  = bigNum("—");
        benchWall = bigNum("—");
        benchDuty = bigNum("—");
        col.addView(card(section("GPU 时间 / 墙钟 / 占空比"),
                numRow("GPU 时间", benchGpu,  "ms"),
                numRow("墙钟",     benchWall, "s"),
                numRow("占空比",   benchDuty, "%"),
                note("占空比 = GPU 时间 ÷ 墙钟。**≈100% 才说明这一项真的在压 GPU**；"
                   + "远小于 100% 说明测到的主要是提交/驱动往返（此前的 1.3 ms 提交往返就是这么来的）。")));

        benchProof = text("（等待本次运行的自证行…）", 11, C_DIM, false);
        benchProof.setTypeface(Typeface.MONOSPACE);
        tint(benchProof, C_DIM);
        benchProof.setLineSpacing(0, 1.15f);
        col.addView(card(section("本次口径 / 自证行（原样来自原生）"), benchProof,
                note("例如 `fill 负载: 全屏三角形 + 每像素 64 次循环` —— 口径写在结果旁边，数字才经得起对比。")));

        benchScore = bigNum("—");
        col.addView(card(section("综合分"), benchScore,
                note("口径（与原生一致）：分 = 填充率 + 带宽×100 + 三角形吞吐÷1000；"
                   + "**失败档按 0 计入**，所以含失败项的分数不可比。")));

        benchNote = text("还没有本次结果。", 11, C_DIM, false);
        benchNote.setLineSpacing(0, 1.2f);
        col.addView(card(section("本次结果（成绩归属）"), benchNote,
                note("每次开跑前旧成绩一律作废；下面会标出成绩来自哪个驱动 —— "
                   + "避免把上一次（例如系统驱动）的数字当成这一次的。")));

        rankRulesTx = note("排序口径（透明）：综合分 = 0.4×fill/最优fill + 0.3×blit/最优blit + 0.3×draw/最优draw，"
                + "各自相对最优归一化 ⇒ 满分 100；**任一项失败/DEVICE_LOST ⇒ 不计入排名**，单列失败表。"
                + "每档 5 秒，逐个驱动串行。");
        rankTableTx = text("（还没跑）", 11, C_TEXT, false);
        rankTableTx.setTypeface(Typeface.MONOSPACE);
        rankTableTx.setLineSpacing(0, 1.2f);
        failBox = new LinearLayout(this);
        failBox.setOrientation(LinearLayout.VERTICAL);
        rankFailTx = text("（无失败项）", 11, C_DIM, false);
        rankFailTx.setTypeface(Typeface.MONOSPACE);

        col.addView(card(section("⚑ 一键跑全部驱动（排名）"),
                rankRulesTx,
                btnPrimary("一键跑全部驱动（逐个串行 · fill/blit/draw 各 5 秒）", v -> runAllDriversSweep()),
                btnTonal("复制排名表", v -> copyRankTable()),
                btnTonal("一键跑全部渲染器（转译链未接入）", v -> runAllRenderersStub()),
                rankTableTx));

        col.addView(card(section("失败项（不计入排名）"), rankFailTx));

        col.addView(card(section("全流程与对比"),
                actionBtn("一键全流程自测（冒烟→三角形→能力清单→三种跑分）", v -> runAll()),
                actionBtn("把当前驱动设为对比基准 A", v -> setBaseline()),
                actionBtn("A/B 对比：基准 A vs 当前 B", v -> compareAB()),
                note("A/B 会依次跑 冒烟 + 三角形 + 三种跑分，最后两段汇总数字可直接对比。")));
        return scroll(col);
    }

    /* =========================================================================
     *  ⑥ 日志
     * ========================================================================= */
    private View buildLogPage() {
        LinearLayout col = col();
        col.addView(card(section("日志操作"),
                actionBtn("读取启动器日志（FCL / ZalithLauncher / ZL2）", v -> readLauncherLogs()),
                actionBtn("授权「所有文件访问」", v -> requestAllFiles()),
                actionBtn("导出 / 分享日志", v -> exportLog()),
                actionBtn("清空日志", v -> {
                    synchronized (fullLog) { fullLog.setLength(0); }
                    sharedTruncated = false;      /* ★ C10：复位 ⇒ 下次写共享日志会重新截断（否则旧内容留着） */
                    logView.setText("");
                    log("（日志已清空；共享日志 gputest-last.log 的截断标记已复位）");
                })));

        col.addView(section("实时日志"));
        logView = new TextView(this);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setTextSize(10);
        tint(logView, C_LOG_TX);
        bgKeys.put(logView, C_LOG_BG * 100 + 12);
        logView.setBackground(cardBg(c(C_LOG_BG), c(C_STROKE), 12));
        int p = dp(10);
        logView.setPadding(p, p, p, p);
        logView.setMovementMethod(new ScrollingMovementMethod());
        logView.setTextIsSelectable(true);
        col.addView(logView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return col;   /* 日志页自己不滚动，交给 logView 内部滚动 */
    }

    /* =========================================================================
     *  原生调用与结果处理
     * ========================================================================= */
    private String stageDriver(String src) {
        if (src == null) return null;
        if ("system".equals(src)) return "system";            /* 交给原生走系统 loader */
        try {
            String h = sha16(new File(src));
            File dst = new File(getFilesDir(), "drv_" + h + ".so");
            if (dst.isFile() && dst.length() > 0) return dst.getAbsolutePath();
            InputStream in = new FileInputStream(src);
            OutputStream out = new FileOutputStream(dst);
            byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            in.close(); out.close();
            dst.setReadable(true, true);
            return dst.getAbsolutePath();
        } catch (Throwable t) { log("  ✗ 暂存驱动失败: " + t); return null; }
    }

    private void runNative(final String title, final int which, final String mode) {
        final String p = selectedPath();
        log("\n=== " + title + " ===");
        if (p == null) { log("  ✗ 还没选驱动"); toast("先在「驱动」页选一个驱动"); return; }
        if (!claim(title)) return;                                    /* B5 */
        log("  驱动: " + p);
        setStatus(title + "：运行中…", C_WARN);
        new Thread(() -> {
            try {
                String staged = stageDriver(p);
                if (staged == null) { setStatus(title + "：暂存驱动失败", C_BAD); return; }
                if (which == 2) invalidateBench(new File(p).getName());    /* A1：跑分前作废旧成绩 */
                beginTask(title);
                try {
                    String out;
                    if (which == 0)      out = nativeIcdSmoke(staged);
                    else if (which == 1) out = nativeTri(staged);
                    else if (which == 3) out = nativeCaps(staged);
                    else                 out = nativeBench(staged, 5, mode);
                    handleNativeOutput(title, out);
                } catch (Throwable t) {
                    log("  ✗ 原生调用异常: " + t);
                    setStatus(title + "：原生调用异常", C_BAD);
                } finally { endTask(); }
            } finally { release(); }
        }).start();
    }

    private void runSmoke() { runNative("ICD 冒烟", 0, null); }
    private void runTri()   { runNative("三角形绘制 + 像素校验", 1, null); }
    private void runCaps()  { runNative("能力清单", 3, null); }
    private void runBench(String mode) { runNative("跑分 · " + mode, 2, mode); }

    private void runAll() {
        final String p = selectedPath();
        log("\n=== ⑩ 一键全流程自测 ===");
        if (p == null) { log("  ✗ 还没选驱动"); toast("先在「驱动」页选一个驱动"); return; }
        if (!claim("一键全流程")) return;                              /* B5 */
        log("  驱动: " + p);
        setStatus("全流程：运行中…", C_WARN);
        invalidateBench(new File(p).getName());                        /* A1 */
        new Thread(() -> {
            try {
            String staged = stageDriver(p);
            if (staged == null) { setStatus("全流程：暂存驱动失败", C_BAD); return; }
            beginTask("一键全流程");
            try {
                handleNativeOutput("冒烟", nativeIcdSmoke(staged));
                handleNativeOutput("三角形像素校验", nativeTri(staged));
                handleNativeOutput("能力清单", nativeCaps(staged));
                handleNativeOutput("三种跑分", nativeBenchAll(staged, 4));
                log("\n  ✅ 全流程结束 —— 到「日志」页导出这段即可。");
                setStatus("全流程结束（见日志）", C_OK);
            } catch (Throwable t) {
                log("  ✗ 原生调用异常: " + t);
                setStatus("全流程异常", C_BAD);
            } finally { endTask(); }
            } finally { release(); }
        }).start();
    }

    private void runSystemBaseline() {
        if (!claim("系统驱动对照")) return;                             /* B5 */
        invalidateBench("system（系统 Vulkan loader）");                /* A1 */
        log("\n=== ⑬ 系统驱动对照（对照组） ===");
        log("  用系统 Vulkan loader 跑同一套测试：系统驱动全过而自定义驱动挂 ⇒ 问题在驱动侧。");
        setStatus("系统驱动对照：运行中…", C_WARN);
        new Thread(() -> {
            beginTask("系统驱动对照");
            try {
                handleNativeOutput("系统驱动 · 冒烟", nativeIcdSmoke("system"));
                handleNativeOutput("系统驱动 · 三角形", nativeTri("system"));
                handleNativeOutput("系统驱动 · 跑分", nativeBenchAll("system", 3));
                setStatus("系统驱动对照结束（见日志）", C_OK);
            } catch (Throwable t) {
                log("  ✗ 原生调用异常: " + t);
                setStatus("系统驱动对照异常", C_BAD);
            } finally { endTask(); release(); }
        }).start();
    }

    private void setBaseline() {
        String p = selectedPath();
        if (p == null) { log("\n  ✗ 还没选驱动"); return; }
        baselinePath = p;
        log("\n=== ⑫ 对比基准 A 已设为 ===\n  " + p + "\n  sha256(16)=" + sha16(new File(p)));
        setStatus("基准 A = " + new File(p).getName(), C_ACCENT);
        updateSelectedHeader();
        toast("已设为对比基准");
    }

    private void compareAB() {
        final String a = baselinePath, b = selectedPath();
        log("\n=== ⑫b A/B 对比 ===");
        if (a == null || b == null) {
            log("  ✗ 需要先设基准（⑫）并选好另一个驱动");
            toast("先设基准并选另一个驱动");
            return;
        }
        if (!claim("A/B 对比")) return;                                  /* B5 */
        log("  基准 A: " + a);
        log("  对照 B: " + b);
        setStatus("A/B 对比：运行中…", C_WARN);
        new Thread(() -> {
            try {
                String sa = stageFor(a, "a.so"), sb = stageFor(b, "b.so");
                if (sa == null || sb == null) { log("  ✗ 准备驱动文件失败"); setStatus("A/B 准备失败", C_BAD); return; }
                beginTask("A/B 对比");
                String[][] ab = { { "A: " + new File(a).getName(), sa }, { "B: " + new File(b).getName(), sb } };
                for (String[] pair : ab) {
                    log("\n  ────── 驱动 " + pair[0] + " ──────");
                    invalidateBench(pair[0]);                                /* A1：每一段各自作废 */
                    handleNativeOutput("A/B · " + pair[0] + " 冒烟", nativeIcdSmoke(pair[1]));
                    handleNativeOutput("A/B · " + pair[0] + " 三角形", nativeTri(pair[1]));
                    handleNativeOutput("A/B · " + pair[0] + " 跑分", nativeBenchAll(pair[1], 3));
                }
                log("\n  ✅ 对比结束：两段汇总数字可直接对比。");
                setStatus("A/B 对比结束（见日志）", C_OK);
            } catch (Throwable t) {
                log("  ✗ A/B 异常: " + t);
                setStatus("A/B 异常", C_BAD);
            } finally { endTask(); release(); }
        }).start();
    }

    private String stageFor(String src, String name) {
        try {
            File dst = new File(getFilesDir(), name);
            InputStream in = new FileInputStream(src);
            OutputStream out = new FileOutputStream(dst);
            byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            in.close(); out.close();
            dst.setReadable(true, true);
            return dst.getAbsolutePath();
        } catch (Throwable t) { log("  ✗ 暂存失败(" + name + "): " + t); return null; }
    }

    /** 原生文本统一处理：进日志 + 顶部状态行上色 + 解析分数/结论进卡片。 */
    private void handleNativeOutput(String title, String out) {
        logBlock(title, out);
        final int v = verdictOf(out);
        final String first = firstMeaningfulLine(out);
        ui.post(() -> {
            if (first != null) headerStatus.setText(first);
            tint(headerStatus, v);
        });
        /* (c)：正确性校验（像素校验 / 判定 / 包围盒）⇒ 独立卡片，绝不写进跑分卡 */
        if (out != null && (out.contains("像素校验") || out.contains("绘制正确")
                || out.contains("无绘制输出") || out.contains("存在异常像素") || out.contains("包围盒"))) {
            final int vv = verdictOf(out);
            StringBuilder sb = new StringBuilder();
            for (String ln : out.split("\n")) {
                String t = ln.trim();
                if (t.length() == 0) continue;
                if (t.contains("判定") || t.contains("包围盒") || t.contains("采样点")
                        || t.contains("校验") || t.contains("红=") || t.contains("红=")) {
                    if (sb.length() > 0) sb.append('\n');
                    sb.append(t.length() > 150 ? t.substring(0, 150) + "…" : t);
                }
            }
            final String txt = sb.length() == 0 ? firstMeaningfulLine(out) : sb.toString();
            ui.post(() -> {
                if (validNote != null) {
                    validNote.setText(txt == null ? "（无校验输出）" : txt);
                    tint(validNote, vv);
                }
            });
        }
        parseScores(out);
        if (out != null && out.contains("[设备]")) lastDeviceRaw = out;    /* 压力页的 VENDOR/RENDERER 面板 */
        if (out != null && out.contains("[设备]")) {
            final String caps = out;
            ui.post(() -> { if (capsText != null) capsText.setText(tail(caps, 26)); });
        }
    }

    /** A1：开跑前先把上一次的成绩作废 —— 绝不让"上一次（例如系统驱动）"的数字被当成本次成绩。 */
    private void invalidateBench(String tag) {
        benchOwner = tag;
        ui.post(() -> {
            if (benchNote != null) { benchNote.setText("⏳ 正在跑：" + tag + " …（上一次的成绩已作废）"); tint(benchNote, C_WARN); }
            if (benchFill != null)  { benchFill.setText("—");  tint(benchFill, C_DIM); }
            if (benchBlit != null)  { benchBlit.setText("—");  tint(benchBlit, C_DIM); }
            if (benchDraw != null)  { benchDraw.setText("—");  tint(benchDraw, C_DIM); }
            if (benchScore != null) { benchScore.setText("—"); tint(benchScore, C_DIM); }
            if (benchGpu != null)   { benchGpu.setText("—");   tint(benchGpu, C_DIM); }
            if (benchWall != null)  { benchWall.setText("—");  tint(benchWall, C_DIM); }
            if (benchDuty != null)  { benchDuty.setText("—");  tint(benchDuty, C_DIM); }
            if (benchProof != null) { benchProof.setText("（等待本次运行的自证行…）"); tint(benchProof, C_DIM); }
            if (ovScore != null)    { ovScore.setText("—");    tint(ovScore, C_DIM); }
            if (ovSummary != null)  ovSummary.setText("（正在跑 " + tag + " …）");
        });
    }

    private void parseScores(String out) {
        if (out == null) return;
        final int v = verdictOf(out);
        final String owner = benchOwner;

        /* ★ A1：失败 ⇒ 清空旧值 + 明确横幅（"失败/不给分"与"0 分"必须能分清） */
        if (v == C_BAD) {
            final String r = grab(out, "提交失败 r=(-?[0-9]+)");
            final String why = (r != null)
                    ? ("提交失败 r=" + r + (r.equals("-4") ? "（-4 = VK_ERROR_DEVICE_LOST）" : ""))
                    : "本次运行失败（详见日志）";
            ui.post(() -> {
                if (benchFill != null)  { benchFill.setText("失败");   tint(benchFill, C_BAD); }
                if (benchBlit != null)  { benchBlit.setText("失败");   tint(benchBlit, C_BAD); }
                if (benchDraw != null)  { benchDraw.setText("失败");   tint(benchDraw, C_BAD); }
                if (benchScore != null) { benchScore.setText("不给分"); tint(benchScore, C_BAD); }
                if (benchGpu != null)   { benchGpu.setText("失败");  tint(benchGpu, C_BAD); }
                if (benchWall != null)  { benchWall.setText("失败"); tint(benchWall, C_BAD); }
                if (benchDuty != null)  { benchDuty.setText("失败"); tint(benchDuty, C_BAD); }
                if (benchProof != null) { benchProof.setText("本次失败 ⇒ 无有效时间/口径"); tint(benchProof, C_BAD); }
                if (ovScore != null)    { ovScore.setText("不给分");    tint(ovScore, C_BAD); }
                if (ovSummary != null)  ovSummary.setText("⚠ 本次跑分失败：" + why + "（上一次的成绩已作废，避免误读）");
                if (benchNote != null)  { benchNote.setText("⚠ 本次失败（驱动：" + owner + "）：" + why + " —— 不给分"); tint(benchNote, C_BAD); }
            });
            return;
        }

        /* ① 优先取原生**自己的成绩行** `== 分数(fill) = 558 Mpixel/s ==`，再退回汇总标签 */
        String fill  = grab(out, "分数\\(fill\\)\\s*=\\s*([0-9.]+)");
        String blit  = grab(out, "分数\\(blit\\)\\s*=\\s*([0-9.]+)");
        String tri   = grab(out, "分数\\(draw\\)\\s*=\\s*([0-9.]+)");
        if (fill == null) fill = grab(out, "填充率\\s*:\\s*([0-9.]+)");
        if (blit == null) blit = grab(out, "拷贝带宽\\s*:\\s*([0-9.]+)");
        if (tri  == null) tri  = grab(out, "三角形吞吐\\s*:\\s*([0-9.]+)");
        final String score = grab(out, "综合分:\\s*([0-9.]+)");
        if (fill == null && blit == null && tri == null && score == null) return;   /* 这批输出里没有成绩 */
        parseTiming(out);                                                           /* (c)：时间三件套 */
        try { if (fill != null) lastFill = Double.parseDouble(fill); } catch (Throwable ignored) { }
        try { if (blit != null) lastBlit = Double.parseDouble(blit); } catch (Throwable ignored) { }
        try { if (tri  != null) lastDraw = Double.parseDouble(tri);  } catch (Throwable ignored) { }
        final boolean anyFail = out.contains("失败统计")
                && !Pattern.compile("失败统计\\s*:\\s*fill=0 blit=0 draw=0", Pattern.DOTALL).matcher(out).find();
        final String fFill = fill, fBlit = blit, fTri = tri;   /* lambda 捕获需 final ✓ */
        ui.post(() -> {
            if (fFill != null && benchFill != null) { setNumFade(benchFill, fFill); tint(benchFill, anyFail ? C_WARN : C_OK); }
            if (fBlit != null && benchBlit != null) { setNumFade(benchBlit, fBlit); tint(benchBlit, anyFail ? C_WARN : C_OK); }
            if (fTri != null && benchDraw != null)  { setNumFade(benchDraw, fTri);  tint(benchDraw, anyFail ? C_WARN : C_OK); }
            if (score != null) {
                if (benchScore != null) { benchScore.setText(score); tint(benchScore, anyFail ? C_WARN : C_OK); }
                if (ovScore != null)    { ovScore.setText(score);    tint(ovScore, anyFail ? C_WARN : C_OK); }
            }
            if (ovSummary != null) {
                ovSummary.setText("填充 " + nz(fFill) + " Mpixel/s · 带宽 " + nz(fBlit) + " GB/s · 三角形 "
                        + nz(fTri) + " 个/s" + (score != null ? "  ⇒  综合分 " + score : ""));
            }
            if (benchNote != null) {
                benchNote.setText("✅ 本次成绩（驱动：" + owner + "）"
                        + (anyFail ? " · ⚠ 含失败项 ⇒ 分数不可比" : ""));
                tint(benchNote, anyFail ? C_WARN : C_OK);
            }
        });
    }

    /**
     * (c)：解析 GPU 时间 / 墙钟 / 占空比，并**原样收集**原生的自证行（口径）。
     * 解析用宽模式（GPU 时间 / GPU TIME / gpu_ms；墙钟 / WALL / 时长）—— 原生措辞变了也不会静默失败：
     * 三个数没解析到时保持"—"，并由 §自证行 把原始行展示出来（口径与数字同屏，便于核对）。
     */
    private void parseTiming(String out) {
        if (out == null) return;
        String gpu = null, wall = null;
        try {
            Matcher mg = P_GPU_MS.matcher(out);
            if (mg.find()) gpu = mg.group(1);
            Matcher mw = P_WALL_S.matcher(out);
            if (mw.find()) wall = mw.group(1);
        } catch (Throwable ignored) { }
        try {
            if (gpu != null)  lastGpuMs = Double.parseDouble(gpu);
            if (wall != null) lastWallS = Double.parseDouble(wall);
        } catch (Throwable ignored) { }
        String dutyS = grab(out, "GPU\\s*占空比\\s*[=:]\\s*([0-9.]+)");
        if (dutyS == null) dutyS = grab(out, "([0-9.]+)\\s*%\\s*墙钟");
        if (dutyS != null) { try { lastDuty = Double.parseDouble(dutyS); } catch (Throwable ignored) { } }
        StringBuilder proof = new StringBuilder();
        for (String ln : out.split("\n")) {
            String t = ln.trim();
            if (t.length() == 0) continue;
            if (t.contains("负载") || t.contains("口径") || t.contains("自证") || t.contains("每操作")
                    || t.contains("GPU 时间") || t.contains("GPU时间") || t.contains("占空比")) {
                if (proof.length() > 0) proof.append('\n');
                proof.append(t.length() > 150 ? t.substring(0, 150) + "…" : t);
            }
        }
        final String fg = gpu, fw = wall;
        final String fp = proof.length() == 0 ? null : proof.toString();
        Double duty = null;
        try {
            if (fg != null && fw != null) {
                double w = Double.parseDouble(fw);
                if (w > 0) duty = Double.parseDouble(fg) / 1000.0 / w * 100.0;
            }
        } catch (Throwable ignored) { }
        final Double fd = duty;
        ui.post(() -> {
            if (fg != null && benchGpu != null)   { benchGpu.setText(fg);   tint(benchGpu, C_OK); }
            if (fw != null && benchWall != null)  { benchWall.setText(fw);  tint(benchWall, C_TEXT); }
            if (fd != null && benchDuty != null) {
                benchDuty.setText(String.format(Locale.ROOT, "%.1f", fd));
                tint(benchDuty, fd >= 80 ? C_OK : (fd >= 30 ? C_WARN : C_BAD));
            }
            if (fp != null && benchProof != null) { benchProof.setText(fp); tint(benchProof, C_DIM); }
            if (fg == null && fw == null && benchNote != null && fp == null)
                benchNote.setText("⚠ 未在原生输出里找到 GPU 时间/墙钟字段（口径行也没有）—— 请核对原生措辞");
        });
    }

    private static final Pattern FAILS = Pattern.compile("失败=([0-9]+)");

    /** 绿=通过 / 红=失败 / 黄=警告 —— 从原生文本里判。 */
    private int verdictOf(String text) {
        if (text == null || text.length() == 0) return C_NEUTRAL;
        if (text.contains("DEVICE_LOST") || text.contains("判定: 驱动不可用")
                || text.contains("X 驱动不可用") || text.contains("无绘制输出")
                || text.contains("存在异常像素") || text.contains("初始化失败")
                || text.contains("回读失败") || text.contains("原生调用异常")) return C_BAD;
        Matcher m = FAILS.matcher(text);
        while (m.find()) {
            try { if (Long.parseLong(m.group(1)) > 0) return C_BAD; } catch (Throwable ignored) { }
        }
        if (text.contains("判定: 不稳定") || text.contains("超时") || text.contains("退化")
                || text.contains("跳过") || text.contains("不可用")) return C_WARN;
        if (text.contains("绘制正确") || text.contains("驱动可用") || text.contains("OK 渲染就绪")
                || text.contains("综合分")) return C_OK;
        return C_NEUTRAL;
    }

    private String grab(String text, String regex) {
        if (text == null) return null;
        try {
            Matcher m = Pattern.compile(regex).matcher(text);
            return m.find() ? m.group(1) : null;
        } catch (Throwable t) { return null; }
    }

    private String nz(String s) { return s == null ? "—" : s; }

    private String firstMeaningfulLine(String out) {
        if (out == null) return null;
        for (String ln : out.split("\n")) {
            String s = ln.trim();
            if (s.length() == 0) continue;
            if (s.startsWith("[")) continue;
            return s.length() > 90 ? s.substring(0, 90) + "…" : s;
        }
        return null;
    }

    /* =========================================================================
     *  ⑪ 启动器日志 / ⑪b 授权 / ⑦ 拉起启动器
     * ========================================================================= */
    private static final String[] LOG_CANDIDATES = {
        "/sdcard/FCL/log/latest_game.log",
        "/sdcard/FCL/log/fcl.log",
        "/sdcard/ZalithLauncher/log/latest_game.log",
        "/sdcard/ZalithLauncher2/log/latest_game.log",
        "/sdcard/Android/data/com.movtery.zalithlauncher.v2/files/latest_game.log",
        "/sdcard/Android/data/com.movtery.zalithlauncher.v2/files/log/latest_game.log",
        "/sdcard/Android/data/com.tungsten.fcl/files/latest_game.log",
    };

    private void readLauncherLogs() {
        log("\n=== ⑪ 读取启动器日志 ===");
        setStatus("读取启动器日志…", C_NEUTRAL);
        new Thread(() -> {
            boolean any = false;
            for (String p : LOG_CANDIDATES) {
                File f = new File(p);
                if (!f.isFile()) continue;
                any = true;
                log("  ── " + p);
                log("     " + f.length() + " 字节，最后修改 " + new Date(f.lastModified()));
                tailInto(f, 120);
            }
            List<File> found = new ArrayList<>();
            for (String d : new String[]{ "/sdcard/FCL", "/sdcard/ZalithLauncher",
                                          "/sdcard/ZalithLauncher2", "/sdcard/games" }) {
                collectLogs(new File(d), found, 2);
            }
            for (File f : found) {
                log("  ── " + f.getAbsolutePath() + "  (" + f.length() + " 字节)");
                tailInto(f, 60);
                any = true;
            }
            if (!any) {
                log("  未找到日志。点「⑪b 授权所有文件访问」后再试（ZL2 日志在其私有目录）。");
                setStatus("未找到启动器日志（可能需要⑪b 授权）", C_WARN);
            } else {
                setStatus("启动器日志已读入（见日志页）", C_OK);
            }
        }).start();
    }

    private void collectLogs(File dir, List<File> out, int depth) {
        if (depth < 0 || dir == null) return;
        File[] fs = dir.listFiles();
        if (fs == null) return;
        for (File f : fs) {
            if (f.isDirectory()) collectLogs(f, out, depth - 1);
            else if (f.getName().endsWith(".log") && f.length() > 0) out.add(f);
        }
    }

    private void tailInto(File f, int lines) {
        try {
            ArrayDeque<String> q = new ArrayDeque<>();
            java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.InputStreamReader(new FileInputStream(f)));
            String ln;
            while ((ln = r.readLine()) != null) { q.addLast(ln); if (q.size() > lines) q.removeFirst(); }
            r.close();
            for (String x : q) log("     | " + x);
        } catch (Throwable t) { log("     ✗ 读取失败: " + t); }
    }

    private void requestAllFiles() {
        try {
            if (Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager()) {
                log("\n  ✓ 已经拥有「所有文件访问」权限。");
                setStatus("已有所有文件访问权限", C_OK);
                return;
            }
            Intent i = new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            i.setData(Uri.parse("package:" + getPackageName()));
            startActivity(i);
            log("\n  已打开「所有文件访问」授权页 ⇒ 授予后返回，再点「⑪ 读取启动器日志」。");
            setStatus("等待授权…", C_WARN);
        } catch (Throwable t) {
            try {
                startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                setStatus("已打开全部应用的文件访问页", C_WARN);
            } catch (Throwable t2) {
                log("  ✗ 无法打开授权页: " + t2);
                setStatus("无法打开授权页", C_BAD);
            }
        }
    }

    private void launchChooser() {
        log("\n=== ⑦ Minecraft 启动器 ===");
        PackageManager pm = getPackageManager();
        try {
            for (PackageInfo pi : pm.getInstalledPackages(0)) {
                String n = pi.packageName.toLowerCase(Locale.ROOT);
                if (n.contains("zalith") || n.contains("fcl") || n.contains("foldcraft") || n.contains("pojav")) {
                    log("  ✓ 发现: " + pi.packageName + " [" + pi.versionName + "]");
                    Intent i = pm.getLaunchIntentForPackage(pi.packageName);
                    if (i != null) {
                        try { startActivity(i); log("    → 已拉起"); setStatus("已拉起 " + pi.packageName, C_OK); }
                        catch (Throwable t) { log("    ✗ " + t); setStatus("拉起失败", C_BAD); }
                    }
                    return;
                }
            }
        } catch (Throwable t) { log("  ✗ 枚举包失败: " + t); }
        log("  ✗ 没找到已安装的启动器（ZalithLauncher2 / FCL / Pojav）");
        setStatus("没找到启动器", C_WARN);
    }

    /* =========================================================================
     *  ⑧ 导出日志（不走 file://：正文内嵌 + 落盘到外部私有目录）
     * ========================================================================= */
    private void exportLog() {
        try {
            String dump;
            synchronized (fullLog) { dump = fullLog.toString(); }
            String name = "gputest-"
                    + new SimpleDateFormat("MMdd-HHmmss", Locale.ROOT).format(new Date()) + ".log";
            File dir = getExternalFilesDir(null);
            File f = (dir != null) ? new File(dir, name) : new File(getFilesDir(), name);   /* D12：回退 */
            if (dir == null) log("  ! 外部目录不可用，回退到应用私有目录");
            OutputStream os = new FileOutputStream(f);
            os.write(dump.getBytes("UTF-8"));
            os.close();
            String path = f.getAbsolutePath();
            log("\n=== ⑧ 日志已导出 ===\n  " + path);
            Intent s = new Intent(Intent.ACTION_SEND);
            s.setType("text/plain");
            String body = dump.length() > 400000 ? dump.substring(dump.length() - 400000) : dump;
            s.putExtra(Intent.EXTRA_SUBJECT, "GPU 驱动测试台日志");
            s.putExtra(Intent.EXTRA_TEXT, body);
            startActivity(Intent.createChooser(s, "分享日志"));
            setStatus("日志已导出：" + path, C_OK);
        } catch (Throwable t) {
            log("  ✗ 导出失败: " + t);
            setStatus("导出失败", C_BAD);
        }
    }

    /* =========================================================================
     *  工具
     * ========================================================================= */
    private String sha16(File f) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            InputStream in = new FileInputStream(f);
            byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            in.close();
            byte[] d = md.digest();            /* 只调一次（反复 digest 第二次返回空数组 ⇒ AIOOBE） */
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 8 && i < d.length; i++) sb.append(String.format("%02x", d[i]));
            return sb.toString();
        } catch (Throwable t) { return "?"; }
    }

    private void logBlock(String title, String out) {
        log("  ── " + title + " ──");
        if (out == null) { log("  （无输出）"); return; }
        for (String line : out.split("\n")) log("  " + line);
    }

    private void log(String s) {
        final String line = new SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(new Date()) + "  " + s;
        synchronized (fullLog) { fullLog.append(line).append('\n'); }
        appendShared(line);
        ui.post(() -> {
            if (logView == null) return;
            logView.append(line + "\n");
            try {   /* 自动滚到底：实时日志 */
                if (logView.getLayout() != null) {
                    int scroll = logView.getLayout().getLineTop(logView.getLineCount()) - logView.getHeight();
                    if (scroll > 0) logView.scrollTo(0, scroll);
                }
            } catch (Throwable ignored) { }
        });
    }

    private void setStatus(String s, int color) {
        ui.post(() -> {
            if (headerStatus == null) return;
            headerStatus.setText(s);
            tint(headerStatus, color);
        });
    }

    private String tail(String text, int lines) {
        String[] all = text.split("\n");
        int from = Math.max(0, all.length - lines);
        StringBuilder sb = new StringBuilder();
        for (int i = from; i < all.length; i++) sb.append(all[i]).append('\n');
        return sb.toString();
    }

    /* ---- 视图工厂 ---- */
    private ScrollView scroll(View child) {
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.addView(child, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return sv;
    }

    private LinearLayout col() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(2), dp(2), dp(2), dp(2));
        return c;
    }

    /** 凝光卡片/控件底：半透表面 + 极轻线性渐变 + 可选高光边（LayerDrawable）。全平台 API ✓ */
    private android.graphics.drawable.Drawable themedBg(int fillKey, int radiusDp) {
        int base = c(fillKey);
        GradientDrawable g = new GradientDrawable();
        if (x(4) > 0) {
            g.setOrientation(GradientDrawable.Orientation.TOP_BOTTOM);
            g.setColors(new int[] { lighten(base, x(4)), withAlpha(base, x(3)) });
        } else {
            g.setColor(withAlpha(base, x(3)));
        }
        g.setCornerRadius(dp(radiusDp));
        g.setStroke(dp(1), c(C_STROKE));
        if (x(5) <= 0) return g;
        GradientDrawable hl = new GradientDrawable();          /* 内层 1dp 高光边 ⇒ "凝光"的高光感 */
        hl.setColor(0x00000000);
        hl.setCornerRadius(dp(Math.max(1, radiusDp - 1)));
        hl.setStroke(dp(1), withAlpha(0xFFFFFF, x(5)));
        return new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[] { g, hl });
    }

    private GradientDrawable cardBg(int fill, int stroke, int radiusDp) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(radiusDp));
        g.setStroke(dp(1), stroke);
        return g;
    }

    private LinearLayout card(View... children) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        bgKeys.put(c, C_CARD * 100 + 14);
        c.setBackground(themedBg(C_CARD, x(0)));               /* 主题切换时按新圆角/半透重刷 ✓ */
        int p = dp(14);
        c.setPadding(p, p, p, p);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(6), 0, dp(6));
        c.setLayoutParams(lp);
        if (children != null) for (View v : children) if (v != null) c.addView(v);
        return c;
    }

    private TextView text(String s, int sizeSp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sizeSp);
        textKeys.put(t, color);                      /* color 现在是调色板键 */
        t.setTextColor(c(color));
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private TextView section(String s) {
        TextView t = text(s, 14, C_ACCENT, true);
        t.setPadding(0, 0, 0, dp(6));
        return t;
    }

    private TextView note(String s) {
        TextView t = text(s, 11, C_DIM, false);
        t.setLineSpacing(0, 1.25f);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }

    private TextView bigNum(String s) {
        TextView t = text(s, 34, C_ACCENT, true);
        t.setGravity(Gravity.END);
        return t;
    }

    private LinearLayout numRow(String label, TextView num, String unit) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.addView(text(label, 13, C_TEXT, false),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        r.addView(num);
        r.addView(text(" " + unit, 11, C_DIM, false));
        r.setPadding(0, dp(6), 0, dp(6));
        return r;
    }

    private Button actionBtn(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(13);
        textKeys.put(b, C_TEXT);
        b.setTextColor(c(C_TEXT));
        b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        bgKeys.put(b, C_BTN_BG * 100 + 12);
        b.setBackground(themedBg(C_BTN_BG, x(1)));
        fluid(b);                                              /* ② 柔性触控反馈 */
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(4), 0, dp(4));
        b.setLayoutParams(lp);
        actionButtons.add(b);                        /* B5：统一 enable/disable */
        return b;
    }

    /* =========================================================================
     *  ⑦ 压力页（B站参考图语义）：Auto Increment [4.0] ppf + 统计面板 + 进展条
     *  停止条件（写进界面）：① 平均帧时 > 阈值（默认 100 ms）② 原生报失败/DEVICE_LOST ③ 手动停止
     *  停止时**保留最后一帧画面**（诊断价值）✓
     * ========================================================================= */
    private View buildStressPage() {
        LinearLayout col = col();

        /* 画面（与画面页同一套显示链路：乒乓 Bitmap + ImageView） */
        stressView = new ImageView(this);
        stressView.setAdjustViewBounds(true);
        stressView.setScaleType(ImageView.ScaleType.CENTER_INSIDE);
        stressView.setBackgroundColor(c(C_RENDER_BG));
        col.addView(card(section("压力画面（" + STRESS_W + "x" + STRESS_H + "，越堆越多的半透明三角形）"),
                stressView,
                note("停止时保留最后一帧 —— 崩之前那一帧才是诊断证据。")));

        /* 控制：Auto Increment + ppf */
        stressAuto = new CheckBox(this);
        stressAuto.setText("Auto Increment（每帧自动加量）");
        stressAuto.setChecked(true);
        stressAuto.setTextColor(c(C_TEXT));
        textKeys.put(stressAuto, C_TEXT);
        stressPpf = new EditText(this);
        stressPpf.setText("4");
        stressPpf.setTextColor(c(C_TEXT));
        stressPpf.setHintTextColor(c(C_DIM));
        textKeys.put(stressPpf, C_TEXT);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(stressAuto, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(text("ppf", 13, C_DIM, false));
        row.addView(stressPpf, new LinearLayout.LayoutParams(dp(72), ViewGroup.LayoutParams.WRAP_CONTENT));
        stressBtn = actionBtn("▶ 开始压力测试", v -> toggleStress());
        stressNote = note("停止条件：① 平均帧时 > 100 ms ② 原生报失败/DEVICE_LOST ③ 手动停止。");
        col.addView(card(section("控制"), row, stressBtn, stressNote));

        /* 进展条（学图里 0% → 100%） */
        stressProg = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        stressProg.setMax(stressCap);
        stressProgTx = text("0 / " + stressCap + "（0%）", 12, C_DIM, false);
        col.addView(card(section("自动加量进展（当前三角形数 / 上限）"), stressProg, stressProgTx));

        /* 清屏色 R/G/B（默认 8/8/25） */
        stressRgbTx = text("R: 8    G: 8    B: 25", 13, C_TEXT, false);
        LinearLayout rgbCol = new LinearLayout(this);
        rgbCol.setOrientation(LinearLayout.VERTICAL);
        String[] names = { "R", "G", "B" };
        for (int i = 0; i < 3; i++) {
            final int ch = i;
            SeekBar sb = new SeekBar(this);
            sb.setMax(255);
            sb.setProgress(stressRgb[i]);
            sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(SeekBar b, int p, boolean fromUser) {
                    stressRgb[ch] = p;
                    refreshRgbText();
                    try { nativeStressClearColor(stressRgb[0], stressRgb[1], stressRgb[2]); }
                    catch (Throwable ignored) { }        /* 原生未接入时不崩 */
                }
                @Override public void onStartTrackingTouch(SeekBar b) { }
                @Override public void onStopTrackingTouch(SeekBar b) { }
            });
            rgbCol.addView(text(names[i], 12, C_DIM, false));
            rgbCol.addView(sb);
        }
        col.addView(card(section("清屏色（与参考图默认一致：R8 G8 B25）"), stressRgbTx, rgbCol,
                note("清屏色：压力场景的背景色（与参考图默认一致，便于逐像素比对）。"
                   + "R/G/B 我这边已存在字段里，你加一行接口我立刻接上。")));

        /* 统计面板（等宽小字，学图里那块） */
        stressPanel = text("（等待原生压力接口）", 11, C_TEXT, false);
        stressPanel.setTypeface(Typeface.MONOSPACE);
        stressPanel.setLineSpacing(0, 1.2f);
        col.addView(card(section("统计面板"), stressPanel,
                note("VENDOR / RENDERER / VERSION 用 Vulkan 等价物（设备名 / 驱动版本 / API 版本）；"
                   + "glGetString 那三条要 GLES 侧，原生将来可补。")));
        return scroll(col);
    }

    private void refreshRgbText() {
        ui.post(() -> { if (stressRgbTx != null)
            stressRgbTx.setText("R: " + stressRgb[0] + "    G: " + stressRgb[1] + "    B: " + stressRgb[2]); });
    }

    private void toggleStress() {
        if (stressRunning) stopStress("手动停止");
        else startStress();
    }

    private void startStress() {
        final String p = selectedPath();
        if (p == null) { toast("先在「驱动」页选一个驱动"); return; }
        if (stressRunning) return;
        if (!claim("压力测试")) return;                                   /* B5：与跑分/画面互斥 */
        stressTri = 0;
        refreshStressProgress();
        stressBtn.setText("■ 停止");
        stressBtn.setTextColor(c(C_BAD));
        setStatus("压力测试：初始化…", C_WARN);
        beginTask("压力测试");
        new Thread(() -> {
            try {
                String staged = stageDriver(p);
                if (staged == null) { setStatus("压力测试：暂存驱动失败", C_BAD); return; }
                String out;
                try { out = nativeStressInit(staged, STRESS_W, STRESS_H); }
                catch (Throwable t) {
                    setStatus("压力测试：原生接口未接入（等 v5.1）", C_WARN);
                    log("  ⚠ nativeStressInit 不可用: " + t);
                    ui.post(() -> { stressBtn.setText("▶ 开始压力测试"); stressBtn.setTextColor(c(C_TEXT)); });
                    endTask(); release();
                    return;
                }
                logBlock("stressInit", out);
                stressBufs[0] = Bitmap.createBitmap(STRESS_W, STRESS_H, Bitmap.Config.ARGB_8888);
                stressBufs[1] = Bitmap.createBitmap(STRESS_W, STRESS_H, Bitmap.Config.ARGB_8888);
                ui.post(() -> { if (stressView != null) stressView.setImageBitmap(stressBufs[0]); });
                stressRunning = true;
                stressThread = new Thread(this::stressLoop, "gpu-stress");
                stressThread.start();
                setStatus("压力测试：运行中", C_OK);
            } catch (Throwable t) {
                log("  ✗ 压力测试启动异常: " + t);
                setStatus("压力测试启动异常", C_BAD);
                endTask(); release();
            }
        }, "gpu-stress-init").start();
    }

    private void stressLoop() {
        int buf = 1, frames = 0;
        double sumMs = 0;
        while (stressRunning) {
            final Bitmap target = stressBufs[buf];
            buf ^= 1;
            String stat;
            try { stat = nativeStressFrame(stressTri); }
            catch (Throwable t) { log("  ✗ nativeStressFrame 不可用: " + t); break; }
            if (stat != null && (stat.contains("DEVICE_LOST") || stat.contains("失败") || stat.contains("X "))) {
                logBlock("压力测试失败", stat);
                setStatus("压力测试失败（已保留最后一帧）", C_BAD);
                break;
            }
            try { String e = nativeStressBlit(target); if (e != null && e.contains("X ")) { logBlock("压力同步失败", e); break; } }
            catch (Throwable t) { log("  ✗ nativeStressBlit 不可用: " + t); break; }
            frames++; progressToken++;                                     /* 看门狗：有进展 ⇒ 静默续期 */
            final int tri = stressTri;
            ui.post(() -> {
                if (stressView != null) { stressView.setImageBitmap(target); stressView.invalidate(); }
                updateStressPanel(stat, tri);
            });
            Double ms = parseD(stat, "平均帧时");
            if (ms != null) { sumMs += ms; if (ms > stressMaxFrameMs) { log("  ⏹ 平均帧时 " + ms + " ms 超过阈值 " + stressMaxFrameMs + " ms ⇒ 自动停止（保留最后一帧）"); break; } }
            if (stressAuto != null && stressAuto.isChecked()) {
                int ppf = 4;
                try { ppf = Math.max(1, Integer.parseInt(stressPpf.getText().toString().trim())); } catch (Throwable ignored) { }
                stressTri = Math.min(stressCap, stressTri + ppf);
                refreshStressProgress();
                if (stressTri >= stressCap) { log("  ⏹ 已达上限 " + stressCap + " 个三角形 ⇒ 自动停止"); break; }
            }
        }
        final int n = frames;
        final double avg = n > 0 ? sumMs / n : 0;
        stressRunning = false;
        try { nativeStressStop(); } catch (Throwable ignored) { }
        ui.post(() -> {
            if (stressBtn != null) { stressBtn.setText("▶ 开始压力测试"); stressBtn.setTextColor(c(C_TEXT)); }
            if (stressProgTx != null) stressProgTx.setText(stressTri + " / " + stressCap
                    + "（" + (stressCap > 0 ? stressTri * 100 / stressCap : 0) + "%）· 共 " + n + " 帧"
                    + (avg > 0 ? String.format(Locale.ROOT, " · 平均 %.1f ms", avg) : ""));
        });
        log("  压力测试结束：帧数 " + n + "，三角形数 " + stressTri + "，保留最后一帧 ✓");
        endTask(); release();
    }

    private void stopStress(String why) {
        if (!stressRunning && stressThread == null) return;
        stressRunning = false;
        final Thread t = stressThread;
        stressThread = null;
        setStatus("压力测试：正在停止（" + why + "）", C_WARN);
        new Thread(() -> {
            if (t != null) { try { t.join(3600); } catch (InterruptedException ignored) { } }
            try { nativeStressStop(); } catch (Throwable ignored) { }
            ui.post(() -> { if (stressBtn != null) { stressBtn.setText("▶ 开始压力测试"); stressBtn.setTextColor(c(C_TEXT)); } });
            setStatus("压力测试已停止（" + why + "）· 最后一帧保留在画面上", C_NEUTRAL);
            log("  压力测试已停止：" + why);
            endTask(); release();
        }, "gpu-stress-stop").start();
    }

    private void refreshStressProgress() {
        ui.post(() -> {
            if (stressProg != null) stressProg.setProgress(Math.min(stressCap, stressTri), true);  /* ④ 进度 200ms 动效（平台 API） */
            if (stressProgTx != null) stressProgTx.setText(stressTri + " / " + stressCap
                    + "（" + (stressCap > 0 ? stressTri * 100 / stressCap : 0) + "%）");
        });
    }

    /** 统计面板：等宽小字，字段尽量从原生文本里抓；抓不到就显示 "—"（不静默）。 */
    private void updateStressPanel(String stat, int tri) {
        if (stressPanel == null) return;
        StringBuilder b = new StringBuilder();
        b.append(stressDeviceLines());
        Double ms  = parseD(stat, "平均帧时");
        Double fps = parseD(stat, "FPS");
        Double tps = parseD(stat, "三角形/s");
        Double pct = parseD(stat, "Rendered/Screen");
        Double gpu = parseD(stat, "GPU 时间");
        b.append("平均帧时    : ").append(ms  == null ? "—" : String.format(Locale.ROOT, "%.2f ms/Frame", ms)).append('\n');
        b.append("FPS        : ").append(fps == null ? "—" : String.format(Locale.ROOT, "%.1f", fps)).append('\n');
        b.append("三角形数    : ").append(tri).append('\n');
        b.append("三角形/s    : ").append(tps == null ? "—" : String.format(Locale.ROOT, "%.0f", tps)).append('\n');
        b.append("Rendered/Screen %: ").append(pct == null ? "—" : String.format(Locale.ROOT, "%.1f%%", pct)).append('\n');
        b.append("GPU 时间    : ").append(gpu == null ? "—" : String.format(Locale.ROOT, "%.2f ms", gpu)).append('\n');
        b.append("clear color : R: ").append(stressRgb[0]).append("  G: ").append(stressRgb[1])
         .append("  B: ").append(stressRgb[2]);
        stressPanel.setText(b.toString());
        stressPanel.setTextColor(c(C_TEXT));
    }

    /** VENDOR / RENDERER / VERSION（用 Vulkan 等价物：设备名 / 驱动版本 / API 版本）。 */
    private String stressDeviceLines() {
        String name = grab(lastDeviceRaw, "name\\s*:\\s*([^\\n]+)");
        String api  = grab(lastDeviceRaw, "apiVersion\\s*:\\s*([^\\n]+)");
        String drv  = grab(lastDeviceRaw, "driverVer\\s*:\\s*([^\\n]+)");
        StringBuilder b = new StringBuilder();
        b.append("VENDOR/RENDERER : ").append(name == null ? "—（先跑④冒烟或⑭能力清单）" : name.trim()).append('\n');
        b.append("VERSION(api)    : ").append(api == null ? "—" : api.trim()).append('\n');
        b.append("driverVersion   : ").append(drv == null ? "—" : drv.trim()).append('\n');
        return b.toString();
    }

    /** 宽容数值抓取：字段名 + 12 字符内出现数字即可（原生措辞变了也不会静默失败）。 */
    private Double parseD(String text, String label) {
        if (text == null) return null;
        try {
            java.util.regex.Matcher m = java.util.regex.Pattern.compile(
                    java.util.regex.Pattern.quote(label) + "[^0-9\\-]{0,14}(-?[0-9]+(?:\\.[0-9]+)?)").matcher(text);
            return m.find() ? Double.valueOf(m.group(1)) : null;
        } catch (Throwable t) { return null; }
    }

    /* =========================================================================
     *  v6.1 App 骨架：App Bar(56dp) + 底部导航(64dp) + FAB + 页内分段(pill)
     *  只用平台 API（无 androidx）；图标走 getIdentifier（**本构建不生成 R.java**）
     * ========================================================================= */
    private int drawableId(String name) {
        try { return getResources().getIdentifier(name, "drawable", getPackageName()); }
        catch (Throwable t) { return 0; }
    }

    /** 图标视图：优先 res/drawable 里的矢量图；取不到就退化成几何字符（任何构建都能显示）。 */
    private View iconView(String name, String glyph, int sizeDp, int key) {
        int id = drawableId(name);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp));
        if (id != 0) {
            ImageView iv = new ImageView(this);
            iv.setImageResource(id);
            iv.setColorFilter(c(key));
            iv.setLayoutParams(lp);
            iconKeys.put(iv, key);
            return iv;
        }
        TextView tv = new TextView(this);
        tv.setText(glyph);
        tv.setTextSize(sizeDp * 0.62f);
        tv.setGravity(Gravity.CENTER);
        tint(tv, key);
        tv.setLayoutParams(lp);
        return tv;
    }

    private TextView pill(String label, int key, boolean filled) {
        TextView t = new TextView(this);
        t.setText(label);
        t.setTextSize(12);
        t.setSingleLine(true);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(12), dp(6), dp(12), dp(6));
        t.setTextColor(c(filled ? C_TAB_ON_TX : key));
        textKeys.put(t, filled ? C_TAB_ON_TX : key);
        stylePill(t, filled);
        fluid(t);                                              /* 分段 pill 也吃柔性反馈 ✓ */
        return t;
    }

    private void stylePill(TextView t, boolean filled) {
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(999));
        if (filled) g.setColor(c(C_TAB_ON_BG));
        else { g.setColor(0x00000000); g.setStroke(dp(1), c(C_STROKE)); }
        t.setBackground(g);
    }

    private int blend(int color, int alphaHex) { return (color & 0x00FFFFFF) | (alphaHex << 24); }

    /** A. App Bar：标题(22/500) + 忙碌指示 + 驱动 chip + 🎨主题；下面一条 12sp 状态行。 */
    private View buildAppBar() {
        appBar = new LinearLayout(this);
        appBar.setOrientation(LinearLayout.VERTICAL);
        appBar.setBackgroundColor(withAlpha(c(C_CARD), x(3)));   /* ③ 半透磨砂（ColorOS 90%） */
        appBar.setElevation(dp(3));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(dp(56));
        row.setPadding(dp(16), 0, dp(8), 0);

        appTitle = new TextView(this);
        appTitle.setText(DEST_NAMES[0]);
        appTitle.setTextSize(22);
        appTitle.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        tint(appTitle, C_TEXT);
        row.addView(appTitle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        busyChip = pill("● 忙", C_WARN, false);
        busyChip.setVisibility(View.GONE);
        row.addView(busyChip);

        driverChip = pill("驱动: 未选择", C_ACCENT, false);
        driverChip.setOnClickListener(v -> switchTab(TAB_DRIVER));
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        clp.setMargins(dp(8), 0, dp(8), 0);
        driverChip.setLayoutParams(clp);
        row.addView(driverChip);
        headerDriver = driverChip;                        /* 兼容老代码：只 setText/setTextColor */

        View th = iconView("ic_theme", "🎨", 24, C_TEXT);
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(dp(48), dp(48));
        th.setLayoutParams(tlp);
        th.setOnClickListener(v -> cycleTheme());
        row.addView(th);

        appBar.addView(row);

        headerStatus = new TextView(this);
        headerStatus.setText("就绪。先到「测试 → 驱动」扫描或提取一个 .so。");
        headerStatus.setTextSize(12);
        headerStatus.setSingleLine(true);
        headerStatus.setEllipsize(TextUtils.TruncateAt.END);
        headerStatus.setPadding(dp(16), 0, dp(16), dp(6));
        tint(headerStatus, C_NEUTRAL);
        appBar.addView(headerStatus);
        return appBar;
    }

    /** C. 底部导航一项：56×32 激活指示器 + 24dp 图标 + 12sp 标签，整项 ≥64dp（热区达标）。 */
    private View navItem(final int i) {
        LinearLayout item = new LinearLayout(this);
        item.setOrientation(LinearLayout.VERTICAL);
        item.setGravity(Gravity.CENTER);
        item.setMinimumHeight(dp(64));
        item.setPadding(0, dp(6), 0, dp(6));

        FrameLayout stack = new FrameLayout(this);
        View ind = new View(this);
        ind.setLayoutParams(new FrameLayout.LayoutParams(dp(56), dp(32), Gravity.CENTER));
        navIndicators[i] = ind;
        stack.addView(ind);

        View ic = iconView(DEST_ICONS[i], DEST_GLYPHS[i], 24, C_DIM);
        navIcons[i] = ic;
        stack.addView(ic, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        item.addView(stack, new LinearLayout.LayoutParams(dp(56), dp(32)));

        TextView lab = new TextView(this);
        lab.setText(DEST_NAMES[i]);
        lab.setTextSize(12);
        lab.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        lab.setGravity(Gravity.CENTER);
        lab.setPadding(0, dp(4), 0, 0);
        tint(lab, C_DIM);
        navLabels[i] = lab;
        item.addView(lab);

        item.setOnClickListener(v -> selectDest(i, -1, true));
        fluid(item);                                           /* 底部导航项柔性反馈 ✓ */
        return item;
    }

    private View buildBottomNav() {
        bottomNav = new LinearLayout(this);
        bottomNav.setOrientation(LinearLayout.HORIZONTAL);
        bottomNav.setMinimumHeight(dp(64));
        bottomNav.setBackgroundColor(withAlpha(c(C_CARD), x(3)));
        bottomNav.setElevation(dp(3));
        for (int i = 0; i < DEST_NAMES.length; i++) {
            navItems[i] = navItem(i);
            bottomNav.addView(navItems[i], new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        }
        return bottomNav;
    }

    private void styleBottomNav() {
        for (int i = 0; i < 4; i++) {
            boolean on = (i == curDest);
            if (navIndicators[i] != null) {
                GradientDrawable g = new GradientDrawable();
                g.setCornerRadius(dp(16));
                g.setColor(on ? blend(c(C_ACCENT), 0x22) : 0x00000000);
                navIndicators[i].setBackground(g);
            }
            int key = on ? C_ACCENT : C_DIM;
            if (navLabels[i] != null) tint(navLabels[i], key);
            if (navIcons[i] instanceof ImageView) {
                ((ImageView) navIcons[i]).setColorFilter(c(key));
                iconKeys.put(navIcons[i], key);
            } else if (navIcons[i] instanceof TextView) tint((TextView) navIcons[i], key);
        }
    }

    /** 每页一个 FAB（同一时刻只有一个填充按钮 ⇒ 符合"一屏最多一个主操作"）。 */
    private TextView buildFab() {
        fab = new TextView(this);
        fab.setTextSize(14);
        fab.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        fab.setGravity(Gravity.CENTER);
        fab.setPadding(dp(20), dp(14), dp(20), dp(14));
        fab.setElevation(dp(6));
        fab.setOnClickListener(v -> { Runnable a = fabAction; if (a != null) a.run(); });
        return fab;
    }

    private FrameLayout.LayoutParams fabLp() {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.END | Gravity.BOTTOM);
        lp.setMargins(0, 0, dp(16), dp(16));
        return lp;
    }

    /* v9.27: 当前是否走原生（系统 Vulkan loader）路径 */
    private boolean isSystemDriver() { String p = selectedPath(); return p == null || p.equals("system"); }

    /* v9.48 · Shizuku/Stellar 探针（经 IShizukuService 公开路径）*/
    private static final String[] SHZ_CMDS = { "getprop ro.product.model", "getprop ro.board.platform", "getprop ro.hardware", "getprop | grep -i -e gpu -e vulkan -e mali -e mediatek | head -15", "dumpsys SurfaceFlinger | head -12", "dumpsys thermalservice | head -60", "dumpsys gpu | head -30", "cat /proc/meminfo | head -6", "cat /proc/loadavg", "ls /sys/class/devfreq/", "ls /sys/class/thermal/ 2>&1 | head -6", "ls /sys/kernel/ged/ 2>&1 | head -4", "dumpsys gfxinfo com.dsh.gputest | head -22", "dumpsys meminfo com.dsh.gputest | head -16", "dumpsys battery | head -12" };   /* v11.5: 本 App 内存 + 电池温度（binder 路 ✓）*/   /* v10.6: 渲染性能统计（binder 路 ✓）*/   /* v10.2: + thermalservice/gpu（走 binder，绕开 SELinux ✓）*/

    private void shizukuProbe() {
        if (!claim("Shizuku 探针")) return;
        setStatus("Shizuku 探针：检查服务…", C_WARN);
        new Thread(() -> {
            StringBuilder sb = new StringBuilder();
            try {
                sb.append("binder 可达 = ").append(rikka.shizuku.Shizuku.pingBinder()).append("\n");
                sb.append("uid = ").append(rikka.shizuku.Shizuku.getUid()).append("   version = ").append(rikka.shizuku.Shizuku.getVersion()).append("\n");
                boolean ok = rikka.shizuku.Shizuku.checkSelfPermission() == android.content.pm.PackageManager.PERMISSION_GRANTED;
                sb.append("权限 = ").append(ok ? "已授权" : "未授权").append("\n");
                if (!ok) {
                    ui.post(() -> { try { rikka.shizuku.Shizuku.requestPermission(0); } catch (Throwable t) { log("  ! requestPermission: " + t); } });
                    sb.append("已发起授权请求 -> 请在 Stellar 界面点「允许」，然后再点一次本按钮").append("\n");
                } else {
                    moe.shizuku.server.IShizukuService svc = moe.shizuku.server.IShizukuService.Stub.asInterface(rikka.shizuku.Shizuku.getBinder());
                    sb.append("service = ").append(svc == null ? "null" : "ok").append("\n");
                    if (svc != null) for (String c : SHZ_CMDS) sb.append("$ ").append(c).append("\n").append(runShizuku(svc, c)).append("\n");
                }
            } catch (Throwable t) { sb.append("X 异常: ").append(t).append("\n"); }
            final String txt = sb.toString();
            logBlock("Shizuku 探针", txt);
            shzDataText = txt;   /* v10.1: 存下来给设置页的面板用 ✓ */
            final String ft = txt;
            ui.post(() -> { if (shzDataTx != null) shzDataTx.setText(ft); });
            ui.post(() -> { setStatus("Shizuku 探针：完成（原文见日志）", C_OK); try { new android.app.AlertDialog.Builder(this).setTitle("Shizuku / Stellar 探针").setMessage(txt).setPositiveButton("好", null).show(); } catch (Throwable ignored) { } });
            endTask(); release();
        }, "shizuku-probe").start();
    }

    /* v10.3 · 实时监视（不占 busy 位，可与光追/跑分同时跑 ✓）*/
    private static final String[] LIVE_CMDS = {
        "dumpsys thermalservice | grep -m6 -e Current -e GPU -e CPU",
        "dumpsys gpu | grep -m3 -e Global -e Driver", "dumpsys gfxinfo com.dsh.gputest | grep -m10 -e Total -e Janky -e percentile", "dumpsys meminfo com.dsh.gputest | grep -m6 -e TOTAL -e Native -e Graphics" };   /* v11.5: 实时也带内存 ✓ */   /* v10.6: 真渲染性能（帧时间分布/卡顿率）*/
    /* v10.4 · 趋势解析与绘制（块字符 sparkline ✓ 零依赖 ✓）*/
    private void liveParse(String t) {
        try {
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("mValue=([0-9.]+), mType=[0-9]+, mName=GPU").matcher(t);
            float last = -1f;
            while (m.find()) last = Float.parseFloat(m.group(1));
            if (last > 0f) { liveGpuT = last; liveHist.add(last); if (liveHist.size() > 24) liveHist.remove(0); }
            java.util.regex.Matcher m2 = java.util.regex.Pattern.compile("Global total: ([0-9]+)").matcher(t);
            if (m2.find()) liveMemMB = Float.parseFloat(m2.group(1)) / 1048576f;
            java.util.regex.Matcher m3 = java.util.regex.Pattern.compile("Total frames rendered: ([0-9]+)").matcher(t);
            if (m3.find()) liveFrames = Integer.parseInt(m3.group(1));
            java.util.regex.Matcher m4 = java.util.regex.Pattern.compile("Janky frames: [0-9]+ .([0-9.]+)%.*").matcher(t);   /* v10.9: 无反斜杠正则（heredoc 会吃反斜杠 ✗）*/
            if (m4.find()) liveJankPct = (int) Float.parseFloat(m4.group(1));
            java.util.regex.Matcher m5 = java.util.regex.Pattern.compile("50th percentile: ([0-9]+)ms").matcher(t);
            if (m5.find()) liveF50 = Integer.parseInt(m5.group(1));
            java.util.regex.Matcher m6 = java.util.regex.Pattern.compile("50th gpu percentile: ([0-9]+)ms").matcher(t);
            if (m6.find()) liveG50 = Integer.parseInt(m6.group(1));
        } catch (Throwable ignored) { }
    }
    private String liveRender() {
        StringBuilder b = new StringBuilder();
        b.append("GPU 温度：").append(liveGpuT > 0 ? String.format(java.util.Locale.US, "%.1f °C", liveGpuT) : "（等待首次采样…）").append("");
        if (liveHist.size() >= 2) {
            float mn = 999f, mx = -999f;
            for (float v : liveHist) { if (v < mn) mn = v; if (v > mx) mx = v; }
            String[] sp = { "_", ".", "-", "=", "+", "*", "#", "@" };
            b.append("趋势（最近 ").append(liveHist.size()).append(" 次）：");
            for (float v : liveHist) {
                int k = (mx - mn) < 0.01f ? 3 : (int) ((v - mn) / (mx - mn) * 7f + 0.5f);
                if (k < 0) k = 0; if (k > 7) k = 7;
                b.append(sp[k]);
            }
            b.append(String.format(java.util.Locale.US, "   最低 %.1f / 最高 %.1f °C", mn, mx));
        }
        if (liveMemMB > 0f) b.append(String.format(java.util.Locale.US, "GPU 显存：%.0f MB", liveMemMB));
        if (liveFrames > 0) b.append(String.format(java.util.Locale.US, "帧：%d 帧 · 卡顿 %d%% · 帧时间 50th %dms · GPU 50th %dms", liveFrames, liveJankPct, liveF50, liveG50));
        b.append("");
        return b.toString();
    }

    /* v11.0 · 跑分 HTML 报告：成绩 + 温度趋势图 + 帧统计 + 设备信息（内联样式，零依赖 ✓）*/
    private void exportHtmlReport() {
        try {
            char NL = (char) 10;
            StringBuilder h = new StringBuilder();
            h.append("<!doctype html><meta charset=utf-8><title>GPU 驱动测试台 报告</title>");
            h.append("<style>body{font-family:sans-serif;background:#101216;color:#e8e8e8;padding:18px}");
            h.append("h2{color:#4ea1ff}pre{background:#1a1d23;padding:10px;border-radius:8px;overflow:auto;font-size:12px}");
            h.append("table{border-collapse:collapse}td,th{border:1px solid #333;padding:6px 10px;font-size:13px}");
            h.append("</style><h2>GPU 驱动测试台 · 报告</h2>");
            h.append("<div>生成时间：").append(new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.US).format(new java.util.Date())).append("</div>");
            h.append("<div>机型：").append(android.os.Build.MODEL).append(" · 平台：").append(android.os.Build.HARDWARE).append(" · Android ").append(android.os.Build.VERSION.RELEASE).append("</div>");
            h.append("<h3>成绩</h3><pre>").append(scoreTable == null ? "（本次无成绩）" : scoreTable.getText().toString()).append("</pre>");
            h.append("<h3>光追配置快照</h3><table><tr><th>阴影采样</th><th>反射弹射</th><th>材质细节</th><th>光晕</th><th>灯绕行</th><th>镜面率</th></tr><tr><td>").append(rtSamples).append("</td><td>").append(rtBounce).append("</td><td>").append(rtDetail == 1 ? "开" : "关").append("</td><td>").append(rtBloom).append("%</td><td>").append(rtLightMove).append("</td><td>").append(rtMirror).append("%</td></tr></table>");
            h.append("<h3>帧统计</h3><table><tr><th>总帧数</th><th>卡顿率</th><th>帧时间 50th</th><th>GPU 50th</th></tr><tr><td>").append(liveFrames).append("</td><td>").append(liveJankPct).append("%</td><td>").append(liveF50).append("ms</td><td>").append(liveG50).append("ms</td></tr></table>");
            h.append("<h3>GPU 温度趋势</h3>");
            if (liveHist.size() > 0) {
                float mn = 999f, mx = -999f;
                for (float v : liveHist) { if (v < mn) mn = v; if (v > mx) mx = v; }
                h.append("<div>最低 ").append(String.format(java.util.Locale.US, "%.1f", mn)).append(" °C · 最高 ").append(String.format(java.util.Locale.US, "%.1f", mx)).append(" °C · 样本 ").append(liveHist.size()).append("</div>");
                h.append("<div style=&apos;height:120px;display:flex;align-items:flex-end;gap:2px;background:#1a1d23;padding:8px;border-radius:8px&apos;>");
                for (float v : liveHist) {
                    int hh = (mx - mn) < 0.01f ? 60 : (int) (10 + 100 * (v - mn) / (mx - mn));
                    h.append("<div style=&apos;width:12px;height:").append(hh).append("px;background:linear-gradient(#ff9d4e,#4ea1ff);border-radius:2px&apos;></div>");
                }
                h.append("</div>");
            } else h.append("<div>（无采样：请先在设置页开启实时监视）</div>");
            h.append("<h3>设备真实数据（Shizuku 探针原文）</h3><pre>").append(shzDataText.length() == 0 ? "（未运行探针）" : shzDataText).append("</pre>");
            String ts = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(new java.util.Date());
            String path = "/sdcard/Download/gputest-report-" + ts + ".html";
            java.io.FileWriter fw = new java.io.FileWriter(path);
            fw.write(h.toString()); fw.close();
            setStatus("报告已导出：" + path, C_OK); log("  HTML 报告 -> " + path);
        } catch (Throwable t) { setStatus("报告导出失败: " + t, C_BAD); log("  X 报告导出失败: " + t); }
    }

    private void liveToggle() {
        liveOn = !liveOn;
        setStatus(liveOn ? "实时监视：开（2 秒一次）" : "实时监视：关", liveOn ? C_OK : C_NEUTRAL);
        if (liveOn) { log("  实时监视 -> 开"); liveTick(); } else if (liveTx != null) { liveTx.setText("（已停止）"); log("  实时监视 -> 关"); }
    }
    private void liveTick() {
        if (!liveOn) return;
        new Thread(() -> {
            String txt;
            try {
                moe.shizuku.server.IShizukuService svc = moe.shizuku.server.IShizukuService.Stub.asInterface(rikka.shizuku.Shizuku.getBinder());
                if (svc == null) txt = "X 服务不可用（请先授权 / 启动 Stellar）";
                else { StringBuilder sb = new StringBuilder(); for (String c : LIVE_CMDS) sb.append(runShizuku(svc, c)); txt = sb.toString(); }
            } catch (Throwable t) { txt = "X " + t; }
            final String ft = txt;
            ui.post(() -> { liveParse(ft); if (liveTx != null) liveTx.setText(liveRender()); });   /* v10.4 解析 + 画趋势 */
            ui.postDelayed(() -> liveTick(), 2000);
        }, "live-poll").start();
    }

    private String runShizuku(moe.shizuku.server.IShizukuService svc, String cmd) {
        try {
            moe.shizuku.server.IRemoteProcess rp = svc.newProcess(new String[] { "sh", "-c", cmd }, null, null);
            if (rp == null) return "（newProcess 返回 null）";
            java.io.InputStream is = new android.os.ParcelFileDescriptor.AutoCloseInputStream(rp.getInputStream());
            java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(is));
            StringBuilder o = new StringBuilder();
            String l; int n = 0;
            while ((l = r.readLine()) != null && n++ < 14) o.append(l).append("\n");
            rp.waitFor();
            return o.length() == 0 ? "（无输出）" : o.toString();
        } catch (Throwable t) { return "X newProcess 失败: " + t; }
    }

    /* ================= v9.53 · 设置卡（本软件专用） ================= */
    private void openStellar() {
        try {
            android.content.Intent i = getPackageManager().getLaunchIntentForPackage("roro.stellar.manager");
            if (i != null) startActivity(i); else toast("未找到 Stellar 管理器");
        } catch (Throwable t) { toast("打开失败: " + t); }
    }

    private void shzRequest() {
        try { rikka.shizuku.Shizuku.requestPermission(0); toast("已发起授权，请在 Stellar 里点「允许」"); }
        catch (Throwable t) { toast("申请失败: " + t); }
    }

    private void shzRefresh() {
        new Thread(() -> {
            String out;
            try {
                boolean up = rikka.shizuku.Shizuku.pingBinder();
                boolean ok = up && rikka.shizuku.Shizuku.checkSelfPermission() == android.content.pm.PackageManager.PERMISSION_GRANTED;
                out = "Shizuku/Stellar: " + (up ? "在线" : "未运行")
                    + " | version=" + (up ? rikka.shizuku.Shizuku.getVersion() : -1)
                    + " | uid=" + (up ? rikka.shizuku.Shizuku.getUid() : -1)
                    + " | 权限=" + (ok ? "已授权" : "未授权");
            } catch (Throwable t) { out = "Shizuku 状态读取失败: " + t; }
            final String f = out;
            ui.post(() -> { if (shzStatusTx != null) shzStatusTx.setText(f); });
            log("  " + f);
        }, "shz-status").start();
    }

    /* ==================================================================
     * v9.55 · 独立「设置」页（底部导航最后一项，排在「记录」之后）
     * ================================================================== */
    private View buildSettingsPage() {
        android.widget.ScrollView sv = new android.widget.ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(14);
        col.setPadding(pad, pad, pad, dp(90));
        sv.addView(col);

        col.addView(section("Shizuku / Stellar（提权通道 · 真数据）"));
        shzStatusTx = text("正在读取 Shizuku 状态…", 11, C_TEXT, false);
        col.addView(shzStatusTx);
        /* v9.62：横排里 btnTonal 是 MATCH_PARENT 宽 ⇒ 后两个被挤出屏幕 ✗ 改纵向堆叠 ✓ */
        col.addView(btnTonal("申请授权（弹 Stellar 授权页）", v -> shzRequest()));
        col.addView(btnTonal("刷新状态", v -> shzRefresh()));
        col.addView(btnTonal("打开 Stellar 管理器", v -> openStellar()));
        /* v10.1 · 真实数据面板：探针结果直接显示在设置页 ✓ 不用再翻对话框 ✓ */
        col.addView(text("授权后可在下方「读取真实数据」查看真机数据（机型/平台/内存/负载/驱动属性/节点清单）✓", 10, C_TEXT, false));
        shzDataTx = text(shzDataText.length() == 0 ? "（点下面按钮读取真实数据）" : shzDataText, 11, C_TEXT, false);
        shzDataTx.setTypeface(Typeface.MONOSPACE);
        col.addView(card(section("设备真实数据（Shizuku 通道）"),
                shzDataTx,
                note("数据来自 uid=2000 提权通道：机型 / 平台 / 内存 / 负载 / 驱动属性 / devfreq+thermal 节点清单 ✓"),
                btnTonal("读取真实数据", v -> shizukuProbe())));
        /* v10.3 · 实时监视：每 2 秒采一次 GPU 温度与显存 ✓ */
        liveTx = text("（未开始）", 11, C_TEXT, false);
        liveTx.setTypeface(Typeface.MONOSPACE);
        col.addView(card(section("实时监视（GPU 温度 / 显存）"),
                liveTx,
                note("每 2 秒调用 dumpsys thermalservice / gpu（binder 路，绕开 SELinux ✓）—— 开着光追时点开，就能看到温度随负载上升 ✓"),
                btnTonal("▶ 开始 / ■ 停止 实时监视", v -> liveToggle())));
        col.addView(btnTonal("导出 HTML 报告（成绩+温度趋势+帧统计）", v -> exportHtmlReport()));   /* v11.0 */

        col.addView(section("渲染"));
        col.addView(btnTonal("帧率上限：30fps / 不限（点击切换）", v -> {
            fpsCap = !fpsCap;
            toast("帧率上限：" + (fpsCap ? "30fps" : "不限"));
            log("  帧率上限 -> " + (fpsCap ? "30fps" : "不限"));
        }));
        LinearLayout r2 = new LinearLayout(this);
        r2.setOrientation(LinearLayout.HORIZONTAL);
        for (final int sz : new int[] { 128, 192, 256, 384, 512, 768 }) {
            Button b = btnTonal(sz + "²", v -> { litSize = sz; toast("默认画面尺寸：" + sz + "x" + sz + "（下次开始生效）"); log("  默认画面尺寸 -> " + sz); });
            b.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            r2.addView(b);
        }
        col.addView(text("默认画面尺寸（点一下设定，下次开始渲染生效）", 10, C_TEXT, false));
        col.addView(r2);

        col.addView(section("维护"));
        col.addView(btnTonal("清空日志（/sdcard/Download/gputest-last.log）", v -> {
            try { new java.io.File("/sdcard/Download/gputest-last.log").delete(); toast("日志已清空"); }
            catch (Throwable t) { toast("清空失败: " + t); }
        }));
        col.addView(btnTonal("导出日志", v -> exportLog()));

        col.addView(section("关于"));
        String info;
        try {
            android.content.pm.PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            info = "包名 " + getPackageName() + " · 版本 " + pi.versionName + " (" + pi.versionCode + ")"
                 + "  |  设备 " + android.os.Build.MODEL + " · Android " + android.os.Build.VERSION.RELEASE
                 + "  |  测试方式 直接 dlopen 驱动 .so（不经系统 loader），另有「原生 Vulkan（系统 loader）」可选 ✓";
        } catch (Throwable t) { info = "版本信息读取失败: " + t; }
        col.addView(text(info, 10, C_TEXT, false));

        shzRefresh();
        return sv;
    }

    static {
        /* v9.56 · Java 未捕获异常记录器：此前只有原生信号会写文件 ⇒ Java 崩溃我完全看不到 ✗ */
        try {
            final java.io.File f = new java.io.File("/sdcard/Download/gputest_java_crash.txt");
            Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
                try {
                    java.io.FileWriter w = new java.io.FileWriter(f, true);
                    w.write("=== " + new java.util.Date() + " | thread=" + t.getName() + " ===\n");
                    w.write(e.toString() + "\n");
                    for (StackTraceElement el : e.getStackTrace()) w.write("  at " + el + "\n");
                    Throwable c = e.getCause();
                    if (c != null) w.write("caused by: " + c + "\n");
                    w.write("\n");
                    w.close();
                } catch (Throwable ignored) { }
            });
        } catch (Throwable ignored) { }
    }

    /* v9.62 · UI 优化第三刀 */
    private void updateFab() {
        if (fab == null) return;
        String label; Runnable act;
        if (curDest == DEST_OVERVIEW)    { label = "扫描驱动";   act = this::scanDrivers; }
        else if (curDest == DEST_TEST)   { label = "一键全流程"; act = this::runAll; }
        else if (curDest == DEST_RENDER) {
            if (segRender == SEG_RENDER_STRESS)   { label = stressRunning ? "■ 停止压力" : "▶ 开始压力"; act = this::toggleStress; }
            else if (segRender == SEG_RENDER_LIT) { label = litRunning ? "■ 停止光影" : "▶ 开始光影"; act = this::toggleLit; }
            else                                  { label = renderRunning ? "■ 停止渲染" : "▶ 开始渲染"; act = this::toggleRender; }
        } else                           { label = "导出日志";   act = this::exportLog; }
        fab.setText(label);
        fabAction = act;
        fab.setVisibility(View.GONE);   /* v9.24: user asked to remove the floating bottom-right button on every page */
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(16));
        g.setColor(c(C_TAB_ON_BG));
        fab.setBackground(g);
        fab.setTextColor(c(C_TAB_ON_TX));
        textKeys.put(fab, C_TAB_ON_TX);
    }

    /** 页内二级分段（pill）：测试[驱动|校验|跑分|对比]、画面[实时渲染|压力]、记录[成绩|报告|日志]。 */
    private View segmented(final int group, String[] labels, String[] icons, int selected) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(dp(12), dp(8), dp(12), dp(8));
        segPills[group] = new TextView[labels.length];
        for (int i = 0; i < labels.length; i++) {
            final int idx = i;
            TextView p = pill(labels[i], C_DIM, i == selected);
            if (icons != null && icons.length > i) {
                int id = drawableId(icons[i]);
                if (id != 0) {
                    android.graphics.drawable.Drawable d = getResources().getDrawable(id, null);
                    d.setBounds(0, 0, dp(16), dp(16));
                    p.setCompoundDrawables(d, null, null, null);
                    p.setCompoundDrawablePadding(dp(6));
                    try { p.setCompoundDrawableTintList(
                            android.content.res.ColorStateList.valueOf(c(i == selected ? C_TAB_ON_TX : C_DIM))); }
                    catch (Throwable ignored) { }
                }
            }
            p.setOnClickListener(v -> {
                if (group == 0)      selectDest(DEST_TEST, idx, true);
                else if (group == 1) selectDest(DEST_RENDER, idx, true);
                else                 selectDest(DEST_RECORDS, idx, true);
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, 0, dp(8), 0);
            p.setLayoutParams(lp);
            segPills[group][i] = p;
            row.addView(p);
        }
        return row;
    }

    private View hub(View seg, View[] pages, int sel) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.addView(seg);
        FrameLayout host = new FrameLayout(this);
        for (int i = 0; i < pages.length; i++) {
            pages[i].setVisibility(i == sel ? View.VISIBLE : View.GONE);
            host.addView(pages[i], new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        col.addView(host, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return col;
    }

    private View buildTestHubPage() {
        hubTestPages = new View[] { buildDriverPage(), buildTestPage(), buildBenchPage(), buildComparePage() };
        return hub(segmented(0, new String[] { "驱动", "校验", "跑分", "对比" },
                new String[] { "ic_tab_driver", "ic_tab_test", "ic_tab_bench", "ic_tab_stress" }, segTest),
                hubTestPages, segTest);
    }

    private View buildRenderHubPage() {
        hubRenderPages = new View[] { buildPicturePage(), buildLitPage(), buildStressPage() };
        return hub(segmented(1, new String[] { "实时渲染", "光影", "压力" },
                new String[] { "ic_tab_render", "ic_tab_bench", "ic_tab_stress" }, segRender),
                hubRenderPages, segRender);
    }

    private View buildRecordsHubPage() {
        hubRecPages = new View[] { buildScoreboardPage(), buildReportPage(), buildLogPage() };
        return hub(segmented(2, new String[] { "成绩", "报告", "日志" },
                new String[] { "ic_tab_bench", "ic_tab_log", "ic_tab_log" }, segRec), hubRecPages, segRec);
    }

    /** ⑤ 搜索过滤：按文本命中显隐行（RadioButton 的文本是两行 Spannable ⇒ 直接 toString 匹配）。 */
    private void filterGroup(RadioGroup g, String q) {
        if (g == null) return;
        for (int i = 0; i < g.getChildCount(); i++) {
            View c = g.getChildAt(i);
            boolean show = q.length() == 0
                    || (c instanceof TextView && ((TextView) c).getText().toString()
                            .toLowerCase(Locale.ROOT).contains(q));
            c.setVisibility(show ? View.VISIBLE : View.GONE);
        }
    }

    private void applySegments() {
        View[][] hubs = { hubTestPages, hubRenderPages, hubRecPages };
        int[] sels = { segTest, segRender, segRec };
        for (int g = 0; g < hubs.length; g++) {
            if (hubs[g] == null) continue;
            for (int i = 0; i < hubs[g].length; i++)
                hubs[g][i].setVisibility(i == sels[g] ? View.VISIBLE : View.GONE);
            styleSegRow(g, sels[g]);
        }
        updateFab();
    }

    private void styleSegRow(int group, int sel) {
        if (segPills[group] == null) return;
        for (int i = 0; i < segPills[group].length; i++) {
            TextView p = segPills[group][i];
            if (p == null) continue;
            boolean on = (i == sel);
            stylePill(p, on);
            int key = on ? C_TAB_ON_TX : C_DIM;
            p.setTextColor(c(key));
            textKeys.put(p, key);
            try { p.setCompoundDrawableTintList(android.content.res.ColorStateList.valueOf(c(key))); }
            catch (Throwable ignored) { }
        }
    }

    /* ---- A/B 对比页 / 成绩页 / 报告页 ---- */
    /* =========================================================================
     *  v7.0 光影设置面板：阴影 / 画面效果 / 负载 / 调试 四组
     *  布尔=Switch（带"它影响什么"的灰字）；离散=pill 分段；倍率=SeekBar 1..64
     *  改动立即生效（nativeLitConfig）并回显原生摘要行；持久化 lit_* ✓
     * ========================================================================= */
    private View buildLitPage() {
        LinearLayout col = col();
        runSafe("光影设置回填", this::litLoadPrefs);
        /* ★ 原来在 onCreate 里**同步调原生**（nativeLitSettings）⇒ 最可能的崩溃点；
         *   改为界面就绪后 800ms 再调，并包保险 ✓ */
        ui.postDelayed(() -> runSafe("光影原生初始化", () -> { litInitFromNative(); litRefreshSummary(); }), 800);

        col.addView(rtBigCard());                   /* ★ 光追：顶部最醒目的大开关 */

        litView = new ImageView(this);
        litView.setAdjustViewBounds(true);
        litView.setScaleType(ImageView.ScaleType.CENTER_INSIDE);
        litView.setBackgroundColor(c(C_RENDER_BG));
        litStats = text("GPU 时间 — ｜ FPS — ｜ 占空比 —", 12, C_TEXT, false);
        litStats.setTypeface(Typeface.MONOSPACE);
        litNativeTx = text("", 11, C_TEXT, false);
        litNativeTx.setTypeface(Typeface.MONOSPACE);
        litNativeTx.setLineSpacing(0, 1.2f);
        litNativeTx.setVisibility(View.GONE);
        col.addView(card(section("光影画面（尺寸可调）"), litView, litStats, litNativeTx,
                note("统计口径与跑分一致：GPU 时间取自时间戳，占空比 = GPU 时间 / 墙钟。"
                   + "下面那行是原生 nativeLitFrame 的**原样返回**（成功绿 / 失败红）。")));
        LinearLayout sizeRow = new LinearLayout(this);          /* v9.18：画面尺寸可调 */
        sizeRow.setOrientation(LinearLayout.HORIZONTAL);
        for (final int sz : new int[] { 128, 192, 256, 384, 512, 640, 768, 1024 })   /* v9.19：更多档位 ✓ */ {
            sizeRow.addView(btnTonal(String.valueOf(sz), v -> {
                litSize = sz;
                setStatus("画面尺寸：" + sz + "×" + sz + "（下次开始生效）", C_NEUTRAL);
                log("  画面尺寸 -> " + sz + "×" + sz);
            }));
        }
        col.addView(sizeRow);
        for (int i = 0; i < sizeRow.getChildCount(); i++) {          /* v9.21：① 等分宽度（原来 8 个 MATCH_PARENT
                                                                     * 只显示第 1 个 ✗）② 从任务锁名单里摘出去 ⇒
                                                                     * 渲染中也能点，不会被 setEnabled(false) 禁用 ✓ */
            View c0 = sizeRow.getChildAt(i);
            c0.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            actionButtons.remove(c0);
        }
        shzRefresh();                                   /* 建页时自动读一次状态 ✓ */


        m3dBtn = btnPrimary("▶ 迷你 3D（独立最简路径）", v -> toggleMini3D());   /* v9.14 */
        col.addView(m3dBtn);
        actionButtons.remove(m3dBtn);
        rt3dBtn = btnPrimary("▶ 光追 3D（硬件 RT · ray query）", v -> toggleRt3D());   /* v9.22 */
        col.addView(rt3dBtn);
        actionButtons.remove(rt3dBtn);                              /* 同样免疫任务锁 ⇒ 随时能停 ✓ */                               /* v9.21 ★ 真凶：claim() 会把 actionButtons 里所有按钮
                                                                     * setEnabled(false)，而停止按钮也在名单里 ⇒ 一旦渲染开始，
                                                                     * 停止按钮自己就被禁用了 ⇒ 点它不触发 onClick、日志一条都没有 ✗
                                                                     * 控制按钮必须免疫任务锁 ✓ */   /* v9.11：全新最简 GPU 场景，用于判断 GPU 路径本身能不能出画 ✓ */

        litSummary = text("（原生摘要未接入时显示本地记录）", 11, C_TEXT, false);
        litSummary.setTypeface(Typeface.MONOSPACE);
        litSummary.setLineSpacing(0, 1.2f);
        col.addView(card(section("当前配置（原生摘要）"), litSummary));

        LinearLayout shadowGroup = new LinearLayout(this);
        shadowGroup.setOrientation(LinearLayout.VERTICAL);
        shadowGroup.addView(section("阴影"));
        shadowGroup.addView(litSwitch("shadows", "阴影", "开：逐帧渲一张阴影深度图并采样；关：只画直射光（快很多）", litShadows));
        shadowGroup.addView(litPills("pcf", "PCF 质量", new int[] { 1, 3, 5 },
                new String[] { "1（硬边）", "3×3", "5×5" }, litPcf,
                "每轴采样数：1 = 硬边阴影，3 = 3×3 软阴影，5 = 5×5 更软（越软越贵）"));
        shadowGroup.addView(litPills("shadowRes", "阴影贴图分辨率", new int[] { 1024, 2048, 4096 },
                new String[] { "1024²", "2048²", "4096²" }, litRes,
                "深度图分辨率：越大阴影越锐利，显存与带宽占用越高"));
        col.addView(card(shadowGroup));

        LinearLayout fxGroup = new LinearLayout(this);
        fxGroup.setOrientation(LinearLayout.VERTICAL);
        fxGroup.addView(section("画面效果"));
        /* v9.98 · 光追配置（每一项都直连渲染路径 ✓ 不是摆设）*/
        fxGroup.addView(litPills("rtSamples", "光追阴影采样数", new int[]{1, 2, 4, 8, 16},
                new String[]{"1", "2", "4", "8", "16"}, rtSamples,
                "每像素向灯打几条阴影射线：越多阴影越软越准，耗时线性上升（RT 最贵的一项）"));
        fxGroup.addView(litPills("rtBounce", "反射弹射", new int[]{0, 1, 2},
                new String[]{"关", "1 次", "2 次"}, rtBounce,
                "0 = 关反射（省约一半时间，适合跑分对比）；1 = 单次；2 = 镜中镜（能看到镜子里的镜子）"));
        fxGroup.addView(litSwitch("rtDetail", "材质细节（程序化）", "木纹/板缝/锈斑/砖缝/污渍；关掉可对比细节值多少帧", rtDetail));
        fxGroup.addView(litPills("rtBloom", "光晕强度", new int[]{0, 50, 100, 200},
                new String[]{"关", "50%", "100%", "200%"}, rtBloom, "灯周围的亮部外溢强度（泛光的解析近似）"));
        fxGroup.addView(litPills("rtLightMove", "灯绕行速度", new int[]{0, 1, 2},
                new String[]{"静止", "慢", "快"}, rtLightMove,
                "静止 = 画面不变，可逐像素比对；慢/快 = 实时渲染演示（阴影与反射逐帧变化）"));
        fxGroup.addView(litPills("rtMirror", "镜面反射率", new int[]{50, 80, 97, 99},
                new String[]{"50%", "80%", "97%", "99%"}, rtMirror, "墙上两面镜子的反射强度"));
        fxGroup.addView(litSwitch("bloom", "泛光（Bloom）",
                "亮部溢出到周边：多一遍降采样+模糊+叠加，影响观感与带宽（3D 场景已生效 · 原生光影待接线）", litBloom));
        fxGroup.addView(litSwitch("tonemap", "色调映射 + 伽马",
                "关掉看到线性原始值（偏暗），打开才是最终观感；逐帧比对时可关（3D 场景已生效 · 原生光影待接线）", litTonemap));
        fxGroup.addView(litSwitch("animate", "动画（随时间变化）",
                "关掉画面静止 ⇒ 便于逐帧逐像素比对正确性（3D 场景已生效 · 原生光影待接线）", litAnimate));
        col.addView(card(fxGroup));

        LinearLayout loadGroup = new LinearLayout(this);
        loadGroup.setOrientation(LinearLayout.VERTICAL);
        loadGroup.addView(section("负载"));
        loadGroup.addView(litPills("cubes", "立方体实例数", new int[] { 16, 36, 100, 400 },
                new String[] { "16", "36", "100", "400" }, litCubes,
                "几何负载阶梯：实例越多，顶点/绘制调用与阴影图负担越高"));
        TextView passLbl = text("每帧光照 pass 倍率（负载放大器）", 14, C_TEXT, true);
        passLbl.setPadding(0, dp(10), 0, dp(2));
        loadGroup.addView(passLbl);
        litPassBar = new SeekBar(this);
        litPassBar.setMax(63);
        litPassBar.setProgress(Math.max(1, litPasses) - 1);
        litPassTx = text("passes = " + litPasses, 12, C_ACCENT, false);
        litPassTx.setTypeface(Typeface.MONOSPACE);
        litPassBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int prog, boolean fromUser) {
                litPasses = prog + 1;
                if (litPassTx != null) litPassTx.setText("passes = " + litPasses);
                if (fromUser) litApply("passes", litPasses);
            }
            @Override public void onStartTrackingTouch(SeekBar b) { }
            @Override public void onStopTrackingTouch(SeekBar b) { }
        });
        loadGroup.addView(litPassBar);
        loadGroup.addView(litPassTx);
        loadGroup.addView(note("1 = 正常；调大后每帧重复跑同样多的光照 pass（1..64）⇒ 压出 GPU 时间差"
                + "（3D 场景已生效 · 原生光影待接线）"));
        col.addView(card(loadGroup));

        LinearLayout dbgGroup = new LinearLayout(this);
        dbgGroup.setOrientation(LinearLayout.VERTICAL);
        dbgGroup.addView(section("调试"));
        dbgGroup.addView(litPills("debugView", "调试视图", new int[] { 0, 1, 2 },
                new String[] { "最终画面", "阴影贴图", "法线" }, litDebug,
                "正确性自检：直接看中间结果（阴影深度图 / 法线），而不是只看最终画面（仅原生光影有此视图，3D 场景暂无）"));
        col.addView(card(dbgGroup));

        col.addView(card(section("恢复"),
                btnTonal("重置默认（阴影开 · PCF 3 · 2048² · 36 个 · 泛光开 · 色调映射开 · 倍率 1 · 最终视图 · 动画开）",
                        v -> litResetDefaults())));

        /* 摘要由上面的 postDelayed 填（不在这里同步调原生 ✓） */
        return scroll(col);
    }

    /** ★ 光追大卡：比其它卡片大一号 + 2dp 主色描边（用户要的"大选项"）。 */
    private View rtBigCard() {
        rtCardBox = new LinearLayout(this);
        rtCardBox.setOrientation(LinearLayout.VERTICAL);
        rtCardBox.setBackground(rtBigCardBg());
        int p = dp(18);
        rtCardBox.setPadding(p, p, p, p);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(6), 0, dp(6));
        rtCardBox.setLayoutParams(lp);

        TextView title = text("光追（Ray Tracing）", 18, C_ACCENT, true);   /* 18sp：比卡片标题(16)大一号 */
        rtCardBox.addView(title);
        rtCardBox.addView(note("硬件加速的光线追踪。**能不能开取决于当前驱动**——"
                + "探测结果会原样贴出来，不支持就明说，绝不用软件假装 ✓"));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(text("开启光追", 16, C_TEXT, true),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        rtSwitch = new android.widget.Switch(this);
        rtSwitch.setChecked(rtOn != 0);
        rtSwitch.setScaleX(1.15f); rtSwitch.setScaleY(1.15f);              /* 触控/视觉都更大一号 */
        row.addView(rtSwitch);
        rtCardBox.addView(row);
        fluid(row);

        rtVerdictTx = text(rtOn != 0 ? "（正在探测…）" : "（开关打开后会立刻探测该驱动的光追能力）", 13, C_DIM, true);
        rtVerdictTx.setPadding(0, dp(6), 0, dp(2));
        rtCardBox.addView(rtVerdictTx);

        rtProbeTx = text("", 11, C_TEXT, false);
        rtProbeTx.setTypeface(Typeface.MONOSPACE);
        rtProbeTx.setLineSpacing(0, 1.2f);
        rtProbeTx.setVisibility(View.GONE);
        rtCardBox.addView(rtProbeTx);

        rtView = new ImageView(this);
        rtView.setAdjustViewBounds(true);
        rtView.setScaleType(ImageView.ScaleType.CENTER_INSIDE);
        rtView.setBackgroundColor(c(C_RENDER_BG));
        rtView.setVisibility(View.GONE);
        rtCardBox.addView(rtView);

        rtRunBtn = btnPrimary("开始光追渲染（硬件）", v -> rtRunToggle());
        rtRunBtn.setEnabled(false);                          /* 只有探测到"支持硬件光追"才可点 ✓ */
        rtCardBox.addView(rtRunBtn);

        rtInitTx = text("", 11, C_TEXT, false);              /* nativeRtInit 返回行：等宽、原样 ✓ */
        rtInitTx.setTypeface(Typeface.MONOSPACE);
        rtInitTx.setLineSpacing(0, 1.2f);
        rtInitTx.setVisibility(View.GONE);
        rtCardBox.addView(rtInitTx);

        rtCpuBtn = btnTonal("CPU 光追参考图", v -> {
            if (busy) { toast("测试进行中，稍后再点"); return; }
            log("  ⏸ CPU 光追参考图：nativeCpuRtRender/nativeCpuRtBlit 尚未导出 ⇒ 本按钮先占位 ✓");
            if (rtInitTx != null) {
                rtInitTx.setVisibility(View.VISIBLE);
                rtInitTx.setText("CPU 光追参考图将在下版接入（逐像素光线求交，结果会明确标注「非硬件光追」）");
                rtInitTx.setTextColor(c(C_WARN));
            }
            toast("CPU 光追参考图将在下版接入");
        });
        rtCardBox.addView(rtCpuBtn);

        rtCardBox.addView(note("本机 GPU（Immortalis-G720）硬件支持光追；但能不能真跑取决于当前驱动 —— "
                + "系统驱动（Mali 闭源）报告支持 ray_query ✓，PanVK（Mesa）报告不支持 ✗。"
                + "探测结果以驱动自报为准，我们不做任何假装。"));

        rtSwitch.setOnCheckedChangeListener((b, checked) -> {
            int v = checked ? 1 : 0;
            if (busy) {                                     /* 硬约束：测试/跑分中不改这个开关 */
                b.setChecked(rtOn != 0);
                toast("测试进行中，稍后再改光追开关");
                return;
            }
            rtOn = v;
            rtPersist(v);
            if (v == 1) rtProbeStart();
            else {
                rtVerdictTx.setText("已关闭光追开关（探测结果保留在下方）");
                rtVerdictTx.setTextColor(c(C_DIM));
                rtRunBtn.setEnabled(false);
            }
        });
        if (rtOn != 0)                                       /* ★ 开机自动探测：延后 + 包保险 ✓ */
            ui.postDelayed(() -> runSafe("开机光追探测", this::rtProbeStart), 1500);
        return rtCardBox;
    }

    private android.graphics.drawable.Drawable rtBigCardBg() {
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(x(0) + 4));                     /* ColorOS 主题下 24dp，其余 16dp */
        g.setColor(withAlpha(c(C_CARD), x(3)));
        g.setStroke(dp(2), c(C_ACCENT));                     /* 2dp 主色描边 ⇒ 一眼看出是"大选项" */
        return g;
    }

    /** 持久化：key **lit_raytracing** ✓（不喂给 nativeLitConfig，因为键表里没有它） */
    private void rtPersist(int v) {
        try { getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt("lit_raytracing", v).apply(); }
        catch (Throwable t) { android.util.Log.w("gputest", "光追开关持久化失败: " + t); }
        log("  光追开关 = " + v + "（lit_raytracing 已保存）");
    }

    /** 打开时跑 nativeRtProbe(当前驱动)，**原样**显示四行 + 结论，并据此决定三态 ✓ */
    /**
     * ★ 光追渲染循环（与画面页同构）：一帧 = nativeRtFrame() → 拷进 Bitmap → 乒乓显示。
     * 停法：① 用户点"■ 停止光追" ② 原生返回 `X …`（失败即停，**保留最后一帧**）
     *      ③ 离开【画面】/切分段/onStop/onDestroy ④ 进程结束
     * 每 30 帧 progressToken++ ⇒ 看门狗有进展就不误报 ✓；UI 线程绝不 join ✓
     */
    private void rtLoop() {
        int buf = 1;
        try {
            while (rtRunning) {
                String s;
                try { s = nativeRtFrame(); }
                catch (Throwable t) { s = "X nativeRtFrame 不可用: " + t; }
                final String line = s == null ? "（无返回）" : s.trim();
                final boolean bad = line.startsWith("X ");
                if (rtInitTx != null) {
                    final String show = line + (bad ? "\n（这一帧保留便于诊断 ✓）" : "");
                    ui.post(() -> { rtInitTx.setText(show); rtInitTx.setTextColor(c(bad ? C_BAD : C_OK)); });
                }
                if (bad) { logBlock("光追失败（保留最后一帧）", line); setStatus("光追失败：见卡片原文", C_BAD); break; }
                try {
                    String e = nativeRtBlit(rtBufs[buf]);
                    if (e != null && e.trim().startsWith("X ")) { logBlock("光追回读失败", e.trim()); break; }
                } catch (Throwable t) { log("  ✗ nativeRtBlit 不可用: " + t); break; }
                final Bitmap target = rtBufs[buf];
                buf ^= 1; rtFrames++;
                if (rtFrames % 30 == 0) progressToken++;                  /* 有进展 ⇒ 看门狗静默续期 ✓ */
                final int fr = rtFrames;
                ui.post(() -> {
                    if (rtView != null) { rtView.setImageBitmap(target); rtView.invalidate(); }
                    if (fr == 1) setStatus("光追：渲染中（硬件）", C_OK);
                });
            }
        } finally {
            final int n = rtFrames;
            rtRunning = false;
            try { nativeRtStop(); } catch (Throwable ignored) { }        /* 安全可调用 ✓ */
            rtState = 0;
            ui.post(() -> {
                if (rtRunBtn != null) { rtRunBtn.setText("开始光追渲染（硬件）"); rtRunBtn.setEnabled(true); }
                setStatus("光追已停止（共 " + n + " 帧，最后一帧保留）", C_NEUTRAL);
            });
            log("  光追循环结束：帧数 " + n + "（最后一帧保留 ✓）");
            endTask(); release();
        }
    }

    /** ② 停止：置标志（循环自己收尾）⇒ 后台 join + nativeRtStop，**不在 UI 线程 join** ✓ */
    private void stopRt(String why) {
        if (!rtRunning && rtThread == null) return;
        rtRunning = false;
        final Thread t = rtThread;
        rtThread = null;
        setStatus("光追：正在停止（" + why + "）", C_WARN);
        new Thread(() -> {
            if (t != null) { try { t.join(2500); } catch (InterruptedException ignored) { } }
            try { nativeRtStop(); } catch (Throwable ignored) { }
            ui.post(() -> {
                if (rtRunBtn != null) { rtRunBtn.setText("开始光追渲染（硬件）"); rtRunBtn.setEnabled(true); }
            });
            setStatus("光追已停止（" + why + "）· 最后一帧保留", C_NEUTRAL);
            log("  光追已停止：" + why);
            endTask(); release();
        }, "gpu-rt-stop").start();
    }

    /** ① 开始/停止光追：真调 nativeRtInit（工作线程），成功后起渲染循环。 */
    private void rtRunToggle() {
        if (busy) { toast("测试进行中，稍后再点"); return; }
        if (rtState == 1) { stopRt("手动停止"); return; }          /* ② 真停：置标志 + 后台 nativeRtStop ✓ */
        final String p = selectedPath();
        if (p == null) { toast("先在「测试 → 驱动」选一个驱动"); return; }
        if (!claim("光追渲染")) return;                             /* busy：与其它原生任务互斥 ✓ */
        rtRunBtn.setEnabled(false);
        rtRunBtn.setText("正在建 BLAS…");
        if (rtInitTx != null) {
            rtInitTx.setVisibility(View.VISIBLE);
            rtInitTx.setText("正在 nativeRtInit(" + new File(p).getName() + ", 512, 512) …");
            rtInitTx.setTextColor(c(C_WARN));
        }
        new Thread(() -> {
            String out = null;
            try { out = nativeRtInit(p, 512, 512); }
            catch (Throwable t) { out = null; log("  ⚠ nativeRtInit 不可用: " + t); }
            final String o = out;
            ui.post(() -> rtShowInit(o));
        }, "rt-init").start();
    }

    /** ② 原生返回**原样**显示：成功 ⇒ ■ 停止光追 + "BLAS 已建，渲染帧接口待接入"；失败 ⇒ 原因原样 + 按钮复位 ✓ */
    private void rtShowInit(String out) {
        if (out == null || out.trim().length() == 0) {
            if (rtInitTx != null) {
                rtInitTx.setVisibility(View.VISIBLE);
                rtInitTx.setText("光追初始化接口未接入（等原生 nativeRtInit 导出）");
                rtInitTx.setTextColor(c(C_WARN));
            }
            rtRunBtn.setText("开始光追渲染（硬件）");
            rtRunBtn.setEnabled(false);
            return;
        }
        final String t = out.trim();
        if (rtInitTx != null) rtInitTx.setVisibility(View.VISIBLE);
        boolean okInit = t.contains("光追就绪") || t.contains("BLAS 已建");
        if (okInit) {
            rtState = 1;
            if (rtInitTx != null) {
                rtInitTx.setText(t + "\nBLAS 已建 ⇒ 启动光追渲染循环 ✓（硬件光追）");
                rtInitTx.setTextColor(c(C_OK));
            }
            rtRunBtn.setText("■ 停止光追");
            rtRunBtn.setEnabled(true);
            setStatus("光追：BLAS 已建 ✓ 渲染中", C_OK);
            if (rtView != null) rtView.setVisibility(View.VISIBLE);
            for (int i = 0; i < 2; i++)                              /* 512×512 ARGB_8888，与 Init 一致 ✓ */
                if (rtBufs[i] == null) rtBufs[i] = Bitmap.createBitmap(512, 512, Bitmap.Config.ARGB_8888);
            rtFrames = 0;
            rtRunning = true;
            rtThread = new Thread(this::rtLoop, "gpu-rt");
            rtThread.start();
        } else {
            rtState = 0;
            if (rtInitTx != null) { rtInitTx.setText(t); rtInitTx.setTextColor(c(C_BAD)); }   /* 为什么不行 ⇒ 原样 ✓ */
            rtRunBtn.setText("开始光追渲染（硬件）");
            rtRunBtn.setEnabled(true);                                                       /* 复位 ⇒ 可重试 ✓ */
            setStatus("光追：初始化失败（原因见卡片）", C_BAD);
            release();                                                                       /* init 失败：把 busy 还回去 ✓ */
        }
        logBlock("光追初始化", t);
    }

    private void rtProbeStart() {
        final String p = selectedPath();
        if (p == null) {
            rtVerdictTx.setText("驱动不可用，无法探测光追");
            rtVerdictTx.setTextColor(c(C_BAD));
            rtProbeTx.setVisibility(View.GONE);
            rtRunBtn.setEnabled(false);
            return;
        }
        if (rtProbing) return;
        rtProbing = true;
        rtVerdictTx.setText("（正在探测：" + new File(p).getName() + " …）");
        rtVerdictTx.setTextColor(c(C_WARN));
        new Thread(() -> {
            String probe = null;
            try { probe = nativeRtProbe(p); }
            catch (Throwable t) { probe = null; log("  ⚠ nativeRtProbe 不可用: " + t); }
            final String pr = probe;
            ui.post(() -> runSafe("光追探测显示", () -> rtShowProbe(pr)));
            rtProbing = false;
        }, "rt-probe").start();
    }

    private void rtShowProbe(String probe) {
        if (rtProbeTx == null) return;
        if (probe == null || probe.trim().length() == 0) {
            rtVerdictTx.setText("光追探测接口未接入（等原生）—— 开关状态已保存");
            rtVerdictTx.setTextColor(c(C_WARN));
            rtProbeTx.setVisibility(View.GONE);
            rtRunBtn.setEnabled(false);
            return;
        }
        rtProbeTx.setText(probe.trim());
        rtProbeTx.setVisibility(View.VISIBLE);
        String low = probe.toLowerCase(Locale.ROOT);
        boolean supported = probe.contains("✅") || probe.contains("硬件光追可用")
                || (probe.contains("支持硬件光追") && !probe.contains("不支持硬件光追"));
        if (supported) {                                     /* ① 支持 ⇒ 可用 + 可点开始 */
            rtVerdictTx.setText("✅ 硬件光追可用 —— 可以真跑");
            rtVerdictTx.setTextColor(c(C_OK));
            rtRunBtn.setEnabled(true);
            rtRunBtn.setText("开始光追渲染（硬件）");
        } else if (probe.contains("不支持") || low.contains("无 ✗") || low.contains("= 无")) {
            rtVerdictTx.setText("⚠ 该驱动不支持硬件光追 ⇒ 将使用「CPU 光追参考图」"
                    + "（结果会明确标注**非硬件光追**）");
            rtVerdictTx.setTextColor(c(C_WARN));
            rtRunBtn.setEnabled(false);
            rtRunBtn.setText("开始光追渲染（硬件·当前驱动不支持）");
        } else {
            rtVerdictTx.setText("⚠ 探测结果无法判定 ⇒ 按「不支持硬件光追」处理（不假装）");
            rtVerdictTx.setTextColor(c(C_WARN));
            rtRunBtn.setEnabled(false);
            rtRunBtn.setText("开始光追渲染（硬件·当前驱动不支持）");
        }
        logBlock("光追探测", probe);
    }

    private int rtSet(String key, int v) {
        if ("rtSamples".equals(key)) rtSamples = v;
        else if ("rtBounce".equals(key)) rtBounce = v;
        else if ("rtDetail".equals(key)) rtDetail = v;
        else if ("rtBloom".equals(key)) rtBloom = v;
        else if ("rtLightMove".equals(key)) rtLightMove = v;
        else if ("rtMirror".equals(key)) rtMirror = v;
        try { nativeRtSettings(rtSamples, rtBounce, rtDetail, rtBloom, rtLightMove, rtMirror); } catch (Throwable t4) { }
        return v;
    }

    private View litSwitch(final String key, String title, String why, int cur) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(text(title, 14, C_TEXT, true),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        android.widget.Switch sw = new android.widget.Switch(this);
        sw.setChecked(cur != 0);
        row.addView(sw);
        box.addView(row);
        box.addView(note(why));
        sw.setOnCheckedChangeListener((b, checked) -> {
            int v = checked ? 1 : 0;
            if ("shadows".equals(key)) litShadows = v;
            else if ("bloom".equals(key)) litBloom = v;
            else if ("tonemap".equals(key)) litTonemap = v;
            else if ("animate".equals(key)) litAnimate = v;
            litApply(key, v);
        });
        fluid(row);
        return box;
    }

    private View litPills(final String key, String title, final int[] vals, String[] labels, int cur, String why) {
        /* v9.98 提示：光追模块复用本构件；选中值通过 rtSet(key, v) 回写字段 ✓ */
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        TextView t = text(title, 14, C_TEXT, true);
        t.setPadding(0, dp(10), 0, dp(2));
        box.addView(t);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        TextView[] pills = new TextView[vals.length];
        int sel = 0;
        for (int i = 0; i < vals.length; i++) if (vals[i] == cur) sel = i;
        final LitRow lr = new LitRow(key, vals, labels, pills, sel);
        for (int i = 0; i < labels.length; i++) {
            final int idx = i;
            TextView pl = pill(labels[i], C_DIM, i == sel);
            pl.setOnClickListener(v -> {
                lr.sel = idx;
                styleLitRow(lr);
                litSetVal(lr.key, lr.vals[idx]);
                litApply(lr.key, lr.vals[idx]);
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, 0, dp(8), 0);
            pl.setLayoutParams(lp);
            pills[i] = pl;
            row.addView(pl);
        }
        litRows.add(lr);
        box.addView(row);
        box.addView(note(why));
        return box;
    }

    private void styleLitRow(LitRow r) {
        for (int i = 0; i < r.pills.length; i++) {
            boolean on = (i == r.sel);
            stylePill(r.pills[i], on);
            int key = on ? C_TAB_ON_TX : C_DIM;
            r.pills[i].setTextColor(c(key));
            textKeys.put(r.pills[i], key);
        }
    }

    private void litSetVal(String key, int v) {
        if (key != null && key.startsWith("rt")) { rtSet(key, v); return; }   /* v9.99: 光追配置走自己的通路 ✓ 不再是摆设 ✗ */
        if ("pcf".equals(key)) litPcf = v;
        else if ("shadowRes".equals(key)) litRes = v;
        else if ("cubes".equals(key)) litCubes = v;
        else if ("debugView".equals(key)) litDebug = v;
    }

    private void litApply(String key, int val) {
        if (key != null && key.startsWith("rt")) { rtSet(key, val); return; }   /* v9.99: 光追配置走自己的通路 ✓ 不再是摆设 ✗ */
        try { getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt("lit_" + key, val).apply(); }
        catch (Throwable t) { android.util.Log.w("gputest", "光影持久化失败: " + t); }
        String sum = null;
        try { sum = nativeLitConfig(key, val); } catch (Throwable t) { sum = null; }
        if (litSummary != null)
            litSummary.setText(sum != null && sum.length() > 0 ? sum
                    : "（原生光影接口未接入 ⇒ 仅本地记录：" + key + "=" + val + "，接入后自动生效）");
        log("  光影设置 " + key + " = " + val + (sum != null ? "  → " + sum : "（原生未接入）"));
        try { nativeMini3DSettings(litShadows, litPcf, litRes, litBloom, litTonemap, litAnimate, litPasses); } catch (Throwable t2) { }
        try { nativeRtSettings(rtSamples, rtBounce, rtDetail, rtBloom, rtLightMove, rtMirror); } catch (Throwable t3) { }
    }

    private void litRefreshSummary() {
        try {
            String sum = nativeLitConfig("shadows", litShadows);
            if (litSummary != null && sum != null && sum.length() > 0) litSummary.setText(sum);
        } catch (Throwable ignored) { }
    }

    private void litLoadPrefs() {
        try {
            android.content.SharedPreferences sp = getSharedPreferences(PREFS, MODE_PRIVATE);
            litShadows = sp.getInt("lit_shadows", litShadows);
            litPcf     = sp.getInt("lit_pcf", litPcf);
            litRes     = sp.getInt("lit_shadowRes", litRes);
            litCubes   = sp.getInt("lit_cubes", litCubes);
            litBloom   = sp.getInt("lit_bloom", litBloom);
            litTonemap = sp.getInt("lit_tonemap", litTonemap);
            litPasses  = sp.getInt("lit_passes", litPasses);
            litDebug   = sp.getInt("lit_debugView", litDebug);
            litAnimate = sp.getInt("lit_animate", litAnimate);
            rtOn       = sp.getInt("lit_raytracing", rtOn);        /* 光追开关 ✓ */
        } catch (Throwable t) { android.util.Log.w("gputest", "读取光影设置失败: " + t); }
    }

    private void litInitFromNative() {
        try {
            String csv = nativeLitSettings();
            if (csv == null) return;
            String[] q = csv.trim().split("\\s*,\\s*");
            if (q.length < 9) return;
            android.content.SharedPreferences sp = getSharedPreferences(PREFS, MODE_PRIVATE);
            if (!sp.contains("lit_shadows"))   litShadows = Integer.parseInt(q[0].trim());
            if (!sp.contains("lit_pcf"))       litPcf     = Integer.parseInt(q[1].trim());
            if (!sp.contains("lit_shadowRes")) litRes     = Integer.parseInt(q[2].trim());
            if (!sp.contains("lit_cubes"))     litCubes   = Integer.parseInt(q[3].trim());
            if (!sp.contains("lit_bloom"))     litBloom   = Integer.parseInt(q[4].trim());
            if (!sp.contains("lit_tonemap"))   litTonemap = Integer.parseInt(q[5].trim());
            if (!sp.contains("lit_passes"))    litPasses  = Integer.parseInt(q[6].trim());
            if (!sp.contains("lit_debugView")) litDebug   = Integer.parseInt(q[7].trim());
            if (!sp.contains("lit_animate"))   litAnimate = Integer.parseInt(q[8].trim());
        } catch (Throwable t) { /* 未接入 ⇒ 用默认值 ✓ */ }
    }

    private void litResetDefaults() {
        litShadows = 1; litPcf = 3; litRes = 2048; litCubes = 36;
        litBloom = 1; litTonemap = 1; litPasses = 1; litDebug = 0; litAnimate = 1;
        litApply("shadows", 1);    litApply("pcf", 3);       litApply("shadowRes", 2048);
        litApply("cubes", 36);     litApply("bloom", 1);     litApply("tonemap", 1);
        litApply("passes", 1);     litApply("debugView", 0); litApply("animate", 1);
        if (litPassBar != null) litPassBar.setProgress(0);
        if (litPassTx != null) litPassTx.setText("passes = 1");
        toast("光影设置已重置为默认");
    }

    private void toggleLit() { if (litRunning) stopLit("手动停止"); else startLit(); }

    private void startLit() {
        final String p = selectedPath();
        if (p == null) { toast("先在「测试 → 驱动」选一个驱动"); return; }
        if (litRunning) return;
        if (!claim("光影渲染")) return;
        litRunning = true; updateFab();
        setStatus("光影：初始化中…", C_WARN);
        beginTask("光影预览");
        new Thread(() -> {
            try {
                String staged = stageDriver(p);
                if (staged == null) { setStatus("光影：暂存驱动失败", C_BAD); }
                else {
                    try {
                        String initOut = nativeLitInit(staged, litSize, litSize);
                        logBlock("litInit", initOut);
                        litShowNative(initOut, initOut != null && initOut.contains("光影就绪"));
                    } catch (Throwable t) {
                        log("  ⚠ nativeLitInit 不可用: " + t);
                        litShowNative("X nativeLitInit 不可用: " + t, false);
                        setStatus("光影：原生接口未接入", C_WARN);
                        throw t;
                    }
                    for (int i = 0; i < 2; i++)
                        if (litBufs[i] == null) litBufs[i] = Bitmap.createBitmap(litSize, litSize, Bitmap.Config.ARGB_8888);
                    ui.post(() -> { if (litView != null) litView.setImageBitmap(litBufs[0]); });
                    setStatus("光影：运行中（可实时改开关）", C_OK);
                    litThread = new Thread(this::litLoop, "gpu-lit");
                    litThread.start();
                    return;                              /* 循环线程接管 ⇒ 此处不 release */
                }
            } catch (Throwable t) {
                log("  ✗ 光影启动失败: " + t);
            }
            litRunning = false;
            endTask(); release();
            ui.post(this::updateFab);
        }, "gpu-lit-init").start();
    }

    /** 把原生返回行**原样**贴到面板（成功绿 / 失败红）✓ */

    /* v9.18：UI 贴图合并 —— 渲染线程每帧都 post(setImageBitmap+setText) 会把主线程刷爆，
     * 点击事件排在几千个贴图任务之后 ⇒ 所有「停止」按钮看起来都失灵 ✗
     * 现在同一时刻只允许一个贴图任务排队，多余的帧直接丢弃（画面照样连续）✓ */
    private volatile long uiLastMs = 0;                          /* v9.19：上次真正贴图的时间 */
    private void postFrame(Runnable r) {
        long now = System.currentTimeMillis();
        if (now - uiLastMs < 70) return;                         /* v9.19：UI 更新限到 ~14fps —— 只合并不够，
                                                                  * 主线程仍被 ~60fps 的 512² 贴图占满 ⇒ 触摸事件挤不进去，
                                                                  * 停止按钮因此完全没反应 ✗ 现在把主线程腾出来 ✓ */
        uiLastMs = now;
        if (!uiPending.compareAndSet(false, true)) return;
        ui.post(() -> { try {
            if (!busy) for (Button b : actionButtons) if (b != null && !b.isEnabled()) b.setEnabled(true);   /* v9.21：自愈 ——
                * 万一某条失败路径漏了 release()，按钮不会被永久禁用（这正是你遇到的整页点不动）✓ */
            r.run();
        } finally { uiPending.set(false); } });
    }

    /* ==========================================================================
     * v9.11 · 迷你 3D（独立最简路径）—— 自转立方体
     * 与光影/光追完全独立：自己的上下文、图像、管线、命令池、回读缓冲。
     * 用来判定「GPU 路径本身能不能出画」：它出图 ⇒ 复杂管线的问题；它也不出 ⇒ 回显层的问题。
     * ========================================================================== */
    private void toggleMini3D() {
        if (m3dRunning) { log("  ▶ 收到「停止迷你3D」点击（v9.19 探针）"); m3dRunning = false; setStatus("迷你3D：停止中…", C_WARN); if (m3dBtn != null) m3dBtn.setText("▶ 迷你 3D（独立最简路径）"); return; }
        final String p = selectedPath();
        if (p == null) { toast("先在「测试 → 驱动」选一个驱动"); return; }
        if (!claim("迷你3D")) return;
        m3dRunning = true;
        if (m3dBtn != null) m3dBtn.setText("■ 停止迷你 3D");   /* v9.14：给用户一个明确的停止入口 ✓ */
        setStatus("迷你3D：初始化中…", C_WARN);
        new Thread(() -> {
            try {
                String staged = stageDriver(p);
                if (staged == null) { setStatus("迷你3D：暂存驱动失败", C_BAD); }
                else {
                    int r = nativeMini3DInit(staged, litSize, litSize);
                    log("  mini3D init -> " + r);
                    if (r == 0) { litShowNative("X 迷你3D 初始化失败（看日志）", false); setStatus("迷你3D：初始化失败（看日志）", C_BAD); }
                    else {
                        setStatus("迷你3D：运行中（独立路径 · 自转立方体）", C_OK);   /* v9.13 修：原先成功后不更新状态 ⇒ UI 永远停在初始化中 ✗ */
                        for (int i = 0; i < 2; i++) if (litBufs[i] == null) litBufs[i] = Bitmap.createBitmap(litSize, litSize, Bitmap.Config.ARGB_8888);
                        /* v9.17 修正：自检图必须**可变** —— Bitmap.createBitmap(int[],…) 返回的是不可变位图，
                         * 会让 AndroidBitmap_lockPixels 失败 ⇒ 双缓冲里那一半永远停在自检渐变色 ⇒ 画面一闪一闪 ✗
                         * 自检的使命（证明显示层可用）已经完成，这里直接去掉，改成两块干净的**可变**位图 ✓ */
                        for (int i2 = 0; i2 < 2; i2++) {
                            litBufs[i2] = Bitmap.createBitmap(litSize, litSize, Bitmap.Config.ARGB_8888);
                            litBufs[i2].eraseColor(0xFF101820);
                        }
                        mini3dLoop();
                    }
                }
            } catch (Throwable t) { log("  ✗ 迷你3D 异常: " + t); }
            m3dRunning = false;
            endTask(); release();
            ui.post(this::updateFab);
        }, "mini3d-init").start();
    }

    /* ==========================================================================
     * v9.22 · 硬件光追接入 3D 测试
     *   复用同一套已验证的显示路径（nativeXxxFrame → Blit 进 Bitmap → litView 乒乓显示），
     *   只把渲染换成硬件 ray query（BLAS/TLAS + compute）。探测/初始化/每帧返回**原样进日志** ✓
     * ========================================================================== */
    private void toggleRt3D() {
        if (rt3dRunning) { log("  ▶ 收到「停止光追3D」点击"); rt3dRunning = false; setStatus("光追3D：停止中…", C_WARN);
                           if (rt3dBtn != null) rt3dBtn.setText("▶ 光追 3D（硬件 RT · ray query）"); return; }
        final String p = selectedPath();
        if (p == null) { toast("先在「测试 → 驱动」选一个驱动"); return; }
        if (!claim("光追3D")) return;
        rt3dRunning = true;
        if (rt3dBtn != null) rt3dBtn.setText("■ 停止光追 3D");
        setStatus("光追3D：探测扩展…", C_WARN);
        new Thread(() -> {
            try {
                String probe;
                try { probe = nativeRtProbe(p); } catch (Throwable t) { probe = "X nativeRtProbe: " + t; }
                logBlock("光追扩展探测", probe);                     /* 原样 ⇒ 支不支持、缺哪个扩展，日志里直接看 ✓ */
                String init;
                try { init = nativeRtInit(p, litSize, litSize); } catch (Throwable t) { init = "X nativeRtInit: " + t; }
                logBlock("光追3D 初始化", init);
                litShowNative(init, init != null && !init.trim().startsWith("X "));
                if (init == null || init.trim().startsWith("X ")) { rt3dRunning = false; }
                else {
                    for (int i = 0; i < 2; i++) { litBufs[i] = Bitmap.createBitmap(litSize, litSize, Bitmap.Config.ARGB_8888); litBufs[i].eraseColor(0xFF101820); }
                    rt3dLoop();
                }
            } catch (Throwable t) { log("  ✗ 光追3D 异常: " + t); }
            rt3dRunning = false;
            endTask(); release();
            ui.post(() -> { if (rt3dBtn != null) rt3dBtn.setText("▶ 光追 3D（硬件 RT · ray query）"); updateFab(); });
        }, "rt3d-init").start();
    }

    private void rt3dLoop() {
        int buf = 0, frames = 0;
        try {
            while (rt3dRunning) {
                final long t0 = System.currentTimeMillis();
                String s;
                try { s = nativeRtFrame(); } catch (Throwable t) { s = "X nativeRtFrame 不可用: " + t; }
                final String line = s == null ? "（无返回）" : s.trim();
                if (line.startsWith("X ")) { litShowNative(line, false); logBlock("光追3D失败", line); break; }
                String e;
                try { e = nativeRtBlit(litBufs[buf]); } catch (Throwable t) { e = "X nativeRtBlit 不可用: " + t; }
                final Bitmap target = litBufs[buf];
                buf ^= 1; frames++;
                if (frames == 1 || frames % 30 == 0) log("  光追3D 帧 " + frames + " | 帧行: " + line + " | 回读: " + e);
                litShowNative(line + " ｜ " + e, !(e != null && e.trim().startsWith("X ")));
                final int fr = frames; final String st = (e == null ? line : e);
                postFrame(() -> { if (litView != null) { litView.setImageBitmap(target); litView.invalidate(); } updateLitStats(st, fr); });
                if (fpsCap) { long used = System.currentTimeMillis() - t0; if (used < 33) { try { Thread.sleep(33 - used); } catch (Throwable ignored) { } } }   /* v9.53 设置卡里的帧率上限 */
                if (e != null && e.trim().startsWith("X ")) break;
                /* v9.25: unlimited frame rate (user request); UI updates still throttled in postFrame */
            }
        } finally {
            try { nativeRtStop(); } catch (Throwable ignored) { }
            setStatus("光追3D已停止（最后一帧保留）", C_NEUTRAL);
            log("  光追3D 结束：帧数 " + frames);
        }
    }

    private void mini3dLoop() {
        int buf = 0, frames = 0;
        try {
            while (m3dRunning) {
                final long t0 = System.currentTimeMillis();      /* v9.20：限帧计时 */
                String stat;
                try { stat = nativeMini3DFrame(litBufs[buf]); }
                catch (Throwable t) { litShowNative("X nativeMini3DFrame 不可用: " + t, false); log("  ✗ nativeMini3DFrame: " + t); break; }
                boolean bad = (stat != null && (stat.startsWith("X ") || stat.contains("DEVICE_LOST")));
                litShowNative(stat, !bad);
                if (bad) { logBlock("迷你3D失败", stat); break; }
                final Bitmap target = litBufs[buf];
                buf ^= 1; frames++;
                if (frames == 1 || frames % 300 == 0) log("  迷你3D 帧 " + frames + " · " + stat);   /* v9.13：像素统计进日志 ⇒ 离线可复核 ✓ */
                final int fr = frames; final String st = stat;
                postFrame(() -> { if (litView != null) { litView.setImageBitmap(target); litView.invalidate(); } updateLitStats(st, fr); });
                if (fpsCap) { long used = System.currentTimeMillis() - t0; if (used < 33) { try { Thread.sleep(33 - used); } catch (Throwable ignored) { } } }   /* v9.53 设置卡里的帧率上限 */
                /* v9.25: unlimited frame rate (user request); UI updates still throttled in postFrame */   /* v9.20：~30fps 上限 ——
                    * 不限帧会让 GPU 队列永远满，Android 合成器（同一个 GPU）被饿死 ⇒ 触摸事件送不进 App ⇒ 所有按钮点不动 ✗ */
            }
        } finally {
            try { nativeMini3DStop(); } catch (Throwable ignored) { }
            final int n = frames;
            ui.post(() -> { if (litStats != null && n == 0) litStats.setText("（无帧：初始化失败）"); });
            setStatus("迷你3D已停止（最后一帧保留）", C_NEUTRAL);
            log("  迷你3D 结束：帧数 " + n);
            if (m3dBtn != null) ui.post(() -> m3dBtn.setText("▶ 迷你 3D（独立最简路径）"));
        }
    }

    private void litShowNative(final String line, final boolean ok) {
        if (litNativeTx == null) return;
        final String t = line == null ? "（无返回）" : line.trim();
        ui.post(() -> runSafe("光影返回行显示", () -> {
            litNativeTx.setVisibility(View.VISIBLE);
            litNativeTx.setText(ok ? t : t + "\n（这一帧保留便于诊断 ✓）");
            litNativeTx.setTextColor(c(ok ? C_OK : C_BAD));
        }));
    }

    private void litLoop() {
        int buf = 1, frames = 0;
        try {
            while (litRunning) {
                String stat;
                try { stat = nativeLitFrame(); }
                catch (Throwable t) { litShowNative("X nativeLitFrame 不可用: " + t, false); log("  ✗ nativeLitFrame: " + t); break; }
                boolean bad = (stat != null && (stat.contains("DEVICE_LOST") || stat.startsWith("X ")));
                litShowNative(stat, !bad);
                if (bad) {
                    logBlock("光影失败", stat);
                    setStatus("光影失败（已保留最后一帧）", C_BAD);
                    break;
                }
                try { nativeLitBlit(litBufs[buf]); }
                catch (Throwable t) { log("  ✗ nativeLitBlit: " + t); break; }
                final Bitmap target = litBufs[buf];
                buf ^= 1; frames++;
                progressToken++;
                final String st = stat; final int fr = frames;
                postFrame(() -> {
                    if (litView != null) { litView.setImageBitmap(target); litView.invalidate(); }
                    updateLitStats(st, fr);
                });
            }
        } finally {
            litRunning = false;
            try { nativeLitStop(); } catch (Throwable ignored) { }
            final int n = frames;
            ui.post(() -> {
                updateFab();
                if (litStats != null && n == 0) litStats.setText("（无帧：原生接口未接入或已失败）");
            });
            setStatus("光影已停止（最后一帧保留）", C_NEUTRAL);
            log("  光影结束：帧数 " + n);
            endTask(); release();
        }
    }

    private void updateLitStats(String stat, int frames) {
        if (litStats == null) return;
        try {
        Double gpu = parseD(stat, "GPU 时间"), fps = parseD(stat, "FPS"), duty = parseD(stat, "占空比");
        litStats.setText(String.format(Locale.ROOT, "GPU 时间 %.2f ms ｜ FPS %.1f ｜ 占空比 %.1f%% ｜ 帧 %d",
                gpu == null ? 0.0 : gpu, fps == null ? 0.0 : fps, duty == null ? 0.0 : duty, frames));
        } catch (Throwable t) { android.util.Log.w("gputest", "光影统计行刷新失败: " + t); }
    }

    private void stopLit(String why) {
        if (!litRunning && litThread == null) return;
        litRunning = false;
        final Thread t = litThread;
        litThread = null;
        setStatus("光影：正在停止（" + why + "）", C_WARN);
        new Thread(() -> {
            if (t != null) { try { t.join(3000); } catch (InterruptedException ignored) { } }
            try { nativeLitStop(); } catch (Throwable ignored) { }
            ui.post(this::updateFab);
            setStatus("光影已停止（" + why + "）· 最后一帧保留", C_NEUTRAL);
            log("  光影已停止：" + why);
            endTask(); release();
        }, "gpu-lit-stop").start();
    }

    private View buildComparePage() {
        LinearLayout col = col();
        col.addView(card(section("A/B 对比"),
                note("先「设为基准 A」，再选另一个驱动点「开始对比」——两段汇总数字可直接并列。"),
                btnPrimary("设为对比基准 A", v -> setBaseline()),
                btnTonal("开始 A/B 对比", v -> compareAB())));
        return scroll(col);
    }

    private View buildScoreboardPage() {
        LinearLayout col = col();
        scoreEmpty = emptyState("ic_tab_bench", "▥", "还没有成绩",
                "到「测试 → 跑分」跑一次，这里会列出各档结果与口径。",
                "去跑分", () -> selectDest(DEST_TEST, SEG_TEST_BENCH, true));
        col.addView(scoreEmpty);
        scoreTable = text("", 12, C_TEXT, false);
        scoreTable.setTypeface(Typeface.MONOSPACE);
        scoreTable.setLineSpacing(0, 1.2f);
        scoreTable.setVisibility(View.GONE);
        col.addView(card(section("本次会话成绩（含口径）"),
                note("跑一次跑分后，这里会列出各档成绩与口径。"),
                scoreTable));
        return scroll(col);
    }

    private View buildReportPage() {
        LinearLayout col = col();
        col.addView(card(section("导出"),
                btnPrimary("导出 / 分享日志", v -> exportLog()),
                btnTonal("复制设备信息", v -> copyDeviceInfo()),
                note("HTML 报告：到「设置 → 导出 HTML 报告」一键生成（成绩 + 温度趋势柱状图 + 帧统计 + 光追配置快照）✓")));
        return scroll(col);
    }

    /* ---- 语义按钮：主操作(填充) / 次操作(tonal) / 破坏性(红) ---- */
    private Button btnPrimary(String s, View.OnClickListener l) {
        Button b = actionBtn(s, l);
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(12));
        g.setColor(c(C_TAB_ON_BG));
        b.setBackground(g);
        b.setTextColor(c(C_TAB_ON_TX));
        textKeys.put(b, C_TAB_ON_TX);
        bgKeys.remove(b);
        primaryBtns.add(b);
        return b;
    }

    private Button btnTonal(String s, View.OnClickListener l) { return actionBtn(s, l); }

    private Button btnDanger(String s, View.OnClickListener l) {
        Button b = actionBtn(s, l);
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(12));
        g.setColor(blend(c(C_BAD), 0x22));
        g.setStroke(dp(1), c(C_BAD));
        b.setBackground(g);
        tint(b, C_BAD);
        bgKeys.remove(b);
        dangerBtns.add(b);
        return b;
    }

    /** 空状态：图标 + 一句说明 + 一个按钮（Material 空状态模式）。 */
    private View emptyState(String icon, String glyph, String title, String sub, String btn, Runnable act) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setGravity(Gravity.CENTER);
        c.setPadding(dp(24), dp(32), dp(24), dp(32));
        View ic = iconView(icon, glyph, 48, C_DIM);
        c.addView(ic);
        TextView t = text(title, 16, C_TEXT, true);
        t.setGravity(Gravity.CENTER);
        t.setPadding(0, dp(12), 0, dp(4));
        c.addView(t);
        TextView s = text(sub, 12, C_DIM, false);
        s.setGravity(Gravity.CENTER);
        s.setPadding(0, 0, 0, dp(16));
        c.addView(s);
        if (btn != null) {
            Button b = btnTonal(btn, v -> { if (act != null) act.run(); });
            c.addView(b, new LinearLayout.LayoutParams(dp(200), ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        return card(c);
    }

    /* =========================================================================
     *  一键跑全部驱动（v6.3）：claim 一次 ⇒ 串行遍历 ⇒ 每完成一个立刻出排名行
     *  ⚠ 排序口径必须与界面上的说明一致（rankRulesTx / 日志同一句话）
     * ========================================================================= */
    private boolean isSystemPath(String p) { return "system".equals(p); }

    /** ③ 从原生输出里取"那一行错误原文"（dlopen 缺库 / DEVICE_LOST / 提交失败…），截断成一行。 */
    private String firstErrLine(String out) {
        if (out == null) return "";
        for (String ln : out.split("\n")) {
            String t = ln.trim();
            if (t.startsWith("X ") || t.contains("dlopen failed") || t.contains("not found")
                    || t.contains("DEVICE_LOST") || t.contains("提交失败") || t.contains("调用异常")) {
                return t.length() > 110 ? t.substring(0, 110) + "…" : t;
            }
        }
        return "";
    }

    private void runAllDriversSweep() {
        if (sweepRunning) { toast("已在跑全部驱动"); return; }
        if (!claim("一键跑全部驱动")) return;
        final java.util.List<String> paths = new ArrayList<>(driverPaths);
        if (paths.isEmpty()) { release(); log("  ✗ 驱动列表为空 —— 先到「测试→驱动」扫描或提取"); toast("先扫描驱动"); return; }
        sweepRunning = true;
        sweepNums.clear(); sweepNames.clear(); sweepOk.clear(); sweepNote.clear();
        log("\n=== ⚑ 一键跑全部驱动（共 " + paths.size() + " 个 · fill/blit/draw 各 5 秒 · 串行）===");
        log("  排序口径：综合分 = 0.4×fill/最优fill + 0.3×blit/最优blit + 0.3×draw/最优draw（相对最优归一化，满分 100）；"
            + "任一项失败 ⇒ 不计入排名，单列失败表");
        ui.post(() -> {
            rankTableTx.setText("（正在跑… 每完成一个就更新一行）");
            rankFailTx.setText("（跑完汇总）");
        });
        beginTask("一键跑全部驱动");
        new Thread(() -> {
            try {
                for (int i = 0; i < paths.size(); i++) {
                    final String path = paths.get(i);
                    final String nm = sweepLabel(path);                    /* ④ 用插件显示名，不再全是同名 .so */
                    final int idx = i + 1, total = paths.size();
                    log("\n  ────── [" + idx + "/" + total + "] " + nm + " ──────");
                    setStatus("全部驱动 " + idx + "/" + total + "：" + nm, C_WARN);
                    String staged = stageDriver(path);
                    if (staged == null) { recordSweep(nm, false, 0, 0, 0, 0, "暂存驱动失败"); renderRank(); continue; }
                    double fill = 0, blit = 0, draw = 0, duty = 0;
                    boolean ok = true; String note0 = "";
                    for (String mode : new String[] { "fill", "blit", "draw" }) {
                        invalidateBench(nm + " · " + mode);
                        lastFill = lastBlit = lastDraw = lastDuty = -1;      /* 每个模式前重置 */
                        String out;
                        try { out = nativeBench(staged, 5, mode); }
                        catch (Throwable t) { out = "X 原生调用异常: " + t; }
                        handleNativeOutput("全部驱动[" + idx + "/" + total + "] · " + mode, out);
                        parseScores(out); parseTiming(out);                  /* ① 复用已验证的单跑解析器（幂等） */
                        progressToken++;
                        double v = "fill".equals(mode) ? lastFill : ("blit".equals(mode) ? lastBlit : lastDraw);
                        if (lastDuty >= 0) duty = lastDuty;
                        else if (lastGpuMs >= 0 && lastWallS > 0) duty = lastGpuMs / 1000.0 / lastWallS * 100.0;
                        boolean bad = (v <= 0) || verdictOf(out) == C_BAD;
                        if (bad) {
                            ok = false;
                            if (note0.length() == 0) {
                                String err = firstErrLine(out);
                                note0 = mode + " 失败" + (err.length() > 0 ? "：" + err : "");
                            }
                        }
                        if ("fill".equals(mode)) fill = v <= 0 ? 0 : v;
                        if ("blit".equals(mode)) blit = v <= 0 ? 0 : v;
                        if ("draw".equals(mode)) draw = v <= 0 ? 0 : v;
                        log("      " + mode + " = " + (v <= 0 ? "失败/无数据" : String.format(Locale.ROOT, "%.2f", v))
                            + (lastDuty >= 0 ? String.format(Locale.ROOT, "  （GPU 占空比 %.1f%%）", lastDuty) : ""));
                    }
                    recordSweep(nm, ok, fill, blit, draw, duty, note0);
                    renderRank();                                   /* 每完成一个 ⇒ 立刻重排（实时） */
                }
                final String table = rankTableText();
                log("\n=== ⚑ 排名表（口径：0.4/0.3/0.3 相对最优归一化，满分 100；失败不计入）===\n" + table);
                if (rankFailText().length() > 4) log("\n--- 失败项（不计入排名）---\n" + rankFailText());
                setStatus("全部驱动跑完（" + paths.size() + " 个）—— 排名见「跑分」页", C_OK);
            } catch (Throwable t) {
                log("  ✗ 一键跑全部驱动异常: " + t);
                setStatus("全部驱动异常", C_BAD);
            } finally {
                sweepRunning = false;
                endTask(); release();
            }
        }, "gpu-sweep").start();
    }

    /** ④ 把来源折成短尾段：插件 com.x.y.zenith ⇒ zenith；目录 Download ⇒ Download。 */
    private String shortTag(String source) {
        if (source == null) return "?";
        String t = source;
        int sp = t.indexOf(' ');
        if (sp >= 0 && sp + 1 < t.length()) t = t.substring(sp + 1);
        int dot = t.lastIndexOf('.');
        if (dot >= 0 && dot + 1 < t.length()) t = t.substring(dot + 1);
        int sl = t.lastIndexOf('/');
        if (sl >= 0 && sl + 1 < t.length()) t = t.substring(sl + 1);
        return t.length() > 18 ? t.substring(0, 18) : t;
    }

    private String sweepLabel(String path) {
        if (isSystemPath(path)) return "system（系统驱动·对照）";          /* ⑤ 保留 */
        String lab = driverLabels.get(path);
        if (lab == null || lab.length() == 0) lab = new File(path).getName();
        return lab;
    }

    /**
     * ③ 磨砂底衬：把当前页画进 1/4 缩放的 Bitmap 当快照，API 31+ 用平台
     * `RenderEffect.createBlurEffect` 真模糊**这一层**（页面与文字层完全不受影响 ✓）。
     * 只在切页时刷新一次（不逐帧），且测试进行中不刷新（不干扰观感判断 ✓）。
     */
    private void refreshBackdrop(int dest) {
        if (blurBackdrop == null || destPages[dest] == null) return;
        if (busy) { blurBackdrop.setVisibility(View.GONE); return; }        /* 硬约束：测试中不动效 */
        try {
            View page = destPages[dest];
            int w = page.getWidth(), h = page.getHeight();
            if (w <= 0 || h <= 0) { blurBackdrop.setVisibility(View.GONE); return; }
            Bitmap bmp = Bitmap.createBitmap(Math.max(1, w / 4), Math.max(1, h / 4), Bitmap.Config.ARGB_8888);
            android.graphics.Canvas cv = new android.graphics.Canvas(bmp);
            cv.scale(0.25f, 0.25f);
            page.draw(cv);
            blurBackdrop.setImageBitmap(bmp);
            if (Build.VERSION.SDK_INT >= 31) {
                blurBackdrop.setRenderEffect(android.graphics.RenderEffect.createBlurEffect(
                        24f, 24f, android.graphics.Shader.TileMode.CLAMP));
            }
            /* v9.8：修「整页糊」bug —— 快照层 MATCH_PARENT 且 addView 在页面之后（盖在页面上），
             *   一旦 VISIBLE 就把整页变成「放大回去的 1/4 快照 + 24px 模糊」，切页后最明显。
             *   这里让它始终隐藏 ✓（将来要磨砂观感：改回 VISIBLE 并按底栏 64dp 裁剪快照）*/
            blurBackdrop.setVisibility(View.GONE);
        } catch (Throwable t) {
            android.util.Log.w("gputest", "磨砂底衬失败（回退到半透色）: " + t);
            if (blurBackdrop != null) blurBackdrop.setVisibility(View.GONE);
        }
    }

    private void runAllRenderersStub() {
        log("  ⏸ 「一键跑全部渲染器」：**转译链尚未接入（等原生 v6.4）** —— 本按钮先占位，不做任何原生调用。");
        toast("转译链尚未接入（等原生 v6.4）");
    }

    private void recordSweep(String name, boolean ok, double fill, double blit, double draw, double duty, String note0) {
        sweepNames.add(name); sweepOk.add(ok); sweepNote.add(note0 == null ? "" : note0);
        sweepNums.add(new double[] { fill, blit, draw, duty });
    }

    /** 归一化 + 排序 + 出表（口径与界面/日志一致）。 */
    private double[] sweepScore() {
        double mf = 0, mb = 0, md = 0;
        for (int i = 0; i < sweepNums.size(); i++) {
            if (!sweepOk.get(i)) continue;
            double[] v = sweepNums.get(i);
            mf = Math.max(mf, v[0]); mb = Math.max(mb, v[1]); md = Math.max(md, v[2]);
        }
        double[] sc = new double[sweepNums.size()];
        for (int i = 0; i < sweepNums.size(); i++) {
            if (!sweepOk.get(i)) { sc[i] = -1; continue; }
            double[] v = sweepNums.get(i);
            sc[i] = 100.0 * (0.4 * (mf > 0 ? v[0] / mf : 0)
                           + 0.3 * (mb > 0 ? v[1] / mb : 0)
                           + 0.3 * (md > 0 ? v[2] / md : 0));
        }
        return sc;
    }

    private String rankTableText() {
        double[] sc = sweepScore();
        Integer[] order = new Integer[sweepNums.size()];
        for (int i = 0; i < order.length; i++) order[i] = i;
        java.util.Arrays.sort(order, (a, b) -> Double.compare(sc[b], sc[a]));
        StringBuilder b = new StringBuilder();
        b.append(String.format(Locale.ROOT, "%-3s %-28s %7s %10s %8s %10s %7s  %s%n",
                "#", "驱动", "综合分", "fill", "blit", "draw", "占空比", "备注"));
        int rank = 0;
        for (Integer i : order) {
            if (!sweepOk.get(i)) continue;                       /* 失败项不进排名 */
            rank++;
            double[] v = sweepNums.get(i);
            b.append(String.format(Locale.ROOT, "%-3d %-28s %7.1f %10.2f %8.2f %10.0f %6.1f%%  %s%n",
                    rank, trim(sweepNames.get(i), 28), sc[i], v[0], v[1], v[2], v[3],
                    i == 0 && isSystemPathName(sweepNames.get(i)) ? "系统驱动(对照)" : ""));
        }
        if (rank == 0) b.append("（没有可计入排名的驱动 —— 全部失败）\n");
        return b.toString();
    }

    private String rankFailText() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < sweepNums.size(); i++)
            if (!sweepOk.get(i))
                b.append(String.format(Locale.ROOT, "%-34s %s%n", trim(sweepNames.get(i), 34),
                        sweepNote.get(i).length() == 0 ? "失败（DEVICE_LOST/无数据）" : sweepNote.get(i)));
        return b.length() == 0 ? "（无失败项）" : b.toString();
    }

    private boolean isSystemPathName(String n) { return n != null && n.contains("系统驱动"); }

    private String trim(String s, int n) { return s == null ? "" : (s.length() <= n ? s : s.substring(0, n - 1) + "…"); }

    private void renderRank() {
        final String t = rankTableText(), f = rankFailText();
        ui.post(() -> {
            if (rankTableTx != null) rankTableTx.setText(t);
            if (rankFailTx != null) rankFailTx.setText(f);
        });
    }

    private void copyRankTable() {
        try {
            String txt = rankTableText() + "\n--- 失败项（不计入排名）---\n" + rankFailText();
            android.content.ClipboardManager cm = (android.content.ClipboardManager)
                    getSystemService(android.content.Context.CLIPBOARD_SERVICE);
            cm.setPrimaryClip(android.content.ClipData.newPlainText("gputest rank", txt));
            log("\n=== 排名表（已复制到剪贴板）===\n" + txt);
            toast("排名表已复制");
        } catch (Throwable t) { log("  ✗ 复制排名表失败: " + t); }
    }

    private void copyDeviceInfo() {
        try {
            String info = stressDeviceLines();
            android.content.ClipboardManager cm = (android.content.ClipboardManager)
                    getSystemService(android.content.Context.CLIPBOARD_SERVICE);
            cm.setPrimaryClip(android.content.ClipData.newPlainText("gputest device", info));
            toast("设备信息已复制");
        } catch (Throwable t) { log("  ✗ 复制失败: " + t); }
    }

    /** 主题切换后重刷 App 骨架（App Bar / 底栏 / FAB / 分段 / 图标）。 */
    private void applyChromeColors() {
        if (appBar != null) appBar.setBackgroundColor(withAlpha(c(C_CARD), x(3)));
        if (bottomNav != null) bottomNav.setBackgroundColor(withAlpha(c(C_CARD), x(3)));
        for (java.util.Map.Entry<View, Integer> e : iconKeys.entrySet()) {
            View v = e.getKey(); Integer k = e.getValue();
            if (v instanceof ImageView && k != null) ((ImageView) v).setColorFilter(c(k));
        }
        if (busyChip != null) stylePill(busyChip, false);
        if (driverChip != null) stylePill(driverChip, false);
        styleBottomNav();
        for (LitRow lr : litRows) styleLitRow(lr);          /* 光影面板 pill 也跟主题走 ✓ */
        if (rtCardBox != null) rtCardBox.setBackground(rtBigCardBg());   /* 光追大卡描边/圆角跟主题 ✓ */
        styleSegRow(0, segTest);
        styleSegRow(1, segRender);
        styleSegRow(2, segRec);
        for (Button b : primaryBtns) {                      /* 主操作按钮：填充主色 */
            GradientDrawable g = new GradientDrawable();
            g.setCornerRadius(dp(12));
            g.setColor(c(C_TAB_ON_BG));
            b.setBackground(g);
            b.setTextColor(c(C_TAB_ON_TX));
        }
        for (Button b : dangerBtns) {                       /* 破坏性按钮：红字红边 */
            GradientDrawable g = new GradientDrawable();
            g.setCornerRadius(dp(12));
            g.setColor(blend(c(C_BAD), 0x22));
            g.setStroke(dp(1), c(C_BAD));
            b.setBackground(g);
            b.setTextColor(c(C_BAD));
        }
        updateFab();
    }

    /** 状态栏/导航栏跟随主题（浅色主题用深色图标）。 */
    private void applyWindowChrome() {
        try {
            getWindow().setStatusBarColor(c(C_CARD));
            getWindow().setNavigationBarColor(c(C_BG));
            View dv = getWindow().getDecorView();
            int flags = dv.getSystemUiVisibility();
            if (themeIdx == 1) flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;   /* 浅色主题：深色状态栏图标 */
            else               flags &= ~View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
            dv.setSystemUiVisibility(flags);
        } catch (Throwable t) { android.util.Log.w("gputest", "窗口配色失败: " + t); }
    }

    /* ---------------------------- 主题机制 ---------------------------- */
    /** 给 TextView 上色并**登记键**（切换主题时按同一个键重刷 ⇒ 不会留旧色）。 */
    /** ④ 数字变化：150ms 透明度交叉淡入（不做滚动计数）。 */
    private void setNumFade(TextView v, String s) {
        if (v == null) return;
        v.animate().cancel();
        v.setAlpha(0.35f);
        v.setText(s);
        v.animate().alpha(1f).setDuration(150).start();
    }

    private void tint(TextView v, int key) {
        if (v == null) return;
        textKeys.put(v, key);
        v.setTextColor(c(key));
    }

    private String themeLabel() { return "🎨 主题：" + THEMES[themeIdx].name + "（点按循环切换）"; }

    private void cycleTheme() { applyTheme(themeIdx + 1, true); }

    /** 切换主题：换调色板 → 重刷所有登记过的视图 → 记住选择。**不动任何布局结构** ✓ */
    private void applyTheme(int idx, boolean persist) {
        themeIdx = ((idx % THEMES.length) + THEMES.length) % THEMES.length;
        CUR = THEMES[themeIdx].p.clone();
        CURX = THEMES[themeIdx].x.clone();
        if (rootV != null) rootV.setBackgroundColor(c(C_BG));
        for (java.util.Map.Entry<View, Integer> e : textKeys.entrySet()) {
            View v = e.getKey(); Integer k = e.getValue();
            if (v instanceof TextView && k != null) ((TextView) v).setTextColor(c(k));
        }
        for (java.util.Map.Entry<View, Integer> e : bgKeys.entrySet()) {
            View v = e.getKey(); Integer k = e.getValue();
            if (v == null || k == null) continue;
            int fillKey = k / 100, radius = k % 100;
            if (fillKey == C_CARD) radius = x(0);              /* 卡片圆角跟随主题（ColorOS 20dp ✓） */
            if (fillKey == C_BTN_BG) radius = x(1);            /* 按钮圆角跟随主题（16dp ✓） */
            if (radius == 0) v.setBackgroundColor(c(fillKey));
            else if (fillKey == C_CARD || fillKey == C_BTN_BG) v.setBackground(themedBg(fillKey, radius));
            else v.setBackground(cardBg(c(fillKey), c(C_STROKE), radius));
        }
        for (java.util.Map.Entry<Button, Boolean> e : tabStates.entrySet())
            if (e.getKey() != null) styleTab(e.getKey(), Boolean.TRUE.equals(e.getValue()));
        if (themeBtn != null) themeBtn.setText(themeLabel());
        applyChromeColors();                                 /* v6.1：App Bar/底栏/FAB/分段 跟着主题走 */
        applyWindowChrome();                                 /* 状态栏/导航栏跟随主题 */
        if (persist) {
            try {
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                        .putString(PREF_THEME, THEMES[themeIdx].id).apply();
            } catch (Throwable t) { android.util.Log.w("gputest", "主题持久化失败: " + t); }
        }
        log("  主题已切换为「" + THEMES[themeIdx].name + "」"
            + (persist ? "（已记住，重启保持）" : ""));
    }

    /** 启动时读回上次选的主题（SharedPreferences("gputest") / key = "theme" / 值为 id）。 */
    private void loadThemePref() {
        try {
            String id = getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_THEME, THEMES[0].id);
            for (int i = 0; i < THEMES.length; i++)
                if (THEMES[i].id.equals(id)) { themeIdx = i; break; }
        } catch (Throwable t) { android.util.Log.w("gputest", "读取主题失败: " + t); }
        CUR = THEMES[themeIdx].p.clone();
    }

    /** ② 柔性触控反馈：按下 0.96 + 下移 1dp（120ms），抬起/取消回弹（220ms Overshoot）。
     *  只挂在**可点控件**上；结果卡不挂（避免干扰读数的观感判断）✓ */
    private void fluid(final View v) {
        if (v == null) return;
        v.setOnTouchListener((view, ev) -> {
            try {
                switch (ev.getActionMasked()) {
                    case android.view.MotionEvent.ACTION_DOWN:
                        view.animate().scaleX(0.96f).scaleY(0.96f).translationY(dp(1))
                            .setDuration(120)
                            .setInterpolator(new android.view.animation.PathInterpolator(0.0f, 0.0f, 0.2f, 1.0f))
                            .start();
                        break;
                    case android.view.MotionEvent.ACTION_UP:
                    case android.view.MotionEvent.ACTION_CANCEL:
                        view.animate().scaleX(1f).scaleY(1f).translationY(0f)
                            .setDuration(220)
                            .setInterpolator(new android.view.animation.OvershootInterpolator(1.2f))
                            .start();
                        break;
                    default: break;
                }
            } catch (Throwable ignored) { }
            return false;                                       /* 返回 false ⇒ 点击事件照常派发 ✓ */
        });
    }

    /** ★ 启动保险（硬规矩）：建页失败只降级这一页 + 写日志，绝不让整个 App 进不去 ✓ */
    private View safePage(String name, java.util.function.Supplier<View> maker) {
        try { return maker.get(); }
        catch (Throwable t) {
            log("  ✗ 建页失败[" + name + "]（已降级，其它页可用）: " + t);
            LinearLayout box = new LinearLayout(this);
            box.setOrientation(LinearLayout.VERTICAL);
            box.setPadding(dp(16), dp(16), dp(16), dp(16));
            box.addView(text("「" + name + "」页构建失败（已降级）", 16, C_BAD, true));
            box.addView(note("原因：" + t + "\n（启动保险：不影响其它页面与功能）"));
            return scroll(box);
        }
    }

    /** ★ 启动保险：把"启动期自动做的事"跑在 try/catch 里并写日志 ✓ */
    private void runSafe(String tag, Runnable r) {
        try { r.run(); }
        catch (Throwable t) { log("  ✗ [" + tag + "] 启动期自动动作失败（已忽略）: " + t); }
    }

    private int dp(int v) { return (int) (v * getResources().getDisplayMetrics().density + 0.5f); }

    private void toast(String s) { ui.post(() -> Toast.makeText(this, s, Toast.LENGTH_SHORT).show()); }

/* v2.6：日志同时落到共享目录，便于外部读取/发我诊断（有「所有文件访问」时写 Download，
     * 否则退回 App 外部目录）。首次写入会清空上一轮。 */
    private static volatile boolean sharedTruncated = false;
    private String appVersion() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (Throwable t) { return "?"; }
    }

    private void appendShared(String line) {
        try {
            java.io.File dir = new java.io.File("/sdcard/Download");
            if (!dir.isDirectory() || !dir.canWrite()) dir = getExternalFilesDir(null);
            if (dir == null) dir = getFilesDir();                  /* D12：回退到应用私有目录 */
            if (dir == null) return;
            java.io.File f = new java.io.File(dir, "gputest-last.log");
            java.io.FileWriter w = new java.io.FileWriter(f, sharedTruncated);
            if (!sharedTruncated) sharedTruncated = true;
            w.write(line); w.write("\n"); w.close();
        } catch (Throwable t) {
            /* 绝不能在这里调 log() —— log() 会回调 appendShared，会无限递归 ✗ */
            android.util.Log.w("gputest", "appendShared 失败: " + t);
        }
    }
}
