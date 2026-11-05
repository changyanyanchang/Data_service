#pragma once
#include "model.h"
#include "stratigraphic_framework.h"
#include <utility>

namespace paleo::data {

// 读取诊断：阶段耗时为工作线程累计值，可能大于墙钟 elapsedMs。
// 映射字节数是视图大小累计值，不是磁盘实际读取量；系统缓存状态另行注明。
struct SeismicReadStatistics {
    double elapsedMs=0, positionMs=0, readMs=0, validationMs=0, conversionMs=0;
    long long readCalls=0, seekCalls=0, bytesRead=0, mappedBytes=0, tracesRead=0;
    int workerCount=1, cacheHits=0, cacheMisses=0, completedTiles=0, totalTiles=0;
    std::string ioMode;
};

// SEG-Y 文件自身提供的事实。快速检查只读取文件头和少量道头，不读取整个体。
// geometryStatus 明确区分抽样规则网格与完整索引，不能把抽样检查称为全文件校验。
struct SeismicFileInfo {
    std::string engine;             // 实际使用的 segyio 版本及本地补丁说明。
    std::string sourceFile;
    long long fileBytes = 0;
    long long traceCount = 0;      // 固定道长布局核验后计算；不是振幅数组长度。
    int sampleCount = 0;
    int sampleFormat = 0;          // SEG-Y 格式码，例如 1=IBM32、5=IEEE32。
    std::string byteOrder;         // big-endian / little-endian。
    long long firstTraceOffset = 0;
    int extendedTextHeaders = 0;
    double firstTime = Missing;   // 来源道头的延迟时间；不代表已确认地质参考面。
    double sampleInterval = Missing;
    std::string timeUnit;          // 时间轴使用 ms；OWT/TWT 仍由地震体约定单独保存。
    std::string textHeader;        // 已解码的主文本头，供人工核对坐标与处理说明。
    std::string geometryStatus;    // sampled-regular / full-header-index / unavailable。
    int inlineMin = 0, inlineMax = 0, inlineStep = 0, inlineCount = 0;
    int crosslineMin = 0, crosslineMax = 0, crosslineStep = 0, crosslineCount = 0;
    int checkedHeaders = 0;
    int coordinateUnitsCode = 0;    // SEG-Y 道头单位码；1=长度，但不能仅据此断言是米。
    int coordinateScalar = 0;      // 原始缩放码：正数乘，负数除以绝对值，0视为1。
    std::string coordinateLengthUnit; // m/ft only when both trace and binary headers explicitly declare length units
    std::vector<std::string> warnings;
    std::vector<double> cornerX, cornerY; // four actual corner traces: min/min, min/max, max/max, max/min; empty if unavailable
};

// 测区平面范围的一个角点。顺序固定为
// (inlineMin,crosslineMin) (inlineMin,crosslineMax) (inlineMax,crosslineMax)
// (inlineMax,crosslineMin)，与宿主工程既有的测区角点约定一致。
// 坐标是测网仿射在该格点上的取值；拟合不可用时为 Missing，不以相邻格点替代。
struct SeismicCoordinateCorner {
    int inlineNumber = 0;
    int crosslineNumber = 0;
    double x = Missing, y = Missing;
    bool tracePresent = false; // 该格点是否有实际道；false 表示角点由仿射给出而非实测道位置
};

// 地震体平面坐标范围：测线号域 + 四角点 + 包围盒 + 拟合精度。
// 与 SeismicFileInfo 的分工：那里报告文件自身抽查到的事实，这里把那些事实
// 换算成可直接上图或裁剪的平面范围。范围只覆盖已确认存在的测线格点，
// 不填补缺道、不外推到测网之外，也不做任何投影换算。
// 号域与坐标可能分别不可得：status 说明是哪一层缺失，不要用 0 顶替 Missing。
struct SeismicCoordinateRange {
    long long volumeId = 0;
    std::string dataFile;
    std::string coordinateSystem; // 与 SeismicVolume.geometry 一致；空表示未确认
    std::string coordinateUnit;   // m/ft；空表示未确认
    std::string geometryStatus;   // sampled-regular / full-header-index / unavailable
    int inlineMin = 0, inlineMax = 0, inlineStep = 0, inlineCount = 0;
    int crosslineMin = 0, crosslineMax = 0, crosslineStep = 0, crosslineCount = 0;
    std::vector<SeismicCoordinateCorner> corners; // complete 时 4 个；坐标不可得时为空
    double minX = Missing, minY = Missing, maxX = Missing, maxY = Missing; // 角点包围盒
    bool affineValid = false;     // 抽样道头是否拟合出可用的测网仿射
    int fitHeaderSamples = 0;     // 参与拟合的道头数；0 表示未拟合
    double fitRmsResidual = Missing, fitMaxResidual = Missing; // 坐标单位，越界即不可信
    // complete：号域与四角点齐全；xy-unavailable：缺可验证的平面XY或拟合失败，
    // 只返回号域；grid-unavailable：连规则测线网格都未确认，号域也不返回。
    std::string status;
    std::vector<std::string> warnings;
};

struct SeismicTraceReadOptions {
    int firstSample = 0;           // 从第几个样点开始，0为首样点。
    int sampleCount = 0;           // 0表示读取到本道末尾。
    int maxSamples = 1000000;      // 本次最多返回的样点数，超限报错，不暗中截断。
};

// 一道真实数值及其自身时间轴。traceIndex是文件中从0开始的位置，不是来源道号。
struct SeismicTraceResult {
    long long traceIndex = -1;
    long long traceNumber = -1;    // 原道头5–8字节的值，0保留；不保证它等于文件道位置。
    int inlineNumber = 0, crosslineNumber = 0;
    double x = Missing, y = Missing; // 已应用来源缩放码；不自动转换坐标系。
    double firstTime = Missing, sampleInterval = Missing;
    std::string timeUnit;
    std::vector<float> amplitudes; // 负振幅正常保留；缺失/非有限值为NaN。
};

// 基础处理只作用于本次返回的小数组。原始文件和数据库中的登记信息不变。
// 顺序：逐道去均值 → 固定增益 → 滑窗RMS自动增益 → 可选对称限幅。
struct SeismicProcessingOptions {
    bool removeMean = false;      // 开启会改变原本为0的样点；不会自动识别静音/补零区段。
    double gain = 1.0;
    int agcWindowSamples = 0;      // 0关闭AGC，否则为>=3的奇数；窗口不跨道。
    double clipAmplitude = Missing; // NaN关闭，否则须>0；限幅到[-值,+值]。
};

struct SeismicStatistics {
    long long finiteCount = 0, missingCount = 0;
    double minimum = Missing, maximum = Missing;
    double mean = Missing, rms = Missing; // 忽略NaN；全缺失时保持NaN。
};

// 地震层位/小层定位器：支持按四等分小层（T70-T72-S1..S4）、父层位（T70-T72）或直接指定顶底层位资源 ID。
struct SeismicStratigraphicTarget {
    std::string schemeId;          // 空可尝试默认方案；显式方案/修订或强类型 ID 解析失败即报错，不回退。
    std::string revision;          // 空表示读取当前头修订
    SubIntervalId subIntervalId;   // 精确小层 ID；可读名称填 unitName，不将显示名当作 ID。
    IntervalId intervalId;         // 精确父单元 ID；与 subIntervalId 同传时必须一致。
    std::string unitName;          // 可读名称（如 "T70-T72-S1" 或 "T70-T72"）
    long long topHorizonId = 0;    // 直接指定顶/底资源；与显式方案共用时须匹配该修订的关联。
    long long bottomHorizonId = 0;
    int subIndex = 0;              // 0=整层段；1..4=四等分中的第 k 个小层
};

// 逐道变深度/变时窗采样输出模式：
enum class HorizonWindowSampleMode {
    NativeVariableSamples, // 保留每道在 [topTime, bottomTime] 内的原生变长样点（CSR 紧凑数组）
    ResampledFixedCount,   // 每道在自身 [topTime, bottomTime] 内等比例重采样为固定 K 个样点（N × K 规则张量，供地震相预测直接输入）
    SummaryAttributesOnly  // 仅返回每道在该小层窗口内的统计属性与顶底时深，不拷贝波形数组
};

// 空间查询域类型：
enum class HorizonSpatialDomainKind {
    FullHorizonGrid,       // 小层有效覆盖的全部测网点（按 inlineStride / crosslineStride 步进）
    InlineCrosslineBox,    // 指定 Inline/Crossline 矩形子区
    InlineSection,         // 单条 Inline 测线剖面（fixedLineNumber）
    CrosslineSection,      // 单条 Crossline 测线剖面（fixedLineNumber）
    ArbitraryTraceList,    // 指定的一组 (inline, crossline) 道列表（explicitTraces）
    ArbitraryXYPath        // 任意折线/井间连线 XY 路径（xyPoints）
};

// Explicit project-scoped selection for depth conversion. No display-name or
// minimum-ID fallback is used when a well has no selected relation.
struct SeismicTimeDepthBinding { long long wellId = 0, timeDepthId = 0; };

struct SeismicHorizonWindowQuery {
    std::string undeclaredTimeUnit, undeclaredTimeType; // explicit convention for undeclared sources only
    long long volumeId = 0;                  // 地震体 ID（0 表示自动选用工区主三维地震体）
    SeismicStratigraphicTarget target;       // 目标小层（如 T70-T72-S1）或父层位（T70-T72）
    HorizonSpatialDomainKind domainKind = HorizonSpatialDomainKind::FullHorizonGrid;

    // 空间范围控制：
    int inlineMin = 0, inlineMax = 0, inlineStride = 1;
    int crosslineMin = 0, crosslineMax = 0, crosslineStride = 1;
    int fixedLineNumber = 0;                 // InlineSection / CrosslineSection 时使用
    std::vector<std::pair<int, int>> explicitTraces; // ArbitraryTraceList: (inline, crossline)
    std::vector<std::pair<double, double>> xyPoints; // ArbitraryXYPath: (x, y)

    // 窗口外扩与输出控制：
    double padTopMs = 0.0;                   // 窗口顶界向上额外外扩毫秒数
    double padBottomMs = 0.0;                // 窗口底界向下额外外扩毫秒数
    HorizonWindowSampleMode sampleMode = HorizonWindowSampleMode::ResampledFixedCount;
    int resampledSampleCount = 32;           // ResampledFixedCount 模式下每道重采样点数 K
    bool computeWindowAttributes = true;     // 同步计算每道在该小层内的 RMS/均幅/峰值/能量属性
    bool computeDepthRange = true;           // 利用工区时深关系换算每道 [topDepthM, bottomDepthM]
    long long maxValues = 32000000;          // 样点预算上限
    // Empty uses persisted time-depth preferences. Missing/invalid physical
    // declarations keep depths unavailable; amplitude extraction can still work.
    std::vector<SeismicTimeDepthBinding> timeDepthBindings;
    long long maxDepthEvaluations = 8000000; // 两界逐井 IDW 求值总预算，1..64000000；不截断已选关系。未请求深度时忽略。
};

// 单道在该小层/层位下的实际变时深窗口元数据：
struct TraceHorizonWindowMeta {
    long long traceIndex = -1;
    int inlineNumber = 0;
    int crosslineNumber = 0;
    double x = Missing, y = Missing;

    // 该点由层位面 + 四等分比例算出的精确时间窗口（每道不同）：
    double parentTopTimeMs = Missing;        // 父层位顶界时间（如 T70 在该点的时间）
    double parentBottomTimeMs = Missing;     // 父层位底界时间（如 T72 在该点的时间）
    double windowTopTimeMs = Missing;        // 当前小层（如 S1）在该点的顶界时间
    double windowBottomTimeMs = Missing;     // 当前小层（如 S1）在该点的底界时间

    // 经工区时深关系换算的该点深度范围（每道不同）：
    double windowTopDepthM = Missing;
    double windowBottomDepthM = Missing;
    double thicknessTimeMs = Missing;
    double thicknessDepthM = Missing;

    // NativeVariableSamples 模式下在 amplitudes 数组中的偏移与原生样点数：
    long long sampleOffset = 0;
    int nativeSampleCount = 0;
    double firstSampleTimeMs = Missing;

    // 该道在本小层窗口内的统计属性（供属性成图与地震相特征直接消费）：
    float rmsAmplitude = std::numeric_limits<float>::quiet_NaN();
    float meanAbsAmplitude = std::numeric_limits<float>::quiet_NaN();
    float maxPeakAmplitude = std::numeric_limits<float>::quiet_NaN();
    float minTroughAmplitude = std::numeric_limits<float>::quiet_NaN();
    float energyAmplitude = std::numeric_limits<float>::quiet_NaN();

    // 1=有效小层窗口，0=缺道，2=层位缺失/尖灭/倒置待复核
    std::uint8_t status = 0;
    // 0=not requested, 1=both depth bounds resolved, 2=unavailable/review.
    std::uint8_t depthStatus = 0;
};

struct SeismicHorizonWindowResult {
    long long volumeId = 0;
    std::string schemeId;
    InterpretationRevisionId schemeRevision;
    IntervalId intervalId;
    SubIntervalId subIntervalId;
    std::string unitDisplayName;             // 如 "T70-T72-S1"
    int subIndex = 0;                        // 1..4（或 0 表示整层）
    double fractionTop = 0.0, fractionBottom = 1.0; // 如 S1 为 [0.0, 0.25]

    double sampleIntervalMs = Missing;
    std::string timeUnit = "ms";
    std::string depthUnit = "m";
    HorizonWindowSampleMode sampleMode = HorizonWindowSampleMode::ResampledFixedCount;
    int fixedSampleCount = 0;                // ResampledFixedCount 时等于 K

    // 逐道元数据（长度 = N 道）：每道携带各自的 [windowTopTimeMs, windowBottomTimeMs] 与 [windowTopDepthM, windowBottomDepthM]
    std::vector<TraceHorizonWindowMeta> traces;

    // 样点数组：
    // - NativeVariableSamples：按 trace.sampleOffset .. +trace.nativeSampleCount 存放每道原生样点；
    // - ResampledFixedCount：长度 = traces.size() * fixedSampleCount，行优先 [traceIdx * K + k]；
    std::vector<float> amplitudes;

    std::size_t validTraceCount = 0;
    std::size_t reviewTraceCount = 0;
    SeismicReadStatistics statistics;
    std::string depthStatus = "not_requested"; // available / partial / unavailable
    std::string depthType, depthReference;     // Converted values use confirmed MD/m.
    std::vector<SeismicTimeDepthBinding> usedTimeDepthBindings;
    std::vector<std::string> depthWarnings;   // Bounded diagnostics; no fabricated depths.
};

}
