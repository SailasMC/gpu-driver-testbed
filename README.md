# GPU 驱动测试台 (gpu-driver-testbed)

免 root、不依赖 Shizuku 的真机 **GPU/Vulkan 驱动测试与压力工具**。

- **两种测试路径**：① 直连 Vulkan 驱动（把任意 ICD 当驱动加载）② 转译链（渲染器 → Vulkan 驱动）
- **权威跑分口径**：全部用 **GPU 时间戳**计时（不只是 CPU 墙钟），并给出 **GPU 占空比**
- **压力测试**：三角形数自动递增（Auto Increment ppf），压力下记录**驱动失效阈值**
- **参考图比对**：内置 **CPU 软件渲染器**（确定性）产出参考帧，与 GPU 帧逐像素比对（±2 LSB）
- **驱动/渲染器分类**：按插件元数据（`driver` / `renderer` / `FCLNativePlugin`）区分两层

## 下载
最新版本见 [Releases](../../releases)。APK 直接安装即可。

## 目录
```
app/        App 源码（AndroidManifest.xml / src / jni / res / build_app.sh）
tools/      Vulkan 转发垫片、ICD 探测、驱动打包脚本
docs/       使用说明
research/   方法论与调研（含权威跑分口径的出处）
scripts/    release.sh —— 一条命令构建 + 打 tag + 发 Release
```

## 发布
```bash
bash scripts/release.sh 5.3 "压力页崩点量化；CPU 参考图比对 UI"
```

## 说明
自签名调试包，不保证任何设备可用；跑分口径为横向对比口径，不等同于 3DMark/GFXBench 分数。
