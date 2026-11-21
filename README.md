# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## raster 多边形填充（自相交，两阶段）

`raster/src/polygon.c` + `raster/src/rpf.h` 在不改动原三角形光栅化路径的前提下，新增自相交多边形填充，明确拆成两个阶段：

1. **覆盖判定（phase 1）**
   - 扇形三角剖分只负责记录每个像素被哪些扇形三角形覆盖（三角形号查询）；
   - 内外判定用独立的解析法：对每个像素中心 `(x+0.5, y+0.5)` 做水平射线，半开边规则下同时得到
     穿越数（奇偶规则用 `crossings mod 2`）与有向缠绕数（非零规则用 `winding != 0`）。
   - 射线法对自相交多边形仍然成立；扇形带符号求和在此情形不成立（五角星中心会被错误抵消），故不用于判内外。
2. **着色（phase 2）**：只读 phase 1 的覆盖记录，按 `opts.rule`（`RPF_RULE_EVEN_ODD` / `RPF_RULE_NONZERO`）查规则表决定内外再写颜色。`opts.mode = RPF_MODE_OFF` 时跳过着色，target 的 `rgb/z` 与调用前逐字节一致（与现状画面相同），但覆盖统计仍完整产出。

可查询产物（见 `RPFStats` / `rpf_pixel_get` / `rpf_rule_table`）：
- 覆盖像素数（两规则内部并集）、奇偶填充像素数、非零填充像素数、每扇形三角形覆盖像素数、溢出像素数；
- 任意像素的穿越数、缠绕数、覆盖它的三角形号；
- `(parity, winding) -> 两规则各自内外标记 / 选中规则颜色 / 像素数` 的规则表；
- parity / winding 调试图（PGM）和文本报告。

参数越界一律返回负值并经 `rpf_last_error()` 指明字段，例如 `field 'opts.rule' out of range: 7`。

### 构建与验证

```
make -C raster test     # 原三角形测试 + 多边形测试（含手算断言、关开关字节一致、字段报错）
make -C raster bench    # 32 与 256 两档 benchmark，产物在 raster/bench_out/
python3 -I verify/ref_verify.py raster/bench_out/bench.json --strict   # 独立精确参考逐像素核对
python3 -I verify/hand_check.py                                        # 16x16 蝴蝶结手算表复核
```

`verify/` 下的 Python 与 C 实现完全独立：用 `fractions.Fraction` 精确算术重算射线穿越，不共享任何代码。当前结果：两档网格、两种形状共 133120 个像素逐格零差异，手算 6 个样本全部吻合。

