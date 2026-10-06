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
    public native String nativeStressInit(String soPath, int w, int h);
    public native String nativeStressFrame(int triCount);
    public native String nativeStressBlit(Bitmap bmp);
    public native void   nativeStressStop();

    /* ---------------- 配色 ---------------- */
    /* ============================ 主题系统 ============================
     * 下面这些 C_* **不是颜色值，而是调色板键**（1..17）—— 取色一律 c(key)。
     * 好处：① 全文不再有任何硬编码颜色；② 切换主题 = 换一张调色板 + 重刷已注册视图。
     * 键的顺序必须与 Theme.p[] 一致（key-1 即下标）。 */
    private static final int C_BG = 1, C_CARD = 2, C_STROKE = 3, C_TEXT = 4, C_DIM = 5,
                             C_ACCENT = 6, C_OK = 7, C_BAD = 8, C_WARN = 9, C_NEUTRAL = 10,
                             C_TAB_ON_BG = 11, C_TAB_OFF_BG = 12, C_TAB_ON_TX = 13,
                             C_BTN_BG = 14, C_LOG_BG = 15, C_LOG_TX = 16, C_RENDER_BG = 17;

    private static final class Theme {
        final String id, name; final int[] p;
        Theme(String id, String name, int[] p) { this.id = id; this.name = name; this.p = p; }
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
    };

    private static final String PREFS = "gputest", PREF_THEME = "theme";   /* 持久化：SharedPreferences("gputest").getString("theme", "dark") */
    private static int themeIdx = 0;
    private static int[] CUR = THEMES[0].p.clone();

    private static int c(int key) {
        int i = key - 1;
        return (i >= 0 && i < CUR.length) ? CUR[i] : 0xFF888888;
    }

    /* ---------------- 页签 ---------------- */
    private static final String[] TAB_NAMES = { "概览", "驱动", "测试", "画面", "跑分", "日志", "压力" };
    private static final int TAB_OVERVIEW = 0, TAB_DRIVER = 1, TAB_TEST = 2,
                             TAB_PICTURE = 3, TAB_BENCH = 4, TAB_LOG = 5, TAB_STRESS = 6;
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
        ui.post(() -> { for (Button b : actionButtons) if (b != null) b.setEnabled(false); });
        return true;
    }

    private void release() {
        synchronized (this) { busy = false; busyWhat = null; }
        ui.post(() -> { for (Button b : actionButtons) if (b != null) b.setEnabled(true); });
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
        int p = dp(12);
        rootV.setPadding(p, p, p, dp(6));

        rootV.addView(text("GPU 驱动测试台 v2.0", 19, C_TEXT, true));
        rootV.addView(text("OPPO PHZ110 · 天玑 9300 · Immortalis-G720 MC12 · 无 root", 11, C_DIM, false));

        headerDriver = text("当前驱动：未选择", 13, C_ACCENT, true);
        headerDriver.setPadding(0, dp(6), 0, 0);
        rootV.addView(headerDriver);

        headerStatus = text("就绪。先到「驱动」页扫描或提取一个 .so。", 12, C_NEUTRAL, false);
        headerStatus.setSingleLine(true);
        headerStatus.setEllipsize(TextUtils.TruncateAt.END);
        rootV.addView(headerStatus);

        /* --- 手写页签栏 --- */
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setPadding(0, dp(8), 0, dp(6));
        for (int i = 0; i < TAB_NAMES.length; i++) {
            final int idx = i;
            Button t = new Button(this);
            t.setText(TAB_NAMES[i]);
            /* v5.4：页签图标（按名字匹配，不依赖页签顺序 ✓）*/
            int ic = tabIconRes(TAB_NAMES[i]);
            if (ic != 0) {
                t.setCompoundDrawablesWithIntrinsicBounds(0, ic, 0, 0);
                t.setCompoundDrawablePadding(dp(2));
            }
            t.setAllCaps(false);
            t.setTextSize(12);
            t.setPadding(0, dp(2), 0, dp(2));
            t.setMinWidth(0); t.setMinimumWidth(0);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            lp.setMargins(dp(2), 0, dp(2), 0);
            t.setLayoutParams(lp);
            t.setOnClickListener(v -> switchTab(idx));
            tabBtns[i] = t;
            bar.addView(t);
        }
        rootV.addView(bar);

        content = new FrameLayout(this);
        rootV.addView(content, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        tabPages[TAB_OVERVIEW] = buildOverviewPage();
        tabPages[TAB_DRIVER]   = buildDriverPage();
        tabPages[TAB_TEST]     = buildTestPage();
        tabPages[TAB_PICTURE]  = buildPicturePage();
        tabPages[TAB_BENCH]    = buildBenchPage();
        tabPages[TAB_LOG]      = buildLogPage();
        tabPages[TAB_STRESS]   = buildStressPage();
        for (View v : tabPages) {
            v.setVisibility(View.GONE);
            content.addView(v, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }

        setContentView(rootV);
        switchTab(TAB_OVERVIEW);
        log("GPU 驱动测试台 v" + appVersion() + " 启动。");
        refreshDeviceCards();
        scanDrivers();
    }

    @Override protected void onStop() {
        /* B7：退到后台就停渲染（放 onStop 不放 onPause —— 文件选择器/授权页只触发 onPause） */
        if (renderRunning || renderThread != null) stopRender("退到后台");
        if (stressRunning || stressThread != null) stopStress("退到后台");
        super.onStop();
    }

    @Override protected void onDestroy() {
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
    private void switchTab(int idx) {
        if (idx == curTab) return;
        /* 离开「画面」页 -> 按规格停掉渲染，避免后台空转 */
        if (curTab == TAB_PICTURE && (renderRunning || renderThread != null)) stopRender("切走页面");
        if (curTab == TAB_STRESS && (stressRunning || stressThread != null)) stopStress("切走页面");
        curTab = idx;
        for (int i = 0; i < tabPages.length; i++) {
            tabPages[i].setVisibility(i == idx ? View.VISIBLE : View.GONE);
            styleTab(tabBtns[i], i == idx);
        }
    }

    /* v5.4：页签图标映射（按名字，顺序无关；用 getIdentifier 而不依赖 R —— 免 Gradle 构建未生成 R.java）*/
    private int tabIconRes(String n) {
        String nm = null;
        if (n == null) return 0;
        if (n.contains("概览")) nm = "ic_tab_overview";
        else if (n.contains("驱动")) nm = "ic_tab_driver";
        else if (n.contains("测试")) nm = "ic_tab_test";
        else if (n.contains("画面")) nm = "ic_tab_render";
        else if (n.contains("压力")) nm = "ic_tab_stress";
        else if (n.contains("跑分")) nm = "ic_tab_bench";
        else if (n.contains("日志")) nm = "ic_tab_log";
        if (nm == null) return 0;
        return getResources().getIdentifier(nm, "drawable", getPackageName());
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
        col.addView(card(section("快捷操作"),
                themeBtn,
                actionBtn("① 扫描驱动与渲染器（分类：ICD / 渲染器）", v -> { switchTab(TAB_DRIVER); scanDrivers(); }),
                actionBtn("⑩ 一键全流程自测（冒烟→三角形→三种跑分）", v -> runAll()),
                actionBtn("② 从 APK 提取驱动 .so", v -> { switchTab(TAB_DRIVER); pickApkForExtract(); }),
                actionBtn("⑦ 拉起 Minecraft 启动器", v -> launchChooser()),
                actionBtn("⑪ 读取启动器日志（FCL / ZL2）", v -> { switchTab(TAB_LOG); readLauncherLogs(); })));
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
        g.append("测试方式  : 直接 dlopen 驱动 .so + vk_icdGetInstanceProcAddr\n");
        g.append("            （不经过系统 Vulkan loader）");
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
                actionBtn("① 扫描驱动与渲染器（后台分类，不卡界面）", v -> scanDrivers()),
                actionBtn("② 从 APK 提取驱动 .so", v -> pickApkForExtract()),
                actionBtn("③ 安装驱动插件 APK（会话式安装，不用 file://）", v -> pickApkForInstall()),
                actionBtn("⑪b 授权「所有文件访问」（读启动器日志/更多目录）", v -> requestAllFiles()),
                actionBtn("⑬ 系统驱动对照（同一套测试跑系统驱动）", v -> runSystemBaseline())));

        col.addView(section("① Vulkan 驱动（选一个用于测试）"));
        driverGroup = new RadioGroup(this);
        driverGroup.setOrientation(RadioGroup.VERTICAL);
        driverGroup.setOnCheckedChangeListener((g, id) -> updateSelectedHeader());
        col.addView(card(driverGroup,
                note("这一组是「渲染器底下的驱动」——启动器里 Vulkan 驱动器下拉的那几个。")));

        col.addView(section("② 渲染器（跑在 Vulkan 驱动之上）"));
        rendererGroup = new RadioGroup(this);
        rendererGroup.setOrientation(RadioGroup.VERTICAL);
        rendererGroup.setOnCheckedChangeListener((g, id) -> updateSelectedHeader());
        col.addView(card(rendererGroup,
                note("本版只把分类与命名做对（选中会记下来，头部常驻显示）。"
                   + "「渲染器 × 驱动」的组合测试是下一步：用 Vulkan 垫片把渲染器的 "
                   + "dlopen(\"libvulkan.so\") 转发到上面选中的驱动 ICD。")));

        col.addView(section("③ 打不开 / 未知（仅列出，不参与测试）"));
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
            sys.setText("🖥  使用系统驱动（对照 · 系统 Vulkan loader）");
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
        headerDriver.setText("驱动: " + (d == null ? "未选择" : new File(d).getName())
                + "  ｜  渲染器: " + (r == null ? "未选择" : new File(r).getName()));
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
                actionBtn("④ ICD 冒烟（dlopen → 协商 → 实例 → 设备 → 队列）", v -> runSmoke()),
                actionBtn("⑤ 三角形绘制 + 像素校验（应出现纯红三角形）", v -> runTri()),
                actionBtn("⑭ 能力清单（设备 / 限制 / 扩展 / 内存堆）", v -> runCaps())));

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
                actionBtn("⑥ 填充率 fill", v -> runBench("fill")),
                actionBtn("⑥b 拷贝带宽 blit", v -> runBench("blit")),
                actionBtn("⑥c 三角形吞吐 draw", v -> runBench("draw"))));

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

        col.addView(card(section("全流程与对比"),
                actionBtn("⑩ 一键全流程自测（冒烟→三角形→能力清单→三种跑分）", v -> runAll()),
                actionBtn("⑫ 把当前驱动设为对比基准 A", v -> setBaseline()),
                actionBtn("⑫b A/B 对比：基准 A vs 当前 B", v -> compareAB()),
                note("A/B 会依次跑 冒烟 + 三角形 + 三种跑分，最后两段汇总数字可直接对比。")));
        return scroll(col);
    }

    /* =========================================================================
     *  ⑥ 日志
     * ========================================================================= */
    private View buildLogPage() {
        LinearLayout col = col();
        col.addView(card(section("日志操作"),
                actionBtn("⑪ 读取启动器日志（FCL / ZalithLauncher / ZL2）", v -> readLauncherLogs()),
                actionBtn("⑪b 授权「所有文件访问」", v -> requestAllFiles()),
                actionBtn("⑧ 导出 / 分享日志", v -> exportLog()),
                actionBtn("⑨ 清空日志", v -> {
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

    private void runSmoke() { runNative("④ ICD 冒烟", 0, null); }
    private void runTri()   { runNative("⑤ 三角形绘制 + 像素校验", 1, null); }
    private void runCaps()  { runNative("⑭ 能力清单", 3, null); }
    private void runBench(String mode) { runNative("⑥ 跑分 · " + mode, 2, mode); }

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

        final String fill  = grab(out, "填充率\\s*:\\s*([0-9.]+)");
        final String blit  = grab(out, "拷贝带宽\\s*:\\s*([0-9.]+)");
        final String tri   = grab(out, "三角形吞吐\\s*:\\s*([0-9.]+)");
        final String score = grab(out, "综合分:\\s*([0-9.]+)");
        if (fill == null && blit == null && tri == null && score == null) return;   /* 这批输出里没有成绩 */
        parseTiming(out);                                                           /* (c)：时间三件套 */
        final boolean anyFail = out.contains("失败统计")
                && !Pattern.compile("失败统计\\s*:\\s*fill=0 blit=0 draw=0", Pattern.DOTALL).matcher(out).find();
        ui.post(() -> {
            if (fill != null && benchFill != null) { benchFill.setText(fill); tint(benchFill, anyFail ? C_WARN : C_OK); }
            if (blit != null && benchBlit != null) { benchBlit.setText(blit); tint(benchBlit, anyFail ? C_WARN : C_OK); }
            if (tri != null && benchDraw != null)  { benchDraw.setText(tri);  tint(benchDraw, anyFail ? C_WARN : C_OK); }
            if (score != null) {
                if (benchScore != null) { benchScore.setText(score); tint(benchScore, anyFail ? C_WARN : C_OK); }
                if (ovScore != null)    { ovScore.setText(score);    tint(ovScore, anyFail ? C_WARN : C_OK); }
            }
            if (ovSummary != null) {
                ovSummary.setText("填充 " + nz(fill) + " Mpixel/s · 带宽 " + nz(blit) + " GB/s · 三角形 "
                        + nz(tri) + " 个/s" + (score != null ? "  ⇒  综合分 " + score : ""));
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
        c.setBackground(cardBg(c(C_CARD), c(C_STROKE), 14));
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
        b.setBackground(cardBg(c(C_BTN_BG), c(C_STROKE), 12));
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
                    stressRgb[ch] = p; refreshRgbText();
                }
                @Override public void onStartTrackingTouch(SeekBar b) { }
                @Override public void onStopTrackingTouch(SeekBar b) { }
            });
            rgbCol.addView(text(names[i], 12, C_DIM, false));
            rgbCol.addView(sb);
        }
        col.addView(card(section("清屏色（与参考图默认一致：R8 G8 B25）"), stressRgbTx, rgbCol,
                note("原生侧需要接一个 setter 才能生效 —— 建议 `nativeStressClearColor(int r,int g,int b)`；"
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
            if (stressProg != null) stressProg.setProgress(Math.min(stressCap, stressTri));
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

    /* ---------------------------- 主题机制 ---------------------------- */
    /** 给 TextView 上色并**登记键**（切换主题时按同一个键重刷 ⇒ 不会留旧色）。 */
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
        if (rootV != null) rootV.setBackgroundColor(c(C_BG));
        for (java.util.Map.Entry<View, Integer> e : textKeys.entrySet()) {
            View v = e.getKey(); Integer k = e.getValue();
            if (v instanceof TextView && k != null) ((TextView) v).setTextColor(c(k));
        }
        for (java.util.Map.Entry<View, Integer> e : bgKeys.entrySet()) {
            View v = e.getKey(); Integer k = e.getValue();
            if (v == null || k == null) continue;
            int fillKey = k / 100, radius = k % 100;
            if (radius == 0) v.setBackgroundColor(c(fillKey));
            else v.setBackground(cardBg(c(fillKey), c(C_STROKE), radius));
        }
        for (java.util.Map.Entry<Button, Boolean> e : tabStates.entrySet())
            if (e.getKey() != null) styleTab(e.getKey(), Boolean.TRUE.equals(e.getValue()));
        if (themeBtn != null) themeBtn.setText(themeLabel());
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
