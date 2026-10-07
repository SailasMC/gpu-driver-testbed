// GPU 驱动测试台 · 原生模块 v2
//   nativeIcdSmoke(so)              —— ICD 冒烟：dlopen → 协商 → 建实例 → 枚举设备 → 建设备/队列
//   nativeTri(so)                   —— 真绘制：建渲染目标 + 图形管线，画三角形，回读像素并校验（应为纯红）
//   nativeBench(so, seconds, mode)  —— 跑分：mode = fill | blit | draw
// 不依赖系统 Vulkan loader：直接调 ICD 入口。
#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "shaders.h"
#include "shaders_lit.h"
#include "shaders_rt.h"   /* 光追 ray_query 计算着色器，已编进 .so ✓ */   /* v7.0：光影四段着色器，已编进 .so ✓ */
#include "shaders_lit2.h"   /* v7.8：动态 PCF + 线性输出 + 后处理开关 */
#include "shaders_mini3d.h"   /* v9.11：迷你 3D 自转立方体的 SPIR-V ✓ */
#include <android/bitmap.h>
#include <elf.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <ucontext.h>   /* v9.6: ucontext_t ⇒ 崩溃现场的 PC/SP/LR（原先被丢弃 ✗）✓ */
#include <unwind.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>   /* v2.0: 直接往 Java Bitmap 里写像素（画面页） */

#define LOG(...) do { int _n = snprintf(g_log + g_len, sizeof(g_log) - g_len, __VA_ARGS__); \
                      if (_n > 0) g_len += (_n < (int)sizeof(g_log) - g_len ? _n : (int)sizeof(g_log) - g_len - 1); } while (0)

static char g_log[128 * 1024];
static char g_step[192];   /* v2.1：当前正卡在哪一步（看门狗读它）*/
#define STEP(...) do { snprintf(g_step, sizeof(g_step), __VA_ARGS__); } while (0)
static int  g_len;

typedef VkResult (*PFN_neg)(uint32_t *);
typedef PFN_vkVoidFunction (*PFN_gipa)(VkInstance, const char *);
typedef PFN_vkVoidFunction (*PFN_gdpa)(VkDevice, const char *);

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }

// ---------------------------------------------------------------- 上下文
typedef struct {
    void *h; PFN_gipa gipa;
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfam;
    VkPhysicalDeviceProperties props;
    PFN_vkGetDeviceProcAddr gdpa;
} Ctx;

static PFN_gipa icd_open(const char *path, void **handle) {
    /* v0.5: 特殊路径 "system" ⇒ 用系统 Vulkan loader 作对照组。
     * 系统 loader 的 vkGetInstanceProcAddr 是标准入口，后续流程完全一致，
     * 于是同一套测试可以直接在系统驱动上跑一遍，用于判断"失败怪驱动还是怪设备"。 */
    if (path && strcmp(path, "system") == 0) {
        LOG("[1] 系统驱动（系统 Vulkan loader 作为对照组）\n");
        /* v2.7：优先用**绝对路径**的系统 loader —— 将来 APK 里放了 libvulkan.so 垫片，
         * 相对名会命中垫片而不是系统驱动 ✗ */
        void *h = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("/system/lib/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) { LOG("    X 打不开系统 Vulkan loader: %s\n", dlerror()); return NULL; }
        PFN_gipa gipa = (PFN_gipa) dlsym(h, "vkGetInstanceProcAddr");
        LOG("[2] 系统 loader vkGetInstanceProcAddr=%s\n", gipa ? "有" : "无");
        if (!gipa) { LOG("    X 系统 loader 无入口\n"); dlclose(h); return NULL; }
        *handle = h;
        return gipa;
    }
    LOG("[1] dlopen(%s)\n", path);
    /* v6.5：有些驱动插件链接了**系统私有库**（如 libcutils.so）—— 应用命名空间默认不给 ✗
     * ⇒ 先把它们按绝对路径以 RTLD_GLOBAL 载入，插件随后就能解析到（best-effort，失败不致命）*/
    static const char *PRE[] = { "/system/lib64/libcutils.so", "/system/lib64/liblog.so",
                                 "/system/lib64/libbase.so", "/system/lib64/libutils.so",
                                 "/system/lib64/libnativewindow.so", "/system/lib64/libsync.so", NULL };
    for (int i = 0; PRE[i]; i++) {
        void *p = dlopen(PRE[i], RTLD_NOW | RTLD_GLOBAL);
        if (p) LOG("  （预载 %s 成功）\n", PRE[i]);
    }
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOG("    X %s\n", dlerror()); return NULL; }
    PFN_neg neg = (PFN_neg) dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_gipa gipa = (PFN_gipa) dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) gipa = (PFN_gipa) dlsym(h, "vk_icdGetInstanceProcAddrLunar");
    LOG("[2] negotiate=%s gipa=%s\n", neg ? "有" : "无", gipa ? "有" : "无");
    if (!gipa) { LOG("    X 不是可加载 ICD\n"); dlclose(h); return NULL; }
    if (neg) { uint32_t v = 7; LOG("    协商版本 -> %u (VkResult %d)\n", v, neg(&v)); }
    *handle = h; return gipa;
}

/* v3.0：统一取 Vulkan 入口。
 * 关键教训：**实例级函数必须用 gipa(instance, …) 取** ——
 * 系统 loader 对 gipa(NULL, "vkEnumeratePhysicalDevices") 等返回 NULL ✗，
 * 这就是日志里"X 关键入口缺失：vkEnumeratePhysicalDevices …"的根因。 */
static PFN_vkVoidFunction c_gipa(Ctx *c, const char *name) {
    PFN_vkVoidFunction f = NULL;
    if (c->inst) f = c->gipa(c->inst, name);
    if (!f) f = c->gipa(NULL, name);
    return f;
}

static int ctx_open(Ctx *c, const char *path) {
    memset(c, 0, sizeof(*c));
    c->gipa = icd_open(path, &c->h);
    if (!c->gipa) return 0;
    /* 只有**全局**函数能在实例之前取（loaders 对实例级函数此时返回 NULL ✗） */
    PFN_vkCreateInstance create = (PFN_vkCreateInstance) c_gipa(c, "vkCreateInstance");
    /* v2.9：**不要**把 vkGetDeviceProcAddr 放进前置门槛 ✗
     * 它是设备级函数，系统 loader 在实例建立之前不会提供
     * ⇒ 这正是"使用系统驱动"从来跑不通的原因（日志里那句"X 关键入口缺失"）。
     * 改为建好实例后再取（并逐个报出到底缺了谁，便于以后一眼定位）。*/
    if (!create) { LOG("X 缺少 vkCreateInstance（连全局入口都没有）\n"); return 0; }

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "GPUTest", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    STEP("vkCreateInstance（建 Vulkan 实例）");
    LOG("[3] 即将调用 vkCreateInstance …（若日志停在这里 ⇒ 驱动卡在建实例）\n");
    VkResult r = create(&ici, NULL, &c->inst);
    STEP("(已返回) vkCreateInstance");
    LOG("[3] vkCreateInstance -> %d %s\n", r, r == VK_SUCCESS ? "OK" : "失败");
    if (r != VK_SUCCESS) return 0;

    /* v3.0：实例已建 ⇒ 现在按规范取实例级与设备级入口（**这是之前全失败的原因**） */
    PFN_vkEnumeratePhysicalDevices enumPd = (PFN_vkEnumeratePhysicalDevices) c_gipa(c, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties getProps = (PFN_vkGetPhysicalDeviceProperties) c_gipa(c, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQf = (PFN_vkGetPhysicalDeviceQueueFamilyProperties) c_gipa(c, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice createDev = (PFN_vkCreateDevice) c_gipa(c, "vkCreateDevice");
    c->gdpa = (PFN_vkGetDeviceProcAddr) c_gipa(c, "vkGetDeviceProcAddr");
    if (!enumPd || !getProps || !getQf || !createDev || !c->gdpa) {
        LOG("X 实例已建，但仍缺失入口：%s%s%s%s%s\n",
            enumPd ? "" : "vkEnumeratePhysicalDevices ",
            getProps ? "" : "vkGetPhysicalDeviceProperties ",
            getQf ? "" : "vkGetPhysicalDeviceQueueFamilyProperties ",
            createDev ? "" : "vkCreateDevice ",
            c->gdpa ? "" : "vkGetDeviceProcAddr ");
        return 0;
    }
    LOG("[3b] 实例级入口已取齐（用 gipa(instance, …) ✓）\n");

    uint32_t n = 0; enumPd(c->inst, &n, NULL);
    LOG("[4] 物理设备数: %u\n", n);
    if (!n) return 0;
    VkPhysicalDevice pds[8]; if (n > 8) n = 8;
    enumPd(c->inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; getProps(pds[i], &p);
        LOG("    设备%u: %s | 类型=%d | api=%u.%u.%u | 驱动=0x%08x (%u.%u.%u) | vendor=0x%04x device=0x%04x\n",
            i, p.deviceName, p.deviceType,
            VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
            p.driverVersion, VK_VERSION_MAJOR(p.driverVersion), VK_VERSION_MINOR(p.driverVersion), VK_VERSION_PATCH(p.driverVersion),
            p.vendorID, p.deviceID);
    }
    c->pd = pds[0]; getProps(c->pd, &c->props);

    uint32_t nq = 0; getQf(c->pd, &nq, NULL);
    VkQueueFamilyProperties qfs[16]; if (nq > 16) nq = 16;
    getQf(c->pd, &nq, qfs);
    c->qfam = 0; int found = 0;
    for (uint32_t i = 0; i < nq; i++)
        if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfs[i].queueCount > 0) { c->qfam = i; found = 1; break; }
    LOG("[5] 队列族 %u 个；选用 #%u (图形=%s)\n", nq, c->qfam, found ? "是" : "否");
    if (!found) {
        LOG("X 没有任何支持图形的队列族（queueCount>0）⇒ 该驱动无法做图形测试，后续结论不可信\n");
        return 0;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = c->qfam, .queueCount = 1, .pQueuePriorities = &prio };
    /* v9.23: enable device extensions/features on demand (fixes RT missing build entry). */
    PFN_vkEnumerateDeviceExtensionProperties enumDevExt =
        (PFN_vkEnumerateDeviceExtensionProperties) c_gipa(c, "vkEnumerateDeviceExtensionProperties");
    static const char *wantExt[] = { "VK_KHR_acceleration_structure", "VK_KHR_ray_query",
                                     "VK_KHR_deferred_host_operations", "VK_KHR_buffer_device_address",
                                     "VK_KHR_spirv_1_4", "VK_KHR_shader_float_controls" };
    static const char *useExt[8];
    uint32_t useExtN = 0;
    const uint32_t wantN = (uint32_t) (sizeof(wantExt) / sizeof(wantExt[0]));
    int hasAS = 0, hasRQ = 0, hasDH = 0, hasBDA = 0;
    if (enumDevExt) {
        uint32_t en = 0;
        if (enumDevExt(c->pd, NULL, &en, NULL) == VK_SUCCESS && en > 0 && en < 4096) {
            static VkExtensionProperties eps[4096];
            if (enumDevExt(c->pd, NULL, &en, eps) == VK_SUCCESS) {
                for (uint32_t i = 0; i < wantN; i++) {
                    int hit = 0;
                    for (uint32_t k = 0; k < en; k++)
                        if (!strcmp(eps[k].extensionName, wantExt[i])) { hit = 1; break; }
                    if (!hit) continue;
                    if (!strcmp(wantExt[i], "VK_KHR_acceleration_structure")) hasAS = 1;
                    if (!strcmp(wantExt[i], "VK_KHR_ray_query")) hasRQ = 1;
                    if (!strcmp(wantExt[i], "VK_KHR_deferred_host_operations")) hasDH = 1;
                    if (!strcmp(wantExt[i], "VK_KHR_buffer_device_address")) hasBDA = 1;
                    if (useExtN < 8) useExt[useExtN++] = wantExt[i];
                }
            }
        }
    }
    int api12 = (VK_VERSION_MAJOR(c->props.apiVersion) > 1) ||
                (VK_VERSION_MAJOR(c->props.apiVersion) == 1 && VK_VERSION_MINOR(c->props.apiVersion) >= 2);
    int useAS = hasAS && (api12 || (hasBDA && hasDH));
    int useRQ = hasRQ && useAS;
    LOG("[6a] dev ext cand: AS=%d RQ=%d DH=%d BDA=%d api12=%d -> enable %u | AS=%d RQ=%d",
        hasAS, hasRQ, hasDH, hasBDA, api12, useExtN, useAS, useRQ);

    PFN_vkGetPhysicalDeviceFeatures2 getF2 = (PFN_vkGetPhysicalDeviceFeatures2) c_gipa(c, "vkGetPhysicalDeviceFeatures2");
    if (!getF2) getF2 = (PFN_vkGetPhysicalDeviceFeatures2) c_gipa(c, "vkGetPhysicalDeviceFeatures2KHR");
    VkPhysicalDeviceAccelerationStructureFeaturesKHR q_as = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR q_rq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    if (getF2 && useAS) {
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q_as };
        q_as.pNext = useRQ ? (void *) &q_rq : NULL;
        getF2(c->pd, &f2);
    }
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rqf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    void *fNext = NULL;
    if (useAS && q_as.accelerationStructure) {
        asf.accelerationStructure = VK_TRUE; asf.pNext = NULL; fNext = &asf;
        if (useRQ && q_rq.rayQuery) { rqf.rayQuery = VK_TRUE; rqf.pNext = fNext; fNext = &rqf; }
        LOG("[6b] RT features requested: accelerationStructure=1 rayQuery=%d", (useRQ && q_rq.rayQuery) ? 1 : 0);
    } else if (useAS) {
        LOG("[6b] accelerationStructure feature NOT reported -> skip (device create as usual)");
    }

    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = fNext,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = useExtN, .ppEnabledExtensionNames = useExt };
    STEP("vkCreateDevice（建逻辑设备）");
    LOG("[6] 即将调用 vkCreateDevice …\n");
    r = createDev(c->pd, &dci, NULL, &c->dev);
    STEP("(已返回) vkCreateDevice");
    LOG("[6] vkCreateDevice -> %d %s\n", r, r == VK_SUCCESS ? "OK" : "失败");
    if (r != VK_SUCCESS) return 0;

    PFN_vkGetDeviceQueue getQ = (PFN_vkGetDeviceQueue) c->gdpa(c->dev, "vkGetDeviceQueue");
    if (!getQ) return 0;
    getQ(c->dev, c->qfam, 0, &c->q);
    LOG("[loader] %s | device=%s", (path && !strcmp(path, "system")) ? "system-loader" : (path ? path : "(null)"), c->props.deviceName);   /* v9.27: 一眼看清走系统 loader 还是直接 ICD */
    LOG("[7] 设备与队列就绪\n");

    return 1;
}
static void ctx_close(Ctx *c) {
    /* v3.2（原生泄漏修复）：先把设备排空，再**真正销毁 device 与 instance**。
     * Vulkan 的对象是树状的 —— 销毁 device 会回收它的图像/缓冲/内存/池/fence/管线，
     * 销毁 instance 会回收设备与实例级对象。之前一个都不销毁 ⇒ 每跑一次测试漏一整套 ✗ */
    if (c->dev) {
        PFN_vkDeviceWaitIdle wi = (PFN_vkDeviceWaitIdle) c->gdpa(c->dev, "vkDeviceWaitIdle");
        if (wi) wi(c->dev);
        PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice) c_gipa(c, "vkDestroyDevice");
        if (dd) { dd(c->dev, NULL); LOG("(已销毁 vkDevice)\n"); }
        c->dev = NULL;
    }
    if (c->inst) {
        PFN_vkDestroyInstance di = (PFN_vkDestroyInstance) c_gipa(c, "vkDestroyInstance");
        if (di) { di(c->inst, NULL); LOG("(已销毁 vkInstance)\n"); }
        c->inst = NULL;
    }
    /* v1.1: 不再 dlclose —— 此时 Vulkan 对象仍存活，卸载 ICD 是崩溃源；
     * 系统 loader 同样从不卸载 ICD，本进程是短命进程，保持映射即可。 */
    (void) c;
}

// ------------------------------------------------------- 渲染目标/资源
typedef struct {
    VkImage img; VkDeviceMemory mem; VkImageView view; VkRenderPass rp; VkFramebuffer fb;
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkBuffer buf; VkDeviceMemory bmem;
    uint32_t W, H;
} Target;

#define MEM_NONE 0xFFFFFFFFu   /* v2.0(B5)：把"没找到"与"类型 0"分开 */
/* v9.5：**严格**内存类型选取 —— 只接受 flags 完全满足的类型，找不到就 MEM_NONE ✓
 * 与 pick_mem 的区别：pick_mem 会"退一步"用任何类型（对图像之类可以 ✓），
 * 但对**主机可见缓冲**绝不能退化 ✗（退化 ⇒ vkMapMemory 失败 ⇒ memcpy(NULL) 崩溃 ✓ 已踩 ✓）*/
static uint32_t pick_mem_strict(Ctx *c, uint32_t bits, VkMemoryPropertyFlags want) {
    PFN_vkGetPhysicalDeviceMemoryProperties gm = (PFN_vkGetPhysicalDeviceMemoryProperties)
        c_gipa(c, "vkGetPhysicalDeviceMemoryProperties");
    if (!gm) return MEM_NONE;
    VkPhysicalDeviceMemoryProperties mp; memset(&mp, 0, sizeof(mp));
    gm(c->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount && i < 32; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return MEM_NONE;
}
static uint32_t pick_mem(Ctx *c, uint32_t bits, VkMemoryPropertyFlags want) {
    PFN_vkGetPhysicalDeviceMemoryProperties gm = (PFN_vkGetPhysicalDeviceMemoryProperties)
        c_gipa(c, "vkGetPhysicalDeviceMemoryProperties");
    if (!gm) { LOG("X 驱动不支持 vkGetPhysicalDeviceMemoryProperties，无法选择内存类型\n"); return MEM_NONE; }
    VkPhysicalDeviceMemoryProperties mp; gm(c->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) {
            LOG("! 内存类型回退：找不到带 flags=0x%x 的类型，退用类型 %u(flags=0x%x) —— 若随后回读失败，先怀疑这里\n",
                (unsigned) want, i, (unsigned) mp.memoryTypes[i].propertyFlags);
            return i;
        }
    LOG("X 没有任何内存类型匹配位掩码 0x%x\n", bits);
    return MEM_NONE;
}

static int target_make(Ctx *c, Target *t, uint32_t W, uint32_t H, int with_rp) {
    memset(t, 0, sizeof(*t)); t->W = W; t->H = H;
    PFN_vkCreateImage mk = (PFN_vkCreateImage) c->gdpa(c->dev, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements req = (PFN_vkGetImageMemoryRequirements) c->gdpa(c->dev, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory alloc = (PFN_vkAllocateMemory) c->gdpa(c->dev, "vkAllocateMemory");
    PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory) c->gdpa(c->dev, "vkBindImageMemory");
    PFN_vkCreateImageView mkview = (PFN_vkCreateImageView) c->gdpa(c->dev, "vkCreateImageView");
    if (!mk || !req || !alloc || !bind || !mkview) return 0;

    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (mk(c->dev, &ici, NULL, &t->img) != VK_SUCCESS) { LOG("X 建图像失败\n"); return 0; }
    VkMemoryRequirements mr; req(c->dev, t->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = pick_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (alloc(c->dev, &mai, NULL, &t->mem) != VK_SUCCESS) { LOG("X 分配图像内存失败\n"); return 0; }
    bind(c->dev, t->img, t->mem, 0);

    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (mkview(c->dev, &vci, NULL, &t->view) != VK_SUCCESS) { LOG("X 建视图失败\n"); return 0; }

    if (with_rp) {
        PFN_vkCreateRenderPass mkrp = (PFN_vkCreateRenderPass) c->gdpa(c->dev, "vkCreateRenderPass");
        PFN_vkCreateFramebuffer mkfb = (PFN_vkCreateFramebuffer) c->gdpa(c->dev, "vkCreateFramebuffer");
        if (!mkrp || !mkfb) return 0;
        VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1, .pColorAttachments = &ref };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
        if (mkrp(c->dev, &rci, NULL, &t->rp) != VK_SUCCESS) { LOG("X 建 render pass 失败\n"); return 0; }
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = t->rp, .attachmentCount = 1, .pAttachments = &t->view, .width = W, .height = H, .layers = 1 };
        if (mkfb(c->dev, &fci, NULL, &t->fb) != VK_SUCCESS) { LOG("X 建 framebuffer 失败\n"); return 0; }
    }

    PFN_vkCreateCommandPool mkpool = (PFN_vkCreateCommandPool) c->gdpa(c->dev, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers alloccb = (PFN_vkAllocateCommandBuffers) c->gdpa(c->dev, "vkAllocateCommandBuffers");
    PFN_vkCreateFence mkf = (PFN_vkCreateFence) c->gdpa(c->dev, "vkCreateFence");
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,  /* v1.1: 反复 begin 同一 CB 必须可重置，否则违规 */
        .queueFamilyIndex = c->qfam };
    if (!mkpool || mkpool(c->dev, &pci, NULL, &t->pool) != VK_SUCCESS) { LOG("X 建命令池失败\n"); return 0; }
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = t->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    alloccb(c->dev, &cbai, &t->cb);
    VkFenceCreateInfo fci2 = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    mkf(c->dev, &fci2, NULL, &t->fence);
    return t->cb && t->fence;
}

static VkResult submit_wait(Ctx *c, Target *t) {
    PFN_vkQueueSubmit sub = (PFN_vkQueueSubmit) c->gdpa(c->dev, "vkQueueSubmit");
    PFN_vkWaitForFences wf = (PFN_vkWaitForFences) c->gdpa(c->dev, "vkWaitForFences");
    PFN_vkResetFences rf = (PFN_vkResetFences) c->gdpa(c->dev, "vkResetFences");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &t->cb };
    VkResult r = sub(c->q, 1, &si, t->fence);
    if (r != VK_SUCCESS) return r;
    r = wf(c->dev, 1, &t->fence, VK_TRUE, 3000000000ull);
    if (r == VK_ERROR_DEVICE_LOST)
        LOG("    !! fence 等待返回 VK_ERROR_DEVICE_LOST(-4) —— 驱动/GPU 侧真的崩了\n");
    else if (r == VK_TIMEOUT)
        LOG("    !! fence 等待超时 VK_TIMEOUT(-2) —— 命令流没跑完（与 -4 是两种病）\n");
    else if (r != VK_SUCCESS)
        LOG("    !! fence 等待返回 %d\n", r);
    rf(c->dev, 1, &t->fence);
    return r;
}

// 把图像拷回主机缓冲并返回指针
static int readback(Ctx *c, Target *t, void **out) {
    PFN_vkCreateBuffer mkb = (PFN_vkCreateBuffer) c->gdpa(c->dev, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements breq = (PFN_vkGetBufferMemoryRequirements) c->gdpa(c->dev, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory alloc = (PFN_vkAllocateMemory) c->gdpa(c->dev, "vkAllocateMemory");
    PFN_vkBindBufferMemory bindb = (PFN_vkBindBufferMemory) c->gdpa(c->dev, "vkBindBufferMemory");
    PFN_vkMapMemory map = (PFN_vkMapMemory) c->gdpa(c->dev, "vkMapMemory");
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdCopyImageToBuffer cp = (PFN_vkCmdCopyImageToBuffer) c->gdpa(c->dev, "vkCmdCopyImageToBuffer");
    if (!mkb || !breq || !alloc || !bindb || !map || !begin || !end || !cp) return 0;

    VkDeviceSize sz = (VkDeviceSize) t->W * t->H * 4;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (mkb(c->dev, &bci, NULL, &t->buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; breq(c->dev, t->buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = pick_mem(c, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    if (alloc(c->dev, &mai, NULL, &t->bmem) != VK_SUCCESS) return 0;
    bindb(c->dev, t->buf, t->bmem, 0);

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin(t->cb, &bi);
    VkBufferImageCopy region = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { t->W, t->H, 1 } };
    cp(t->cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->buf, 1, &region);
    end(t->cb);
    if (submit_wait(c, t) != VK_SUCCESS) { LOG("X 回读提交失败\n"); return 0; }
    if (map(c->dev, t->bmem, 0, VK_WHOLE_SIZE, 0, out) != VK_SUCCESS) { LOG("X 映射回读缓冲失败\n"); return 0; }
    return 1;
}

static jstring finish(JNIEnv *env, const char *path, jstring jso) { (void)path; (void)jso; return (*env)->NewStringUTF(env, g_log); }

// ---------------------------------------------------------------- 冒烟
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeIcdSmoke(JNIEnv *env, jobject th, jstring jso) {
    LOG("原生入口: nativeIcdSmoke 进入\n");
    (void) th; g_len = 0; g_log[0] = 0;
    const char *path = (*env)->GetStringUTFChars(env, jso, NULL);
    LOG("=== ICD 冒烟测试 ===\n驱动: %s\n", path);
    Ctx c;
    if (ctx_open(&c, path)) LOG("\n== 判定: 驱动可用 ==\n");
    else                LOG("\n== 判定: 驱动不可用（见上） ==\n");
    ctx_close(&c);
    (*env)->ReleaseStringUTFChars(env, jso, path);
    return finish(env, path, jso);
}

// ------------------------------------------------------------ 三角形 + 校验
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeTri(JNIEnv *env, jobject th, jstring jso) {
    LOG("原生入口: nativeTri 进入\n");
    (void) th; g_len = 0; g_log[0] = 0;
    const char *path = (*env)->GetStringUTFChars(env, jso, NULL);
    LOG("=== 三角形绘制 + 像素校验 ===\n驱动: %s\n", path);
    Ctx c;
    if (!ctx_open(&c, path)) { LOG("X 驱动不可用\n"); goto out; }
    {
        Target t;
        if (!target_make(&c, &t, 256, 256, 1)) { LOG("X 渲染目标创建失败\n"); goto out2; }

        PFN_vkCreateShaderModule mksh = (PFN_vkCreateShaderModule) c.gdpa(c.dev, "vkCreateShaderModule");
        PFN_vkCreatePipelineLayout mkpl = (PFN_vkCreatePipelineLayout) c.gdpa(c.dev, "vkCreatePipelineLayout");
        PFN_vkCreateGraphicsPipelines mkgp = (PFN_vkCreateGraphicsPipelines) c.gdpa(c.dev, "vkCreateGraphicsPipelines");
        PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer) c.gdpa(c.dev, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer) c.gdpa(c.dev, "vkEndCommandBuffer");
        PFN_vkCmdBeginRenderPass brp = (PFN_vkCmdBeginRenderPass) c.gdpa(c.dev, "vkCmdBeginRenderPass");
        PFN_vkCmdEndRenderPass erp = (PFN_vkCmdEndRenderPass) c.gdpa(c.dev, "vkCmdEndRenderPass");
        PFN_vkCmdBindPipeline bp = (PFN_vkCmdBindPipeline) c.gdpa(c.dev, "vkCmdBindPipeline");
        PFN_vkCmdDraw draw = (PFN_vkCmdDraw) c.gdpa(c.dev, "vkCmdDraw");
        if (!mksh || !mkpl || !mkgp || !begin || !end || !brp || !erp || !bp || !draw) { LOG("X 绘制入口缺失\n"); goto out2; }

        VkShaderModuleCreateInfo vsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = tri_vert_spv_len * 4, .pCode = tri_vert_spv };
        VkShaderModuleCreateInfo fsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = tri_frag_spv_len * 4, .pCode = tri_frag_spv };
        VkShaderModule vs = NULL, fs = NULL;
        VkResult r = mksh(c.dev, &vsci, NULL, &vs);
        LOG("[8] 顶点着色器模块 -> %d\n", r);
        r = mksh(c.dev, &fsci, NULL, &fs);
        LOG("[9] 片元着色器模块 -> %d\n", r);
        if (!vs || !fs) { LOG("X 着色器模块创建失败\n"); goto out2; }

        VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                .offset = 0, .size = 8 };   /* v4.0: angle + steps */
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };   /* v2.0: 旋转角 */
    (void) pcr;
        VkPipelineLayout pl = NULL; mkpl(c.dev, &plci, NULL, &pl);

        VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkViewport vp = { 0, 0, (float) t.W, (float) t.H, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { t.W, t.H } };
        VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb2 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba };
        VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
            .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pColorBlendState = &cb2, .layout = pl, .renderPass = t.rp, .subpass = 0 };
        VkPipeline pipe = NULL;
        r = mkgp(c.dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
        LOG("[10] 图形管线 -> %d %s\n", r, r == VK_SUCCESS ? "OK" : "失败");
        if (r != VK_SUCCESS) { LOG("X 管线创建失败（这本身就是重要结果）\n"); goto out2; }

        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin(t.cb, &bi);
        VkClearValue cv = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = t.rp, .framebuffer = t.fb, .renderArea = { { 0, 0 }, { t.W, t.H } },
            .clearValueCount = 1, .pClearValues = &cv };
        brp(t.cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        bp(t.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        {
            PFN_vkCmdPushConstants pc3 = (PFN_vkCmdPushConstants) c.gdpa(c.dev, "vkCmdPushConstants");
            float ang0 = 0.0f;
            if (pc3) pc3(t.cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &ang0);   /* v2.0: 未设置的 push constant 是未定义行为 */
        }
        draw(t.cb, 3, 1, 0, 0);
        erp(t.cb);
        end(t.cb);
        r = submit_wait(&c, &t);
        LOG("[11] 提交并等待 fence -> %d %s\n", r, r == VK_SUCCESS ? "完成" : "失败/超时");

        if (r == VK_SUCCESS) {
            void *px = NULL;
            if (readback(&c, &t, &px) && px) {
                unsigned char *p = (unsigned char *) px;
                // 三角形覆盖左下大三角区域；抽样若干点判断"有红"与"纯红"
                long red = 0, black = 0, other = 0;
                // 中心偏下一点必然落在三角形内
                int cx = t.W / 2, cy = (int) (t.H * 0.75);
                unsigned char *cpx = p + ((size_t) cy * t.W + cx) * 4;
                LOG("[12] 采样点 (%d,%d) RGBA = %u,%u,%u,%u\n", cx, cy, cpx[0], cpx[1], cpx[2], cpx[3]);
                for (uint32_t y = 0; y < t.H; y += 4)
                    for (uint32_t x = 0; x < t.W; x += 4) {
                        unsigned char *q = p + ((size_t) y * t.W + x) * 4;
                        if (q[0] > 200 && q[1] < 60 && q[2] < 60) red++;
                        else if (q[0] < 40 && q[1] < 40 && q[2] < 40) black++;
                        else other++;
                    }
                LOG("[13] 采样统计: 红=%ld 黑=%ld 其它=%ld\n", red, black, other);
                /* 先算红色包围盒，再据此判定（v1.1：原判定没要求 black>0，
                 * 于是"整屏全红"这种典型渲染 bug 会被误判成正确 ✗）*/
                int minx = (int) t.W, miny = (int) t.H, maxx = -1, maxy = -1;
                for (uint32_t y = 0; y < t.H; y++)
                    for (uint32_t x = 0; x < t.W; x++) {
                        unsigned char *q = p + ((size_t) y * t.W + x) * 4;
                        if (q[0] > 200 && q[1] < 60 && q[2] < 60) {
                            if ((int) x < minx) minx = x; if ((int) x > maxx) maxx = x;
                            if ((int) y < miny) miny = y; if ((int) y > maxy) maxy = y;
                        }
                    }
                LOG("[14] 红色包围盒: %s\n", maxx < 0 ? "无红色像素" :
                    "见下一行");
                if (maxx >= 0) {
                    long all = (long) t.W * t.H;
                    LOG("     x[%d,%d] y[%d,%d]  红占比 %.1f%%\n",
                        minx, maxx, miny, maxy, 100.0 * (red * 16) / all);
                    int bw = maxx - minx + 1, bh = maxy - miny + 1;
                    LOG("     期望：一个下宽上尖的三角形（面积≈1/2 包围盒）\n");
                    LOG("     实测包围盒 %dx%d，三角形应约占其一半\n", bw, bh);
                }
                LOG("\n== 判定: %s ==\n",
                    (red > 0 && black > 0 && other == 0)
                        ? "绘制正确（有三角形 + 有清屏底色，且无异常像素）"
                        : (red > 0 && black == 0)
                          ? "异常：整屏都是红（典型渲染 bug：clear 未生效/覆盖全屏）"
                          : (red > 0 ? "绘制有输出但存在异常像素（可能正是渲染 bug）" : "无绘制输出"));
            } else LOG("X 像素回读失败\n");
        }
out2:   ;
    }
out:
    ctx_close(&c);
    (*env)->ReleaseStringUTFChars(env, jso, path);
    return finish(env, path, jso);
}


// ============================================================================
//  v4.0 · 权威计时：GPU 时间戳
//  依据 Khronos Vulkan 官方示例 timestamp_queries：
//   · 只用 TOP_OF_PIPE / BOTTOM_OF_PIPE 两级（"popular CPU/GPU profilers" 的做法，
//     官方说明这组组合"在多数 GPU 上能给出正确的近似计时"）
//   · timestampPeriod 为 0 ⇒ 不支持；结果必须用 VK_QUERY_RESULT_64_BIT（32 位会溢出）
//   · 换算：delta_ms = (ts1 - ts0) * timestampPeriod / 1e6
//   · 不能跨队列比较时间戳
// ============================================================================
typedef struct {
    VkQueryPool pool; double period_ms; uint32_t count; int ok;
    PFN_vkCmdResetQueryPool reset; PFN_vkCmdWriteTimestamp write; PFN_vkGetQueryPoolResults get;
} Timer;

static int timer_init(Ctx *c, Timer *tm, uint32_t count) {
    memset(tm, 0, sizeof(*tm));
    if (c->props.limits.timestampPeriod == 0.0f) {
        LOG("! 该驱动不支持 GPU 时间戳（timestampPeriod=0）⇒ 本档只能报墙钟时间\n");
        return 0;
    }
    tm->reset = (PFN_vkCmdResetQueryPool) c_gipa(c, "vkCmdResetQueryPool");
    tm->write = (PFN_vkCmdWriteTimestamp) c_gipa(c, "vkCmdWriteTimestamp");
    tm->get   = (PFN_vkGetQueryPoolResults) c_gipa(c, "vkGetQueryPoolResults");
    PFN_vkCreateQueryPool mk = (PFN_vkCreateQueryPool) c_gipa(c, "vkCreateQueryPool");
    if (!tm->reset || !tm->write || !tm->get || !mk) { LOG("! 缺查询池入口 ⇒ 本档只报墙钟\n"); return 0; }
    VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = count };
    if (mk(c->dev, &qi, NULL, &tm->pool) != VK_SUCCESS) { LOG("! 建查询池失败 ⇒ 本档只报墙钟\n"); return 0; }
    tm->period_ms = c->props.limits.timestampPeriod / 1000000.0;
    tm->count = count; tm->ok = 1;
    LOG("GPU 时间戳: 可用 ✓ timestampPeriod=%.4f ns/tick（本次报告 GPU 时间与墙钟两个数）\n",
        c->props.limits.timestampPeriod);
    return 1;
}
static void timer_begin(Timer *tm, VkCommandBuffer cb) {
    if (!tm->ok) return;
    tm->reset(cb, tm->pool, 0, 2);
    tm->write(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tm->pool, 0);
}
static void timer_end(Timer *tm, VkCommandBuffer cb) {
    if (!tm->ok) return;
    tm->write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tm->pool, 1);
}
static double timer_result_ms(Ctx *c, Timer *tm) {
    if (!tm->ok) return -1.0;
    uint64_t ts[2] = { 0, 0 };
    VkResult r = tm->get(c->dev, tm->pool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r != VK_SUCCESS) return -1.0;
    return (double) (ts[1] - ts[0]) * tm->period_ms;
}


// ============================================================================
//  v7.3 · 硬件光追（VK_KHR_ray_query，无 RT pipeline）
//  为什么能用：系统驱动报告 VK_KHR_acceleration_structure + ray_query ✓
//  结构：BLAS（三角形网格）→ TLAS（实例）→ compute 里 rayQueryEXT → 存储图像 → 读回
//  不支持该扩展的驱动 ⇒ 本模块直接拒绝并明确报原因（绝不假装 ✓）
// ============================================================================
typedef struct {
    int shadows;        /* 阴影开关 0/1 */
    int pcfTaps;        /* PCF 质量：1 / 3 / 5（每轴采样数）*/
    int shadowRes;      /* 阴影贴图分辨率：1024 / 2048 / 4096 */
    int cubes;          /* 立方体实例数：16 / 36 / 100 / 400（几何负载阶梯）*/
    int bloom;          /* 泛光开关 0/1 */
    int tonemap;        /* 色调映射+伽马 0/1 */
    int passesPerFrame; /* 每帧重复光照 pass 的次数（负载倍率 1..64）*/
    int debugView;      /* 0=最终画面 1=阴影贴图 2=法线（正确性自检用）*/
    int animate;        /* 是否随时间动（关掉便于逐帧比对 ✓）*/
} LitCfg;

static LitCfg g_lit = { 1, 3, 2048, 36, 1, 1, 1, 0, 1 };

typedef struct {
    VkAccelerationStructureKHR blas, tlas;
    VkBuffer blasBuf, tlasBuf, instBuf, vbuf, ibuf, scratch, rbuf;
    VkDeviceMemory blasMem, tlasMem, instMem, vMem, iMem, scratchMem, rMem;
    VkImage img; VkDeviceMemory imgMem; VkImageView view;
    VkDescriptorSetLayout dsl; VkDescriptorPool dpool; VkDescriptorSet dset;
    VkPipelineLayout pl; VkPipeline pipe; VkCommandPool cpool; VkCommandBuffer cb; VkFence fence;
    VkQueryPool qp; double period; int tsOk;
    Ctx ctx; int ok; int w, h; uint32_t ntri;
} RtState;
static RtState g_rtx;

static VkDeviceAddress buf_addr(Ctx *c, VkBuffer b) {
    PFN_vkGetBufferDeviceAddress f = (PFN_vkGetBufferDeviceAddress) c_gipa(c, "vkGetBufferDeviceAddress");
    if (!f) f = (PFN_vkGetBufferDeviceAddress) c_gipa(c, "vkGetBufferDeviceAddressKHR");
    VkBufferDeviceAddressInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b };
    return f ? f(c->dev, &bi) : 0;
}
/* 简易缓冲分配（device-local + host-visible 各一，便于我们直接读写）*/
static int mk_buf(Ctx *c, VkDeviceSize sz, VkBufferUsageFlags use, VkBuffer *out, VkDeviceMemory *mem, int host) {
    PFN_vkCreateBuffer cb_ = (PFN_vkCreateBuffer) c->gdpa(c->dev, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements gm = (PFN_vkGetBufferMemoryRequirements) c->gdpa(c->dev, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory) c->gdpa(c->dev, "vkAllocateMemory");
    PFN_vkBindBufferMemory bm = (PFN_vkBindBufferMemory) c->gdpa(c->dev, "vkBindBufferMemory");
    if (!cb_ || !gm || !am || !bm) return 0;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
                              .usage = use | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT };
    if (cb_(c->dev, &bc, NULL, out) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; gm(c->dev, *out, &mr);
    uint32_t mt;
    if (host) {
        /* 两级降级：先要 HOST_VISIBLE|HOST_COHERENT，退一步只要 HOST_VISIBLE ✓；
         * 都拿不到就**明确失败** ✗（绝不给主机缓冲返回不可见类型，否则映射必失败 ✓）*/
        mt = pick_mem_strict(c, mr.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == MEM_NONE) mt = pick_mem_strict(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (mt == MEM_NONE) {
            LOG("  ! mk_buf(host): 该设备没有 HOST_VISIBLE 内存类型匹配 0x%x ⇒ 拒绝（不退化 ✓）\n", mr.memoryTypeBits);
            return 0;
        }
    } else {
        mt = pick_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == MEM_NONE) return 0;
    }
    VkMemoryAllocateFlagsInfo fi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .pNext = &fi, .allocationSize = mr.size, .memoryTypeIndex = mt };
    if (am(c->dev, &ai, NULL, mem) != VK_SUCCESS) return 0;
    return bm(c->dev, *out, *mem, 0) == VK_SUCCESS;
}

/* 场景网格：UV 球（约 480 三角形）+ 一块地面 */
static int rt_make_mesh(Ctx *c, float **vout, uint32_t **iout, uint32_t *ntri, uint32_t *nvout) {   /* v9.83: 顶点数由生成器返回，不再硬编码 ✗→✓ */
    /* v9.78 · 游戏感场景：6 个球（与 rt_comp.comp 里的 SC/SR 表逐项一致 ✓）+ 棋盘格地面 */
    /* v9.79 · 室内竞技场（暗区突围风）：地板/天花/四墙 + 6 木箱 + 2 油桶
     * 与 rt_comp.comp 的 BOX/SPH/R/H 逐项一致 ✓ */
    const int SEG = 20, RING = 10, NSP = 3, NB = 12;   /* v9.93: 表里其实有 12 行（含两面镜）—— 之前 NB=10 把镜子截掉了 ✗ */
    static const float SPH[NSP][4] = { { -0.9f, 0.55f, 0.6f, 0.55f }, { 1.5f, 0.45f, -2.6f, 0.45f },
                                       { 3.1f, 0.60f, 1.9f, 0.60f } };
    static const float BOX[NB][6] = {
        { -1.6f, 0.45f, -1.5f, 0.45f, 0.45f, 0.45f },
        {  0.2f, 0.35f, -2.2f, 0.35f, 0.35f, 0.35f },
        {  1.9f, 0.60f, -0.8f, 0.60f, 0.60f, 0.60f },
        { -2.6f, 0.30f,  1.2f, 0.30f, 0.30f, 0.30f },
        {  0.9f, 0.50f,  1.8f, 0.50f, 0.50f, 0.50f },
        {  2.4f, 0.40f,  2.6f, 0.40f, 0.40f, 0.40f },
        { -3.3f, 0.70f, -0.6f, 0.70f, 0.70f, 0.70f },
        { -3.3f, 2.10f, -0.6f, 0.70f, 0.70f, 0.70f },   /* 叠放 */
        {  0.2f, 1.05f, -2.2f, 0.35f, 0.35f, 0.35f },   /* 小箱摞在大箱上 */
        {  2.4f, 1.20f,  2.6f, 0.40f, 0.40f, 0.40f },
        { -4.36f, 1.60f, 0.0f, 0.06f, 1.55f, 2.60f },   /* ★ 左墙镜面 */
        {  4.36f, 1.60f, 0.0f, 0.06f, 1.55f, 2.60f },   /* ★ 右墙镜面 */
    };
    int nv = NSP * (RING + 1) * (SEG + 1) + NB * 8 + 4 + 20;
    float *v = (float *) malloc(sizeof(float) * nv * 3);
    uint32_t *idx = (uint32_t *) malloc(sizeof(uint32_t) * (NSP * SEG * RING * 6 + NB * 36 + 6 + 60));   /* v9.82: 房间四边形正反各一份 */
    int vi = 0;
    for (int si = 0; si < NSP; si++) {
        for (int r = 0; r <= RING; r++) {
            float phi = (float) r / RING * 3.14159265f;
            for (int sg = 0; sg <= SEG; sg++) {
                float th = (float) sg / SEG * 6.2831853f;
                v[vi*3+0] = SPH[si][3] * sinf(phi) * cosf(th) + SPH[si][0];
                v[vi*3+1] = SPH[si][3] * cosf(phi) + SPH[si][1];
                v[vi*3+2] = SPH[si][3] * sinf(phi) * sinf(th) + SPH[si][2];
                vi++;
            }
        }
    }
    for (int bi = 0; bi < NB; bi++) {
        for (int k = 0; k < 8; k++) {
            v[vi*3+0] = BOX[bi][0] + ((k & 1) ? BOX[bi][3] : -BOX[bi][3]);
            v[vi*3+1] = BOX[bi][1] + ((k & 2) ? BOX[bi][4] : -BOX[bi][4]);
            v[vi*3+2] = BOX[bi][2] + ((k & 4) ? BOX[bi][5] : -BOX[bi][5]);
            vi++;
        }
    }
    int base = vi;   /* 地面 4 顶点 */
    const float R = 4.5f, H = 4.2f;   /* v9.79: 室内 9x9、层高 4.2 */
    float gp[4][3] = { { -R,0,-R }, { R,0,-R }, { R,0,R }, { -R,0,R } };
    for (int i = 0; i < 4; i++) { v[vi*3+0]=gp[i][0]; v[vi*3+1]=gp[i][1]; v[vi*3+2]=gp[i][2]; vi++; }
    {
        float qs[5][4][3] = {
            { {-R,H,-R}, { R,H,-R}, { R,H,R}, {-R,H,R} },
            { {-R,0,-R}, { R,0,-R}, { R,H,-R}, {-R,H,-R} },
            { {-R,0, R}, { R,0, R}, { R,H, R}, {-R,H, R} },
            { {-R,0,-R}, {-R,0,R}, {-R,H,R}, {-R,H,-R} },
            { { R,0,-R}, { R,0,R}, { R,H,R}, { R,H,-R} },
        };
        for (int qi2 = 0; qi2 < 5; qi2++) for (int k = 0; k < 4; k++) {
            v[vi*3+0]=qs[qi2][k][0]; v[vi*3+1]=qs[qi2][k][1]; v[vi*3+2]=qs[qi2][k][2]; vi++;
        }
    }
    uint32_t *ii = idx; int ni = 0;
    for (int si = 0; si < NSP; si++) {
        int off = si * (RING + 1) * (SEG + 1);
        for (int r = 0; r < RING; r++) for (int sg = 0; sg < SEG; sg++) {
            uint32_t a = off + r*(SEG+1)+sg, b = a+1, cc = a+(SEG+1), d = cc+1;
            ii[ni++]=a; ii[ni++]=cc; ii[ni++]=b;  ii[ni++]=b; ii[ni++]=cc; ii[ni++]=d;
        }
    }
    for (int bi = 0; bi < NB; bi++) {
        uint32_t o = (uint32_t)(NSP * (RING + 1) * (SEG + 1) + bi * 8);
        uint32_t q[8]; for (int k = 0; k < 8; k++) q[k] = o + (uint32_t) k;
        int f[6][4] = { {0,2,3,1}, {4,5,7,6}, {0,1,5,4}, {2,6,7,3}, {0,4,6,2}, {1,3,7,5} };
        for (int fi = 0; fi < 6; fi++) {
            ii[ni++]=q[f[fi][0]]; ii[ni++]=q[f[fi][1]]; ii[ni++]=q[f[fi][2]];
            ii[ni++]=q[f[fi][0]]; ii[ni++]=q[f[fi][2]]; ii[ni++]=q[f[fi][3]];
        }
    }
    {
        uint32_t fb = (uint32_t)(NSP * (RING + 1) * (SEG + 1) + NB * 8 + 4);   /* 天花/四墙顶点起点 */
        for (int qi2 = 0; qi2 < 5; qi2++) {
            uint32_t a = fb + (uint32_t) qi2 * 4;
            /* v9.82: 正反两面都生成 —— 无论绕序/是否开背面剔除都能被射线命中 ✓ */
            ii[ni++]=a; ii[ni++]=a+1; ii[ni++]=a+2;  ii[ni++]=a; ii[ni++]=a+2; ii[ni++]=a+3;
            ii[ni++]=a+2; ii[ni++]=a+1; ii[ni++]=a;  ii[ni++]=a+3; ii[ni++]=a+2; ii[ni++]=a;
        }
    }
    ii[ni++]=base; ii[ni++]=base+1; ii[ni++]=base+2; ii[ni++]=base; ii[ni++]=base+2; ii[ni++]=base+3;
    *vout = v; *iout = idx; *ntri = (uint32_t)(ni / 3); *nvout = (uint32_t) vi;
    return 1;
}


/* 光追推常量（与 rt.comp 的 push_constant 块逐字段对齐 ✓ 112 字节）*/
static int g_rt_opt[6] = { 4, 1, 1, 100, 1, 97 };   /* v9.98 光追配置：采样/弹射/细节/光晕/灯速/镜面 */
typedef struct { float right[4]; float up[4]; float fwd[4]; float camPos[4]; float lightDir[4]; float light2[4]; float fov; int shadowOn; int bounces; float t; int rtOpt[4]; } RtPC;   /* v9.98: rtOpt[4]，去 _pad ⇒ 124 字节 ✓ */   /* v9.91: +light2 点光源 */   /* v9.69: 传相机基，不再用 invVP ✓ */
static VkBuffer g_scBuf = VK_NULL_HANDLE, g_scIdx = VK_NULL_HANDLE;        /* v9.90 全局：S1a 写入 / S1b 绑定 跨函数共用 ✓ */
static VkDeviceMemory g_scBufMem = VK_NULL_HANDLE, g_scIdxMem = VK_NULL_HANDLE;

/* 直接构造「NDC→世界」矩阵（= 视投影的逆），避免写通用 4x4 求逆 ✓ */
static void rt_inv_vp(float *m, float aspect, const float *eye, const float *fwd,
                      const float *right, const float *up, float fovy) {
    float th = tanf(fovy * 0.5f), tw = th * aspect;
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    for (int k = 0; k < 3; k++) {
        m[0 + k]  = right[k] * tw;                 /* 列 0 */
        m[4 + k]  = up[k] * th;                    /* 列 1 */
        m[8 + k]  = -fwd[k];                       /* 列 2 */
        m[12 + k] = eye[k];                        /* 列 3 */
    }
    m[11] = -1.0f;                                  /* 透视除法的 w = -z_view ✓ */
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRtInit(JNIEnv *env, jobject th, jstring jso, jint w, jint h) {
    LOG("光影入口: nativeRtInit 进入\n");
    (void) th;
    const char *path = jso ? (*env)->GetStringUTFChars(env, jso, NULL) : NULL;
    char out[320];
    if (g_rtx.ok) { ctx_close(&g_rtx.ctx); memset(&g_rtx, 0, sizeof(g_rtx)); }
    if (!ctx_open(&g_rtx.ctx, path ? path : "system")) {
        if (path) (*env)->ReleaseStringUTFChars(env, jso, path);
        return (*env)->NewStringUTF(env, "光追：驱动不可用");
    }
    if (path) (*env)->ReleaseStringUTFChars(env, jso, path);
    Ctx *c = &g_rtx.ctx;
    g_rtx.w = (int) w; g_rtx.h = (int) h;

    /* 入口（全部自己取 ✓ 因为我们用 VK_NO_PROTOTYPES ✓）*/
    PFN_vkGetBufferDeviceAddressKHR gaddr =
        (PFN_vkGetBufferDeviceAddressKHR) c_gipa(c, "vkGetBufferDeviceAddressKHR");
    if (!gaddr) gaddr = (PFN_vkGetBufferDeviceAddressKHR) c_gipa(c, "vkGetBufferDeviceAddress");
    PFN_vkGetAccelerationStructureBuildSizesKHR gsz =
        (PFN_vkGetAccelerationStructureBuildSizesKHR) c_gipa(c, "vkGetAccelerationStructureBuildSizesKHR");
    PFN_vkCreateAccelerationStructureKHR cas =
        (PFN_vkCreateAccelerationStructureKHR) c_gipa(c, "vkCreateAccelerationStructureKHR");
    PFN_vkGetAccelerationStructureDeviceAddressKHR asaddr =
        (PFN_vkGetAccelerationStructureDeviceAddressKHR) c_gipa(c, "vkGetAccelerationStructureDeviceAddressKHR");
    if (!gaddr || !gsz || !cas || !asaddr) {
        ctx_close(c); memset(&g_rtx, 0, sizeof(g_rtx));
        return (*env)->NewStringUTF(env, "光追：该驱动缺少加速结构入口 ⇒ 不支持硬件光追 ✗（不假装 ✓）");
    }
    /* 网格 → 顶点/索引缓冲 */
    float *verts = NULL; uint32_t *idx = NULL; uint32_t ntri = 0;
    uint32_t nvu = 0;
    if (!rt_make_mesh(c, &verts, &idx, &ntri, &nvu)) { ctx_close(c); memset(&g_rtx, 0, sizeof(g_rtx)); return (*env)->NewStringUTF(env, "网格生成失败"); }
    int nv = (int) nvu;   /* v9.83: 真顶点数（原来是硬编码 329 ✗ 导致箱子/房间越界 ✗）*/
    VkDeviceSize vsz = (VkDeviceSize) nv * 12, isz = (VkDeviceSize) ntri * 3 * 4;
    /* v9.84 · S1a：场景数据进 SSBO —— 「唯一真源」第一步（本步不改画面 ✓ 为下一步 S1b 绑定铺路）*/
    {
        VkDeviceSize ssz = (VkDeviceSize) nv * 12, siz = (VkDeviceSize) ntri * 3 * 4;
        if (mk_buf(c, ssz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_scBuf, &g_scBufMem, 1) &&
            mk_buf(c, siz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_scIdx, &g_scIdxMem, 1)) {
            PFN_vkMapMemory m1 = (PFN_vkMapMemory) c->gdpa(c->dev, "vkMapMemory");
            PFN_vkUnmapMemory u1 = (PFN_vkUnmapMemory) c->gdpa(c->dev, "vkUnmapMemory");
            void *p1 = NULL, *p2 = NULL;
            int ok1 = (m1 && m1(c->dev, g_scBufMem, 0, VK_WHOLE_SIZE, 0, &p1) == VK_SUCCESS && p1);
            int ok2 = (m1 && m1(c->dev, g_scIdxMem, 0, VK_WHOLE_SIZE, 0, &p2) == VK_SUCCESS && p2);
            if (ok1) { memcpy(p1, verts, (size_t) ssz); u1(c->dev, g_scBufMem); }
            if (ok2) { memcpy(p2, idx, (size_t) siz); u1(c->dev, g_scIdxMem); }
            snprintf(out, sizeof(out), "场景 SSBO OK 顶点 %u 字节（%d 个）· 索引 %u 字节（%u 三角形）· 上传 %s%s",
                     (unsigned) ssz, nv, (unsigned) siz, ntri, ok1 ? "顶点OK" : "顶点FAIL", ok2 ? " 索引OK" : " 索引FAIL");
            LOG("%s%c", out, 10);   /* v9.85: 不用字面量格式串（-Wformat-security）+ 换行用 %c 避开反斜杠陷阱 */
        } else {
            LOG("%s", "场景 SSBO FAIL（创建失败，仅铺路，不影响渲染）");
        }
    }
    if (!mk_buf(c, vsz, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &g_rtx.vbuf, &g_rtx.vMem, 1) ||
        !mk_buf(c, isz, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &g_rtx.ibuf, &g_rtx.iMem, 1)) {
        free(verts); free(idx); ctx_close(c); memset(&g_rtx, 0, sizeof(g_rtx)); return (*env)->NewStringUTF(env, "顶点/索引缓冲失败"); }
    PFN_vkMapMemory mp = (PFN_vkMapMemory) c->gdpa(c->dev, "vkMapMemory");
    void *p = NULL;
    mp(c->dev, g_rtx.vMem, 0, VK_WHOLE_SIZE, 0, &p); memcpy(p, verts, vsz); 
    PFN_vkUnmapMemory um = (PFN_vkUnmapMemory) c->gdpa(c->dev, "vkUnmapMemory"); um(c->dev, g_rtx.vMem);
    mp(c->dev, g_rtx.iMem, 0, VK_WHOLE_SIZE, 0, &p); memcpy(p, idx, isz); um(c->dev, g_rtx.iMem);
    free(verts); free(idx);

    VkDeviceAddress vaddr = nv ? buf_addr(c, g_rtx.vbuf) : 0;
    VkDeviceAddress iaddr = buf_addr(c, g_rtx.ibuf);

    /* ---- BLAS ---- */
    VkAccelerationStructureGeometryKHR geo; memset(&geo, 0, sizeof(geo));
    geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo.geometry.triangles.vertexData.deviceAddress = vaddr;
    geo.geometry.triangles.vertexStride = 12;
    geo.geometry.triangles.maxVertex = (uint32_t)(nv - 1);
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geo.geometry.triangles.indexData.deviceAddress = iaddr;
    VkAccelerationStructureBuildGeometryInfoKHR bi; memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1; bi.pGeometries = &geo;
    uint32_t prim = ntri;
    VkAccelerationStructureBuildSizesInfoKHR szi; memset(&szi, 0, sizeof(szi));
    szi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    gsz(c->dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prim, &szi);
    if (!mk_buf(c, szi.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, &g_rtx.blasBuf, &g_rtx.blasMem, 0)) {
        ctx_close(c); memset(&g_rtx, 0, sizeof(g_rtx)); return (*env)->NewStringUTF(env, "BLAS 缓冲失败（可能显存不足）"); }
    VkAccelerationStructureCreateInfoKHR aci; memset(&aci, 0, sizeof(aci));
    aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    aci.buffer = g_rtx.blasBuf; aci.size = szi.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (cas(c->dev, &aci, NULL, &g_rtx.blas) != VK_SUCCESS) { ctx_close(c); memset(&g_rtx, 0, sizeof(g_rtx)); return (*env)->NewStringUTF(env, "BLAS 创建失败"); }
    if (!mk_buf(c, szi.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_rtx.scratch, &g_rtx.scratchMem, 0)) {
        ctx_close(c); memset(&g_rtx, 0, sizeof(g_rtx)); return (*env)->NewStringUTF(env, "scratch 缓冲失败"); }
    bi.dstAccelerationStructure = g_rtx.blas;
    bi.scratchData.deviceAddress = buf_addr(c, g_rtx.scratch);
    VkAccelerationStructureBuildRangeInfoKHR ri = { .primitiveCount = prim, .primitiveOffset = 0, .firstVertex = 0, .transformOffset = 0 };
    const VkAccelerationStructureBuildRangeInfoKHR *pri = &ri;
    /* 用一次性命令缓冲提交构建 ✓ */
    PFN_vkCreateCommandPool ccp = (PFN_vkCreateCommandPool) c->gdpa(c->dev, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers acb = (PFN_vkAllocateCommandBuffers) c->gdpa(c->dev, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer bcb = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer ecb = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdBuildAccelerationStructuresKHR build =
        (PFN_vkCmdBuildAccelerationStructuresKHR) c->gdpa(c->dev, "vkCmdBuildAccelerationStructuresKHR");
    PFN_vkQueueSubmit qs = (PFN_vkQueueSubmit) c->gdpa(c->dev, "vkQueueSubmit");
    PFN_vkCreateFence cf = (PFN_vkCreateFence) c->gdpa(c->dev, "vkCreateFence");
    PFN_vkWaitForFences wf = (PFN_vkWaitForFences) c->gdpa(c->dev, "vkWaitForFences");
    if (ccp && acb && bcb && ecb && build && qs && cf && wf) {
        VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,   /* v9.9：每帧反复 begin 同一 CB 必须可重置（原先缺 ⇒ VUID 违规 ⇒ 录制不生效 ⇒ 画面不动）✓ */
        .queueFamilyIndex = c->qfam };
        ccp(c->dev, &cpci, NULL, &g_rtx.cpool);
        VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_rtx.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        acb(c->dev, &cbai, &g_rtx.cb);
        VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        bcb(g_rtx.cb, &cbbi);
        build(g_rtx.cb, 1, &bi, &pri);
        ecb(g_rtx.cb);
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        cf(c->dev, &fci, NULL, &g_rtx.fence);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_rtx.cb };
        if (qs(c->q, 1, &si, g_rtx.fence) != VK_SUCCESS || wf(c->dev, 1, &g_rtx.fence, VK_TRUE, 1000000000ULL) != VK_SUCCESS) {
            ctx_close(c); memset(&g_rtx, 0, sizeof(g_rtx)); return (*env)->NewStringUTF(env, "BLAS 构建提交失败 ⇒ 驱动/GPU 侧问题"); }
    } else {
        ctx_close(c); memset(&g_rtx, 0, sizeof(g_rtx)); return (*env)->NewStringUTF(env, "缺少构建命令入口");
    }
    g_rtx.ntri = ntri;
    g_rtx.ok = 1;
    snprintf(out, sizeof(out), "光追就绪 ✓ BLAS 已建（%u 三角形 = 箱 %d × 桶 %d + 镜面 + 房间）%dx%d   /* v10.3 修正过期描述 */",
             ntri, 12, 24, (int) w, (int) h);
    return (*env)->NewStringUTF(env, out);
}


/* ---- 光追：TLAS + 存储图像 + 描述符 + compute 管线 + dispatch ---- */
static int rt_mk_image(Ctx *c, int w, int h) {
    PFN_vkCreateImage ci = (PFN_vkCreateImage) c->gdpa(c->dev, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements gm = (PFN_vkGetImageMemoryRequirements) c->gdpa(c->dev, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory) c->gdpa(c->dev, "vkAllocateMemory");
    PFN_vkBindImageMemory bm = (PFN_vkBindImageMemory) c->gdpa(c->dev, "vkBindImageMemory");
    PFN_vkCreateImageView cv = (PFN_vkCreateImageView) c->gdpa(c->dev, "vkCreateImageView");
    if (!ci || !gm || !am || !bm || !cv) return 0;
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { (uint32_t) w, (uint32_t) h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (ci(c->dev, &ic, NULL, &g_rtx.img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; gm(c->dev, g_rtx.img, &mr);
    uint32_t mt = pick_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == MEM_NONE) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    if (am(c->dev, &ai, NULL, &g_rtx.imgMem) != VK_SUCCESS) return 0;
    if (bm(c->dev, g_rtx.img, g_rtx.imgMem, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_rtx.img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    return cv(c->dev, &vc, NULL, &g_rtx.view) == VK_SUCCESS;
}

static int rt_build_tlas(Ctx *c) {
    PFN_vkGetAccelerationStructureDeviceAddressKHR asaddr =
        (PFN_vkGetAccelerationStructureDeviceAddressKHR) c_gipa(c, "vkGetAccelerationStructureDeviceAddressKHR");
    PFN_vkGetAccelerationStructureBuildSizesKHR gsz =
        (PFN_vkGetAccelerationStructureBuildSizesKHR) c_gipa(c, "vkGetAccelerationStructureBuildSizesKHR");
    PFN_vkCreateAccelerationStructureKHR cas =
        (PFN_vkCreateAccelerationStructureKHR) c_gipa(c, "vkCreateAccelerationStructureKHR");
    PFN_vkCmdBuildAccelerationStructuresKHR build =
        (PFN_vkCmdBuildAccelerationStructuresKHR) c->gdpa(c->dev, "vkCmdBuildAccelerationStructuresKHR");
    if (!asaddr || !gsz || !cas || !build) return 0;
    if (!mk_buf(c, 64, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &g_rtx.instBuf, &g_rtx.instMem, 1)) return 0;
    PFN_vkMapMemory mp = (PFN_vkMapMemory) c->gdpa(c->dev, "vkMapMemory");
    PFN_vkUnmapMemory um = (PFN_vkUnmapMemory) c->gdpa(c->dev, "vkUnmapMemory");
    void *p = NULL; mp(c->dev, g_rtx.instMem, 0, VK_WHOLE_SIZE, 0, &p);
    VkAccelerationStructureDeviceAddressInfoKHR dai = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = g_rtx.blas };
    VkDeviceAddress ba = asaddr(c->dev, &dai);
    /* 手填 VkAccelerationStructureInstanceKHR —— v9.71 修正字节偏移：
     *   transform=0..47 / customIndex|mask=48(u[12]) / off|flags=52(u[13]) / BLAS地址=56..63(u[14],u[15])
     *   原先写成 u[16/4+0..3]=u[4..7]（字节 16..31）⇒ 全落进 transform 矩阵，
     *   而**真正放 BLAS 地址的 56~63 一直是 0** ⇒ 实例指向空 ⇒ 光线全打空 ✗✗ */
    memset(p, 0, 64);
    float *f = (float *) p;
    f[0] = 1; f[5] = 1; f[10] = 1;                       /* 3x4 变换 = 单位矩阵 */
    uint32_t *u = (uint32_t *) p;
    u[12] = 0xFF000000u;                          /* instanceCustomIndex:24 | mask:8 ⇒ mask=0xFF ✓ */
    u[13] = 0;                                    /* instanceShaderBindingTableRecordOffset:24 | flags:8 */
    u[14] = (uint32_t) (ba & 0xFFFFFFFFu);        /* accelerationStructureReference 低 32 位 */
    u[15] = (uint32_t) (ba >> 32);                /* 高 32 位 */
    um(c->dev, g_rtx.instMem);
    VkAccelerationStructureGeometryKHR g; memset(&g, 0, sizeof(g));
    g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.arrayOfPointers = VK_FALSE;
    g.geometry.instances.data.deviceAddress = buf_addr(c, g_rtx.instBuf);
    VkAccelerationStructureBuildGeometryInfoKHR bi; memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1; bi.pGeometries = &g;
    uint32_t prim = 1;
    VkAccelerationStructureBuildSizesInfoKHR szi; memset(&szi, 0, sizeof(szi));
    szi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    gsz(c->dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prim, &szi);
    if (!mk_buf(c, szi.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, &g_rtx.tlasBuf, &g_rtx.tlasMem, 0)) return 0;
    VkAccelerationStructureCreateInfoKHR aci; memset(&aci, 0, sizeof(aci));
    aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    aci.buffer = g_rtx.tlasBuf; aci.size = szi.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (cas(c->dev, &aci, NULL, &g_rtx.tlas) != VK_SUCCESS) return 0;
    VkBuffer sc; VkDeviceMemory scm;
    if (!mk_buf(c, szi.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &sc, &scm, 0)) return 0;
    bi.dstAccelerationStructure = g_rtx.tlas;
    bi.scratchData.deviceAddress = buf_addr(c, sc);
    VkAccelerationStructureBuildRangeInfoKHR ri = { .primitiveCount = 1 };
    const VkAccelerationStructureBuildRangeInfoKHR *pri = &ri;
    PFN_vkResetFences rf = (PFN_vkResetFences) c->gdpa(c->dev, "vkResetFences");
    PFN_vkBeginCommandBuffer bcb = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer ecb = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkQueueSubmit qs = (PFN_vkQueueSubmit) c->gdpa(c->dev, "vkQueueSubmit");
    PFN_vkWaitForFences wf = (PFN_vkWaitForFences) c->gdpa(c->dev, "vkWaitForFences");
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    bcb(g_rtx.cb, &cbbi); build(g_rtx.cb, 1, &bi, &pri); ecb(g_rtx.cb);
    rf(c->dev, 1, &g_rtx.fence); 
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_rtx.cb };
    return qs(c->q, 1, &si, g_rtx.fence) == VK_SUCCESS && wf(c->dev, 1, &g_rtx.fence, VK_TRUE, 1000000000ULL) == VK_SUCCESS;
}

/* 计算管线 + 描述符（binding0=TLAS binding1=存储图像 ✓）*/
static int rt_mk_pipeline(Ctx *c) {
    PFN_vkCreateShaderModule mksh = (PFN_vkCreateShaderModule) c->gdpa(c->dev, "vkCreateShaderModule");
    PFN_vkCreateDescriptorSetLayout mkdl = (PFN_vkCreateDescriptorSetLayout) c->gdpa(c->dev, "vkCreateDescriptorSetLayout");
    PFN_vkCreateDescriptorPool mkdp = (PFN_vkCreateDescriptorPool) c->gdpa(c->dev, "vkCreateDescriptorPool");
    PFN_vkAllocateDescriptorSets ads = (PFN_vkAllocateDescriptorSets) c->gdpa(c->dev, "vkAllocateDescriptorSets");
    PFN_vkUpdateDescriptorSets uds = (PFN_vkUpdateDescriptorSets) c->gdpa(c->dev, "vkUpdateDescriptorSets");
    PFN_vkCreatePipelineLayout mkpl = (PFN_vkCreatePipelineLayout) c->gdpa(c->dev, "vkCreatePipelineLayout");
    PFN_vkCreateComputePipelines mkcp = (PFN_vkCreateComputePipelines) c->gdpa(c->dev, "vkCreateComputePipelines");
    if (!mksh || !mkdl || !mkdp || !ads || !uds || !mkpl || !mkcp) return 0;
    VkDescriptorSetLayoutBinding b[4];   /* v9.89 S1b: +b2 顶点SSBO +b3 索引SSBO */
    memset(b, 0, sizeof(b));
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b };
    if (mkdl(c->dev, &dl, NULL, &g_rtx.dsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 } };
    VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    if (mkdp(c->dev, &dp, NULL, &g_rtx.dpool) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_rtx.dpool, .descriptorSetCount = 1, .pSetLayouts = &g_rtx.dsl };
    if (ads(c->dev, &da, &g_rtx.dset) != VK_SUCCESS) return 0;
    VkWriteDescriptorSetAccelerationStructureKHR wa; memset(&wa, 0, sizeof(wa));
    wa.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &g_rtx.tlas;
    VkDescriptorImageInfo ii = { .imageView = g_rtx.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[4]; memset(w, 0, sizeof(w));
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].pNext = &wa;
    w[0].dstSet = g_rtx.dset; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = g_rtx.dset; w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &ii;
    VkDescriptorBufferInfo bi2; bi2.buffer = g_scBuf; bi2.offset = 0; bi2.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo bi3; bi3.buffer = g_scIdx; bi3.offset = 0; bi3.range = VK_WHOLE_SIZE;
    w[2].dstSet = g_rtx.dset; w[2].dstBinding = 2; w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &bi2;
    w[3].dstSet = g_rtx.dset; w[3].dstBinding = 3; w[3].descriptorCount = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &bi3;
    uds(c->dev, 4, w, 0, NULL);
    VkShaderModuleCreateInfo sm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = rt_comp_spv_len * 4, .pCode = rt_comp_spv };
    VkShaderModule mod = NULL;
    if (mksh(c->dev, &sm, NULL, &mod) != VK_SUCCESS) return 0;
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(RtPC) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &g_rtx.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (mkpl(c->dev, &plci, NULL, &g_rtx.pl) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo cp = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" },
        .layout = g_rtx.pl };
    if (mkcp(c->dev, VK_NULL_HANDLE, 1, &cp, NULL, &g_rtx.pipe) != VK_SUCCESS) return 0;
    /* 读回缓冲（主机可见 ✓）*/
    return mk_buf(c, (VkDeviceSize) g_rtx.w * g_rtx.h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_rtx.rbuf, &g_rtx.rMem, 1);
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRtFrame(JNIEnv *env, jobject th) {
    LOG("光影入口: nativeRtFrame 进入\n");
    (void) th;
    char out[320];
    if (!g_rtx.ok) return (*env)->NewStringUTF(env, "光追未初始化");
    Ctx *c = &g_rtx.ctx;
    if (!g_rtx.tlas) {
        if (!rt_build_tlas(c)) return (*env)->NewStringUTF(env, "X TLAS 构建失败 ⇒ 驱动/GPU 侧问题");
        if (!rt_mk_image(c, g_rtx.w, g_rtx.h)) return (*env)->NewStringUTF(env, "X 存储图像创建失败");
        if (!rt_mk_pipeline(c)) return (*env)->NewStringUTF(env, "X 光追管线创建失败");
        LOG("光追：TLAS + 存储图像 + 管线就绪\n");
    }
    PFN_vkResetFences rf = (PFN_vkResetFences) c->gdpa(c->dev, "vkResetFences");
    PFN_vkBeginCommandBuffer bcb = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer ecb = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdBindPipeline bp = (PFN_vkCmdBindPipeline) c->gdpa(c->dev, "vkCmdBindPipeline");
    PFN_vkCmdBindDescriptorSets bds = (PFN_vkCmdBindDescriptorSets) c->gdpa(c->dev, "vkCmdBindDescriptorSets");
    PFN_vkCmdPushConstants pc_ = (PFN_vkCmdPushConstants) c->gdpa(c->dev, "vkCmdPushConstants");
    PFN_vkCmdDispatch disp = (PFN_vkCmdDispatch) c->gdpa(c->dev, "vkCmdDispatch");
    PFN_vkCmdPipelineBarrier bar = (PFN_vkCmdPipelineBarrier) c->gdpa(c->dev, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImageToBuffer c2b = (PFN_vkCmdCopyImageToBuffer) c->gdpa(c->dev, "vkCmdCopyImageToBuffer");
    PFN_vkQueueSubmit qs = (PFN_vkQueueSubmit) c->gdpa(c->dev, "vkQueueSubmit");
    PFN_vkWaitForFences wf = (PFN_vkWaitForFences) c->gdpa(c->dev, "vkWaitForFences");
    if (!rf || !bcb || !ecb || !bp || !bds || !pc_ || !disp || !bar || !c2b || !qs || !wf)
        return (*env)->NewStringUTF(env, "X 光追命令入口缺失");
    RtPC k; memset(&k, 0, sizeof(k));
    float eye[3] = { 3.2f, 2.4f, 3.2f };
    float fwd[3] = { -0.62f, -0.42f, -0.62f }, right[3] = { 0.707f, 0.0f, -0.707f }, up[3] = { 0, 1, 0 };
    /* v9.69: 直接传相机基（与已验证能出图的迷你 3D 同一套射线公式）✓ */
    /* v9.80 · 相机绕行（室内环绕，游戏实机感）：半径 2.8 m 在房间里绕，看向 (0,0.8,0) ✓ */
    {
        float ang = (float) fmod(now_ms(), 20000.0) / 20000.0f * 6.2831853f;   /* v9.81: now_ms 返回 double，取模要用 fmod ✓ */
        float ex = 2.8f * sinf(ang), ez = 2.8f * cosf(ang), ey = 2.3f;
        float tx = 0.0f - ex, ty = 0.85f - ey, tz = 0.0f - ez;
        float len = sqrtf(tx*tx + ty*ty + tz*tz); if (len < 1e-4f) len = 1e-4f;
        eye[0] = ex; eye[1] = ey; eye[2] = ez;
        fwd[0] = tx/len; fwd[1] = ty/len; fwd[2] = tz/len;
        right[0] = fwd[1]*0.0f - fwd[2]*1.0f; right[1] = fwd[2]*0.0f - fwd[0]*0.0f; right[2] = fwd[0]*1.0f - fwd[1]*0.0f;
        len = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]); if (len < 1e-4f) len = 1e-4f;
        right[0] /= len; right[1] /= len; right[2] /= len;
        up[0] = right[1]*fwd[2] - right[2]*fwd[1];
        up[1] = right[2]*fwd[0] - right[0]*fwd[2];
        up[2] = right[0]*fwd[1] - right[1]*fwd[0];
    }
    memcpy(k.right, right, 12); k.right[3] = 0.0f;
    memcpy(k.up, up, 12); k.up[3] = 0.0f;
    memcpy(k.fwd, fwd, 12); k.fwd[3] = 0.0f;
    k.fov = 1.0f;
    memcpy(k.camPos, eye, 12); k.camPos[3] = 1.0f;
    k.lightDir[0] = 0.5f; k.lightDir[1] = 1.0f; k.lightDir[2] = 0.3f; k.lightDir[3] = 0.0f;
    /* v9.95 · 灯在房间里移动（实时渲染的关键证据：阴影/反射逐帧变化 ✓）*/
    {
        float la = (float) fmod(now_ms(), 12000.0) / 12000.0f * 6.2831853f;
        k.light2[0] = 2.6f * cosf(la);
        k.light2[1] = 2.7f + 0.7f * sinf(la * 2.0f);     /* 上下轻微起伏 */
        k.light2[2] = 2.6f * sinf(la);
        k.light2[3] = 2.2f;                              /* 强度 */
    }
    k.shadowOn = g_lit.shadows; k.bounces = g_lit.passesPerFrame; k.t = (float) (now_ms() / 1000.0);
    k.rtOpt[0] = g_rt_opt[0] > 0 ? g_rt_opt[0] : 1;   /* 阴影采样数 */
    k.rtOpt[1] = g_rt_opt[1];                        /* 反射弹射 0/1/2 */
    k.rtOpt[2] = (g_rt_opt[2] & 1) | ((g_rt_opt[5] & 0xFF) << 8);   /* bit0 细节, bit8-15 镜面% */
    k.rtOpt[3] = g_rt_opt[3];                        /* 光晕 % */
    VkCommandBufferBeginInfo bi2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    double t0 = now_ms();
    if (bcb(g_rtx.cb, &bi2) != VK_SUCCESS) return (*env)->NewStringUTF(env, "X begin 失败");
    bp(g_rtx.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rtx.pipe);
    bds(g_rtx.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rtx.pl, 0, 1, &g_rtx.dset, 0, NULL);
    pc_(g_rtx.cb, g_rtx.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(k), &k);
    disp(g_rtx.cb, (uint32_t) ((g_rtx.w + 7) / 8), (uint32_t) ((g_rtx.h + 7) / 8), 1);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = g_rtx.img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    bar(g_rtx.cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t) g_rtx.w, (uint32_t) g_rtx.h, 1 } };
    c2b(g_rtx.cb, g_rtx.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rtx.rbuf, 1, &bc);
    ecb(g_rtx.cb);
    rf(c->dev, 1, &g_rtx.fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_rtx.cb };
    VkResult r = qs(c->q, 1, &si, g_rtx.fence);
    if (r == VK_SUCCESS) r = wf(c->dev, 1, &g_rtx.fence, VK_TRUE, 3000000000ULL);
    double t1 = now_ms();
    if (r != VK_SUCCESS)
        snprintf(out, sizeof(out), "X 光追提交失败 r=%d（%s）三角形=%u —— 这一帧保留", r,
                 r == -4 ? "VK_ERROR_DEVICE_LOST" : "见 VkResult 表", g_rtx.ntri);
    else
        snprintf(out, sizeof(out), "光追光栅化完成 ✓ BLAS=%u 三角形 · 每像素 %d 次追踪 · 墙钟 %.2f ms · %.1f FPS",
                 g_rtx.ntri, k.bounces, t1 - t0, (t1 - t0) > 0 ? 1000.0 / (t1 - t0) : 0.0);
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRtBlit(JNIEnv *env, jobject th, jobject bmp) {
    LOG("光影入口: nativeRtBlit 进入\n");
    (void) th;
    if (!g_rtx.ok || !g_rtx.rbuf) return (*env)->NewStringUTF(env, "光追无图像");
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS) return (*env)->NewStringUTF(env, "取位图信息失败");
    /* v11.4 · 回读节流：原生每帧都拷 4 MB，而 UI 侧有 70ms 节流 ⇒ 大部分拷贝白做 ✗
     * 只在足够大的分辨率下、且距上次回读 >60ms 时才真拷（小分辨率照旧 ✓ 不影响诊断）*/
    {
        static double lastBlit = 0.0;
        double nowb = now_ms();
        if (g_rtx.w * g_rtx.h >= 512 * 512 && (nowb - lastBlit) < 60.0)
            return (*env)->NewStringUTF(env, "回读跳过（UI 未消费，省一次 4MB 拷贝）");
        lastBlit = nowb;
    }
    void *pix = NULL;
    if (AndroidBitmap_lockPixels(env, bmp, &pix) != ANDROID_BITMAP_RESULT_SUCCESS) return (*env)->NewStringUTF(env, "锁定位图失败");
    double pmin = -1, pmax = -1, pavg = -1;   /* v9.65 */
    PFN_vkMapMemory mp = (PFN_vkMapMemory) g_rtx.ctx.gdpa(g_rtx.ctx.dev, "vkMapMemory");
    PFN_vkUnmapMemory um = (PFN_vkUnmapMemory) g_rtx.ctx.gdpa(g_rtx.ctx.dev, "vkUnmapMemory");
    void *src = NULL;
    if (mp && mp(g_rtx.ctx.dev, g_rtx.rMem, 0, VK_WHOLE_SIZE, 0, &src) == VK_SUCCESS && src) {
        int cw = g_rtx.w < (int) info.width ? g_rtx.w : (int) info.width;
        int ch = g_rtx.h < (int) info.height ? g_rtx.h : (int) info.height;
        for (int y = 0; y < ch; y++) {
            uint8_t *dst = (uint8_t *) pix + (size_t) y * info.stride;
            memcpy(dst, (const uint8_t *) src + (size_t) y * g_rtx.w * 4, (size_t) cw * 4);
        }
        {
            unsigned char *sb = (unsigned char *) src;
            size_t tot = (size_t) g_rtx.w * g_rtx.h * 4, sum = 0, cnt = 0;
            int mn = 255, mx = 0;
            for (size_t i = 0; i < tot; i += 64) {
                int v = sb[i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
                sum += (size_t) v; cnt++;
            }
            if (cnt) { pmin = mn; pmax = mx; pavg = (double) sum / (double) cnt; }
        }
        um(g_rtx.ctx.dev, g_rtx.rMem);
    }
    AndroidBitmap_unlockPixels(env, bmp);
    {
        char o2[256];
        if (pmin < 0) snprintf(o2, sizeof(o2), "X 回读映射失败（vkMapMemory 未成功）");
        else snprintf(o2, sizeof(o2), "已拷入位图 · 像素 min %.0f/max %.0f/均 %.1f · 位图 %dx%d",
                      pmin, pmax, pavg, (int) info.width, (int) info.height);
        return (*env)->NewStringUTF(env, o2);
    }
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRtStop(JNIEnv *env, jobject th) {
    (void) th;
    if (g_rtx.ok) { ctx_close(&g_rtx.ctx); memset(&g_rtx, 0, sizeof(g_rtx)); }
    return (*env)->NewStringUTF(env, "光追已释放");
}


// ============================================================================
//  v7.6 · 光影多 pass 链（地基：阴影贴图 / 颜色目标 / albedo 纹理 / 三条 render pass）
//  链路：① 深度 pass（光空间）→ ② 光照 pass（采样阴影 + PCF + Blinn-Phong）→ ③ 后处理（泛光+暗角）
//  用的是已编进 .so 的四段着色器（shaders_lit.h ✓）
// ============================================================================
typedef struct {
    Ctx ctx; int ok, w, h;
    VkImage shadow; VkDeviceMemory shadowMem; VkImageView shadowView; VkSampler shadowSampler;
    VkImage color;  VkDeviceMemory colorMem;  VkImageView colorView;  VkSampler colorSampler;
    VkImage albedo; VkDeviceMemory albedoMem; VkImageView albedoView;
    VkImage post; VkDeviceMemory postMem; VkImageView postView;      /* 后处理画布（第二张，避免读写同图 ✓）*/
    VkDescriptorSet descPost;
    VkRenderPass rpDepth, rpScene, rpPost;
    VkFramebuffer fbShadow, fbScene, fbPost;
    VkDescriptorSetLayout dsl; VkDescriptorPool dpool; VkDescriptorSet dset;
    VkPipelineLayout plScene, plPost;
    VkPipeline pipeDepth, pipeScene, pipePost;
    VkCommandPool cpool; VkCommandBuffer cb; VkFence fence;
    VkBuffer rbuf; VkDeviceMemory rMem;
    VkQueryPool qp; double period; int tsOk;
    int shadowRes, nInst;
} LitState;
static LitState g_litx;

typedef struct { float mvp[16]; float lightVP[16]; int nInst; float t; int pcf; int shadows; int shadowRes; int _pad; } LitPC;  /* 144 ✓ 与 lit.frag v2 对齐 */
typedef struct { int bloom; int tonemap; int debugView; float texel; } LitPC2;   /* 16 ✓ 与 post.frag v2 对齐 */

static VkImage mk_img(Ctx *c, int w, int h, VkFormat fmt, VkImageUsageFlags use, VkImage *img, VkDeviceMemory *mem) {
    if (!c || !c->dev) { LOG("  ! mk_img: 上下文/设备为空 ⇒ 拒绝执行\n"); return NULL; }
    if (!img || !mem) { LOG("  ! mk_img: 输出指针为空\n"); return NULL; }
    LOG("  · mk_img %dx%d fmt=%d\n", w, h, (int) fmt);
    PFN_vkCreateImage ci = (PFN_vkCreateImage) c->gdpa(c->dev, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements gm = (PFN_vkGetImageMemoryRequirements) c->gdpa(c->dev, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory) c->gdpa(c->dev, "vkAllocateMemory");
    PFN_vkBindImageMemory bm = (PFN_vkBindImageMemory) c->gdpa(c->dev, "vkBindImageMemory");
    if (!ci || !gm || !am || !bm) return NULL;
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { (uint32_t) w, (uint32_t) h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (ci(c->dev, &ic, NULL, img) != VK_SUCCESS) return NULL;
    VkMemoryRequirements mr; gm(c->dev, *img, &mr);
    uint32_t mt = pick_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == MEM_NONE) return NULL;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    return (am(c->dev, &ai, NULL, mem) == VK_SUCCESS && bm(c->dev, *img, *mem, 0) == VK_SUCCESS) ? *img : NULL;
}
static VkImageView mk_view(Ctx *c, VkImage img, VkFormat fmt, VkImageAspectFlags asp) {
    if (!c || !c->dev) { LOG("  ! mk_view: 设备为空\n"); return NULL; }
    PFN_vkCreateImageView cv = (PFN_vkCreateImageView) c->gdpa(c->dev, "vkCreateImageView");
    VkImageView v = NULL;
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { asp, 0, 1, 0, 1 } };
    return (cv && cv(c->dev, &vc, NULL, &v) == VK_SUCCESS) ? v : NULL;
}
static int mk_rp_depth(Ctx *c, VkRenderPass *rp) {
    PFN_vkCreateRenderPass mk = (PFN_vkCreateRenderPass) c->gdpa(c->dev, "vkCreateRenderPass");
    if (!mk) return 0;
    VkAttachmentDescription a = { .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentReference r = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp; memset(&sp, 0, sizeof(sp));
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 0; sp.pDepthStencilAttachment = &r;
    VkSubpassDependency d = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &d };
    return mk(c->dev, &ci, NULL, rp) == VK_SUCCESS;
}
static int mk_rp_scene(Ctx *c, VkRenderPass *rp) {
    PFN_vkCreateRenderPass mk = (PFN_vkCreateRenderPass) c->gdpa(c->dev, "vkCreateRenderPass");
    if (!mk) return 0;
    VkAttachmentDescription a = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentReference r = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp; memset(&sp, 0, sizeof(sp));
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &r;
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp };
    return mk(c->dev, &ci, NULL, rp) == VK_SUCCESS;
}
/* 后处理 pass 的 render pass：结束布局直接是 TRANSFER_SRC（后面直接拷回 ✓ 省一次屏障 ✓）*/
static int mk_rp_post(Ctx *c, VkRenderPass *rp) {
    PFN_vkCreateRenderPass mk = (PFN_vkCreateRenderPass) c->gdpa(c->dev, "vkCreateRenderPass");
    if (!mk) return 0;
    VkAttachmentDescription a = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference r = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp; memset(&sp, 0, sizeof(sp));
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &r;
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &a, .subpassCount = 1, .pSubpasses = &sp };
    return mk(c->dev, &ci, NULL, rp) == VK_SUCCESS;
}

static VkSampler mk_sampler(Ctx *c, int compare) {
    PFN_vkCreateSampler mk = (PFN_vkCreateSampler) c->gdpa(c->dev, "vkCreateSampler");
    VkSampler s = NULL;
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .compareEnable = compare ? VK_TRUE : VK_FALSE, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL };
    return (mk && mk(c->dev, &si, NULL, &s) == VK_SUCCESS) ? s : NULL;
}


/* albedo：程序化棋盘+渐变（64×64 ✓），经 staging 上传 */
static int lit_mk_albedo(Ctx *c) {
    const int N = 64;
    uint8_t *px = (uint8_t *) malloc((size_t) N * N * 4);
    if (!px) return 0;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int chk = (((x >> 3) ^ (y >> 3)) & 1) ? 200 : 120;
        uint8_t *p = px + ((size_t) y * N + x) * 4;
        p[0] = (uint8_t) (chk * 0.85f); p[1] = (uint8_t) (chk * 0.90f); p[2] = (uint8_t) chk; p[3] = 255;
    }
    if (!mk_img(c, N, N, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &g_litx.albedo, &g_litx.albedoMem)) {
        LOG("  ! albedo: 图像创建失败\n"); free(px); return 0;
    }
    g_litx.albedoView = mk_view(c, g_litx.albedo, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    /* ★★ 这里原先是崩溃点（sig=11 addr=0x6f）：vkMapMemory 的返回值与 p 都没判 ✗
     *    一旦拿到的内存类型不是 HOST_VISIBLE（pick_mem 走"回退告警"那条路 ✓），
     *    映射就失败、p 保持 NULL ⇒ memcpy(NULL, …) 直接炸 ✓。现在每一步都判 ✓ */
    VkBuffer st = NULL; VkDeviceMemory stm = NULL;
    if (!mk_buf(c, (VkDeviceSize) N * N * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &st, &stm, 1)) {
        LOG("  ! albedo: 暂存缓冲分配失败\n"); free(px); return 0;
    }
    PFN_vkMapMemory mp = (PFN_vkMapMemory) c->gdpa(c->dev, "vkMapMemory");
    PFN_vkUnmapMemory um = (PFN_vkUnmapMemory) c->gdpa(c->dev, "vkUnmapMemory");
    if (!mp || !um) { LOG("  ! albedo: 缺 map/unmap 入口\n"); free(px); return 0; }
    void *p = NULL;
    VkResult mr = mp(c->dev, stm, 0, VK_WHOLE_SIZE, 0, &p);
    if (mr != VK_SUCCESS || !p) {
        LOG("  ! albedo: 映射失败 r=%d p=%p ⇒ 得到的内存类型多半不是 HOST_VISIBLE（这是真问题，不是崩溃 ✓）\n", mr, p);
        free(px); return 0;
    }
    memcpy(p, px, (size_t) N * N * 4);
    um(c->dev, stm);
    LOG("  · albedo: %d 字节已写入暂存缓冲 ✓\n", N * N * 4);
    free(px);
    PFN_vkBeginCommandBuffer bcb = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer ecb = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier bar = (PFN_vkCmdPipelineBarrier) c->gdpa(c->dev, "vkCmdPipelineBarrier");
    /* 用自己的函数指针类型（不依赖头文件里的 typedef 形态 ✓）*/
    typedef void (*PFN_c2i_own)(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy *);
    PFN_c2i_own c2i = (PFN_c2i_own) c->gdpa(c->dev, "vkCmdCopyBufferToImage");
    PFN_vkQueueSubmit qs = (PFN_vkQueueSubmit) c->gdpa(c->dev, "vkQueueSubmit");
    PFN_vkWaitForFences wf = (PFN_vkWaitForFences) c->gdpa(c->dev, "vkWaitForFences");
    PFN_vkResetFences rf = (PFN_vkResetFences) c->gdpa(c->dev, "vkResetFences");
    if (!bcb || !ecb || !bar || !c2i || !qs || !wf || !rf) { LOG("  ! albedo: 缺命令入口\n"); return 0; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    bcb(g_litx.cb, &bi);
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = g_litx.albedo, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    bar(g_litx.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { N, N, 1 } };
    c2i(g_litx.cb, st, g_litx.albedo, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);   /* v9.7 修：去掉多传的 layout，改回标准 6 参数 ABI ✓ */
    VkImageMemoryBarrier b2 = b1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar(g_litx.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    ecb(g_litx.cb);
    rf(c->dev, 1, &g_litx.fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_litx.cb };
    return qs(c->q, 1, &si, g_litx.fence) == VK_SUCCESS && wf(c->dev, 1, &g_litx.fence, VK_TRUE, 1000000000ULL) == VK_SUCCESS;
}

/* 描述符：binding0 = 比较采样器 + 阴影贴图；binding1 = 采样器 + albedo */
static int lit_mk_desc(Ctx *c) {
    PFN_vkCreateDescriptorSetLayout mkdl = (PFN_vkCreateDescriptorSetLayout) c->gdpa(c->dev, "vkCreateDescriptorSetLayout");
    PFN_vkCreateDescriptorPool mkdp = (PFN_vkCreateDescriptorPool) c->gdpa(c->dev, "vkCreateDescriptorPool");
    PFN_vkAllocateDescriptorSets ads = (PFN_vkAllocateDescriptorSets) c->gdpa(c->dev, "vkAllocateDescriptorSets");
    PFN_vkUpdateDescriptorSets uds = (PFN_vkUpdateDescriptorSets) c->gdpa(c->dev, "vkUpdateDescriptorSets");
    if (!mkdl || !mkdp || !ads || !uds) return 0;
    VkDescriptorSetLayoutBinding b[2]; memset(b, 0, sizeof(b));
    for (int i = 0; i < 2; i++) {
        b[i].binding = (uint32_t) i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b };
    if (mkdl(c->dev, &dl, NULL, &g_litx.dsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 };
    VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (mkdp(c->dev, &dp, NULL, &g_litx.dpool) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_litx.dpool, .descriptorSetCount = 1, .pSetLayouts = &g_litx.dsl };
    if (ads(c->dev, &da, &g_litx.dset) != VK_SUCCESS) return 0;
    VkDescriptorImageInfo ii[2] = {
        { g_litx.shadowSampler, g_litx.shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { g_litx.colorSampler,  g_litx.albedoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    VkWriteDescriptorSet w[2]; memset(w, 0, sizeof(w));
    for (int i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = g_litx.dset;
        w[i].dstBinding = (uint32_t) i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i];
    }
    uds(c->dev, 2, w, 0, NULL);
    /* 后处理用的第二组：binding0 = 颜色画布（场景）、binding1 = 阴影图（调试视图用）✓ */
    VkDescriptorSetAllocateInfo da2 = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_litx.dpool, .descriptorSetCount = 1, .pSetLayouts = &g_litx.dsl };
    if (ads(c->dev, &da2, &g_litx.descPost) != VK_SUCCESS) return 0;
    VkDescriptorImageInfo ip[2] = {
        { g_litx.colorSampler, g_litx.colorView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { g_litx.shadowSampler, g_litx.shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    VkWriteDescriptorSet wp[2]; memset(wp, 0, sizeof(wp));
    for (int i = 0; i < 2; i++) {
        wp[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wp[i].dstSet = g_litx.descPost;
        wp[i].dstBinding = (uint32_t) i; wp[i].descriptorCount = 1;
        wp[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wp[i].pImageInfo = &ip[i];
    }
    uds(c->dev, 2, wp, 0, NULL);
    return 1;
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeLitInit(JNIEnv *env, jobject th, jstring jso, jint w, jint h) {
    LOG("光影入口: nativeLitInit 进入\n");
    (void) th;
    const char *path = jso ? (*env)->GetStringUTFChars(env, jso, NULL) : NULL;
    char out[300];
    if (g_litx.ok) { ctx_close(&g_litx.ctx); memset(&g_litx, 0, sizeof(g_litx)); }
    if (!ctx_open(&g_litx.ctx, path ? path : "system")) {
        if (path) (*env)->ReleaseStringUTFChars(env, jso, path);
        return (*env)->NewStringUTF(env, "光影：驱动不可用");
    }
    if (path) (*env)->ReleaseStringUTFChars(env, jso, path);
    Ctx *c = &g_litx.ctx;
    if ((int) w < 16 || (int) h < 16 || (int) w > 4096 || (int) h > 4096) {
        ctx_close(c); memset(&g_litx, 0, sizeof(g_litx));
        return (*env)->NewStringUTF(env, "光影：尺寸非法（需 16..4096）");
    }
    g_litx.w = (int) w; g_litx.h = (int) h;
    int sr = g_lit.shadowRes; if (sr < 512) sr = 1024; g_litx.shadowRes = sr;
    PFN_vkCreateCommandPool ccp = (PFN_vkCreateCommandPool) c->gdpa(c->dev, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers acb = (PFN_vkAllocateCommandBuffers) c->gdpa(c->dev, "vkAllocateCommandBuffers");
    PFN_vkCreateFence cf = (PFN_vkCreateFence) c->gdpa(c->dev, "vkCreateFence");
    if (!ccp || !acb || !cf) { ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：缺命令入口"); }
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,   /* v9.9：每帧反复 begin 同一 CB 必须可重置（原先缺 ⇒ VUID 违规 ⇒ 录制不生效 ⇒ 画面不动）✓ */
        .queueFamilyIndex = c->qfam };
    ccp(c->dev, &cpci, NULL, &g_litx.cpool);
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_litx.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    acb(c->dev, &cbai, &g_litx.cb);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    cf(c->dev, &fci, NULL, &g_litx.fence);
    /* 拆开链式调用：每一步单独判空 + 单独日志 ⇒ 崩溃/失败能精确落到某一步 ✓ */
    LOG("  [L1] 建阴影贴图 %dx%d (D32)\n", sr, sr);
    if (!mk_img(c, sr, sr, VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &g_litx.shadow, &g_litx.shadowMem)) {
        ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：阴影贴图创建失败（显存？）");
    }
    LOG("  [L2] 阴影贴图视图\n");
    g_litx.shadowView = mk_view(c, g_litx.shadow, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!g_litx.shadowView) { ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：阴影视图失败"); }
    LOG("  [L3] 建颜色画布 %dx%d\n", (int) w, (int) h);
    if (!mk_img(c, (int) w, (int) h, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &g_litx.color, &g_litx.colorMem)) {
        ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：颜色画布创建失败（显存？）");
    }
    LOG("  [L4] 颜色画布视图\n");
    g_litx.colorView = mk_view(c, g_litx.color, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!g_litx.colorView) { ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：颜色视图失败"); }
    LOG("  [L5] 建后处理画布 %dx%d\n", (int) w, (int) h);
    if (!mk_img(c, (int) w, (int) h, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &g_litx.post, &g_litx.postMem)) {
        ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：后处理画布创建失败（显存？）");
    }
    LOG("  [L6] 后处理画布视图\n");
    g_litx.postView = mk_view(c, g_litx.post, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!g_litx.postView) { ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：后处理视图失败"); }
    g_litx.shadowSampler = mk_sampler(c, 1);
    g_litx.colorSampler = mk_sampler(c, 0);
    if (!lit_mk_albedo(c)) { ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：albedo 纹理上传失败"); }
    if (!mk_rp_depth(c, &g_litx.rpDepth) || !mk_rp_scene(c, &g_litx.rpScene) || !mk_rp_post(c, &g_litx.rpPost)) {
        ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：render pass 创建失败");
    }
    if (!lit_mk_desc(c)) { ctx_close(c); memset(&g_litx, 0, sizeof(g_litx)); return (*env)->NewStringUTF(env, "光影：描述符创建失败"); }
    g_litx.ok = 1;
    snprintf(out, sizeof(out), "光影就绪 ✓ 阴影图 %d² · 画布 %dx%d · albedo 64² · 三条 pass（深度/光照/后处理）",
             sr, (int) w, (int) h);
    return (*env)->NewStringUTF(env, out);
}


/* 紧凑的 look-at × perspective（列主序 ✓ 不做通用求逆）*/
static void lit_mvp(float *m, float aspect, const float *eye, const float *tgt, float fovy) {
    float f[3] = { tgt[0]-eye[0], tgt[1]-eye[1], tgt[2]-eye[2] };
    float fl = sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); if (fl < 1e-6f) fl = 1;
    for (int i = 0; i < 3; i++) f[i] /= fl;
    float up[3] = { 0, 1, 0 };
    float r[3] = { f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0] };
    float rl = sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]); if (rl < 1e-6f) { r[0]=1; r[1]=0; r[2]=0; rl=1; }
    for (int i = 0; i < 3; i++) r[i] /= rl;
    float u[3] = { r[1]*f[2]-r[2]*f[1], r[2]*f[0]-r[0]*f[2], r[0]*f[1]-r[1]*f[0] };
    float t = tanf(fovy * 0.5f), n = 0.1f, fa = 100.0f;
    for (int i = 0; i < 16; i++) m[i] = 0;
    /* 列主序：view 的行点乘 + 投影缩放合并 */
    m[0] = r[0]/(t*aspect); m[4] = r[1]/(t*aspect); m[8]  = r[2]/(t*aspect);
    m[1] = u[0]/t;          m[5] = u[1]/t;          m[9]  = u[2]/t;
    m[2] = -f[0];           m[6] = -f[1];           m[10] = -f[2];
    m[12] = -(r[0]*eye[0]+r[1]*eye[1]+r[2]*eye[2])/(t*aspect);
    m[13] = -(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2])/t;
    m[14] =  (f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    m[15] = 1.0f;
    /* 深度：Vulkan 0..1（把 z 从 [-1,1] 映射，简化处理 ✓）*/
    m[10] = -f[2] * (fa/(fa-n)); m[14] = (f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]) * (fa/(fa-n)) - (fa*n/(fa-n));
}

static int lit_mk_pipes(Ctx *c) {
    PFN_vkCreateShaderModule mksh = (PFN_vkCreateShaderModule) c->gdpa(c->dev, "vkCreateShaderModule");
    PFN_vkCreatePipelineLayout mkpl = (PFN_vkCreatePipelineLayout) c->gdpa(c->dev, "vkCreatePipelineLayout");
    PFN_vkCreateGraphicsPipelines mkgp = (PFN_vkCreateGraphicsPipelines) c->gdpa(c->dev, "vkCreateGraphicsPipelines");
    if (!mksh || !mkpl || !mkgp) return 0;
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(LitPC) };
    VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (mkpl(c->dev, &pl, NULL, &g_litx.plScene) != VK_SUCCESS) return 0;

    VkPipelineShaderStageCreateInfo st; 
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    /* ① 深度管线（阴影 pass ✓ 无颜色附件）*/
    { VkShaderModuleCreateInfo sm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = depth_vert_spv_len * 4, .pCode = depth_vert_spv };
      VkShaderModule mod = NULL; if (mksh(c->dev, &sm, NULL, &mod) != VK_SUCCESS) return 0;
      st = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = mod, .pName = "main" };
      VkPipelineColorBlendStateCreateInfo cb0 = cb; cb0.attachmentCount = 0; cb0.pAttachments = NULL;
      VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 1, .pStages = &st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb0, .pDepthStencilState = &ds, .pDynamicState = &dsi,
        .layout = g_litx.plScene, .renderPass = g_litx.rpDepth, .subpass = 0 };
      if (mkgp(c->dev, VK_NULL_HANDLE, 1, &gp, NULL, &g_litx.pipeDepth) != VK_SUCCESS) return 0; }
    /* ② 光照管线（lit_vert + lit_frag ✓）*/
    { VkShaderModuleCreateInfo sv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = lit_vert_spv_len * 4, .pCode = lit_vert_spv };
      VkShaderModuleCreateInfo sf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = lit_frag2_spv_len * 4, .pCode = lit_frag2_spv };   /* v2：动态 PCF + 线性 ✓ */
      VkShaderModule mv = NULL, mf = NULL;
      if (mksh(c->dev, &sv, NULL, &mv) != VK_SUCCESS || mksh(c->dev, &sf, NULL, &mf) != VK_SUCCESS) return 0;
      VkPipelineShaderStageCreateInfo st2[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = mv, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = mf, .pName = "main" } };
      VkPipelineDepthStencilStateCreateInfo ds2 = ds; ds2.depthTestEnable = VK_FALSE; ds2.depthWriteEnable = VK_FALSE;
      VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st2, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDepthStencilState = &ds2, .pDynamicState = &dsi,
        .layout = g_litx.plScene, .renderPass = g_litx.rpScene, .subpass = 0 };
      if (mkgp(c->dev, VK_NULL_HANDLE, 1, &gp, NULL, &g_litx.pipeScene) != VK_SUCCESS) return 0; }
    return 1;
    /* 后处理管线布局：同一套描述符（binding0=颜色画布 binding1=阴影图 ✓）+ 16 字节推常量 ✓ */
    VkPushConstantRange pcr2 = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                 .offset = 0, .size = sizeof(LitPC2) };
    VkPipelineLayoutCreateInfo pl2 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &g_litx.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr2 };
    if (mkpl(c->dev, &pl2, NULL, &g_litx.plPost) != VK_SUCCESS) return 0;
    { VkShaderModuleCreateInfo sv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = post_vert_spv_len * 4, .pCode = post_vert_spv };
      VkShaderModuleCreateInfo sf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = post_frag2_spv_len * 4, .pCode = post_frag2_spv };
      VkShaderModule mv = NULL, mf = NULL;
      if (mksh(c->dev, &sv, NULL, &mv) != VK_SUCCESS || mksh(c->dev, &sf, NULL, &mf) != VK_SUCCESS) return 0;
      VkPipelineShaderStageCreateInfo st3[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = mv, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = mf, .pName = "main" } };
      VkPipelineDepthStencilStateCreateInfo ds3 = ds; ds3.depthTestEnable = VK_FALSE; ds3.depthWriteEnable = VK_FALSE;
      VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st3, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDepthStencilState = &ds3, .pDynamicState = &dsi,
        .layout = g_litx.plPost, .renderPass = g_litx.rpPost, .subpass = 0 };
      if (mkgp(c->dev, VK_NULL_HANDLE, 1, &gp, NULL, &g_litx.pipePost) != VK_SUCCESS) return 0; }
}
static int lit_mk_fbs(Ctx *c) {
    PFN_vkCreateFramebuffer mk = (PFN_vkCreateFramebuffer) c->gdpa(c->dev, "vkCreateFramebuffer");
    if (!mk) return 0;
    VkImageView a = g_litx.shadowView;
    VkFramebufferCreateInfo f1 = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g_litx.rpDepth,
        .attachmentCount = 1, .pAttachments = &a, .width = (uint32_t) g_litx.shadowRes, .height = (uint32_t) g_litx.shadowRes, .layers = 1 };
    if (mk(c->dev, &f1, NULL, &g_litx.fbShadow) != VK_SUCCESS) return 0;
    VkImageView b = g_litx.colorView;
    VkFramebufferCreateInfo f2 = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g_litx.rpScene,
        .attachmentCount = 1, .pAttachments = &b, .width = (uint32_t) g_litx.w, .height = (uint32_t) g_litx.h, .layers = 1 };
    if (mk(c->dev, &f2, NULL, &g_litx.fbScene) != VK_SUCCESS) return 0;
    VkImageView pv = g_litx.postView;
    VkFramebufferCreateInfo f3 = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g_litx.rpPost,
        .attachmentCount = 1, .pAttachments = &pv, .width = (uint32_t) g_litx.w, .height = (uint32_t) g_litx.h, .layers = 1 };
    return mk(c->dev, &f3, NULL, &g_litx.fbPost) == VK_SUCCESS;
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeLitFrame(JNIEnv *env, jobject th) {
    LOG("光影入口: nativeLitFrame 进入\n");
    (void) th;
    char out[448];
    if (!g_litx.ok) return (*env)->NewStringUTF(env, "光影未初始化");
    Ctx *c = &g_litx.ctx;
    if (!g_litx.pipeScene) {
        if (!lit_mk_pipes(c)) return (*env)->NewStringUTF(env, "X 光影管线创建失败");
        if (!lit_mk_fbs(c))   return (*env)->NewStringUTF(env, "X 光影 framebuffer 创建失败");
        if (!g_litx.rbuf && !mk_buf(c, (VkDeviceSize) g_litx.w * g_litx.h * 4,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_litx.rbuf, &g_litx.rMem, 1))
            return (*env)->NewStringUTF(env, "X 光影回读缓冲失败");
        /* v8.8 修复：这里原先是 (Timer *)&g_litx.qp 的强转 —— 把约 48 字节的 Timer
         * 写进 8 字节的 VkQueryPool 字段 ⇒ 越界砸坏结构后面的内存 ⇒ 第一次
         * nativeLitFrame 就野指针崩（addr=0x6f）✗。真正的计时器在下面 static Timer ltm ✓ */
    }
    static Timer ltm; static int ltInit = 0;
    if (!ltInit) { ltInit = 1; if (!timer_init(c, &ltm, 2)) g_litx.tsOk = 0; else g_litx.tsOk = 1; }
    PFN_vkBeginCommandBuffer bcb = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer ecb = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdBeginRenderPass brp = (PFN_vkCmdBeginRenderPass) c->gdpa(c->dev, "vkCmdBeginRenderPass");
    PFN_vkCmdEndRenderPass erp = (PFN_vkCmdEndRenderPass) c->gdpa(c->dev, "vkCmdEndRenderPass");
    PFN_vkCmdBindPipeline bp = (PFN_vkCmdBindPipeline) c->gdpa(c->dev, "vkCmdBindPipeline");
    PFN_vkCmdBindDescriptorSets bds = (PFN_vkCmdBindDescriptorSets) c->gdpa(c->dev, "vkCmdBindDescriptorSets");
    PFN_vkCmdPushConstants pc_ = (PFN_vkCmdPushConstants) c->gdpa(c->dev, "vkCmdPushConstants");
    PFN_vkCmdDraw dr = (PFN_vkCmdDraw) c->gdpa(c->dev, "vkCmdDraw");
    PFN_vkCmdSetViewport sv = (PFN_vkCmdSetViewport) c->gdpa(c->dev, "vkCmdSetViewport");
    PFN_vkCmdSetScissor ss = (PFN_vkCmdSetScissor) c->gdpa(c->dev, "vkCmdSetScissor");
    PFN_vkCmdPipelineBarrier bar = (PFN_vkCmdPipelineBarrier) c->gdpa(c->dev, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImageToBuffer c2b = (PFN_vkCmdCopyImageToBuffer) c->gdpa(c->dev, "vkCmdCopyImageToBuffer");
    PFN_vkQueueSubmit qs = (PFN_vkQueueSubmit) c->gdpa(c->dev, "vkQueueSubmit");
    PFN_vkWaitForFences wf = (PFN_vkWaitForFences) c->gdpa(c->dev, "vkWaitForFences");
    PFN_vkResetFences rf = (PFN_vkResetFences) c->gdpa(c->dev, "vkResetFences");
    if (!bcb||!ecb||!brp||!erp||!bp||!bds||!pc_||!dr||!sv||!ss||!bar||!c2b||!qs||!wf||!rf)
        return (*env)->NewStringUTF(env, "X 光影命令入口缺失");
    float eye[3] = { 4.0f, 3.2f, 4.0f }, tgt[3] = { 0, 1.0f, 0 };
    float lightEye[3] = { 4.0f, 6.0f, 2.0f };
    LitPC pk; memset(&pk, 0, sizeof(pk));
    lit_mvp(pk.mvp, (float) g_litx.w / (float) g_litx.h, eye, tgt, 0.9f);
    lit_mvp(pk.lightVP, 1.0f, lightEye, tgt, 1.2f);
    int nInst = g_lit.cubes; pk.nInst = nInst;
    pk.t = g_lit.animate ? (float) (now_ms() / 1000.0) : 0.0f;   /* 动画开关 ✓ */
    pk.pcf = g_lit.pcfTaps;                                      /* PCF 质量开关 ✓ */
    pk.shadows = g_lit.shadows;                                  /* 阴影开关 ✓ */
    pk.shadowRes = g_litx.shadowRes;                             /* 阴影图分辨率开关 ✓ */
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    double t0 = now_ms();
    if (bcb(g_litx.cb, &bi) != VK_SUCCESS) return (*env)->NewStringUTF(env, "X begin 失败");
    if (g_litx.tsOk) timer_begin(&ltm, g_litx.cb);
    /* ① 阴影 pass */
    VkClearValue cd; memset(&cd, 0, sizeof(cd)); cd.depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
    VkRenderPassBeginInfo r1 = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g_litx.rpDepth,
        .framebuffer = g_litx.fbShadow, .renderArea = { {0,0}, { (uint32_t) g_litx.shadowRes, (uint32_t) g_litx.shadowRes } },
        .clearValueCount = 1, .pClearValues = &cd };
    brp(g_litx.cb, &r1, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v1 = { 0, 0, (float) g_litx.shadowRes, (float) g_litx.shadowRes, 0, 1 };
    VkRect2D s1 = { {0,0}, { (uint32_t) g_litx.shadowRes, (uint32_t) g_litx.shadowRes } };
    sv(g_litx.cb, 0, 1, &v1); ss(g_litx.cb, 0, 1, &s1);
    bp(g_litx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_litx.pipeDepth);
    pc_(g_litx.cb, g_litx.plScene, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pk), &pk);
    dr(g_litx.cb, 36, (uint32_t) nInst, 0, 0);
    erp(g_litx.cb);
    /* ② 光照 pass（阴影图已被 render pass 转到 SHADER_READ_ONLY ✓）*/
    VkClearValue cc; memset(&cc, 0, sizeof(cc)); cc.color.float32[0] = 0.05f; cc.color.float32[1] = 0.06f; cc.color.float32[2] = 0.08f; cc.color.float32[3] = 1;
    VkViewport v2 = { 0, 0, (float) g_litx.w, (float) g_litx.h, 0, 1 };
    VkRect2D s2 = { {0,0}, { (uint32_t) g_litx.w, (uint32_t) g_litx.h } };
    int reps = g_lit.passesPerFrame; if (reps < 1) reps = 1;    /* 负载倍率开关 ✓ 真的重复跑 */
    for (int rep = 0; rep < reps; rep++) {
        VkRenderPassBeginInfo r2 = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g_litx.rpScene,
            .framebuffer = g_litx.fbScene, .renderArea = { {0,0}, { (uint32_t) g_litx.w, (uint32_t) g_litx.h } },
            .clearValueCount = 1, .pClearValues = &cc };
        brp(g_litx.cb, &r2, VK_SUBPASS_CONTENTS_INLINE);
        sv(g_litx.cb, 0, 1, &v2); ss(g_litx.cb, 0, 1, &s2);
        bp(g_litx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_litx.pipeScene);
        bds(g_litx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_litx.plScene, 0, 1, &g_litx.dset, 0, NULL);
        pc_(g_litx.cb, g_litx.plScene, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pk), &pk);
        dr(g_litx.cb, 36, (uint32_t) nInst, 0, 0);
        erp(g_litx.cb);
    }
    /* ③ 后处理 pass（泛光 / 色调映射 / 调试视图 —— 三个开关在这里生效 ✓）*/
    if (g_litx.pipePost) {
        VkRenderPassBeginInfo r3 = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g_litx.rpPost,
            .framebuffer = g_litx.fbPost, .renderArea = { {0,0}, { (uint32_t) g_litx.w, (uint32_t) g_litx.h } },
            .clearValueCount = 1, .pClearValues = &cc };
        brp(g_litx.cb, &r3, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport v3 = { 0, 0, (float) g_litx.w, (float) g_litx.h, 0, 1 };
        VkRect2D s3 = { {0,0}, { (uint32_t) g_litx.w, (uint32_t) g_litx.h } };
        sv(g_litx.cb, 0, 1, &v3); ss(g_litx.cb, 0, 1, &s3);
        bp(g_litx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_litx.pipePost);
        bds(g_litx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_litx.plPost, 0, 1, &g_litx.descPost, 0, NULL);
        LitPC2 p2 = { g_lit.bloom, g_lit.tonemap, g_lit.debugView, 1.0f / (float) (g_litx.w > 0 ? g_litx.w : 1) };
        pc_(g_litx.cb, g_litx.plPost, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p2), &p2);
        dr(g_litx.cb, 3, 1, 0, 0);              /* 全屏三角形 ✓ */
        erp(g_litx.cb);
    }
    if (g_litx.tsOk) timer_end(&ltm, g_litx.cb);
    /* ④ 回读（后处理画布的 render pass 结束布局已是 TRANSFER_SRC ⇒ 无需额外屏障 ✓；没建后处理时退回颜色画布 ✓）*/
    VkImage srcImg = g_litx.pipePost ? g_litx.post : g_litx.color;
    if (!g_litx.pipePost) {
        VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = g_litx.color, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        bar(g_litx.cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    }
    VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t) g_litx.w, (uint32_t) g_litx.h, 1 } };
    c2b(g_litx.cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_litx.rbuf, 1, &bc);
    ecb(g_litx.cb);
    rf(c->dev, 1, &g_litx.fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_litx.cb };
    VkResult r = qs(c->q, 1, &si, g_litx.fence);
    if (r == VK_SUCCESS) r = wf(c->dev, 1, &g_litx.fence, VK_TRUE, 3000000000ULL);
    double t1 = now_ms();
    double gms = g_litx.tsOk ? timer_result_ms(c, &ltm) : -1.0;
    /* v9.9 诊断：把回读缓冲映射一下采 3 个统计量 ⇒ 下次若还是没画面，一眼分辨
     *   「GPU 没写像素」还是「写进去了但是黑的」✓ */
    double pxmin = -1, pxmax = -1, pxavg = -1;
    {
        PFN_vkMapMemory pmp = (PFN_vkMapMemory) c->gdpa(c->dev, "vkMapMemory");
        PFN_vkUnmapMemory pum = (PFN_vkUnmapMemory) c->gdpa(c->dev, "vkUnmapMemory");
        void *pp = NULL;
        if (pmp && pum && pmp(c->dev, g_litx.rMem, 0, VK_WHOLE_SIZE, 0, &pp) == VK_SUCCESS && pp) {
            unsigned char *bb = (unsigned char *) pp;
            size_t tot = (size_t) g_litx.w * g_litx.h * 4, sum = 0, cnt = 0; int mn = 255, mx = 0;
            for (size_t i = 0; i < tot; i += 64) { int v = bb[i]; if (v < mn) mn = v; if (v > mx) mx = v; sum += (size_t) v; cnt++; }
            if (cnt) { pxmin = mn; pxmax = mx; pxavg = (double) sum / (double) cnt; }
            pum(c->dev, g_litx.rMem);
        }
    }
    if (r != VK_SUCCESS)
        snprintf(out, sizeof(out), "X 光影提交失败 r=%d（%s）实例=%d —— 这一帧保留", r,
                 r == -4 ? "VK_ERROR_DEVICE_LOST" : "见 VkResult 表", nInst);
    else
        snprintf(out, sizeof(out), "光影完成 ✓ 2 pass（深度 %d² + 光照 %dx%d）· 实例 %d · 阴影=%s PCF=%d · GPU %.2f ms · 墙钟 %.2f ms · %.1f FPS · 占空比 %.1f%% · 像素 min %.0f/max %.0f/均 %.1f",
                 g_litx.shadowRes, g_litx.w, g_litx.h, nInst, g_lit.shadows ? "开" : "关", g_lit.pcfTaps,
                 gms, t1 - t0, (t1 - t0) > 0 ? 1000.0 / (t1 - t0) : 0.0, (gms > 0 && (t1 - t0) > 0) ? 100.0 * gms / (t1 - t0) : 0.0, pxmin, pxmax, pxavg);
    return (*env)->NewStringUTF(env, out);
}
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeLitBlit(JNIEnv *env, jobject th, jobject bmp) {
    LOG("光影入口: nativeLitBlit 进入\n");
    (void) th;
    if (!g_litx.ok || !g_litx.rbuf) return (*env)->NewStringUTF(env, "光影无图像");
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS) return (*env)->NewStringUTF(env, "取位图信息失败");
    void *pix = NULL;
    if (AndroidBitmap_lockPixels(env, bmp, &pix) != ANDROID_BITMAP_RESULT_SUCCESS) return (*env)->NewStringUTF(env, "锁定位图失败");
    PFN_vkMapMemory mp = (PFN_vkMapMemory) g_litx.ctx.gdpa(g_litx.ctx.dev, "vkMapMemory");
    PFN_vkUnmapMemory um = (PFN_vkUnmapMemory) g_litx.ctx.gdpa(g_litx.ctx.dev, "vkUnmapMemory");
    void *src = NULL;
    if (mp && mp(g_litx.ctx.dev, g_litx.rMem, 0, VK_WHOLE_SIZE, 0, &src) == VK_SUCCESS && src) {
        int cw = g_litx.w < (int) info.width ? g_litx.w : (int) info.width;
        int ch = g_litx.h < (int) info.height ? g_litx.h : (int) info.height;
        for (int y = 0; y < ch; y++) {
            uint8_t *dst = (uint8_t *) pix + (size_t) y * info.stride;
            memcpy(dst, (const uint8_t *) src + (size_t) y * g_litx.w * 4, (size_t) cw * 4);
        }
        um(g_litx.ctx.dev, g_litx.rMem);
    }
    AndroidBitmap_unlockPixels(env, bmp);
    return (*env)->NewStringUTF(env, "已拷入位图");
}
JNIEXPORT void JNICALL
Java_com_dsh_gputest_MainActivity_nativeLitStop(JNIEnv *env, jobject th) {
    LOG("光影入口: nativeLitStop 进入\n");
    (void) th;
    if (g_litx.ok) { ctx_close(&g_litx.ctx); memset(&g_litx, 0, sizeof(g_litx)); }
    /* 返回类型已改为 void（与 Java 声明一致 ✓）*/
}


// ============================================================================
//  v8.1 · CPU 光追参考图（逐像素光线求交，**纯 CPU** ⇒ 任何驱动都能出）
//  用途：驱动不支持硬件光追时（如 PanVK）也有真产出；结果必须标注「非硬件光追」✓
//  实现：地面 + 3 球，主光线 + 4 次抖动阴影采样（软阴影）+ 1 次反射 ✓
// ============================================================================
static uint32_t *g_cpurt = NULL; static int g_cpurt_w = 0, g_cpurt_h = 0;
typedef struct { float r, g, b; } C3;
static const float RT_EYE[3] = { 3.0f, 2.6f, 4.2f };
static const float RT_LIGHT[3] = { 4.5f, 6.0f, 3.0f };
static const float RT_SPH[3][4] = {           /* x,y,z,r */
    { -1.30f, 0.85f, 0.10f, 0.85f },
    {  0.25f, 0.60f,-0.90f, 0.60f },
    {  1.35f, 1.00f, 0.55f, 1.00f } };
static const C3 RT_COL[3] = { { 0.85f, 0.25f, 0.25f }, { 0.25f, 0.75f, 0.35f }, { 0.30f, 0.45f, 0.90f } };

static int rt_hit_spheres(const float *o, const float *d, float *t, float *n, int *mi) {
    int hit = 0; float best = 1e30f;
    for (int i = 0; i < 3; i++) {
        float ox = o[0]-RT_SPH[i][0], oy = o[1]-RT_SPH[i][1], oz = o[2]-RT_SPH[i][2];
        float b = ox*d[0]+oy*d[1]+oz*d[2];
        float c = ox*ox+oy*oy+oz*oz - RT_SPH[i][3]*RT_SPH[i][3];
        float disc = b*b - c;
        if (disc < 0) continue;
        float q = -b - sqrtf(disc);
        if (q > 1e-3f && q < best) {
            best = q; hit = 1; *mi = i;
            float px = o[0]+d[0]*q-RT_SPH[i][0], py = o[1]+d[1]*q-RT_SPH[i][1], pz = o[2]+d[2]*q-RT_SPH[i][2];
            float l = sqrtf(px*px+py*py+pz*pz); if (l < 1e-6f) l = 1;
            n[0] = px/l; n[1] = py/l; n[2] = pz/l;
        }
    }
    if (hit) *t = best;
    return hit;
}
static int rt_hit(const float *o, const float *d, float *t, float *n, C3 *col) {
    float ts, ns[3]; int mi = 0;
    int hs = rt_hit_spheres(o, d, &ts, ns, &mi);
    float tp = 1e30f;
    if (d[1] < -1e-6f) { float q = -(o[1] - 0.0f) / d[1]; if (q > 1e-3f) tp = q; }
    if (!hs && tp >= 1e30f) return 0;
    if (hs && ts < tp) { *t = ts; n[0]=ns[0]; n[1]=ns[1]; n[2]=ns[2]; *col = RT_COL[mi]; }
    else {
        *t = tp;
        float px = o[0]+d[0]*tp, pz = o[2]+d[2]*tp;
        n[0] = 0; n[1] = 1; n[2] = 0;
        int chk = (((int) floorf(px * 1.5f)) + ((int) floorf(pz * 1.5f))) & 1;
        col->r = chk ? 0.75f : 0.55f; col->g = chk ? 0.75f : 0.55f; col->b = chk ? 0.78f : 0.58f;
    }
    return 1;
}
static int rt_occluded(const float *p, const float *ld) {
    float t, n[3]; C3 c;
    return rt_hit(p, ld, &t, n, &c) && t < 1e30f;
}
static C3 rt_trace(const float *o, const float *d, int depth) {
    float t, n[3]; C3 albedo;
    C3 out = { 0.05f, 0.07f, 0.11f };                 /* 背景 */
    if (!rt_hit(o, d, &t, n, &albedo)) return out;
    float p[3] = { o[0]+d[0]*t, o[1]+d[1]*t, o[2]+d[2]*t };
    float L[3] = { RT_LIGHT[0]-p[0], RT_LIGHT[1]-p[1], RT_LIGHT[2]-p[2] };
    float ll = sqrtf(L[0]*L[0]+L[1]*L[1]+L[2]*L[2]); if (ll < 1e-6f) ll = 1;
    L[0]/=ll; L[1]/=ll; L[2]/=ll;
    float ndl = n[0]*L[0]+n[1]*L[1]+n[2]*L[2]; if (ndl < 0) ndl = 0;
    /* 软阴影：4 次抖动 ✓ */
    float sh = 0.0f;
    const float j[4][2] = { { 0,0 }, { 0.06f,0 }, { 0,0.06f }, { -0.04f,-0.04f } };
    for (int k = 0; k < 4; k++) {
        float ld[3] = { L[0]+j[k][0], L[1], L[2]+j[k][1] };
        float l2 = sqrtf(ld[0]*ld[0]+ld[1]*ld[1]+ld[2]*ld[2]); if (l2<1e-6f) l2=1;
        ld[0]/=l2; ld[1]/=l2; ld[2]/=l2;
        float pp[3] = { p[0]+n[0]*0.002f, p[1]+n[1]*0.002f, p[2]+n[2]*0.002f };
        if (!rt_occluded(pp, ld)) sh += 0.25f;
    }
    float amb = 0.18f, diff = ndl * sh;
    out.r = albedo.r * (amb + 0.9f * diff); out.g = albedo.g * (amb + 0.9f * diff); out.b = albedo.b * (amb + 0.9f * diff);
    /* 一次反射 ✓ */
    if (depth < 1) {
        float dn = d[0]*n[0]+d[1]*n[1]+d[2]*n[2];
        float rd[3] = { d[0]-2*dn*n[0], d[1]-2*dn*n[1], d[2]-2*dn*n[2] };
        float rp[3] = { p[0]+n[0]*0.003f, p[1]+n[1]*0.003f, p[2]+n[2]*0.003f };
        C3 rc = rt_trace(rp, rd, depth + 1);
        out.r += rc.r * 0.28f * albedo.r; out.g += rc.g * 0.28f * albedo.g; out.b += rc.b * 0.28f * albedo.b;
    }
    return out;
}
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCpuRtInit(JNIEnv *env, jobject th, jint w, jint h) {
    LOG("原生入口: nativeCpuRtInit 进入\n");
    (void) th;
    int W = (int) w, H = (int) h;
    if (W < 16) W = 256; if (H < 16) H = 256;
    free(g_cpurt); g_cpurt = (uint32_t *) malloc((size_t) W * H * 4);
    g_cpurt_w = W; g_cpurt_h = H;
    char out[128];
    snprintf(out, sizeof(out), g_cpurt ? "CPU 光追缓冲就绪 %dx%d（非硬件光追 ✓）" : "CPU 光追分配失败", W, H);
    return (*env)->NewStringUTF(env, out);
}
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCpuRtRender(JNIEnv *env, jobject th, jint frame) {
    LOG("原生入口: nativeCpuRtRender 进入\n");
    (void) th;
    if (!g_cpurt) return (*env)->NewStringUTF(env, "CPU 光追未初始化");
    int W = g_cpurt_w, H = g_cpurt_h;
    float aspect = (float) W / (float) H;
    float fwd[3] = { -RT_EYE[0], 1.0f - RT_EYE[1], -RT_EYE[2] };
    float fl = sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]); fwd[0]/=fl; fwd[1]/=fl; fwd[2]/=fl;
    float rt[3] = { fwd[2], 0, -fwd[0] };
    float rl = sqrtf(rt[0]*rt[0]+rt[2]*rt[2]); if (rl < 1e-6f) rl = 1; rt[0]/=rl; rt[2]/=rl;
    float up[3] = { rt[1]*fwd[2]-rt[2]*fwd[1], rt[2]*fwd[0]-rt[0]*fwd[2], rt[0]*fwd[1]-rt[1]*fwd[0] };
    float th2 = tanf(0.9f * 0.5f);
    double t0 = now_ms();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float u = ((float) x + 0.5f) / (float) W * 2.0f - 1.0f;
            float v = 1.0f - ((float) y + 0.5f) / (float) H * 2.0f;
            float d[3] = { fwd[0] + rt[0]*u*th2*aspect + up[0]*v*th2,
                           fwd[1] + rt[1]*u*th2*aspect + up[1]*v*th2,
                           fwd[2] + rt[2]*u*th2*aspect + up[2]*v*th2 };
            float dl = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); d[0]/=dl; d[1]/=dl; d[2]/=dl;
            C3 c = rt_trace(RT_EYE, d, 0);
            /* 色调映射 + 伽马（CPU 侧也走同一套 ✓）*/
            c.r = c.r/(c.r+1.0f); c.g = c.g/(c.g+1.0f); c.b = c.b/(c.b+1.0f);
            c.r = powf(c.r, 1.0f/2.2f); c.g = powf(c.g, 1.0f/2.2f); c.b = powf(c.b, 1.0f/2.2f);
            g_cpurt[(size_t) y * W + x] = 0xFF000000u
                | ((uint32_t) (c.r * 255.0f) << 16) | ((uint32_t) (c.g * 255.0f) << 8) | (uint32_t) (c.b * 255.0f);
        }
    }
    double dt = now_ms() - t0;
    char out[220];
    snprintf(out, sizeof(out),
        "CPU 光追完成（**非硬件光追** ✓）%dx%d · 地面+3 球 · 4 次软阴影采样 + 1 次反射 · 用时 %.0f ms · %.1f 万光线/s · frame=%d",
        W, H, dt, dt > 0 ? (double) W * H / dt / 100.0 : 0.0, (int) frame);
    return (*env)->NewStringUTF(env, out);
}
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCpuRtBlit(JNIEnv *env, jobject th, jobject bmp) {
    LOG("原生入口: nativeCpuRtBlit 进入\n");
    (void) th;
    if (!g_cpurt) return (*env)->NewStringUTF(env, "CPU 光追无图像");
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS) return (*env)->NewStringUTF(env, "取位图信息失败");
    void *pix = NULL;
    if (AndroidBitmap_lockPixels(env, bmp, &pix) != ANDROID_BITMAP_RESULT_SUCCESS) return (*env)->NewStringUTF(env, "锁定位图失败");
    int cw = g_cpurt_w < (int) info.width ? g_cpurt_w : (int) info.width;
    int ch = g_cpurt_h < (int) info.height ? g_cpurt_h : (int) info.height;
    for (int y = 0; y < ch; y++) {
        uint8_t *dst = (uint8_t *) pix + (size_t) y * info.stride;
        memcpy(dst, (const uint8_t *) (g_cpurt + (size_t) y * g_cpurt_w), (size_t) cw * 4);
    }
    AndroidBitmap_unlockPixels(env, bmp);
    return (*env)->NewStringUTF(env, "已拷入位图");
}


// ============================================================================
//  v8.4 · 转译链（渲染器 × 驱动）第一步：准备运行环境
//  原理（学 FCL/ZL2）：渲染器（Zink/MobileGL 等，说的是 GL/EGL）会去 dlopen("libvulkan.so")，
//  我们**把转发垫片打包成 libvulkan.so** ⇒ 它先命中我们的垫片 ⇒ 垫片按 DRIVER_PATH 转发到
//  **用户选中的 Vulkan 驱动** ✓。本函数只做"设环境 + 校验垫片在位"，**不加载渲染器** ✓
//  （加载 GL 实现有崩溃风险，那一步单独设计、单独开关 ✓）
// ============================================================================
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeTranscodePrepare(JNIEnv *env, jobject th, jstring jdrv) {
    (void) th;
    const char *drv = jdrv ? (*env)->GetStringUTFChars(env, jdrv, NULL) : NULL;
    char out[420];
    if (!drv || !*drv) {
        if (drv) (*env)->ReleaseStringUTFChars(env, jdrv, drv);
        return (*env)->NewStringUTF(env, "转译准备：没有选中的驱动（先在①里选一个 ✓）");
    }
    /* ① 垫片是否随包在位（我们打进 lib/arm64-v8a/libvulkan.so ✓）*/
    int shim = 0;
    { void *h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
      if (h) { shim = dlsym(h, "vkGetInstanceProcAddr") ? 1 : 0; }
      /* 刻意不 dlclose：垫片要被后续渲染器复用 ✓ */ }
    /* ② 关键：把选中驱动告诉垫片（vkshim_icd.c 读的就是 DRIVER_PATH ✓）*/
    setenv("DRIVER_PATH", drv, 1);
    /* ③ 顺手把渲染器/驱动都可能用到的环境也备好（学启动器的做法 ✓ 但**不猜**渲染器私有变量 ✗）*/
    setenv("VK_LOADER_DEBUG", "error", 1);
    snprintf(out, sizeof(out),
        "转译环境就绪 ✓\n"
        "  DRIVER_PATH = %s\n"
        "  转发垫片 libvulkan.so：%s\n"
        "  说明：渲染器（Zink/MobileGL 等）一旦调用 dlopen(\"libvulkan.so\")，就会命中我们的垫片，\n"
        "        再由它把全部 Vulkan 调用转发到上面这个驱动 ✓（与 FCL/ZL2 同一套机制 ✓）",
        drv, shim ? "已在包内并可加载 ✓（垫片已接管 libvulkan.so ✓）"
                  : "不在包内 ✗（构建时少了它 ⇒ 转译链无法工作，请检查 lib/arm64-v8a/libvulkan.so）");
    (*env)->ReleaseStringUTFChars(env, jdrv, drv);
    return (*env)->NewStringUTF(env, out);
}

// ---------------------------------------------------------------- 跑分内核
typedef struct { double secs; unsigned long long ops, subs, fails; double sum_us; double gpu_ms; int gpu_ok; double bytes_per_op; double pixels_per_op; } BenchStat;

static void bench_report(const char *label, BenchStat *b) {
    double dt = b->secs;
    LOG("\n--- %s ---\n", label);
    LOG("时长=%.2fs  提交=%llu  操作=%llu  失败=%llu\n", dt, b->subs, b->ops, b->fails);
    if (dt <= 0) return;
    LOG("墙钟=%.2f s%s\n", dt, b->gpu_ok ? "" : "（本次无 GPU 时间戳）");
    if (b->gpu_ok && b->gpu_ms > 0)
        LOG("★ GPU 时间=%.2f ms（%.1f %% 墙钟）⇒ 每次操作 %.4f µs\n",
            b->gpu_ms, 100.0 * b->gpu_ms / (dt * 1000.0), b->ops ? b->gpu_ms * 1000.0 / b->ops : 0.0);
    if (b->gpu_ok && b->gpu_ms > 0)
        LOG("GPU 占空比=%.1f %%（= GPU 时间 / 墙钟；≈100%% 才说明真在压 GPU）\n",
            100.0 * b->gpu_ms / (dt * 1000.0));
    LOG("提交吞吐=%.1f 次/s   提交往返=%.1f µs（含 CPU↔GPU 往返，不等于 GPU 时间）\n", b->subs / dt, b->subs ? b->sum_us / b->subs : 0.0);
    if (b->fails) {   /* v2.0(B10)：有失败就不输出吞吐，免得被截图误读成"能跑" */
        LOG("（本次有失败 ⇒ 不输出吞吐数字，失败率才是结论）\n");
        return;
    }
    /* v6.0：成绩**只用 GPU 时间**（分母 = GPU 秒）—— 墙钟含 CPU 提交往返，
     * 拿它当成绩会让"CPU 慢"被误读成"GPU 慢" ✗ */
    double den = (b->gpu_ok && b->gpu_ms > 0) ? b->gpu_ms / 1000.0 : dt;
    if (b->gpu_ok && b->gpu_ms > 0)
        LOG("※ 以下成绩按 **GPU 时间** 计（不含 CPU 提交往返）\n");
    else
        LOG("※ 该驱动无 GPU 时间戳 ⇒ 成绩只能按墙钟计（含 CPU 影响，仅供参考）\n");
    if (b->pixels_per_op > 0)
        LOG("吞吐=%.1f Mpixel/s  (%.2f Gpixel/s)\n",
            b->ops * b->pixels_per_op / 1e6 / den, b->ops * b->pixels_per_op / 1e9 / den);
    if (b->bytes_per_op > 0)
        LOG("有效带宽=%.2f GB/s（已按「读+写」计；单看一侧要除以 2）\n", b->ops * b->bytes_per_op / 1e9 / den);
}

// fill：全屏三角形 + 可放大片元负载（**不是** vkCmdClearColorImage —— 清屏不是填充测试）
// 依据 glmark2 的 fragment-steps 思路：用 push constant 放大每像素负载
// 计时用 GPU 时间戳（Khronos timestamp_queries 口径）
/* v6.5：成绩分母 —— 有 GPU 时间戳就用 GPU 时间（否则退回墙钟） */
static double score_den(const BenchStat *b) {
    return (b->gpu_ok && b->gpu_ms > 0) ? b->gpu_ms / 1000.0 : b->secs;
}

static int bench_fill(Ctx *c, Target *t, double seconds, BenchStat *st) {
    PFN_vkCreateShaderModule mksh = (PFN_vkCreateShaderModule) c->gdpa(c->dev, "vkCreateShaderModule");
    PFN_vkCreatePipelineLayout mkpl = (PFN_vkCreatePipelineLayout) c->gdpa(c->dev, "vkCreatePipelineLayout");
    PFN_vkCreateGraphicsPipelines mkgp = (PFN_vkCreateGraphicsPipelines) c->gdpa(c->dev, "vkCreateGraphicsPipelines");
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdBeginRenderPass brp = (PFN_vkCmdBeginRenderPass) c->gdpa(c->dev, "vkCmdBeginRenderPass");
    PFN_vkCmdEndRenderPass erp = (PFN_vkCmdEndRenderPass) c->gdpa(c->dev, "vkCmdEndRenderPass");
    PFN_vkCmdBindPipeline bp = (PFN_vkCmdBindPipeline) c->gdpa(c->dev, "vkCmdBindPipeline");
    PFN_vkCmdPushConstants pc = (PFN_vkCmdPushConstants) c->gdpa(c->dev, "vkCmdPushConstants");
    PFN_vkCmdDraw draw = (PFN_vkCmdDraw) c->gdpa(c->dev, "vkCmdDraw");
    if (!mksh || !mkpl || !mkgp || !begin || !end || !brp || !erp || !bp || !pc || !draw) {
        LOG("X fill 所需入口缺失\n"); return 0;
    }
    if (!t->rp || !t->fb) { LOG("X fill 需要带 render pass 的渲染目标\n"); return 0; }
    VkShaderModuleCreateInfo vsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fs_vert_spv_len * 4, .pCode = fs_vert_spv };
    VkShaderModuleCreateInfo fsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fill_frag_spv_len * 4, .pCode = fill_frag_spv };
    VkShaderModule vs = NULL, fs = NULL;
    if (mksh(c->dev, &vsi, NULL, &vs) != VK_SUCCESS || mksh(c->dev, &fsi, NULL, &fs) != VK_SUCCESS) {
        LOG("X fill 着色器模块创建失败\n"); return 0;
    }
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                .offset = 0, .size = 8 };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout pl = NULL;
    if (mkpl(c->dev, &plci, NULL, &pl) != VK_SUCCESS) { LOG("X fill 管线布局失败\n"); return 0; }
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float) t->W, (float) t->H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { t->W, t->H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb2 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb2, .layout = pl, .renderPass = t->rp, .subpass = 0 };
    VkPipeline pipe = NULL;
    if (mkgp(c->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) != VK_SUCCESS) { LOG("X fill 管线创建失败\n"); return 0; }
    LOG("fill 负载: 全屏三角形 + 每像素 %d 次循环（可在日志里核对）\n", 64);

    Timer tm; int has_ts = timer_init(c, &tm, 2);
    VkClearValue cv = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = t->rp, .framebuffer = t->fb, .renderArea = { { 0, 0 }, { t->W, t->H } },
        .clearValueCount = 1, .pClearValues = &cv };
    struct { float angle; int steps; } pcs = { 0.0f, 64 };
    const int per = 8;
    double t0 = now_ms(), deadline = t0 + seconds * 1000.0;
    st->pixels_per_op = (double) t->W * t->H;   /* v4.3：口径归真 = 真像素（步进负载另报一行）*/
    double fill_steps = (double) pcs.steps;   /* 仅用于日志口径自证 */
    while (now_ms() < deadline) {
        VkCommandBufferBeginInfo b = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if (begin(t->cb, &b) != VK_SUCCESS) { st->fails++; break; }
        if (has_ts) timer_begin(&tm, t->cb);
        for (int i = 0; i < per; i++) {
            brp(t->cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            bp(t->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            pc(t->cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, &pcs);
            draw(t->cb, 3, 1, 0, 0);
            erp(t->cb);
        }
        if (has_ts) timer_end(&tm, t->cb);
        end(t->cb);
        double s0 = now_ms(); VkResult r = submit_wait(c, t); double s1 = now_ms();
        if (r != VK_SUCCESS) { st->fails++; LOG("X fill 第 %llu 次提交失败 r=%d\n", st->subs, r); break; }
        if (has_ts) { double g = timer_result_ms(c, &tm); if (g > 0) { st->gpu_ms += g; st->gpu_ok = 1; } }
        st->sum_us += (s1 - s0) * 1000.0; st->subs++; st->ops += per;
    }
    st->secs = (now_ms() - t0) / 1000.0;
    if (st->fails == 0 && st->ops > 0) {
        double px = (double) st->ops * t->W * t->H;
        double den = (st->gpu_ok && st->gpu_ms > 0) ? st->gpu_ms / 1000.0 : st->secs;
        LOG("fill 口径自证: 像素率=%.1f Mpixel/s，片元步进率=%.2f Gstep/s（= 像素率 x %d 步，二者不可混用）\n",
            px / den / 1e6, px * fill_steps / den / 1e9, pcs.steps);
        LOG("fill 几何: 每次 draw 3 顶点 = 1 个全屏三角形（无顶点缓冲、无索引）\n");
    }
    return st->fails == 0;
}

// blit：图像间拷贝（带宽）
static int bench_blit(Ctx *c, Target *a, Target *b, double seconds, BenchStat *st) {
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdCopyImage cp = (PFN_vkCmdCopyImage) c->gdpa(c->dev, "vkCmdCopyImage");
    PFN_vkCmdPipelineBarrier bar = (PFN_vkCmdPipelineBarrier) c->gdpa(c->dev, "vkCmdPipelineBarrier");
    if (!begin || !end || !cp || !bar) { LOG("X blit 入口缺失\n"); return 0; }
    VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    // 摆好布局：A 作源、B 作目标
    VkImageMemoryBarrier bs[2] = {
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = a->img, .subresourceRange = rg,
          .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT },   /* v1.1: 补可用性掩码 */
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = b->img, .subresourceRange = rg,
          .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT } };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin(a->cb, &bi);
    bar(a->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, bs);
    end(a->cb);
    if (submit_wait(c, a) != VK_SUCCESS) { LOG("X blit 预热失败\n"); return 0; }
    VkImageCopy region = { sl, { 0, 0, 0 }, sl, { 0, 0, 0 }, { a->W, a->H, 1 } };
    Timer tm; int has_ts = timer_init(c, &tm, 2);     /* v4.2: GPU 时间戳 */
    const int per = 256;    /* v6.0：再放大，压缩 CPU 提交占比 */
    double t0 = now_ms(), deadline = t0 + seconds * 1000.0;
    st->bytes_per_op = (double) a->W * a->H * 4.0 * 2.0;   /* v4.3: 有效字节 = 读+写（vkpeak 口径）*/
    while (now_ms() < deadline) {
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if (begin(a->cb, &bi) != VK_SUCCESS) { st->fails++; break; }
        if (has_ts) timer_begin(&tm, a->cb);
        for (int i = 0; i < per; i++) cp(a->cb, a->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          b->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (has_ts) timer_end(&tm, a->cb);
        end(a->cb);
        double s0 = now_ms(); VkResult r = submit_wait(c, a); double s1 = now_ms();
        if (r != VK_SUCCESS) { st->fails++; break; }
        if (has_ts) { double g = timer_result_ms(c, &tm); if (g > 0) { st->gpu_ms += g; st->gpu_ok = 1; } }
        st->sum_us += (s1 - s0) * 1000.0; st->subs++; st->ops += per;
    }
    st->secs = (now_ms() - t0) / 1000.0;
    return st->fails == 0;
}

// draw：三角形吞吐（复用 tri 管线）
static int bench_draw(Ctx *c, Target *t, VkPipeline pipe, VkPipelineLayout pl,
                      double seconds, BenchStat *st) {
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdBeginRenderPass brp = (PFN_vkCmdBeginRenderPass) c->gdpa(c->dev, "vkCmdBeginRenderPass");
    PFN_vkCmdEndRenderPass erp = (PFN_vkCmdEndRenderPass) c->gdpa(c->dev, "vkCmdEndRenderPass");
    PFN_vkCmdBindPipeline bp = (PFN_vkCmdBindPipeline) c->gdpa(c->dev, "vkCmdBindPipeline");
    PFN_vkCmdDraw draw = (PFN_vkCmdDraw) c->gdpa(c->dev, "vkCmdDraw");
    if (!begin || !end || !brp || !erp || !bp || !draw) { LOG("X draw 入口缺失\n"); return 0; }
    VkClearValue cv = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = t->rp, .framebuffer = t->fb, .renderArea = { { 0, 0 }, { t->W, t->H } },
        .clearValueCount = 1, .pClearValues = &cv };
    const int draws_per_submit = 4096;   /* v3.1：同上 */
    Timer tm; int has_ts = timer_init(c, &tm, 2);     /* v4.2: GPU 时间戳 */
    double t0 = now_ms(), deadline = t0 + seconds * 1000.0;
    st->pixels_per_op = 0;   /* v2.0(B9)：draw 的语义是"三角形/秒"，不是像素率 */
    while (now_ms() < deadline) {
        VkCommandBufferBeginInfo b = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        if (begin(t->cb, &b) != VK_SUCCESS) { st->fails++; break; }
        if (has_ts) timer_begin(&tm, t->cb);
        brp(t->cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        bp(t->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        {
            PFN_vkCmdPushConstants pcb = (PFN_vkCmdPushConstants) c->gdpa(c->dev, "vkCmdPushConstants");
            struct { float angle; int steps; } pcd = { 0.3f, 0 };
            if (pcb) pcb(t->cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, &pcd);
        }
        for (int i = 0; i < draws_per_submit; i++) draw(t->cb, 3, 1, 0, 0);
        erp(t->cb);
        if (has_ts) timer_end(&tm, t->cb);
        end(t->cb);
        double s0 = now_ms(); VkResult r = submit_wait(c, t); double s1 = now_ms();
        if (r != VK_SUCCESS) { st->fails++; LOG("X 第 %llu 次 draw 提交失败 r=%d\n", st->subs, r); break; }
        if (has_ts) { double g = timer_result_ms(c, &tm); if (g > 0) { st->gpu_ms += g; st->gpu_ok = 1; } }
        st->sum_us += (s1 - s0) * 1000.0; st->subs++; st->ops += draws_per_submit;
    }
    st->secs = (now_ms() - t0) / 1000.0;
    return st->fails == 0;
}

// ---------------------------------------------------------------- 跑分（对外）
// 建三角形管线（draw / tri 共用）
static int make_tri_pipeline(Ctx *c, VkRenderPass rp, VkPipeline *out_pipe, VkPipelineLayout *out_pl) {
    PFN_vkCreateShaderModule mksh = (PFN_vkCreateShaderModule) c->gdpa(c->dev, "vkCreateShaderModule");
    PFN_vkCreatePipelineLayout mkpl = (PFN_vkCreatePipelineLayout) c->gdpa(c->dev, "vkCreatePipelineLayout");
    PFN_vkCreateGraphicsPipelines mkgp = (PFN_vkCreateGraphicsPipelines) c->gdpa(c->dev, "vkCreateGraphicsPipelines");
    if (!mksh || !mkpl || !mkgp) return 0;
    VkShaderModuleCreateInfo vsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = tri_vert_spv_len * 4, .pCode = tri_vert_spv };
    VkShaderModuleCreateInfo fsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = tri_frag_spv_len * 4, .pCode = tri_frag_spv };
    VkShaderModule vs = NULL, fs = NULL;
    if (mksh(c->dev, &vsci, NULL, &vs) != VK_SUCCESS) return 0;
    if (mksh(c->dev, &fsci, NULL, &fs) != VK_SUCCESS) return 0;
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                .offset = 0, .size = 8 };   /* v4.0: angle + steps */
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };   /* v2.0: 旋转角 */
    (void) pcr;
    if (mkpl(c->dev, &plci, NULL, out_pl) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, 256, 256, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { 256, 256 } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb2 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb2, .layout = *out_pl, .renderPass = rp, .subpass = 0 };
    return mkgp(c->dev, VK_NULL_HANDLE, 1, &gpci, NULL, out_pipe) == VK_SUCCESS;
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeBench(JNIEnv *env, jobject th, jstring jso, jint seconds, jstring jmode) {
    LOG("原生入口: nativeBench 进入\n");
    (void) th; g_len = 0; g_log[0] = 0;
    const char *path = (*env)->GetStringUTFChars(env, jso, NULL);
    const char *mode = jmode ? (*env)->GetStringUTFChars(env, jmode, NULL) : "fill";
    int secs = seconds > 0 ? seconds : 5;
    LOG("=== 跑分 mode=%s 时长=%d s ===\n驱动: %s\n", mode, secs, path);
    Ctx c;
    if (!ctx_open(&c, path)) { LOG("X 驱动不可用\n"); goto out; }
    {
        Target t, t2;
        BenchStat st; memset(&st, 0, sizeof(st));
        if (!target_make(&c, &t, 1024, 1024, 1)) { LOG("X 目标创建失败\n"); goto out2; }   /* v4.1: fill 现在走渲染通道 */
        int ok = 0;
        if (!strcmp(mode, "fill")) {
            ok = bench_fill(&c, &t, secs, &st); bench_report("fill 填充率", &st);
            if (ok) LOG("\n== 分数(fill) = %.0f Mpixel/s ==\n", st.ops * st.pixels_per_op / 1e6 / score_den(&st));
        } else if (!strcmp(mode, "blit")) {
            if (!target_make(&c, &t2, 1024, 1024, 0)) { LOG("X 第二目标创建失败\n"); goto out2; }
            ok = bench_blit(&c, &t, &t2, secs, &st); bench_report("blit 拷贝带宽", &st);
            if (ok) LOG("\n== 分数(blit) = %.2f GB/s ==\n", st.ops * st.bytes_per_op / 1e9 / score_den(&st));
        } else if (!strcmp(mode, "draw")) {
            VkPipeline pipe = NULL; VkPipelineLayout pl = NULL;
            if (!make_tri_pipeline(&c, t.rp, &pipe, &pl)) { LOG("X 管线创建失败\n"); goto out2; }
            ok = bench_draw(&c, &t, pipe, pl, secs, &st); bench_report("draw 三角形吞吐", &st);
            if (ok) LOG("\n== 分数(draw) = %.0f 三角形/s ==\n", st.ops * 3 / st.secs);
        } else { LOG("X 未知模式（fill|blit|draw）\n"); }
        if (!ok) LOG("\n== 判定: 不稳定（有失败）—— 不给分 ==\n");
out2:   ;
    }
out:
    ctx_close(&c);
    (*env)->ReleaseStringUTFChars(env, jso, path);
    if (jmode && mode) (*env)->ReleaseStringUTFChars(env, jmode, mode);
    return finish(env, path, jso);
}

// 一键全流程：冒烟 → 三角形校验 → 三种跑分 → 综合分
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeBenchAll(JNIEnv *env, jobject th, jstring jso, jint seconds) {
    LOG("原生入口: nativeBenchAll 进入\n");
    (void) th; g_len = 0; g_log[0] = 0;
    const char *path = (*env)->GetStringUTFChars(env, jso, NULL);
    int secs = seconds > 0 ? seconds : 4;
    LOG("=== 全流程自测 ===\n驱动: %s\n每组 %d 秒\n", path, secs);
    Ctx c;
    if (!ctx_open(&c, path)) { LOG("X 驱动不可用，流程终止\n"); goto out; }
    {
        Target t, t2;
        BenchStat sf, sb, sd; memset(&sf, 0, sizeof(sf)); memset(&sb, 0, sizeof(sb)); memset(&sd, 0, sizeof(sd));
        int okf = 0, okb = 0, okd = 0;
        if (!target_make(&c, &t, 1024, 1024, 1)) { LOG("X 目标创建失败\n"); goto out2; }
        okf = bench_fill(&c, &t, secs, &sf);  bench_report("① fill 填充率", &sf);
        if (!target_make(&c, &t2, 1024, 1024, 0)) { LOG("X 第二目标创建失败\n"); goto out2; }
        okb = bench_blit(&c, &t, &t2, secs, &sb); bench_report("② blit 拷贝带宽", &sb);
        {
            VkPipeline pipe = NULL; VkPipelineLayout pl = NULL;
            if (make_tri_pipeline(&c, t.rp, &pipe, &pl)) {
                okd = bench_draw(&c, &t, pipe, pl, secs, &sd); bench_report("③ draw 三角形吞吐", &sd);
            } else LOG("X 管线创建失败（draw 跳过）\n");
        }
        double fill = okf && sf.secs > 0 ? sf.ops * sf.pixels_per_op / 1e6 / sf.secs : 0;
        double blit = okb && sb.secs > 0 ? sb.ops * sb.bytes_per_op / 1e9 / sb.secs : 0;
        double tri  = okd && sd.secs > 0 ? sd.ops * 3 / sd.secs : 0;
        double score = fill * 1.0 + blit * 100.0 + tri / 1000.0;
        LOG("\n=== 汇总 ===\n");
        LOG("填充率    : %10.1f Mpixel/s %s\n", fill, okf ? "" : "(失败)");
        LOG("拷贝带宽  : %10.2f GB/s     %s\n", blit, okb ? "" : "(失败)");
        LOG("三角形吞吐: %10.0f 个/s     %s\n", tri,  okd ? "" : "(失败)");
        LOG("失败统计  : fill=%llu blit=%llu draw=%llu\n", sf.fails, sb.fails, sd.fails);
        LOG("\n== 综合分: %.0f ==\n", (okf || okb || okd) ? score : 0.0);
        LOG("（口径透明：分 = 填充率 + 带宽×100 + 三角形吞吐÷1000）\n");
out2:   ;
    }
out:
    ctx_close(&c);
    (*env)->ReleaseStringUTFChars(env, jso, path);
    return finish(env, path, jso);
}


// ============================================================================
//  v2.0 画面页：把渲染结果直接写进 Java 的 Bitmap（AndroidBitmap_lockPixels）
//  以及"能力清单"：把设备能力/扩展/限制倒出来（学 Vulkan Hardware Capability Viewer）
// ============================================================================
static Ctx      g_rc;
static Target   g_rt;
static VkPipeline       g_rpipe;
static VkPipelineLayout g_rpl;
static int      g_rinited;
static uint32_t g_rW, g_rH;
static double   g_rT0, g_rSumFrameMs;
static unsigned long long g_rFrames;

static VkResult submit_simple(Ctx *c, Target *t) { return submit_wait(c, t); }

// 把 g_rt 的图像拷回主机缓冲并停在那里，随后由 blit 写进 Bitmap
static unsigned char *g_rPixels;

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRenderInit(JNIEnv *env, jobject th, jstring jso, jint w, jint h) {
    LOG("原生入口: nativeRenderInit 进入\n");
    (void) th; g_len = 0; g_log[0] = 0; g_rinited = 0; g_rPixels = NULL;
    const char *path = (*env)->GetStringUTFChars(env, jso, NULL);
    g_rW = w > 0 ? (uint32_t) w : 512;
    g_rH = h > 0 ? (uint32_t) h : 512;
    LOG("=== 画面页初始化 %ux%u ===\n驱动: %s\n", g_rW, g_rH, path);
    if (!ctx_open(&g_rc, path)) { LOG("X 驱动不可用\n"); goto out; }
    if (!target_make(&g_rc, &g_rt, g_rW, g_rH, 1)) { LOG("X 渲染目标创建失败\n"); goto out; }
    if (!make_tri_pipeline(&g_rc, g_rt.rp, &g_rpipe, &g_rpl)) { LOG("X 图形管线创建失败\n"); goto out; }
    g_rT0 = now_ms(); g_rFrames = 0; g_rSumFrameMs = 0;
    g_rinited = 1;
    LOG("OK 渲染就绪\n");
out:
    (*env)->ReleaseStringUTFChars(env, jso, path);
    return finish(env, NULL, NULL);
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRenderFrame(JNIEnv *env, jobject th, jint frame) {
    LOG("原生入口: nativeRenderFrame 进入\n");
    (void) env; (void) th; g_len = 0; g_log[0] = 0;
    if (!g_rinited) { LOG("X 未初始化\n"); return finish(env, NULL, NULL); }
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer) g_rc.gdpa(g_rc.dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer) g_rc.gdpa(g_rc.dev, "vkEndCommandBuffer");
    PFN_vkCmdBeginRenderPass brp = (PFN_vkCmdBeginRenderPass) g_rc.gdpa(g_rc.dev, "vkCmdBeginRenderPass");
    PFN_vkCmdEndRenderPass erp = (PFN_vkCmdEndRenderPass) g_rc.gdpa(g_rc.dev, "vkCmdEndRenderPass");
    PFN_vkCmdBindPipeline bp = (PFN_vkCmdBindPipeline) g_rc.gdpa(g_rc.dev, "vkCmdBindPipeline");
    PFN_vkCmdPushConstants pc = (PFN_vkCmdPushConstants) g_rc.gdpa(g_rc.dev, "vkCmdPushConstants");
    PFN_vkCmdDraw draw = (PFN_vkCmdDraw) g_rc.gdpa(g_rc.dev, "vkCmdDraw");
    double t0 = now_ms();
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    if (begin(g_rt.cb, &bi) != VK_SUCCESS) { LOG("X begin 失败\n"); return finish(env, NULL, NULL); }
    VkClearValue cv = { .color = { { 0.05f, 0.07f, 0.10f, 1.0f } } };   /* 深蓝底，好看 */
    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_rt.rp, .framebuffer = g_rt.fb,
        .renderArea = { { 0, 0 }, { g_rW, g_rH } }, .clearValueCount = 1, .pClearValues = &cv };
    brp(g_rt.cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    bp(g_rt.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_rpipe);
    float ang = (float) frame * 0.06f;
    if (pc) pc(g_rt.cb, g_rpl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &ang);
    draw(g_rt.cb, 3, 1, 0, 0);
    erp(g_rt.cb);
    end(g_rt.cb);
    VkResult r = submit_simple(&g_rc, &g_rt);
    double dt = now_ms() - t0;
    g_rFrames++; g_rSumFrameMs += dt;
    if (r != VK_SUCCESS) LOG("X 第 %llu 帧提交失败 r=%d\n", g_rFrames, r);
    return finish(env, NULL, NULL);
}

// 把渲染目标拷回主机并写进 Java 的 Bitmap（零额外拷贝：AndroidBitmap_lockPixels）
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRenderBlit(JNIEnv *env, jobject th, jobject bmp) {
    LOG("原生入口: nativeRenderBlit 进入\n");
    (void) th; g_len = 0; g_log[0] = 0;
    if (!g_rinited) { LOG("X 未初始化\n"); return finish(env, NULL, NULL); }
    void *px = NULL;
    if (!readback(&g_rc, &g_rt, &px) || !px) { LOG("X 回读失败\n"); return finish(env, NULL, NULL); }
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS) { LOG("X 取 Bitmap 信息失败\n"); return finish(env, NULL, NULL); }
    void *dst = NULL;
    if (AndroidBitmap_lockPixels(env, bmp, &dst) != ANDROID_BITMAP_RESULT_SUCCESS) { LOG("X 锁 Bitmap 失败\n"); return finish(env, NULL, NULL); }
    unsigned char *d = (unsigned char *) dst;
    unsigned char *s = (unsigned char *) px;
    uint32_t W = info.width < g_rW ? info.width : g_rW;
    uint32_t H = info.height < g_rH ? info.height : g_rH;
    for (uint32_t y = 0; y < H; y++) {
        unsigned char *dr = d + (size_t) y * info.stride;
        unsigned char *sr = s + (size_t) y * g_rW * 4;
        for (uint32_t x = 0; x < W; x++) {
            dr[x*4+0] = sr[x*4+0];   /* R */
            dr[x*4+1] = sr[x*4+1];   /* G */
            dr[x*4+2] = sr[x*4+2];   /* B */
            dr[x*4+3] = 255;         /* A */
        }
    }
    AndroidBitmap_unlockPixels(env, bmp);
    return finish(env, NULL, NULL);
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRenderStats(JNIEnv *env, jobject th) {
    LOG("原生入口: nativeRenderStats 进入\n");
    (void) th; g_len = 0; g_log[0] = 0;
    double wall = (now_ms() - g_rT0) / 1000.0;
    if (!g_rinited) { LOG("未初始化\n"); return finish(env, NULL, NULL); }
    LOG("帧数=%llu  墙钟=%.2fs  平均FPS=%.1f  平均帧时=%.2f ms\n",
        g_rFrames, wall, wall > 0 ? g_rFrames / wall : 0.0,
        g_rFrames ? g_rSumFrameMs / g_rFrames : 0.0);
    return finish(env, NULL, NULL);
}

JNIEXPORT void JNICALL
Java_com_dsh_gputest_MainActivity_nativeRenderStop(JNIEnv *env, jobject th) {
    LOG("原生入口: nativeRenderStop 进入\n");
    (void) env; (void) th;
    g_rinited = 0; g_rPixels = NULL;
}

// ---- 能力清单（学 Vulkan Hardware Capability Viewer 的思路）----
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCaps(JNIEnv *env, jobject th, jstring jso) {
    LOG("原生入口: nativeCaps 进入\n");
    (void) th; g_len = 0; g_log[0] = 0;
    const char *path = (*env)->GetStringUTFChars(env, jso, NULL);
    LOG("=== 驱动能力清单 ===\n驱动: %s\n", path);
    Ctx c;
    if (!ctx_open(&c, path)) { LOG("X 驱动不可用\n"); goto out; }
    {
        VkPhysicalDeviceProperties p = c.props;
        LOG("\n[设备]\n  name      : %s\n  type      : %d\n  apiVersion: %u.%u.%u\n  driverVer : 0x%08x\n  vendorID  : 0x%04x\n  deviceID  : 0x%04x\n",
            p.deviceName, p.deviceType,
            VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
            p.driverVersion, p.vendorID, p.deviceID);
        LOG("\n[关键限制]\n  maxImageDimension2D     : %u\n  maxComputeWorkGroupCount: %u,%u,%u\n  maxComputeWorkGroupSize : %u,%u,%u\n  maxMemoryAllocationCount: %u\n  maxVertexInputAttributes: %u\n",
            p.limits.maxImageDimension2D,
            p.limits.maxComputeWorkGroupCount[0], p.limits.maxComputeWorkGroupCount[1], p.limits.maxComputeWorkGroupCount[2],
            p.limits.maxComputeWorkGroupSize[0], p.limits.maxComputeWorkGroupSize[1], p.limits.maxComputeWorkGroupSize[2],
            p.limits.maxMemoryAllocationCount, p.limits.maxVertexInputAttributes);

        /* 实例扩展 */
        PFN_vkEnumerateInstanceExtensionProperties eie =
            (PFN_vkEnumerateInstanceExtensionProperties) c_gipa(&c, "vkEnumerateInstanceExtensionProperties");
        if (eie) {
            uint32_t n = 0; eie(NULL, &n, NULL);
            LOG("\n[实例扩展] 共 %u 个\n", n);
            if (n) {
                VkExtensionProperties *ep = (VkExtensionProperties *) malloc(sizeof(VkExtensionProperties) * n);
                if (ep && eie(NULL, &n, ep) == VK_SUCCESS)
                    for (uint32_t i = 0; i < n && i < 60; i++) LOG("  %s (rev %u)\n", ep[i].extensionName, ep[i].specVersion);
                if (ep) free(ep);
            }
        }
        /* 设备扩展 */
        PFN_vkEnumerateDeviceExtensionProperties ede =
            (PFN_vkEnumerateDeviceExtensionProperties) c_gipa(&c, "vkEnumerateDeviceExtensionProperties");
        if (ede) {
            uint32_t n = 0; ede(c.pd, NULL, &n, NULL);
            LOG("\n[设备扩展] 共 %u 个\n", n);
            if (n) {
                VkExtensionProperties *ep = (VkExtensionProperties *) malloc(sizeof(VkExtensionProperties) * n);
                if (ep && ede(c.pd, NULL, &n, ep) == VK_SUCCESS)
                    for (uint32_t i = 0; i < n && i < 80; i++) LOG("  %s (rev %u)\n", ep[i].extensionName, ep[i].specVersion);
                if (ep) free(ep);
            }
        }
        /* 内存堆 */
        PFN_vkGetPhysicalDeviceMemoryProperties gm =
            (PFN_vkGetPhysicalDeviceMemoryProperties) c_gipa(&c, "vkGetPhysicalDeviceMemoryProperties");
        if (gm) {
            VkPhysicalDeviceMemoryProperties mp; gm(c.pd, &mp);
            LOG("\n[内存堆] 共 %u 个\n", mp.memoryHeapCount);
            for (uint32_t i = 0; i < mp.memoryHeapCount && i < 8; i++)
                LOG("  堆%u: %.1f MB  flags=0x%x\n", i, mp.memoryHeaps[i].size / 1048576.0, mp.memoryHeaps[i].flags);
            LOG("[内存类型] 共 %u 个\n", mp.memoryTypeCount);
            for (uint32_t i = 0; i < mp.memoryTypeCount && i < 16; i++)
                LOG("  类型%u: heap=%u flags=0x%x\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);
        }
    }
    ctx_close(&c);
out:
    (*env)->ReleaseStringUTFChars(env, jso, path);
    return finish(env, NULL, NULL);
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeLastStep(JNIEnv *env, jobject th) {
    (void) th;
    return (*env)->NewStringUTF(env, g_step[0] ? g_step : "(未开始/已完成)");
}

// ============================================================================
//  v2.2：按**真实符号**判定一个 .so 属于哪一层（驱动 / 垫片 / 渲染器）
//  用户澄清的模型：渲染器跑在 Vulkan 驱动之上，两者必须分开列。
// ============================================================================
// ---------------------------------------------------------------- 静态符号判定
// v2.4：**不再 dlopen 目标** —— v2.3 就是死在这里：MobileGL / OSMesa / shaderconv
// 这类「渲染器」在没有 GL/EGL 环境的进程里被加载时会直接崩（构造函数/依赖不满足）。
// 改成只把文件 mmap 进来看 ELF 的 .dynsym/.dynstr —— 不执行任何目标代码。
static int elf_has_sym(const void *base, size_t size, const char *want) {
    if (size < sizeof(Elf64_Ehdr)) return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *) base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return 0;
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr)) return 0;
    if (eh->e_shoff + (size_t) eh->e_shnum * sizeof(Elf64_Shdr) > size) return 0;
    const Elf64_Shdr *sh = (const Elf64_Shdr *) ((const char *) base + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_entsize == 0) continue;
        if (sh[i].sh_offset + sh[i].sh_size > size) continue;
        if (sh[i].sh_link >= eh->e_shnum) continue;
        const Elf64_Shdr *st = &sh[sh[i].sh_link];
        if (st->sh_offset + st->sh_size > size) continue;
        const char *names = (const char *) base + st->sh_offset;
        const char *syms  = (const char *) base + sh[i].sh_offset;
        size_t n = sh[i].sh_size / sh[i].sh_entsize;
        for (size_t k = 0; k < n; k++) {
            const Elf64_Sym *sy = (const Elf64_Sym *) (syms + k * sh[i].sh_entsize);
            if (sy->st_name >= st->sh_size) continue;
            const char *nm = names + sy->st_name;
            if (strcmp(nm, want) == 0) return 1;
        }
    }
    return 0;
}

// 传入 mmap 的基址，按"先渲染器、再驱动、再垫片"的顺序判层
static const char *elf_classify(const void *base, size_t size, const char **why) {
    static const char *GL_SYMS[] = { "eglGetDisplay", "eglGetPlatformDisplay", "glGetString",
                                     "glXGetProcAddress", "OSMesaCreateContext",
                                     "OSMesaCreateContextExt", "OSMesaMakeCurrent", NULL };
    static const char *ICD_SYMS[] = { "vk_icdGetInstanceProcAddr", "vk_icdGetInstanceProcAddrLunar", NULL };
    for (int i = 0; GL_SYMS[i]; i++)
        if (elf_has_sym(base, size, GL_SYMS[i])) { if (why) *why = GL_SYMS[i]; return "渲染器(GLES/EGL)"; }
    for (int i = 0; ICD_SYMS[i]; i++)
        if (elf_has_sym(base, size, ICD_SYMS[i])) { if (why) *why = ICD_SYMS[i]; return "Vulkan驱动(ICD)"; }
    if (elf_has_sym(base, size, "vkGetInstanceProcAddr")) { if (why) *why = "vkGetInstanceProcAddr"; return "Vulkan垫片/层"; }
    if (why) *why = "无相关符号";
    return "未知(无 Vulkan/EGL 入口)";
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeClassify(JNIEnv *env, jobject th, jstring jso) {
    LOG("原生入口: nativeClassify 进入\n");
    (void) th;
    const char *p = jso ? (*env)->GetStringUTFChars(env, jso, NULL) : NULL;
    char out[192];
    snprintf(out, sizeof(out), "打不开");
    if (p) {
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat stt;
            if (fstat(fd, &stt) == 0 && stt.st_size > 0) {
                size_t sz = (size_t) stt.st_size;
                void *m = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
                if (m != MAP_FAILED) {
                    const char *why = NULL;
                    const char *cls = elf_classify(m, sz, &why);
                    snprintf(out, sizeof(out), "%s", cls);
                    munmap(m, sz);
                } else {
                    snprintf(out, sizeof(out), "打不开(mmapp)");
                }
            } else {
                snprintf(out, sizeof(out), "打不开(fstat)");
            }
            close(fd);
        } else {
            snprintf(out, sizeof(out), "打不开(open errno=%d)", errno);
        }
        (*env)->ReleaseStringUTFChars(env, jso, p);
    }
    return (*env)->NewStringUTF(env, out);
}

// ---------------------------------------------------------------- 崩溃自捕获
// native 崩了没法 catch，但可以装信号处理器把现场写进文件 —— 下次闪退就有据可查。
struct bt_ctx { void **pcs; int n, max; };
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg) {
    struct bt_ctx *b = (struct bt_ctx *) arg;
    if (b->n >= b->max) return _URC_END_OF_STACK;
    uintptr_t pc = (uintptr_t) _Unwind_GetIP(ctx);
    if (pc) b->pcs[b->n++] = (void *) pc;
    return _URC_NO_REASON;
}
/* v9.2：崩溃时**自带栈回溯**（函数名 + 库名）⇒ 不依赖 logcat/Shizuku ✓ */
/* ★ v9.6 崩溃现场增强：原先 (void) uc; 把 ucontext 丢了，只记 _Unwind 的返回地址
 *    （已知出过假帧——unwinder 在无帧信息的驱动代码里走飞）。真凭据 = ucontext 里的 PC ✓ */
static char g_altstk[64 * 1024] __attribute__((aligned(16)));  /* 信号备用栈：显式大小，不用 SIGSTKSZ（bionic 上受限 ✗）*/
static volatile sig_atomic_t g_in_crash = 0;                   /* 递归保护：处理器内再崩 ⇒ 直接退 ✓ */
static const char *crash_basename(const char *p) {             /* 信号处理器内只做无分配操作 ✓ */
    const char *q = p ? strrchr(p, '/') : NULL;
    return q ? q + 1 : (p ? p : "?");
}
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    if (g_in_crash) _exit(139);        /* 崩溃处理途中又崩：立即退，防自递归 ✓ */
    g_in_crash = 1;
    char buf[4096];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "CRASH sig=%d code=%d addr=%p\n",
                  sig, si ? si->si_code : 0, si ? si->si_addr : NULL);
#if defined(__aarch64__)
    /* ★ 真现场：出错指令 PC + 栈指针 SP + 返回地址 LR + 帧指针 FP + fault_address ✓
     *   bionic arm64: mcontext_t == struct sigcontext { fault_address; regs[31]; sp; pc; pstate; }
     *   regs[30]=LR(X30)，regs[29]=FP(X29)——字段名已对照 NDK r27c sysroot 头文件 ✓ */
    ucontext_t *u = (ucontext_t *) uc;
    if (u) {
        uintptr_t pc = (uintptr_t) u->uc_mcontext.pc;
        uintptr_t sp = (uintptr_t) u->uc_mcontext.sp;
        uintptr_t lr = (uintptr_t) u->uc_mcontext.regs[30];
        uintptr_t fp = (uintptr_t) u->uc_mcontext.regs[29];
        n += snprintf(buf + n, sizeof(buf) - n, "FAULT=0x%llx\n",
                      (unsigned long long) u->uc_mcontext.fault_address);
        Dl_info pdi;
        if (pc && dladdr((void *) pc, &pdi) && pdi.dli_sname)
            n += snprintf(buf + n, sizeof(buf) - n, "PC=0x%llx <%s+0x%lx> (%s)\n",
                          (unsigned long long) pc, pdi.dli_sname,
                          (unsigned long) ((char *) pc - (char *) pdi.dli_fbase),
                          pdi.dli_fname ? crash_basename(pdi.dli_fname) : "?");
        else
            n += snprintf(buf + n, sizeof(buf) - n, "PC=0x%llx <符号未知> (dladdr 未命中)\n",
                          (unsigned long long) pc);
        Dl_info ldi;
        if (lr && dladdr((void *) lr, &ldi) && ldi.dli_sname)
            n += snprintf(buf + n, sizeof(buf) - n, "SP=0x%llx LR=0x%llx <%s+0x%lx> (%s) FP=0x%llx\n",
                          (unsigned long long) sp, (unsigned long long) lr, ldi.dli_sname,
                          (unsigned long) ((char *) lr - (char *) ldi.dli_fbase),
                          ldi.dli_fname ? crash_basename(ldi.dli_fname) : "?",
                          (unsigned long long) fp);
        else
            n += snprintf(buf + n, sizeof(buf) - n, "SP=0x%llx LR=0x%llx FP=0x%llx\n",
                          (unsigned long long) sp, (unsigned long long) lr, (unsigned long long) fp);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, "PC=不可用 (uc 为空)\n");
    }
#else
    n += snprintf(buf + n, sizeof(buf) - n, "PC=不可用 (非 arm64 构建)\n");
#endif
    void *pcs[40];
    struct bt_ctx b; b.pcs = pcs; b.n = 0; b.max = 40;
    _Unwind_Backtrace(bt_cb, &b);
    for (int i = 0; i < b.n && n < (int) sizeof(buf) - 160; i++) {
        Dl_info di;
        if (dladdr(pcs[i], &di) && di.dli_sname)
            n += snprintf(buf + n, sizeof(buf) - n, "  #%02d %p %s  (%s+0x%lx)\n",
                          i, pcs[i], di.dli_sname,
                          di.dli_fname ? strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname : "?",
                          (unsigned long) ((char *) pcs[i] - (char *) di.dli_fbase));
        else
            n += snprintf(buf + n, sizeof(buf) - n, "  #%02d %p ?\n", i, pcs[i]);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "---- end ----\n");
    int fd = open("/data/local/tmp/gputest_crash.txt", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) fd = open("/sdcard/Download/gputest_crash.txt", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { ssize_t w = write(fd, buf, n); (void) w; close(fd); }
    /* ★ v9.6：写完现场后，三个信号全部恢复默认再 abort()（原先只 raise(sig) ✗）
     * ① 先把 SIGABRT 也恢复 SIG_DFL ⇒ abort() 引发的 SIGABRT 不会再进本处理器（防自递归）✓
     * ② 走 abort() 的系统路径 ⇒ tombstoned 也能吃到完整现场，双保险 ✓ */
    signal(SIGSEGV, SIG_DFL);
    signal(SIGABRT, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    abort();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) vm; (void) reserved;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    /* ★ v9.6：SA_ONSTACK + sigaltstack —— 处理器在 64KiB 备用栈上跑，
     *   栈耗尽型 SIGSEGV（原栈已不可写）也能照常记录现场 ✓ */
    {
        stack_t ss;
        memset(&ss, 0, sizeof(ss));
        ss.ss_sp = g_altstk;
        ss.ss_size = sizeof(g_altstk);
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    return JNI_VERSION_1_6;
}

// ============================================================================
//  v5.0 · CPU 软件渲染模块（软件光栅化，**确定性** ⇒ 可当 GPU 输出的"真值"）
//  用途：① 用户要的"让 CPU 自己渲染图片" ② 权威做法里的 reference-image validation
//        —— 同一场景，CPU 出一帧当参考，GPU 出一帧逐像素比对（±2 LSB，超差率）
//  刻意只用整数/定点运算 ⇒ 与 libm 舍入、编译器优化无关 ⇒ 跨运行可复现
// ============================================================================
static uint32_t *g_cpu = NULL;
static int g_cw = 0, g_ch = 0;

static void cpu_alloc(int w, int h) {
    if (g_cpu && g_cw == w && g_ch == h) return;
    free(g_cpu); g_cpu = NULL;
    g_cpu = (uint32_t *) malloc((size_t) w * h * 4);
    if (g_cpu) { g_cw = w; g_ch = h; }
    LOG("CPU 参考渲染器: %dx%d 缓冲%s\n", w, h, g_cpu ? "就绪" : "分配失败");
}
static inline void cpu_put(int x, int y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < g_cw && y < g_ch) g_cpu[(size_t) y * g_cw + x] = c;
}
/* 定点余弦（Q16），只靠整数 ⇒ 确定性 */
static int fx_cos(int deg) {
    static const int T[91] = { 65536,65521,65476,65401,65296,65162,64998,64804,64580,64327,64044,63732,63391,63021,
        62622,62194,61737,61252,60738,60196,59626,59028,58403,57750,57070,56363,55630,54871,54086,53275,52439,
        51578,50693,49784,48852,47897,46919,45919,44898,43856,42794,41712,40611,39491,38354,37199,36027,34839,
        33635,32417,31184,29938,28679,27408,26125,24831,23527,22213,20890,19559,18220,16875,15523,14166,12805,
        11439,10070,8698,7323,5947,4569,3190,1811,432,-946,-2323,-3697,-5067,-6431,-7789,-9137,-10474,-11800,
        -13112,-14409,-15691,-16956,-18203,-19431,-20639,-21826,-22991,-24134,-25253,-26348,-27419,-28464,-29483};
    deg %= 360; if (deg < 0) deg += 360;
    if (deg <= 90) return T[deg];
    if (deg <= 180) return -T[180 - deg];
    if (deg <= 270) return -T[deg - 180];
    return T[360 - deg];
}
static int fx_sin(int deg) { return fx_cos(90 - deg); }

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCpuRender(JNIEnv *env, jobject th, jint frame) {
    (void) env; (void) th;
    if (!g_cpu) return (*env)->NewStringUTF(env, "CPU 渲染器未初始化");
    if (g_cw <= 0 || g_ch <= 0) return (*env)->NewStringUTF(env, "尺寸无效");
    /* ① 底：确定性棋盘 + 水平/垂直渐变（覆盖整屏，便于比对时发现区域差异） */
    for (int y = 0; y < g_ch; y++) {
        int gy = y * 255 / (g_ch > 1 ? g_ch - 1 : 1);
        for (int x = 0; x < g_cw; x++) {
            int gx = x * 255 / (g_cw > 1 ? g_cw - 1 : 1);
            int chk = (((x >> 4) ^ (y >> 4)) & 1) ? 40 : 0;
            uint32_t r = (uint32_t) ((gx + chk) & 0xFF);
            uint32_t g = (uint32_t) ((gy + chk) & 0xFF);
            uint32_t b = (uint32_t) (((gx ^ gy) + chk) & 0xFF);
            g_cpu[(size_t) y * g_cw + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    /* ② 旋转三角形：与 GPU 侧同一几何（角度 = frame * 3.4 度），重心坐标 + 整数边缘规则 */
    int ang = (int) frame * 34 / 10;
    int c = fx_cos(ang), sn = fx_sin(ang);
    int cx = g_cw / 2, cy = g_ch / 2;
    int rad = (g_cw < g_ch ? g_cw : g_ch) * 9 / 20;
    int px[3], py[3];
    for (int i = 0; i < 3; i++) {
        int a = ang + i * 120;
        px[i] = cx + ((fx_cos(a) * rad) >> 16);
        py[i] = cy + ((fx_sin(a) * rad) >> 16);
    }
    /* 三角形包围盒 + 边缘函数（整数，半开区间规则，避免边缘重复/漏填） */
    int minx = px[0], maxx = px[0], miny = py[0], maxy = py[0];
    for (int i = 1; i < 3; i++) {
        if (px[i] < minx) minx = px[i]; if (px[i] > maxx) maxx = px[i];
        if (py[i] < miny) miny = py[i]; if (py[i] > maxy) maxy = py[i];
    }
    if (minx < 0) minx = 0; if (miny < 0) miny = 0;
    if (maxx > g_cw - 1) maxx = g_cw - 1; if (maxy > g_ch - 1) maxy = g_ch - 1;
    long area = (long) (px[1] - px[0]) * (py[2] - py[0]) - (long) (px[2] - px[0]) * (py[1] - py[0]);
    if (area != 0) {
        for (int y = miny; y <= maxy; y++) {
            for (int x = minx; x <= maxx; x++) {
                long w0 = (long) (px[1] - x) * (py[2] - y) - (long) (px[2] - x) * (py[1] - y);
                long w1 = (long) (px[2] - x) * (py[0] - y) - (long) (px[0] - x) * (py[2] - y);
                long w2 = (long) (px[0] - x) * (py[1] - y) - (long) (px[1] - x) * (py[0] - y);
                int inside = (area > 0) ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                                        : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                if (inside) {
                    /* 重心插值出红/绿/蓝三顶点色（与 GPU 侧着色器可对拍） */
                    long b0 = w0, b1 = w1, b2 = w2;
                    int rr = (int) ((b0 * 255) / area);
                    int gg = (int) ((b1 * 255) / area);
                    int bb = (int) ((b2 * 255) / area);
                    if (rr < 0) rr = 0; if (rr > 255) rr = 255;
                    if (gg < 0) gg = 0; if (gg > 255) gg = 255;
                    if (bb < 0) bb = 0; if (bb > 255) bb = 255;
                    g_cpu[(size_t) y * g_cw + x] = 0xFF000000u | ((uint32_t) rr << 16) | ((uint32_t) gg << 8) | (uint32_t) bb;
                }
            }
        }
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "CPU 渲染完成 %dx%d（frame=%d，确定性定点实现）", g_cw, g_ch, (int) frame);
    return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCpuInit(JNIEnv *env, jobject th, jint w, jint h) {
    (void) th;
    cpu_alloc((int) w, (int) h);
    char msg[128];
    snprintf(msg, sizeof(msg), g_cpu ? "CPU 渲染器就绪 %dx%d" : "CPU 渲染器分配失败", (int) w, (int) h);
    return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCpuBlit(JNIEnv *env, jobject th, jobject bmp) {
    (void) th;
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
        return (*env)->NewStringUTF(env, "取位图信息失败");
    if (!g_cpu) return (*env)->NewStringUTF(env, "CPU 缓冲未就绪");
    void *pix = NULL;
    if (AndroidBitmap_lockPixels(env, bmp, &pix) != ANDROID_BITMAP_RESULT_SUCCESS)
        return (*env)->NewStringUTF(env, "锁定位图失败");
    int w = (int) info.width, h = (int) info.height;
    for (int y = 0; y < h; y++) {
        uint32_t *dst = (uint32_t *) ((uint8_t *) pix + (size_t) y * info.stride);
        int sy = (g_ch == h) ? y : y * g_ch / h;
        for (int x = 0; x < w; x++) {
            int sx = (g_cw == w) ? x : x * g_cw / w;
            dst[x] = g_cpu[(size_t) sy * g_cw + sx];
        }
    }
    AndroidBitmap_unlockPixels(env, bmp);
    return (*env)->NewStringUTF(env, "已拷入位图");
}

/* 与 GPU 帧逐像素比对（两者都已在同一个 Bitmap 上渲染过 ⇒ 用法：GPU 渲一帧 → 存副本 →
 * CPU 渲一帧 → 拷入 → 调本函数比对副本与当前位图）。容差 ±2 LSB，超差率 ≤0.1% 判过。 */
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCpuCompare(JNIEnv *env, jobject th, jobject bmpA, jobject bmpB) {
    (void) th;
    AndroidBitmapInfo ia, ib;
    if (AndroidBitmap_getInfo(env, bmpA, &ia) != ANDROID_BITMAP_RESULT_SUCCESS ||
        AndroidBitmap_getInfo(env, bmpB, &ib) != ANDROID_BITMAP_RESULT_SUCCESS)
        return (*env)->NewStringUTF(env, "取位图信息失败");
    if (ia.width != ib.width || ia.height != ib.height)
        return (*env)->NewStringUTF(env, "两张图尺寸不同，无法比对");
    void *pa = NULL, *pb = NULL;
    AndroidBitmap_lockPixels(env, bmpA, &pa);
    AndroidBitmap_lockPixels(env, bmpB, &pb);
    long diff = 0, total = 0; int maxd = 0;
    for (uint32_t y = 0; y < ia.height; y++) {
        uint32_t *ra = (uint32_t *) ((uint8_t *) pa + (size_t) y * ia.stride);
        uint32_t *rb = (uint32_t *) ((uint8_t *) pb + (size_t) y * ib.stride);
        for (uint32_t x = 0; x < ia.width; x++) {
            uint32_t a = ra[x], b = rb[x];
            int dr = (int) ((a >> 16) & 0xFF) - (int) ((b >> 16) & 0xFF);
            int dg = (int) ((a >> 8) & 0xFF) - (int) ((b >> 8) & 0xFF);
            int db = (int) (a & 0xFF) - (int) (b & 0xFF);
            if (dr < 0) dr = -dr; if (dg < 0) dg = -dg; if (db < 0) db = -db;
            int d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            if (d > maxd) maxd = d;
            if (d > 2) diff++;                       /* 容差 ±2 LSB */
            total++;
        }
    }
    AndroidBitmap_unlockPixels(env, bmpA);
    AndroidBitmap_unlockPixels(env, bmpB);
    double pct = total ? 100.0 * (double) diff / (double) total : 0.0;
    char msg[220];
    snprintf(msg, sizeof(msg),
        "参考图比对: 超差像素 %ld / %ld = %.4f%%（容差 ±2 LSB）最大偏差 %d ⇒ %s",
        diff, total, pct, maxd, (pct <= 0.1 ? "判定: 一致 ✓" : "判定: 不一致 ✗"));
    return (*env)->NewStringUTF(env, msg);
}

// ============================================================================
//  v5.1 · 三角形压力场景（对齐用户参考图里的 "Auto Increment ppf" 压测）
//  用 glm 交付的 geom_vert/geom_frag：三角形数由 vkCmdDraw 的顶点数决定
//  （gl_VertexIndex 生成几何 ⇒ 零顶点缓冲 ⇒ 压的是图元装配/光栅/片元）
// ============================================================================
static Target g_st_tgt; static int g_st_ready = 0; static int g_st_w = 0, g_st_h = 0;
static VkPipeline g_st_pipe = NULL; static VkPipelineLayout g_st_pl = NULL;
static Timer g_st_tm; static int g_st_ts = 0;
static float g_st_clear[3] = { 8.0f / 255.0f, 8.0f / 255.0f, 25.0f / 255.0f };

static int stress_build_pipeline(Ctx *c) {
    PFN_vkCreateShaderModule mksh = (PFN_vkCreateShaderModule) c->gdpa(c->dev, "vkCreateShaderModule");
    PFN_vkCreatePipelineLayout mkpl = (PFN_vkCreatePipelineLayout) c->gdpa(c->dev, "vkCreatePipelineLayout");
    PFN_vkCreateGraphicsPipelines mkgp = (PFN_vkCreateGraphicsPipelines) c->gdpa(c->dev, "vkCreateGraphicsPipelines");
    if (!mksh || !mkpl || !mkgp) return 0;
    VkShaderModuleCreateInfo vsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = geom_vert_spv_len * 4, .pCode = geom_vert_spv };
    VkShaderModuleCreateInfo fsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = geom_frag_spv_len * 4, .pCode = geom_frag_spv };
    VkShaderModule vs = NULL, fs = NULL;
    if (mksh(c->dev, &vsi, NULL, &vs) != VK_SUCCESS || mksh(c->dev, &fsi, NULL, &fs) != VK_SUCCESS) return 0;
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                .offset = 0, .size = 4 };   /* 只有 int triCount */
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (mkpl(c->dev, &plci, NULL, &g_st_pl) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float) g_st_w, (float) g_st_h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { (uint32_t) g_st_w, (uint32_t) g_st_h } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };   /* 不混合：压光栅而非混合 */
    VkPipelineColorBlendStateCreateInfo cb2 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb2, .layout = g_st_pl, .renderPass = g_st_tgt.rp, .subpass = 0 };
    if (mkgp(c->dev, VK_NULL_HANDLE, 1, &gp, NULL, &g_st_pipe) != VK_SUCCESS) return 0;
    g_st_ts = timer_init(c, &g_st_tm, 2);
    return 1;
}

static Ctx g_st_ctx; static int g_st_ctx_ok = 0;   /* v5.1: 常驻上下文 —— 必须用用户选中的驱动 */

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeStressInit(JNIEnv *env, jobject th, jstring jso, jint w, jint h) {
    LOG("原生入口: nativeStressInit 进入\n");
    (void) th;
    const char *so = jso ? (*env)->GetStringUTFChars(env, jso, NULL) : NULL;
    char out[256];
    if (g_st_ctx_ok) { ctx_close(&g_st_ctx); g_st_ctx_ok = 0; g_st_ready = 0; }
    if (!ctx_open(&g_st_ctx, so ? so : "system")) {
        if (so) (*env)->ReleaseStringUTFChars(env, jso, so);
        return (*env)->NewStringUTF(env, "驱动不可用");
    }
    if (so) (*env)->ReleaseStringUTFChars(env, jso, so);
    g_st_ctx_ok = 1;
    g_st_w = w; g_st_h = h;
    if (!target_make(&g_st_ctx, &g_st_tgt, (uint32_t) w, (uint32_t) h, 1)) {
        LOG("X 压力场景目标创建失败\n"); ctx_close(&g_st_ctx); g_st_ctx_ok = 0;
        return (*env)->NewStringUTF(env, "渲染目标创建失败");
    }
    int ok = stress_build_pipeline(&g_st_ctx);
    g_st_ready = ok;
    snprintf(out, sizeof(out), ok ? "压力场景就绪 %dx%d（常驻上下文，三角形由 gl_VertexIndex 生成）" : "压力场景管线创建失败", (int) w, (int) h);
    if (!ok) { ctx_close(&g_st_ctx); g_st_ctx_ok = 0; }
    return (*env)->NewStringUTF(env, out);
}

/* 三角形覆盖屏幕比例的解析估算（glm 的 geom_vert 是 side×side 网格、边长 1.6/side）
 * ⇒ 单三角形占屏 ≈ (1.6/side)^2 / 4，总覆盖 = min(1, triCount × 该值) */
static double stress_coverage(int tris) {
    if (tris <= 0) return 0.0;
    double side = 1.0;
    while (side * side < (double) tris) side += 1.0;
    double edge = 1.6 / side;                 /* NDC 边长 */
    double area = edge * edge / 4.0;          /* NDC 面积（全屏 NDC = 4）*/
    double cov = (double) tris * area;
    return cov > 1.0 ? 1.0 : cov;
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeStressFrame(JNIEnv *env, jobject th, jint triCount) {
    LOG("原生入口: nativeStressFrame 进入\n");
    (void) th;
    char out[384];
    if (!g_st_ready || !g_st_ctx_ok) return (*env)->NewStringUTF(env, "压力场景未初始化");
    Ctx *c = &g_st_ctx;
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer) c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer) c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdBeginRenderPass brp = (PFN_vkCmdBeginRenderPass) c->gdpa(c->dev, "vkCmdBeginRenderPass");
    PFN_vkCmdEndRenderPass erp = (PFN_vkCmdEndRenderPass) c->gdpa(c->dev, "vkCmdEndRenderPass");
    PFN_vkCmdBindPipeline bp = (PFN_vkCmdBindPipeline) c->gdpa(c->dev, "vkCmdBindPipeline");
    PFN_vkCmdPushConstants pc = (PFN_vkCmdPushConstants) c->gdpa(c->dev, "vkCmdPushConstants");
    PFN_vkCmdDraw draw = (PFN_vkCmdDraw) c->gdpa(c->dev, "vkCmdDraw");
    if (!begin || !end || !brp || !erp || !bp || !pc || !draw) return (*env)->NewStringUTF(env, "入口缺失");
    VkClearValue cv; memset(&cv, 0, sizeof(cv));
    cv.color.float32[0] = g_st_clear[0]; cv.color.float32[1] = g_st_clear[1];
    cv.color.float32[2] = g_st_clear[2]; cv.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_st_tgt.rp, .framebuffer = g_st_tgt.fb,
        .renderArea = { { 0, 0 }, { (uint32_t) g_st_w, (uint32_t) g_st_h } },
        .clearValueCount = 1, .pClearValues = &cv };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    int tris = (int) triCount;
    double t0 = now_ms();
    if (begin(g_st_tgt.cb, &bi) != VK_SUCCESS) return (*env)->NewStringUTF(env, "begin 失败");
    if (g_st_ts) timer_begin(&g_st_tm, g_st_tgt.cb);
    brp(g_st_tgt.cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    bp(g_st_tgt.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_st_pipe);
    pc(g_st_tgt.cb, g_st_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &tris);
    draw(g_st_tgt.cb, (uint32_t) (tris * 3), 1, 0, 0);
    erp(g_st_tgt.cb);
    if (g_st_ts) timer_end(&g_st_tm, g_st_tgt.cb);
    end(g_st_tgt.cb);
    VkResult r = submit_wait(c, &g_st_tgt);
    double t1 = now_ms();
    double gms = g_st_ts ? timer_result_ms(c, &g_st_tm) : -1.0;
    double wall = t1 - t0;
    double cov = stress_coverage(tris);
    if (r != VK_SUCCESS) {
        snprintf(out, sizeof(out),
            "X 提交失败 r=%d（%s）  三角形=%d  平均帧时=%.2f ms  GPU 时间=%.2f ms  FPS=%.1f  "
            "三角形/s=%.0f  Rendered/Screen=%.1f %%  —— 这一帧已保留",
            r, r == -4 ? "VK_ERROR_DEVICE_LOST" : "见 VkResult 表", tris, wall, gms,
            wall > 0 ? 1000.0 / wall : 0.0, gms > 0 ? tris * 1000.0 / gms : 0.0, cov * 100.0);
    } else {
        snprintf(out, sizeof(out),
            "三角形=%d  平均帧时=%.2f ms  GPU 时间=%.2f ms  FPS=%.1f  FPS(GPU)=%.1f  "
            "三角形/s=%.0f  Rendered/Screen=%.1f %%",
            tris, wall, gms, wall > 0 ? 1000.0 / wall : 0.0, gms > 0 ? 1000.0 / gms : 0.0,
            gms > 0 ? tris * 1000.0 / gms : 0.0, cov * 100.0);
    }
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeStressBlit(JNIEnv *env, jobject th, jobject bmp) {
    LOG("原生入口: nativeStressBlit 进入\n");
    (void) th;
    if (!g_st_ready || !g_st_ctx_ok || !g_st_tgt.img) return (*env)->NewStringUTF(env, "压力场景无图像");
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
        return (*env)->NewStringUTF(env, "取位图信息失败");
    void *pix = NULL;
    if (AndroidBitmap_lockPixels(env, bmp, &pix) != ANDROID_BITMAP_RESULT_SUCCESS)
        return (*env)->NewStringUTF(env, "锁定位图失败");
    void *px = NULL;
    if (!readback(&g_st_ctx, &g_st_tgt, &px) || !px) {
        AndroidBitmap_unlockPixels(env, bmp);
        return (*env)->NewStringUTF(env, "回读失败");
    }
    int bw = (int) info.width, bh = (int) info.height;
    int cw = g_st_w < bw ? g_st_w : bw, ch = g_st_h < bh ? g_st_h : bh;
    for (int y = 0; y < ch; y++) {
        uint8_t *dst = (uint8_t *) pix + (size_t) y * info.stride;
        memcpy(dst, (const uint8_t *) px + (size_t) y * g_st_w * 4, (size_t) cw * 4);
    }
    AndroidBitmap_unlockPixels(env, bmp);
    return (*env)->NewStringUTF(env, "已拷入位图");
}

JNIEXPORT void JNICALL
Java_com_dsh_gputest_MainActivity_nativeStressStop(JNIEnv *env, jobject th) {
    LOG("原生入口: nativeStressStop 进入\n");
    (void) th;
    if (g_st_ctx_ok) { ctx_close(&g_st_ctx); g_st_ctx_ok = 0; }
    g_st_ready = 0;
    /* 返回类型已改为 void（与 Java 声明一致 ✓）*/
}

/* glm 要的名字（与 nativeStressClear 同义）*/
JNIEXPORT void JNICALL
Java_com_dsh_gputest_MainActivity_nativeStressClearColor(JNIEnv *env, jobject th, jint r, jint g, jint b) {
    LOG("原生入口: nativeStressClearColor 进入\n");
    (void) th;
    g_st_clear[0] = (float) (r & 0xFF) / 255.0f;
    g_st_clear[1] = (float) (g & 0xFF) / 255.0f;
    g_st_clear[2] = (float) (b & 0xFF) / 255.0f;
    char out[96];
    snprintf(out, sizeof(out), "清屏色已设为 R=%d G=%d B=%d", r & 0xFF, g & 0xFF, b & 0xFF);
    /* 返回类型已改为 void（与 Java 声明一致 ✓）*/
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeStressClear(JNIEnv *env, jobject th, jint r, jint g, jint b) {
    (void) th;
    g_st_clear[0] = (float) (r & 0xFF) / 255.0f;
    g_st_clear[1] = (float) (g & 0xFF) / 255.0f;
    g_st_clear[2] = (float) (b & 0xFF) / 255.0f;
    char out[96];
    snprintf(out, sizeof(out), "清屏色已设为 R=%d G=%d B=%d", r & 0xFF, g & 0xFF, b & 0xFF);
    return (*env)->NewStringUTF(env, out);
}

// ============================================================================
//  v7.0 · 光影（多 pass 光照）设置 —— 开关表 + 状态存储
//  用途：① 把光影负载跑起来 ② 开关可调（用户要求"可以控制一些开关"）
//  这些开关**必须真的影响渲染**（不然就是摆设 ✗）：见 litApply()
// ============================================================================
/* （LitCfg 已上移到光追代码之前，见下方）*/


static const char *lit_summary(char *buf, size_t n) {
    snprintf(buf, n,
        "光影设置：阴影=%s PCF=%dx%d 阴影图=%d 立方体=%d 泛光=%s 色调映射=%s 负载倍率=%d 视图=%s 动画=%s",
        g_lit.shadows ? "开" : "关", g_lit.pcfTaps, g_lit.pcfTaps, g_lit.shadowRes, g_lit.cubes,
        g_lit.bloom ? "开" : "关", g_lit.tonemap ? "开" : "关", g_lit.passesPerFrame,
        g_lit.debugView == 0 ? "最终" : (g_lit.debugView == 1 ? "阴影图" : "法线"),
        g_lit.animate ? "开" : "关");
    return buf;
}

/* 键名 → 取值（Java 侧用字符串键，避免两边常量号对不上 ✓）*/
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeLitConfig(JNIEnv *env, jobject th, jstring jkey, jint jval) {
    LOG("光影入口: nativeLitConfig 进入\n");
    (void) th;
    const char *k = jkey ? (*env)->GetStringUTFChars(env, jkey, NULL) : NULL;
    char out[256];
    if (k) {
        if (!strcmp(k, "shadows"))        g_lit.shadows = jval ? 1 : 0;
        else if (!strcmp(k, "pcf"))       g_lit.pcfTaps = (jval < 1) ? 1 : (jval > 5 ? 5 : jval);
        else if (!strcmp(k, "shadowRes")) g_lit.shadowRes = (jval < 512) ? 512 : (jval > 4096 ? 4096 : jval);
        else if (!strcmp(k, "cubes"))     g_lit.cubes = (jval < 1) ? 1 : (jval > 1024 ? 1024 : jval);
        else if (!strcmp(k, "bloom"))     g_lit.bloom = jval ? 1 : 0;
        else if (!strcmp(k, "tonemap"))   g_lit.tonemap = jval ? 1 : 0;
        else if (!strcmp(k, "passes"))    g_lit.passesPerFrame = (jval < 1) ? 1 : (jval > 64 ? 64 : jval);
        else if (!strcmp(k, "debugView")) g_lit.debugView = jval;
        else if (!strcmp(k, "animate"))   g_lit.animate = jval ? 1 : 0;
        (*env)->ReleaseStringUTFChars(env, jkey, k);
    }
    lit_summary(out, sizeof(out));
    return (*env)->NewStringUTF(env, out);
}

/* 一次性读取全部设置（面板初始化用 ✓）*/
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeLitSettings(JNIEnv *env, jobject th) {
    LOG("光影入口: nativeLitSettings 进入\n");
    (void) th;
    char out[320];
    snprintf(out, sizeof(out), "%d,%d,%d,%d,%d,%d,%d,%d,%d",
        g_lit.shadows, g_lit.pcfTaps, g_lit.shadowRes, g_lit.cubes,
        g_lit.bloom, g_lit.tonemap, g_lit.passesPerFrame, g_lit.debugView, g_lit.animate);
    return (*env)->NewStringUTF(env, out);
}

// ============================================================================
//  v7.0 · 光追（Ray Tracing）能力探测
//  诚实原则：**能开就真开，不能开就明说** ✗ 绝不用"软件假装"糊弄 ✓
//  判据：VK_KHR_acceleration_structure（必需）/ ray_query / ray_tracing_pipeline / deferred_host_operations
// ============================================================================
static int has_ext(const char *list, const char *want) {
    if (!list || !want) return 0;
    size_t n = strlen(want);
    const char *p = list;
    while ((p = strstr(p, want)) != NULL) {
        char c = p[n];
        if (c == '\0' || c == ' ') return 1;      /* 完整词匹配（防 VK_KHR_ray_query_foo）*/
        p += n;
    }
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeRtProbe(JNIEnv *env, jobject th, jstring jso) {
    LOG("原生入口: nativeRtProbe 进入\n");
    (void) th;
    const char *path = jso ? (*env)->GetStringUTFChars(env, jso, NULL) : NULL;
    char out[1024];
    Ctx c;
    if (!ctx_open(&c, path ? path : "system")) {
        if (path) (*env)->ReleaseStringUTFChars(env, jso, path);
        return (*env)->NewStringUTF(env, "光追探测：驱动不可用（先修驱动再谈光追）");
    }
    if (path) (*env)->ReleaseStringUTFChars(env, jso, path);
    /* 实例扩展 */
    PFN_vkEnumerateInstanceExtensionProperties eie =
        (PFN_vkEnumerateInstanceExtensionProperties) c_gipa(&c, "vkEnumerateInstanceExtensionProperties");
    char ext[8192]; ext[0] = '\0';
    uint32_t n = 0;
    if (eie && eie(NULL, &n, NULL) == VK_SUCCESS && n > 0 && n < 400) {
        VkExtensionProperties *ep = (VkExtensionProperties *) malloc(sizeof(VkExtensionProperties) * n);
        if (ep && eie(NULL, &n, ep) == VK_SUCCESS)
            for (uint32_t i = 0; i < n; i++) {
                strncat(ext, ep[i].extensionName, sizeof(ext) - strlen(ext) - 2);
                strncat(ext, " ", sizeof(ext) - strlen(ext) - 2);
            }
        free(ep);
    }
    /* 设备扩展 */
    PFN_vkEnumerateDeviceExtensionProperties ede =
        (PFN_vkEnumerateDeviceExtensionProperties) c_gipa(&c, "vkEnumerateDeviceExtensionProperties");
    n = 0;
    if (ede && c.pd && ede(c.pd, NULL, &n, NULL) == VK_SUCCESS && n > 0 && n < 400) {
        VkExtensionProperties *ep = (VkExtensionProperties *) malloc(sizeof(VkExtensionProperties) * n);
        if (ep && ede(c.pd, NULL, &n, ep) == VK_SUCCESS)
            for (uint32_t i = 0; i < n; i++) {
                strncat(ext, ep[i].extensionName, sizeof(ext) - strlen(ext) - 2);
                strncat(ext, " ", sizeof(ext) - strlen(ext) - 2);
            }
        free(ep);
    }
    int as = has_ext(ext, "VK_KHR_acceleration_structure");
    int rq = has_ext(ext, "VK_KHR_ray_query");
    int rp = has_ext(ext, "VK_KHR_ray_tracing_pipeline");
    int dh = has_ext(ext, "VK_KHR_deferred_host_operations");
    snprintf(out, sizeof(out),
        "【光追能力探测】\n"
        "  VK_KHR_acceleration_structure = %s（加速结构，**必需**）\n"
        "  VK_KHR_ray_query             = %s（光线查询）\n"
        "  VK_KHR_ray_tracing_pipeline  = %s（光追管线）\n"
        "  VK_KHR_deferred_host_operations = %s（异步构建）\n"
        "  ⇒ 判定：%s",
        as ? "有 ✓" : "无 ✗", rq ? "有 ✓" : "无 ✗", rp ? "有 ✓" : "无 ✗", dh ? "有 ✓" : "无 ✗",
        (as && (rq || rp)) ? "该驱动**支持硬件光追** ⇒ 可开真实光追 ✓"
                           : "该驱动**不支持硬件光追** ✗（PanVK/Mesa 现状如此）⇒ 开关将改用「CPU 光追参考图」，并明确标注 ✗ 不假装 ✓");
    ctx_close(&c);
    return (*env)->NewStringUTF(env, out);
}

// ============================================================================
//  v9.11 · 迷你 3D —— 独立最简 GPU 场景（自转立方体）
//  刻意**不复用**光影/光追的任何状态：自己的上下文、颜色图、深度图、渲染通道、
//  管线、命令池、栅栏、回读缓冲。设计上逐条避开今天踩过的坑：
//    ① 命令池带 RESET_COMMAND_BUFFER_BIT，且**每帧先 vkResetCommandBuffer 再 begin**（双保险）
//    ② 全部使用头文件里的 PFN_vk* 原型（绝不手写 typedef ⇒ 杜绝 ABI 错位）
//    ③ 回读前补一道**带 srcAccessMask/dstAccessMask** 的屏障（审查报告 B1 的正确写法）
//    ④ 回读缓冲走 mk_buf(host=1)：拿不到 HOST_VISIBLE 就硬失败，绝不退化
//    ⑤ 映射失败 / 提交失败都会**明确写进返回行**（不再静默）
//  返回行还带像素统计（min/max/均）⇒ 一眼分辨「GPU 没写」还是「写进去是黑的」
// ============================================================================
typedef struct { float mvp[16]; float t; float _pad[3]; int set[8]; } MiniPC;   /* v9.28: + 设置数组，80->112 字节（<=128 push constant 上限）*/   /* 80 字节，按 16 对齐 ✓ */

typedef struct {
    int ok, w, h;
    Ctx ctx;
    VkImage img;      VkDeviceMemory imgMem;  VkImageView view;
    VkImage dimg;     VkDeviceMemory dMem;    VkImageView dview;
    VkRenderPass rp;  VkFramebuffer fb;
    VkPipelineLayout pl; VkPipeline pipe;
    VkShaderModule vs, fs;
    VkCommandPool cpool; VkCommandBuffer cb; VkFence fence;
    VkBuffer rbuf;    VkDeviceMemory rMem;
    float mvp[16];
    double t0;
} Mini3D;
static Mini3D g_m3d;
static int g_m3set[8] = { 1, 3, 2048, 1, 1, 1, 1, 36 };   /* v9.28: 阴影/PCF/阴影图/泛光/色调映射/动画/负载/物体数 */

static void mini3d_destroy(void) {
    if (g_m3d.ctx.dev) {
        PFN_vkDeviceWaitIdle wid = (PFN_vkDeviceWaitIdle) g_m3d.ctx.gdpa(g_m3d.ctx.dev, "vkDeviceWaitIdle");
        if (wid) wid(g_m3d.ctx.dev);
        ctx_close(&g_m3d.ctx);          /* 设备一关，其下所有对象由驱动回收 ✓ */
    }
    memset(&g_m3d, 0, sizeof(g_m3d));
}

JNIEXPORT jint JNICALL Java_com_dsh_gputest_MainActivity_nativeMini3DInit(JNIEnv *env, jobject th, jstring jso, jint w, jint h) {
    (void) th;
    if (g_m3d.ok) return 1;
    const char *so = jso ? (*env)->GetStringUTFChars(env, jso, NULL) : NULL;
    Ctx *c = &g_m3d.ctx;
    LOG("── mini3d init %dx%d · so=%s ──\n", (int) w, (int) h, so ? so : "system");
    int opened = ctx_open(c, so ? so : "system");
    if (so) (*env)->ReleaseStringUTFChars(env, jso, so);
    if (!opened) { LOG("X mini3d: ctx_open 失败\n"); memset(&g_m3d, 0, sizeof(g_m3d)); return 0; }
    g_m3d.w = (int) w; g_m3d.h = (int) h;

    /* ① 颜色图：COLOR_ATTACHMENT | TRANSFER_SRC（回读必需 ✓） */
    if (!mk_img(c, (int) w, (int) h, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &g_m3d.img, &g_m3d.imgMem)) {
        LOG("X mini3d: 颜色图失败\n"); mini3d_destroy(); return 0; }
    g_m3d.view = mk_view(c, g_m3d.img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    /* ② 深度图：有深度就不必纠结三角形绕序 ✓ */
    if (!mk_img(c, (int) w, (int) h, VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &g_m3d.dimg, &g_m3d.dMem)) {
        LOG("X mini3d: 深度图失败\n"); mini3d_destroy(); return 0; }
    g_m3d.dview = mk_view(c, g_m3d.dimg, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!g_m3d.view || !g_m3d.dview) { LOG("X mini3d: 视图失败\n"); mini3d_destroy(); return 0; }

    /* ③ 渲染通道：颜色 finalLayout = TRANSFER_SRC_OPTIMAL ⇒ 回读时布局已就位 ✓ */
    PFN_vkCreateRenderPass crp = (PFN_vkCreateRenderPass) c->gdpa(c->dev, "vkCreateRenderPass");
    if (!crp) { mini3d_destroy(); return 0; }
    VkAttachmentDescription at[2]; memset(at, 0, sizeof(at));
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM;  at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    at[1].format = VK_FORMAT_D32_SFLOAT;      at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dr = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp; memset(&sp, 0, sizeof(sp));
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr; sp.pDepthStencilAttachment = &dr;
    VkSubpassDependency dep; memset(&dep, 0, sizeof(dep));
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sp,
        .dependencyCount = 1, .pDependencies = &dep };
    if (crp(c->dev, &rci, NULL, &g_m3d.rp) != VK_SUCCESS) { LOG("X mini3d: render pass 失败\n"); mini3d_destroy(); return 0; }

    /* ④ framebuffer */
    VkImageView atts[2] = { g_m3d.view, g_m3d.dview };
    PFN_vkCreateFramebuffer cfb = (PFN_vkCreateFramebuffer) c->gdpa(c->dev, "vkCreateFramebuffer");
    VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g_m3d.rp,
        .attachmentCount = 2, .pAttachments = atts, .width = (uint32_t) w, .height = (uint32_t) h, .layers = 1 };
    if (!cfb || cfb(c->dev, &fbci, NULL, &g_m3d.fb) != VK_SUCCESS) { LOG("X mini3d: framebuffer 失败\n"); mini3d_destroy(); return 0; }

    /* ⑤ 着色器模块（SPIR-V 来自 shaders_mini3d.h） */
    PFN_vkCreateShaderModule csm = (PFN_vkCreateShaderModule) c->gdpa(c->dev, "vkCreateShaderModule");
    if (!csm) { mini3d_destroy(); return 0; }
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = mini3d_vert_spv_len * 4, .pCode = mini3d_vert_spv };
    if (csm(c->dev, &smi, NULL, &g_m3d.vs) != VK_SUCCESS) { LOG("X mini3d: vs 模块失败\n"); mini3d_destroy(); return 0; }
    smi.codeSize = mini3d_frag_spv_len * 4; smi.pCode = mini3d_frag_spv;
    if (csm(c->dev, &smi, NULL, &g_m3d.fs) != VK_SUCCESS) { LOG("X mini3d: fs 模块失败\n"); mini3d_destroy(); return 0; }

    /* ⑥ 管线布局：只有 push constant（无描述符 ⇒ 少一半出错面 ✓） */
    PFN_vkCreatePipelineLayout cpl = (PFN_vkCreatePipelineLayout) c->gdpa(c->dev, "vkCreatePipelineLayout");
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MiniPC) };   /* v9.14：片元也读 t ✓ */
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (!cpl || cpl(c->dev, &plci, NULL, &g_m3d.pl) != VK_SUCCESS) { LOG("X mini3d: 管线布局失败\n"); mini3d_destroy(); return 0; }

    /* ⑦ 图形管线 */
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof(st));
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = g_m3d.vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_m3d.fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi; memset(&vi, 0, sizeof(vi)); vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, (float) w, (float) h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { (uint32_t) w, (uint32_t) h } };
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof(vs));
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1; vs.pViewports = &vp; vs.scissorCount = 1; vs.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof(dss));
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof(cba));
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs; memset(&cbs, 0, sizeof(cbs));
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof(gp));
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss;
    gp.pColorBlendState = &cbs; gp.layout = g_m3d.pl; gp.renderPass = g_m3d.rp; gp.subpass = 0;
    PFN_vkCreateGraphicsPipelines cgp = (PFN_vkCreateGraphicsPipelines) c->gdpa(c->dev, "vkCreateGraphicsPipelines");
    if (!cgp || cgp(c->dev, VK_NULL_HANDLE, 1, &gp, NULL, &g_m3d.pipe) != VK_SUCCESS) {
        LOG("X mini3d: 管线创建失败\n"); mini3d_destroy(); return 0; }

    /* ⑧ 命令池（★ 带 RESET 标志）+ 命令缓冲 + 栅栏 */
    PFN_vkCreateCommandPool ccp = (PFN_vkCreateCommandPool) c->gdpa(c->dev, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers acb = (PFN_vkAllocateCommandBuffers) c->gdpa(c->dev, "vkAllocateCommandBuffers");
    PFN_vkCreateFence cf = (PFN_vkCreateFence) c->gdpa(c->dev, "vkCreateFence");
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->qfam };
    if (!ccp || !acb || !cf || ccp(c->dev, &cpci, NULL, &g_m3d.cpool) != VK_SUCCESS) { LOG("X mini3d: 命令池失败\n"); mini3d_destroy(); return 0; }
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_m3d.cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (acb(c->dev, &cbai, &g_m3d.cb) != VK_SUCCESS) { LOG("X mini3d: 命令缓冲失败\n"); mini3d_destroy(); return 0; }
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (cf(c->dev, &fci, NULL, &g_m3d.fence) != VK_SUCCESS) { LOG("X mini3d: 栅栏失败\n"); mini3d_destroy(); return 0; }

    /* ⑨ 回读缓冲（host=1 ⇒ 严格要 HOST_VISIBLE，拿不到就硬失败 ✓） */
    if (!mk_buf(c, (VkDeviceSize) w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_m3d.rbuf, &g_m3d.rMem, 1)) {
        LOG("X mini3d: 回读缓冲失败\n"); mini3d_destroy(); return 0; }

    /* ⑩ 固定相机 MVP（自转在顶点着色器里做 ⇒ 这里只算一次 ✓） */
    float eye[3] = { 3.4f, 2.2f, 3.4f }, tgt[3] = { 0.0f, 0.0f, 0.0f };
    lit_mvp(g_m3d.mvp, (float) w / (float) (h > 0 ? (int) h : 1), eye, tgt, 0.95f);
    g_m3d.t0 = now_ms();
    g_m3d.ok = 1;
    LOG("mini3d: 就绪 ✓ %dx%d（颜色 R8G8B8A8 + 深度 D32，独立管线/命令池）\n", (int) w, (int) h);
    return 1;
}

JNIEXPORT void JNICALL Java_com_dsh_gputest_MainActivity_nativeRtSettings(JNIEnv *env, jobject th,
        jint samples, jint bounce, jint detail, jint bloomPct, jint lightMove, jint mirrorPct) {
    (void) env; (void) th;
    g_rt_opt[0] = samples; g_rt_opt[1] = bounce; g_rt_opt[2] = detail;
    g_rt_opt[3] = bloomPct; g_rt_opt[4] = lightMove; g_rt_opt[5] = mirrorPct;
    snprintf(g_log + g_len, sizeof(g_log) - g_len, "光追配置 -> 采样=%d 弹射=%d 细节=%d 光晕=%d 灯速=%d 镜面=%d", samples, bounce, detail, bloomPct, lightMove, mirrorPct);
    LOG("%s", "\n");
}

JNIEXPORT void JNICALL Java_com_dsh_gputest_MainActivity_nativeMini3DSettings(JNIEnv *env, jobject th, jint shadows, jint pcf, jint res, jint bloom, jint tonemap, jint animate, jint passes) {
    (void) env; (void) th;
    g_m3set[0] = shadows; g_m3set[1] = pcf; g_m3set[2] = res; g_m3set[3] = bloom;
    g_m3set[4] = tonemap; g_m3set[5] = animate; g_m3set[6] = (passes < 1) ? 1 : passes;
    LOG("  mini3D 设置 -> shadows=%d pcf=%d res=%d bloom=%d tonemap=%d anim=%d load=%d",
        shadows, pcf, res, bloom, tonemap, animate, passes);
}

JNIEXPORT jstring JNICALL Java_com_dsh_gputest_MainActivity_nativeMini3DFrame(JNIEnv *env, jobject th, jobject bmp) {
    (void) th;
    char out[420];
    if (!g_m3d.ok) return (*env)->NewStringUTF(env, "X mini3d 未初始化");
    Ctx *c = &g_m3d.ctx;
    PFN_vkResetCommandBuffer  rcb = (PFN_vkResetCommandBuffer)  c->gdpa(c->dev, "vkResetCommandBuffer");
    PFN_vkBeginCommandBuffer  bcb = (PFN_vkBeginCommandBuffer)  c->gdpa(c->dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer    ecb = (PFN_vkEndCommandBuffer)    c->gdpa(c->dev, "vkEndCommandBuffer");
    PFN_vkCmdBeginRenderPass  brp = (PFN_vkCmdBeginRenderPass)  c->gdpa(c->dev, "vkCmdBeginRenderPass");
    PFN_vkCmdEndRenderPass    erp = (PFN_vkCmdEndRenderPass)    c->gdpa(c->dev, "vkCmdEndRenderPass");
    PFN_vkCmdBindPipeline     bp  = (PFN_vkCmdBindPipeline)     c->gdpa(c->dev, "vkCmdBindPipeline");
    PFN_vkCmdPushConstants    pc_ = (PFN_vkCmdPushConstants)    c->gdpa(c->dev, "vkCmdPushConstants");
    PFN_vkCmdDraw             dr  = (PFN_vkCmdDraw)             c->gdpa(c->dev, "vkCmdDraw");
    PFN_vkCmdPipelineBarrier  bar = (PFN_vkCmdPipelineBarrier)  c->gdpa(c->dev, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImageToBuffer c2b = (PFN_vkCmdCopyImageToBuffer) c->gdpa(c->dev, "vkCmdCopyImageToBuffer");
    PFN_vkResetFences         rf  = (PFN_vkResetFences)         c->gdpa(c->dev, "vkResetFences");
    PFN_vkQueueSubmit         qs  = (PFN_vkQueueSubmit)         c->gdpa(c->dev, "vkQueueSubmit");
    PFN_vkWaitForFences       wf  = (PFN_vkWaitForFences)       c->gdpa(c->dev, "vkWaitForFences");
    if (!rcb||!bcb||!ecb||!brp||!erp||!bp||!pc_||!dr||!bar||!c2b||!rf||!qs||!wf)
        return (*env)->NewStringUTF(env, "X mini3d 命令入口缺失");

    rcb(g_m3d.cb, 0);                                   /* ★ 先 reset 再 begin（池也带 RESET 标志 ✓） */
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult r = bcb(g_m3d.cb, &bi);
    if (r != VK_SUCCESS) { snprintf(out, sizeof(out), "X mini3d begin 失败 r=%d", r); return (*env)->NewStringUTF(env, out); }

    VkClearValue cv[2]; memset(cv, 0, sizeof(cv));
    cv[0].color.float32[0] = 0.05f; cv[0].color.float32[1] = 0.06f; cv[0].color.float32[2] = 0.09f; cv[0].color.float32[3] = 1.0f;
    cv[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_m3d.rp, .framebuffer = g_m3d.fb,
        .renderArea = { { 0, 0 }, { (uint32_t) g_m3d.w, (uint32_t) g_m3d.h } },
        .clearValueCount = 2, .pClearValues = cv };
    brp(g_m3d.cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    bp(g_m3d.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_m3d.pipe);
    MiniPC pcv; memset(&pcv, 0, sizeof(pcv));
    memcpy(pcv.mvp, g_m3d.mvp, sizeof(pcv.mvp));
    memcpy(pcv.set, g_m3set, sizeof(pcv.set));   /* v9.28 */
    pcv.t = (float) ((now_ms() - g_m3d.t0) / 1000.0);
    pc_(g_m3d.cb, g_m3d.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcv), &pcv);   /* v9.14 */
    dr(g_m3d.cb, 3, 1, 0, 0);   /* v9.14：全屏三角形，3D 在片元里解析求交 ✓ */
    erp(g_m3d.cb);

    /* ★ 回读前的屏障：带 src/dst accessMask（B1 的正确写法，光影那条路径当年正是缺这个） */
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = g_m3d.img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    bar(g_m3d.cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t) g_m3d.w, (uint32_t) g_m3d.h, 1 } };
    c2b(g_m3d.cb, g_m3d.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_m3d.rbuf, 1, &bc);

    VkResult r2 = ecb(g_m3d.cb);
    if (r2 != VK_SUCCESS) { snprintf(out, sizeof(out), "X mini3d end 失败 r=%d", r2); return (*env)->NewStringUTF(env, out); }
    rf(c->dev, 1, &g_m3d.fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_m3d.cb };
    r = qs(c->q, 1, &si, g_m3d.fence);
    if (r == VK_SUCCESS) r = wf(c->dev, 1, &g_m3d.fence, VK_TRUE, 3000000000ULL);
    if (r != VK_SUCCESS) {
        snprintf(out, sizeof(out), "X mini3d 提交/等待失败 r=%d（%s）", r, r == -4 ? "VK_ERROR_DEVICE_LOST" : "见 VkResult 表");
        return (*env)->NewStringUTF(env, out);
    }

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
        return (*env)->NewStringUTF(env, "X mini3d: 取位图信息失败");
    void *pix = NULL;
    if (AndroidBitmap_lockPixels(env, bmp, &pix) != ANDROID_BITMAP_RESULT_SUCCESS)
        return (*env)->NewStringUTF(env, "X mini3d: 锁定位图失败");
    PFN_vkMapMemory   mp = (PFN_vkMapMemory)   c->gdpa(c->dev, "vkMapMemory");
    PFN_vkUnmapMemory um = (PFN_vkUnmapMemory) c->gdpa(c->dev, "vkUnmapMemory");
    void *src = NULL;
    int mapped = (mp && um && mp(c->dev, g_m3d.rMem, 0, VK_WHOLE_SIZE, 0, &src) == VK_SUCCESS && src);
    double pmin = -1, pmax = -1, pavg = -1;
    if (mapped) {
        unsigned char *sb = (unsigned char *) src;
        size_t tot = (size_t) g_m3d.w * g_m3d.h * 4, sum = 0, cnt = 0;
        int mn = 255, mx = 0;
        for (size_t i = 0; i < tot; i += 64) { int v = sb[i]; if (v < mn) mn = v; if (v > mx) mx = v; sum += (size_t) v; cnt++; }
        if (cnt) { pmin = mn; pmax = mx; pavg = (double) sum / (double) cnt; }
        int cw = g_m3d.w < (int) info.width  ? g_m3d.w : (int) info.width;
        int chh = g_m3d.h < (int) info.height ? g_m3d.h : (int) info.height;
        for (int y = 0; y < chh; y++)
            memcpy((uint8_t *) pix + (size_t) y * info.stride, sb + (size_t) y * g_m3d.w * 4, (size_t) cw * 4);
        um(c->dev, g_m3d.rMem);
    }
    AndroidBitmap_unlockPixels(env, bmp);
    snprintf(out, sizeof(out), "%s 迷你3D ✓ 自转立方体 · 像素 min %.0f/max %.0f/均 %.1f · 位图 %dx%d stride %d",
             mapped ? "已拷入位图" : "X 回读映射失败", pmin, pmax, pavg, (int) info.width, (int) info.height, (int) info.stride);
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT void JNICALL Java_com_dsh_gputest_MainActivity_nativeMini3DStop(JNIEnv *env, jobject th) {
    (void) env; (void) th;
    mini3d_destroy();
}
