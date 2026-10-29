# 高清剖面、时间切片与多点剖面墙

## 为什么原来显得模糊

旧界面的原生测线通常只返回最多 512 道，时间切片默认只返回 128 × 128 个网格点；部分业务入口使用 256 或 512 的上限。剖面绘图又把矩阵缩到最多 1800 × 1800 像素。两次抽样都会丢失细节，放大屏幕上的小图无法恢复。

现在把读取精度和显示缩放分开：界面默认选择“原始分辨率”，读取本次剖面或切片的原始样点，图形保留返回矩阵的全部像素。适合窗口只是显示方式，可切换 1:1 查看、滚轮缩放、拖动平移，或导出原尺寸 PNG。高清不增加原始数据的频带、空间分辨率或信噪比。

## 界面操作

1. 导入 SEG-Y，或打开已经登记其外部路径的工区。
2. 在接口演示中选择 INLINE、XLINE 或水平时间切片，选择地震体及测线号/时间，保持“原始分辨率”勾选，再执行。
3. 图形工具栏可适合窗口、1:1、缩放和导出。图上注明返回尺寸以及是否抽样；高清读取失败时会显示原因。
4. 多点剖面墙可在接口演示的“剖面墙”中选择地震体，按顺序输入多行 `X,Y` 后执行。也可在资料管理中新建剖面路径，保存多个坐标点，再选择“提取剖面”。
5. 剖面结果可切换到立体墙视图、旋转或导出墙图。展开图按返回矩阵原尺寸导出；墙图是带透视投影的示意图，仍会受观察角度与输出尺寸影响。时间轴是相对显示比例，不是时间到深度换算。

XY 必须与地震体已确认的坐标系和单位一致。按坐标提取前需确认采样轴、TWT/OWT 与时间参考面，不自动投影换算。原生 IL/XL、时间切片读取可以在地质约定尚未确认时检查文件自身数据。

## 调用和返回对象

```cpp
#include <paleo_data/service.h>
using namespace paleo::data;

// service 已打开工区；volumeId 与 inlineNumber 来自所选资料。
SectionReadOptions options;
options.fullResolution = true;
options.maxValues = 64000000;
// 可设置 firstTime/lastTime，缩小时间窗而保留窗内全部时间样点。
SeismicSectionResult section;
bool ok = service.readSeismicLine(volumeId, "inline", inlineNumber,
                                  options, section, error);
// 失败先处理 error；成功后访问 section.amplitudes[column * section.samples + sample]。
```

切片同样设置 `TimeSliceReadOptions::fullResolution=true`。振幅布局为 `inlineIndex * crosslineNumbers.size() + crosslineIndex`。`actualTime` 返回实际最近的时间样点，不在两个时间样点之间伪造振幅。

公开接口保留原有默认的有界预览行为，以兼容已有调用者；需要高清时显式设置 `fullResolution` 和足够的 `maxValues`。高清模式忽略仅用于预览的 `maxColumns` / `maxInlineCount` / `maxCrosslineCount`；振幅预算始终有效，超限报错，不自动抽样。预览模式通过 `decimated` 告诉调用者是否降低了空间采样密度。时间样点始终按请求窗口原样返回。

```cpp
SeismicSection request;
request.parentVolumeId = volumeId;
request.sectionName = "多点折线剖面";
request.coordinateSystem = confirmedCoordinateSystem;
request.coordinateUnit = confirmedCoordinateUnit;
request.pathPoints = orderedCoordinates; // vector<SeismicSectionPoint>，至少两个有序坐标点。

// 直接传对象，不必为了临时查询先写数据库。
SeismicSectionResult wall;
bool ok = service.extractSection(request, options, wall, error);
// 要持久保存路径时，先 saveSection，再按 sectionId 提取，结果结构相同。
```

| 字段 | 用途 |
|---|---|
| `amplitudes`、`samples` | 每列完整的原始时间样点；缺道为 NaN，零和负振幅正常保留 |
| `pathX` / `pathY` | 每列在请求路径上的位置，供构建墙的平面几何 |
| `distances` | 从路径起点累计的平面距离，单位与路径坐标一致 |
| `vertexColumns` | 用户输入的起点、转折点、终点分别位于返回矩阵哪一列 |
| `x` / `y` | 实际读取的最近道中心，可能与请求路径略有偏移 |
| `traceIndexes`、`inlineNumbers`、`crosslineNumbers` | 对应来源物理道位置和实际测线编号 |
| `firstTime`、`sampleInterval`、`timeUnit` | 时间轴；不是深度轴 |
| `nativeColumns`、`decimated` | 原始路径列数及是否抽样，供界面明确标注精度 |

路径高清采样按经过的原始网格确定，振幅来自最近道；转折点可能与邻近列取到同一道。重复道不代表新增独立地震信息。展开图横向为列序，真实路径距离由 `distances` 描述；需按等距离绘图时应由业务端明确决定重采样策略。

完整、可编译的三个调用场景见 [案例 10](../examples/getting_started/10_hd_seismic/main.cpp)。它接受工区、地震体编号以及实际测线号、时间或多个 XY 参数。

## 测区平面坐标范围

要把地震体放到地图上、裁剪其它资料或判断某个 XY 是否落在测区内，用 `readSeismicCoordinateRange`。它在一次调用里给出测线号域、四个角点和包围盒，省去宿主自己解析道头、拟合测网仿射。

```cpp
SeismicCoordinateRange range;
bool ok = service.readSeismicCoordinateRange(volumeId, range, error);
// ok 只表示调用完成。范围本身可能不可得，先看 status：
//   complete          号域与四角点齐全
//   xy-unavailable    缺可验证的平面XY或拟合失败，只有号域
//   grid-unavailable  规则测线网格未确认，号域也不给出
if (ok && range.status == "complete" && range.coordinateUnit == "m") {
    // range.minX/minY/maxX/maxY：四角点包围盒，单位同 coordinateUnit
    // range.corners：顺序固定 (ilMin,xlMin) (ilMin,xlMax) (ilMax,xlMax) (ilMax,xlMin)
}
```

| 字段 | 用途 |
|---|---|
| `inlineMin/Max/Step/Count`、`crosslineMin/Max/Step/Count` | 实际测线号域；稀疏或跳号测网按真实值给出，不是数组下标 |
| `corners` | 四个角点的测线与平面坐标；`tracePresent=false` 表示该角点没有实际道，坐标由仿射给出 |
| `minX/minY/maxX/maxY` | 角点包围盒；`complete` 之外一律为 Missing（NaN），不用 0 顶替 |
| `coordinateSystem`、`coordinateUnit` | 透传地震体已确认的约定；空字符串表示尚未确认 |
| `geometryStatus` | `sampled-regular`（有界抽查）/ `full-header-index`（全文件道头索引） |
| `affineValid`、`fitHeaderSamples`、`fitRmsResidual`、`fitMaxResidual` | 采样道头拟合的精度依据；残差过大时范围只作概览 |
| `warnings` | 人读说明：抽查范围、单位未确认、角点缺道、残差超容差等 |

范围只覆盖已确认存在的测线格点：不填补缺道、不外推到测网之外、不做投影换算、不换算单位。角点是仿射取值，不等于该处实测道的坐标；需要逐点精度时用 `readSeismicTrace` 核对实际道头。`coordinateUnitsCode` 不是长度单位（非 0/1）时 XY 可能不是平面长度，此时 `warnings` 会明确指出。

## 工区平面范围（井点 ∪ 地震）

工区范围由两块最小外接矩形合并而成：全部井口点、全部地震体范围。`readProjectPlanarRange` 一次给出两块矩形和合并结果。

```cpp
ProjectPlanarRange area;
bool ok = service.readProjectPlanarRange(area, error);
// 两块矩形各自可用，合并结果单独判断：
// area.wells   井口点 MBR
// area.seismic 地震体范围并集
// area.merged  工区范围；status != complete/partial 时 valid=false
if (ok && area.merged.valid) {
    // area.merged.minX/minY/maxX/maxY：工区范围
}
```

| status | 含义 |
|---|---|
| `complete` | 井点与地震两块都在，坐标系/单位已确认且一致，`merged` 是两者并集 |
| `partial` | 只有一块可得，`merged` 就是那一块（warnings 说明缺哪块） |
| `unavailable` | 两块都不可得，`merged.valid=false` |
| `conventions-unconfirmed` | 两块都在，但至少一侧坐标系/单位为空，不合并 |
| `conventions-mismatch` | 两侧声明不同，或同一侧内部出现多种声明，不合并 |

合并纪律：

- 缺井口坐标的井计数进 `wellsWithoutCoordinates`，不按 0 或井名补位；
- 没有可验证 XY 的地震体记入 `volumesWithoutRange`，不参与并集，也不会把工区范围缩成假值；
- 单个体读取失败只记录并继续，不中断整次查询；取消则整体中止；
- 声明冲突时清空该块矩形四项（全部 Missing），不留下半可信数值；
- 不做单位换算与投影：`m` 与 `ft` 混用、两个不同工程坐标系混用一律判为 mismatch，由调用方先确认约定再谈转换。

井点 MBR 与地震范围若需要按投影后的统一坐标系合并，应先在业务侧完成转换、写回已确认的坐标系与单位，再调用本接口；本接口不猜测转换参数。

## 内存和速度边界

单次最多 6400 万个振幅，float 数组约 256 MB，坐标数组、图像以及处理副本还会增加内存。这个数字是振幅数量保护，不是进程总内存硬上限。原始地震体始终外部引用、只读，不全体加载或复制。

完整水平切片可能要访问数百万道。SEG-Y 通常按道存储，跨道读取一个时间样点仍可能产生大量磁盘访问，因此高清比抽样预览慢，机械硬盘尤为明显。读取在后台执行并可取消。当前已增加批量读取、范围裁剪、分块显示和有界进程内相邻时间缓存，见 [切片性能指南](time-slice-performance.md)。原始体不转换或重排，缓存不写磁盘。

图片用于查看，数值计算应使用返回的浮点数组。PNG 为颜色映射后的显示结果，不是原始振幅的无损数值持久化格式；同一振幅颜色尺度和投影缩放也会影响视觉感受。
