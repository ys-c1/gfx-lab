# mixer：两段式、按总线压缩

旧行为是整段一个参数（一个门限、一个压缩比、一个电平），多轨之间谁轻谁重
都被同一个增益一起压，轻的总线会被重总线拖下去。这里拆成两段：

1. **按总线估电平**（`mx_estimate` / `mx_estimate_buses`）：每条总线各自算
   `peak = max|x|`、`rms = sqrt(mean(x^2))`，按 `cfg.detector` 选出 `level`。
2. **按各自门限压缩**（`mx_compress` / `mx_compress_buses`）：每条总线用自己
   的门限、压缩比算一个静态增益，整条总线（所有声道）统一乘这个增益：

   ```
   gain = (level / threshold) ^ (1/ratio - 1)   当 enabled && ratio > 1 && level > threshold
   gain = 1                                     其余情况
   ```

`mx_process_buses()` 是一次跑完两段；`mx_compress_legacy()` 保留旧的整段行为。

## 关键性质

- **声道联动**：增益按整条总线算一次再乘到每个声道，立体声像不会偏移。
- **退化即旧行为**：`enabled = 0`、`ratio = 1`、或 `level <= threshold` 时
  增益恰为 1，缓冲区逐字节不变（测试覆盖）。
- **单总线批量 == 旧整段**：`mx_compress_legacy()` 就是单总线的
  `mx_process_buses()`，`bench` 里对每条标尺都验证两者逐字节相同。
- **空输入报错**：`frames <= 0` 或总线数为 0 返回 `MX_ERR_EMPTY`，
  `mx_err_str()` 给出原因（"empty input: frames <= 0, or bus count <= 0
  (nothing to compress)"）；批量里只要有一条总线非法，整批都不改写
  （先全部校验再动手）。
- **响度**：报告里的 `loudness_*` 是 RMS dBFS（`20 log10 rms`），不是 LUFS。

## 接口速览

```c
typedef struct { float *buf; int frames; int channels; } MxBusView;
typedef struct { MxDetector detector; float threshold, ratio; int enabled; } MxCompCfg;
typedef struct { float peak, rms, level; int frames, channels; long long samples; } MxLevel;
typedef struct { ... } MxBusReport;   /* 见 mixer.h */

int mx_estimate_buses(const MxBusView*, const MxCompCfg*, int count, MxLevel*);
int mx_compress_buses(MxBusView*, const MxCompCfg*, const MxLevel*, int count, MxBusReport*);
int mx_process_buses(MxBusView*, const MxCompCfg*, int count, MxBusReport*);
int mx_compress_legacy(float *buf, int frames, int channels, const MxCompCfg*, MxBusReport*);
int mx_write_bus_csv(const MxBusReport*, int count, const char *path);
```

`MxBusReport` 每条总线一行：`level`、`gain`、`gain_db`、`reduction_db`、
`peak_before/after`、`rms_before/after`、`loudness_before/after/delta`、
`samples_over_threshold`、`compressed_samples`、`samples`。`mx_write_bus_csv()`
把这张对照表原样导出成 CSV。

## 跑法

```
make -C mixer test    # 222 项检查：手算表逐字段、逐总线电平、声道联动、
                      # 退化路径逐字节、旧行为等价、空输入、参数校验、批量原子性、CSV
make -C mixer demo    # 3 条总线的对照表 + build/buses_demo.csv + demo.wav
make -C mixer bench   # 小图/大图各一遍 + 手算表核对 + 独立 Python 复算
```

`make bench` 依次做：

1. 手算表夹具（8 条总线：过门限、恰好等于门限、RMS 检测、稀疏信号、低门限、
   `ratio = 1`、旁路、立体声联动）导出 `build/buses_hand.csv`；
2. **小图** 1 s、48 kHz、2 声道、4 条总线；**大图** 10 s、2 声道、16 条总线；
   分别报估算/压缩耗时（7 次中位数）、对独立双精度参考的电平与增益误差、
   总峰值前后、压缩样本数、以及每条总线的对照表（导出 `build/buses_small.csv`、
   `build/buses_large.csv`）；
3. `ref_bus.py` 在 Python 里从原始 f32 转储重新算电平、增益、峰值、RMS、
   响度差与压缩样本数，逐格比对 CSV；并把 `build/buses_hand.csv` 与仓库里的
   `mixer/hand_table.csv` 逐格比对；再把 reported gain 乘回输入，逐样本核对
   输出转储。

`hand_table.csv` 是用独立的 Python 脚本按同样公式、float32 取整算出来的期望值
（不是从 C 代码导出的结果），因此 `ref_bus.py` 的手算核对是真正的一一对照。

## 参照结果（本机，-O2）

| 规模 | 总线 | 样本数 | 估算 | 压缩 | 电平误差 | 增益误差 | 峰值 前→后 |
|---|---|---|---|---|---|---|---|
| 小图 | 4 | 0.38 M | 0.33 ms | 0.55 ms | 2.1e-8 | 2.8e-9 | 1.5800 → 0.7385（-6.61 dB） |
| 大图 | 16 | 15.36 M | 14.1 ms | 24.0 ms | 9.5e-9 | 2.7e-8 | 2.0117 → 1.5995（-1.99 dB） |

同一次 `bench` 还会打印："按各自门限有 2/4（小图）、7/16（大图）条总线保持
原样不动；而旧的整段单参数会给每条总线同一个 -7.59 dB / -11.16 dB 增益。"
这正是本次改动的动机：轻的总线不再被重总线带下去。
