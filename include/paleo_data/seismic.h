#pragma once
#include "model.h"

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
    std::vector<std::string> warnings;
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

}
