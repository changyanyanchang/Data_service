# SEG-Y 按需读取与基础处理

地震服务通过统一 `DataService` 调用 vendored segyio C 库。源码与本地补丁在 `vendor/segyio`；运行时不依赖 Python。地震体只保存外部路径与元信息，不复制整块振幅进工区或数据库。

## 对象与方法

2026-10-30：独立文件预览可调用 `inspectSeismicFile`、`readSeismicFileLine`、
`readSeismicFileTimeSlice`，与已登记地震体使用相同的只读 segyio 读取器、预算和
取消规则，不隐式导入或创建工区。已登记对象继续优先用编号接口，以保留来源校验。
`readSeismicGridSection`／`readSeismicFileGridSection` 接收有序 IL／XL 路径，返回
真实道及原始采样；可显式选择沿路径插值，默认最近道。该网格入口的 `distances`
是线号弧长，区别于 XY 剖面的坐标弧长；不推断 CRS 或坐标单位。
原生元信息提供四个实际角点道的坐标（缺失时为空），`coordinateLengthUnit` 仅在
道头长度码和二进制头测量制均明确时报告 m／ft；不会把单位未知的坐标当成米。

| 方法 | 主要输入 | 结果 |
|---|---|---|
| `readVolume` | 地震体编号 | 数据库登记元信息，`traces` 保持空 |
| `inspectSeismic` | 地震体编号 | `SeismicFileInfo`：布局、格式、采样、文本头和几何检查状态 |
| `readSeismicTrace` | 地震体编号、物理道下标、样点窗口 | `SeismicTraceResult` |
| `readSeismicLine` | 地震体编号、`inline`/`crossline`、实际测线号 | `SeismicSectionResult` |
| `readSeismicTimeSlice` | 地震体编号、时间、网格上限 | `SeismicTimeSliceResult` |
| `extractSection` | 已保存 XY 路径剖面编号，或直接传 `SeismicSection` 对象 | 沿多点路径提取的剖面及墙几何 |
| `readTimeSlice` | 地震体编号、已确认时间约定 | 业务时间切片 |

`traceIndex` 是从 0 开始的文件道位置，`traceNumber` 是原道头值，两者不保证相同。INLINE/XLINE 使用实际测线号，不使用数组下标。

原生道和 IL/XL 预览不要求预先填写 CRS 或 TWT/OWT。跨井、层位联动以及按 XY 路径查询仍须明确坐标、时间类型及参考面。文件头信息不会自动升级为已确认的地质约定。

## 简单使用方式

先在界面导入自己的 SEG-Y，并从 `listData("volumes", 0, ...)` 取得编号。以下片段假定已经打开工区：

```cpp
using namespace paleo::data;
std::string error;
SeismicFileInfo info;
if (!service.inspectSeismic(volumeId, info, error)) {
    std::cerr << error << '\n';
    return 1;
}

SeismicTraceReadOptions options;
options.firstSample = 0;
options.sampleCount = 0;  // 读取到本道末尾；仍受 maxSamples 限制。
SeismicTraceResult trace;
if (!service.readSeismicTrace(volumeId, info.traceCount / 2,
                             options, trace, error)) {
    std::cerr << error << '\n';
    return 2;
}
// trace.amplitudes 是实际浮点数组；时间由 firstTime + i * sampleInterval 得到。
```

完整例子见 [案例 09](../examples/getting_started/09_segyio/main.cpp)。命令行用法为 `example_09_segyio <工区目录> [地震体编号]`。

需要完整采样时显式开启 `fullResolution=true`，并提供足够的 `maxValues`（最多 6400 万个振幅）。高清模式超预算报错，不静默抽样。界面默认高清，支持 1:1、原尺寸导出和多坐标点剖面墙，见 [高清读取指南](hd-seismic.md) 与 [案例 10](../examples/getting_started/10_hd_seismic/main.cpp)。

## 数组布局

- 单道：`amplitudes[sample]`，本道有独立首时间与采样间隔。
- 剖面：`amplitudes[column * samples + sample]`，并返回各列实际 XY、IL/XL 和物理道下标。
- 时间切片：`amplitudes[inlineIndex * crosslineNumbers.size() + crosslineIndex]`，返回实际取到的样点时间。

零值和负振幅保留，缺失/不可表示的样点使用 NaN。业务计算应检查有限性，不把零振幅当缺失。

## 基础处理

`processSeismicTrace` 和 `processSeismicSection` 只处理本次返回的小数组。顺序为逐道去均值、固定增益、滑窗 RMS AGC、可选对称限幅。默认不去均值，增益为 1，AGC 和限幅关闭。

AGC 窗口为至少 3 的奇数，窗口不跨道；去均值会改变原本为零的静音/补零区段，不自动识别这些区段。`seismicStatistics` 统计有限值数量、缺失数、范围、均值及 RMS；全缺失时统计数值保持 NaN。

## 有界读取与支持边界

规则网格候选通过有限道头检查得到，实际请求还会核对所选道头。`geometryStatus` 区分 `sampled-regular`、`full-header-index` 和 `unavailable`，抽样规则性不等于全文件质量验证。小型不规则数据可以建立道头索引；大型不规则文件无法直接确定几何时，仍可使用物理道位置读取。

当前适配面向固定道长，支持大文件偏移、常用大/小端数值格式以及明确计数的扩展文本头。未知扩展头数量、额外道头、变长道、24 位样本等不支持布局会明确拒绝。具体格式码、上限和错误行为以 `src/adapters/segy_reader.cpp` 及回归测试为准。

窗口与数组上限由读取选项约束；内存仍有解析器和临时对象开销。文件大小、修改时间和头部指纹用于快速来源检查，不执行全体振幅哈希。原文件移动后通过来源重新定位接口核验；原文件改变后需要明确重新导入。

本服务不包含完整偏移、反演或三维体渲染系统；这些业务可在现有返回对象之上继续实现。
