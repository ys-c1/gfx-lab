# mixer —— 总线混音与带阻尼峰值电平表

## 电平表（meter.c / meter.h）

针对「回落太快、看不出真实动态」的问题，显式分两段实现，两个函数各自可测：

**第一段：按表头阻尼算回落速度（系数）** — `meter_fall_alpha()`

表头阻尼用回落速度 `v = fall_db_per_s`（dB/秒）描述。线性幅度每秒的衰减
因子为 `g = 10^(-v/20)`，换算到每个采样：

```
alpha = g^(1/fs) = 10^(-v / (20*fs))
```

**第二段：按速度更新读数** — `meter_update()` / `meter_process()`

```
target = |x|
peak = target                      若 target >= peak   （攻击：即时冲上去）
peak = max(target, peak * alpha)   否则                （回落：指数阻尼，不低于真实信号）
```

**退化（旧行为）**：`fall_db_per_s <= 0` 时 `alpha = 0`，读数逐点立即跟随
`|x|`，此时块峰值与朴素 `max|x|` 完全一致（单测 `test_meter_legacy` 锁定）。

**空输入报错**：`frames <= 0` 返回 `METER_E_EMPTY(-2)`，`NULL` 返回
`METER_E_NULL(-1)`，参数非法返回 `METER_E_PARAM(-3)`，均可用
`meter_strerror()` 取中文原因。

### 关键量

| 量 | 含义 |
|---|---|
| `alpha` | 每采样阻尼系数（第一段输出） |
| `fall_db_per_s` | 表头阻尼/回落速度 dB/s |
| `dynamic_range_db` | 动态范围，对应线性下限 `10^(-DR/20)` |
| 回落时间 | 读数下降指定 dB 的首穿时间，理论 `drop_db / v` |

### 测试与核对（小图、大图各一遍）

```
make -C mixer test     # C 单元测试（含错误码、退化、黄金点、回落时间）
make -C mixer bench    # 两图计时 + 回落时间 + 导出读数曲线 CSV
python3 -I mixer/tools/verify.py   # 三层模型逐行核对并出报告
```

`verify.py` 用三个独立模型互相印证：

- **理想模型**：Python double 精度，物理真值；
- **f32 模型**：Python 用 `struct` 把每一步运算强制舍入到 binary32，模拟 C 的 float；
- **C 实现**：实际编译运行 `meter.c` 导出的曲线。

C 对 f32 模型应只相差数个 ulp（实现正确性）；f32 对理想模型的差是 float32
连乘累积的物理误差（给解析上界，仅信息性）。回落时间误差分解为
「离散量化 + alpha 系数量化漂移」与「实现误差（应为 0 采样）」。

产物在 `artifacts/`：`verify_report.txt`（核对报告）、`hand_small/large.csv`
（手算表）、`curve_small/large.csv`（C 导出的读数曲线）、`bench.log`、
`c_test.log`。手算表与 C 曲线列结构一致：
`sample,t_s,target_lin,peak_lin,peak_db`，可逐行 diff。

### 文件

- `src/meter.h` / `src/meter.c`：电平表两段式实现
- `bench.c`：小图（fs=10, 20 dB/s, DR 60, 15 点）与大图（fs=48000, 33 dB/s,
  DR 80, 96000 点）基准，导出曲线
- `tools/handcalc.py`：独立 double 手算参考，生成手算表
- `tools/verify.py`：三层核对器

## 混音总线（mixer.c / mixer.h）

`MixBus` 多通道累加混音、增益、48kHz/16bit WAV 输出；`make -C mixer demo`
生成 `demo.wav`。
