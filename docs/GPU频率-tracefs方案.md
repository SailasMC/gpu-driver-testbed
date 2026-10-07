# GPU 频率获取：sysfs 死路 → tracefs 活路（实测配方）

> 结论先说：**未 root 的 OPPO/MTK（PHZ110 / MT6989 / Android 16）能拿到 GPU 频率** ✓
> 走 **tracefs 的 `power/gpu_frequency` 事件**，shell uid（uid=2000，Shizuku/Stellar 通道）即可 ✓
> 关键有两条：**`enable` 文件是 0666** ✓ + **必须先打开总开关 `tracing_on`** ✓（这一条最容易漏 ✗）

---

## 一、被证伪的路（别再试了 ✗）

全部 **Permission denied**（shell 域 `u:r:shell:s0`，uid 2000）：

| 路径 | 结果 |
|---|---|
| `/sys/class/devfreq/13000000.mali/cur_freq` | ✗ Permission denied |
| `/sys/class/devfreq/13000000.mali/max_freq` / `min_freq` / `governor` / `available_frequencies` | ✗ 全部拒绝 |
| `/proc/gpufreqv2/fix_target_opp_index` / `opp_dump` / `aging_dump` | ✗ 全部拒绝 |
| `/sys/kernel/ged/hal/gpu_utilization`（MTK GED） | ✗ 拒绝 |
| `/proc/gpufreq/gpufreq_opp_dump` / `/proc/gpufreq/gpufreq` | ✗ 不存在 |

**重要**：换 **Shizuku UserService** 也没用 ✗ —— Shizuku 服务本身就在 **shell 域**，不比 App 的 `newProcess` 宽 ✓（**实测确认**，省掉了一次白做的实现 ✗）

## 二、活路：tracefs + ftrace 事件

### 关键事实（实测）

| 项 | 结果 |
|---|---|
| `/sys/kernel/tracing/` | ✅ 存在（`/sys/kernel/debug/tracing` 不存在） |
| `events/power/gpu_frequency/enable` | ✅ **`-rw-rw-rw-`（0666，全世界可写）** ← **这是钥匙** |
| `events/power/gpu_work_period/enable` | ✅ 0666 |
| `events/gpu_mem/gpu_mem_total/enable` | ✅ 0666 |
| `tracing_on` | ✅ 0666 可写（**默认是 0** ✗ 不打开就什么都抓不到） |
| `/sys/kernel/tracing/trace` | ✅ 可读（**明文文本**，含时间戳与事件字段） |
| shell 所属组 | 含 **`3012(readtracefs)`** ✓ |
| `/system/bin/perfetto` | ✅ 存在（但**本方案不需要它** ✓） |

### 完整配方（一条复合命令即可）

```sh
T0=$(cat /sys/kernel/tracing/tracing_on)                    # 记原值，便于还原 ✓
echo 1 > /sys/kernel/tracing/tracing_on                     # ★★ 总开关（漏了这步就永远为空 ✗）
echo 1 > /sys/kernel/tracing/events/power/gpu_frequency/enable
sleep 1
grep -a -m8 gpu_frequency /sys/kernel/tracing/trace          # 明文读 ✓
echo 0 > /sys/kernel/tracing/events/power/gpu_frequency/enable
echo $T0 > /sys/kernel/tracing/tracing_on                    # 恢复原值 ⇒ 不留副作用 ✓
```

### 实测输出（本机真实数据）

```
loros.gallery3d-8935 [000] d.h.. 129142.529465: gpu_frequency: state=260000 gpu_id=0
loros.gallery3d-8935 [000] d.h.. 129142.533442: gpu_frequency: state=26000  gpu_id=0
<idle>-0             [000] d.h1. 129142.557762: gpu_frequency: state=26000  gpu_id=0
<idle>-0             [000] d.h1. 129142.558345: gpu_frequency: state=260000 gpu_id=0
```

- `state=` 即频率值，单位 **kHz** ⇒ `260000` = **260 MHz** ✓、`26000` = **26 MHz**（空闲低档 ✓）
- 记录**只在频率变化时产生** ✓（空载 2 秒可能一条都没有 ✗ 这是特性不是 bug ✓）
- 抓取期间缓冲里有 701 条记录 ✓（含其它事件 ✓）

## 三、踩过的坑（按发生顺序）

| 现象 | 真因 | 解法 |
|---|---|---|
| `perfetto` 报 `Could not open /data/local/tmp/gpu.cfg` (errno 13) | perfetto 进程不能读该目录 | 放 **`/data/misc/perfetto-configs/`**（工具自己提示的正解 ✓） |
| `Unexpected character ':'` | trace config 写成**单行 textproto** ✗ | 改**多行** ✓（或用本方案，根本不需要 perfetto ✓） |
| `--txt` 输出 `Binary file matches` | 数值仍是 protobuf 二进制 ✗ 只有事件名是文本 ✓ | **放弃 perfetto**，直接读 `trace` 明文 ✓ |
| event 打开成功但 `trace` 为空 ✗ | **`tracing_on` = 0** ✗ | `echo 1 > tracing_on` ✓ |
| `grep` 报 Permission denied 于 `available_events` | 该文件不可读 ✗ | 读 `events/<sub>/` 目录列表即可 ✓ |

## 四、软件中的落地

| 版本 | 内容 |
|---|---|
| 11.8 | 首次接入（**缺 `tracing_on`** ✗ ⇒ 抓不到） |
| **11.9** | ✅ 补上总开关 + 采完恢复原值 ⇒ 探针与实时监视都能出频率记录 |
| **12.0** | ✅ 解析 `state=` ⇒ MHz + sparkline 趋势（最近 24 次、最低/最高）⇒ 与温度/帧时间并排 |

## 五、还能顺手拿的（同一机制 ✓ 已确认 0666 ✓）

| 事件 | 内容 |
|---|---|
| `gpu_mem/gpu_mem_total` | 逐进程 GPU 显存 ✓ |
| `power/gpu_work_period` | GPU 工作周期 ✓（若为周期事件可直接做占用率 ✓） |

## 六、许可与边界

- 方案只用系统自带的 tracefs 与 ftrace ✓ 无第三方代码 ✓ 无 root ✓
- **必须成对开关并恢复 `tracing_on`** ✓（不要长期占用 ftrace 缓冲 ⇒ 会影响系统 trace/atrace ✓）
- 其它厂商（Adreno 等）事件名可能不同：先列 `events/power/` 再挑 ✓ 一般写作 `gpu_frequency` ✓
