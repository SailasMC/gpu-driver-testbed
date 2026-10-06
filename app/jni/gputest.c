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
#include <android/bitmap.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
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
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    STEP("vkCreateDevice（建逻辑设备）");
    LOG("[6] 即将调用 vkCreateDevice …\n");
    r = createDev(c->pd, &dci, NULL, &c->dev);
    STEP("(已返回) vkCreateDevice");
    LOG("[6] vkCreateDevice -> %d %s\n", r, r == VK_SUCCESS ? "OK" : "失败");
    if (r != VK_SUCCESS) return 0;

    PFN_vkGetDeviceQueue getQ = (PFN_vkGetDeviceQueue) c->gdpa(c->dev, "vkGetDeviceQueue");
    if (!getQ) return 0;
    getQ(c->dev, c->qfam, 0, &c->q);
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
            if (ok) LOG("\n== 分数(fill) = %.0f Mpixel/s ==\n", st.ops * st.pixels_per_op / 1e6 / st.secs);
        } else if (!strcmp(mode, "blit")) {
            if (!target_make(&c, &t2, 1024, 1024, 0)) { LOG("X 第二目标创建失败\n"); goto out2; }
            ok = bench_blit(&c, &t, &t2, secs, &st); bench_report("blit 拷贝带宽", &st);
            if (ok) LOG("\n== 分数(blit) = %.2f GB/s ==\n", st.ops * st.bytes_per_op / 1e9 / st.secs);
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
    (void) env; (void) th;
    g_rinited = 0; g_rPixels = NULL;
}

// ---- 能力清单（学 Vulkan Hardware Capability Viewer 的思路）----
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeCaps(JNIEnv *env, jobject th, jstring jso) {
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
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void) uc;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "CRASH sig=%d addr=%p\n", sig, si ? si->si_addr : NULL);
    int fd = open("/data/local/tmp/gputest_crash.txt", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) fd = open("/storage/emulated/0/Download/gputest_crash.txt", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { write(fd, buf, n); close(fd); }
    signal(sig, SIG_DFL);
    raise(sig);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) vm; (void) reserved;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
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

JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeStressStop(JNIEnv *env, jobject th) {
    (void) th;
    if (g_st_ctx_ok) { ctx_close(&g_st_ctx); g_st_ctx_ok = 0; }
    g_st_ready = 0;
    return (*env)->NewStringUTF(env, "压力场景已释放");
}

/* glm 要的名字（与 nativeStressClear 同义）*/
JNIEXPORT jstring JNICALL
Java_com_dsh_gputest_MainActivity_nativeStressClearColor(JNIEnv *env, jobject th, jint r, jint g, jint b) {
    (void) th;
    g_st_clear[0] = (float) (r & 0xFF) / 255.0f;
    g_st_clear[1] = (float) (g & 0xFF) / 255.0f;
    g_st_clear[2] = (float) (b & 0xFF) / 255.0f;
    char out[96];
    snprintf(out, sizeof(out), "清屏色已设为 R=%d G=%d B=%d", r & 0xFF, g & 0xFF, b & 0xFF);
    return (*env)->NewStringUTF(env, out);
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
