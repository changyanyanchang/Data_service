#pragma once
#include "parser.h"
#include "seismic.h"
#include <memory>
#include <cstdint>

namespace paleo::data {
// 一口井的资料概况。这里只统计当前工区已经入库的资料。
// curves == 0 不代表这口井没有任何资料，它可能只有时深关系或井位坐标。
struct WellSummary {
    long long id=0;             // 数据库中稳定的井编号，不是 vector 的下标。
    std::string name;           // 井名，例如 SYNTH-1。
    int logSets=0, curves=0, tracks=0, samples=0;
    int timeDepthTables=0, trajectorySurveys=0;
    bool hasCoordinates=false;
    int corePhotos=0, attachments=0, markerTracks=0;
};
struct WellInfoRecord { long long id=0; WellInfo value; };
// 平面轴对齐最小外接矩形。四个值同时有效或同时为 Missing，不用 0 表示未知。
// coordinateSystem/Unit 是来源声明，这里不换算单位、不做投影。
struct PlanarExtent {
    double minX=Missing, minY=Missing, maxX=Missing, maxY=Missing;
    std::string coordinateSystem; // 参与求包围的来源声明；空表示未确认
    std::string coordinateUnit;   // 如 m、ft；空表示未确认
    long long pointCount=0;       // 参与求包围的点数（井数或地震体角点数）
    bool valid=false;             // 四项坐标均有限时为 true；false 时四项均为 Missing
};

// 工区平面范围 = 全部井口点矩形 ∪ 全部地震体范围矩形。
// 只有两块都在、且坐标约定已确认并一致时才合并；否则 merged.valid=false 并说明原因，
// 绝不把不同坐标系或单位的数值凑进同一个矩形。单一来源存在时按 partial 给出该块矩形。
// status：complete / partial（只有一块）/ unavailable（两块都没有）/
//         conventions-unconfirmed（两块都在但约定未确认）/ conventions-mismatch（声明冲突）。
struct ProjectPlanarRange {
    PlanarExtent wells;     // 有井口坐标的井
    PlanarExtent seismic;   // 可换算地震体的并集
    PlanarExtent merged;    // 工区范围；不可合并时 valid=false
    int wellCount=0, wellsWithCoordinates=0, wellsWithoutCoordinates=0;
    int volumeCount=0, volumesWithRange=0;
    std::vector<long long> volumesWithoutRange; // 未参与范围的地震体编号，顺序同资料登记
    std::string status;
    std::vector<std::string> warnings; // 缺坐标井数、被跳过体及原因、未合并原因等
};
// 一条资料的“索引卡片”，用于选择要读取哪条资料，不携带全部采样值。
// 拿到 id 后，调用 readCurveSlice / readIntervals / readTimeDepth 等读取实际数据。
struct DataEntry {
    long long id=0;             // 这条资料的稳定编号；只在对应资料类别内解释。
    long long wellId=0;         // 所属井；0 可表示工区级资料或尚未归井。
    long long sourceId=0;       // 来源记录编号，用于追溯原文件。
    long long parentId=0;       // 曲线时为曲线组编号；层位/剖面时为所属地震体编号。
    std::string name;          // 资料名称，例如 GR。
    std::string kind;          // 类别，例如 curves、intervals、time_depth。
    std::string detail;        // 供界面查看的摘要；业务代码不要通过拆分此字符串取数。
    long long count=0;         // 该类别的记录数，例如曲线样点数或本道层段数。
};
struct SourceEntry { long long id=0; std::string path, kind, status, message; long long bytes=0; };
// Complete stored identity for index projections; separate from the original lightweight list contract.
struct SourceIndexEntry {
    long long id=0;
    std::string path, storedPath, kind, status, message;
    long long bytes=0;
    std::string sha256;
};
struct IssueEntry { long long id=0, sourceId=0; std::string code, message, originalText, sheet, resolution; int row=0; bool blocking=false, resolved=false; };
// 通用的分页展示表。rows 是本页内容，totalRows 是总行数；表格值统一为文字。
// 算法计算优先使用下方带 double/vector 字段的专用结果对象。
struct DataTable {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    long long totalRows=0;
};

// 一次曲线读取的真实数值结果，不是文件目录，也不是 JSON 文本。
// 四个数组等长：下标 i 对应同一个采样位置。结果拥有自己的数组，可直接用于计算。
struct CurveSlice {
    long long curveId=0;
    std::string name;                // 曲线名，例如 GR、SP。
    std::string unit;                // 曲线单位，空字符串表示尚未确认。
    std::string depthUnit;           // 深度单位，空字符串表示尚未确认。
    std::vector<double> depths;      // 请求深度范围内的所有采样深度。
    std::vector<double> rawValues;   // 原始值；来源中的 -999.25 等空值标记也可能保留在这里。
    std::vector<double> values;      // 本次选取的值：processed=true 用处理值，false 用原始值。
    std::vector<unsigned char> quality; // 0正常、1来源空值、2阈值异常、4人工修改。
    // 计算前检查 std::isfinite(values[i]) 和质量标记。
    // 特别是读取原始值时，quality=1 的有限哨兵值仍然表示缺失，不能拿来算均值。
};

struct IntervalRecord {
    long long id=0;       // 此层段的数据库编号，修改或删除时使用。
    long long trackId=0;  // 所属分层道编号；同一口井可以有多套不同分层解释。
    GeologicalInterval value; // 真正的层段数据：名称、顶深、底深、岩性/相等。
};

struct FeatureColumn {
    long long curveId=0;
    std::string name,unit;
    std::vector<double> values;              // 这条曲线在公共深度轴上的数值。
    std::vector<unsigned char> quality;
};
// 供后续算法使用的多曲线特征表：一个公共深度轴，多列曲线数值。
// columns[j].values[i] 表示第 j 条曲线在 depths[i] 处的数值。
struct WellFeatureTable {
    long long wellId=0;
    std::string depthUnit;
    std::vector<double> depths;
    std::vector<FeatureColumn> columns;
};
struct IntervalTrackResult { long long trackId=0,wellId=0; std::string name; DepthInfo depthInfo; std::vector<IntervalRecord> intervals; };
struct ImportOptions {
    ParseOptions parsing;
    bool copySources=true;
    bool forceNewBatch=false;
    // 默认重复导入先比较路径、大小、时间和有界内容指纹，未变化则复用已保存结果。
    // 这不是逐字节完整性审计；设为 true 可每次重算完整 SHA-256（SEG-Y仍用原有快速指纹）。
    // 首次导入、变化文件和明确重导入始终进行正常校验及解析。
    bool verifyUnchangedFiles=false;
    int parserThreads=2; // 目录导入的校验/解析线程数，限制为1–4；1用于串行运行。
    // 待解析/待发布任务的估计内存预算；不是进程内存硬上限。
    // >=32MiB的压缩表格/未知大文件单独准备；XML/LAS按流式展开估算，可在预算内并行。
    // 仅登记的照片/归档文件仍可并行校验。
    // 入库始终按文件顺序执行。
    long long maxPendingBytes=256LL*1024*1024;
    std::vector<std::string> excludedDirectories; // Explicit output/staging trees pruned before directory traversal.
};
struct ImportProgress { int completed=0, total=0; std::string file, status; };
struct ImportReport {
    int seen=0, imported=0, registered=0, review=0, failed=0, ignored=0, skipped=0;
    bool cancelled=false;
    std::vector<std::string> errors;
    int cached=0; // skipped 中通过增量检查直接复用的文件数。
    int workerCount=1;
    // 多线程时各阶段为逐文件累计耗时，可能大于 elapsedMs（墙钟总耗时）。
    long long elapsedMs=0, hashMs=0, parseMs=0, copyMs=0, storeMs=0;
    long long hashedBytes=0; // 实际为完整身份校验读取的字节，不含有界增量检查。
};
using ProgressCallback = std::function<void(const ImportProgress&)>;
struct SectionReadOptions {
    double firstTime=Missing, lastTime=Missing; // 请求的时间范围；Missing 表示不限制对应端。
    int maxColumns=1000;                       // 预览模式的列数上限；原始分辨率模式忽略此项。
    long long maxValues=8000000;               // 本次振幅预算，可显式增至64000000；超预算拒绝读取。
    bool fullResolution=false;                // true：保留原始道及时间样点，不为满足预算而降采样。
};
struct SeismicSectionResult {
    int columns=0, samples=0;                  // 剖面列数、每列样点数。
    double firstTime=Missing, sampleInterval=Missing;
    std::string timeUnit;
    std::vector<double> x,y;                   // 每一列实际读取的道位置。
    std::vector<float> amplitudes;             // 列优先：column * samples + sample。
    // 时间 = firstTime + sample * sampleInterval。缺道为 NaN，零振幅是有效数据。
    std::vector<long long> traceIndexes;       // 每列在原文件中的位置；缺道为-1。
    std::vector<int> inlineNumbers, crosslineNumbers; // 每列实际测线编号。
    std::vector<double> pathX,pathY,distances;  // 每列在请求路径上的位置、从起点累计距离（坐标单位）。
    std::vector<int> vertexColumns;            // 折线路径的每个输入顶点对应的列号，顺序不变。
    int nativeColumns=0;                      // 降采样前的原始测线列数/路径经过的网格列数。
    bool decimated=false;                     // true 表示空间列已减少；时间样点始终不抽稀。
};
struct TimeSliceReadOptions {
    int maxInlineCount=512, maxCrosslineCount=512; // 预览时的轴上限。
    long long maxValues=262144;                // 可显式增至64000000；包含X/Y时总内存高于振幅数组。
    bool fullResolution=false;                // true 忽略预览轴上限，预算不足时报错，不静默降采样。
    int inlineMin=std::numeric_limits<int>::min(), inlineMax=std::numeric_limits<int>::max();
    int crosslineMin=std::numeric_limits<int>::min(), crosslineMax=std::numeric_limits<int>::max();
    // 范围含端点，填写真实测线号；范围裁剪不属于降采样。
    std::string ioMode="auto";                 // auto / buffered / mapped / legacy（基准对照）。
    int workerCount=2;                         // 1、2或4，每个工作任务有独立只读句柄。
    int tileSize=128;                          // 32–512，每块最多tileSize²点；规则体沿文件快轴分长条，不改变精度。
    bool useCache=true;
    int adjacentSamples=4;                    // 1–16个相邻时间样点共用一次读取。
    int cacheBudgetMiB=512;                    // 进程内LRU缓存；0禁用，最大1024MiB，不写磁盘。
    bool collectTimings=false;                // 基准测试时启用；避免日常逐道计时开销。
};
struct SeismicTimeSliceResult {
    std::vector<int> inlineNumbers,crosslineNumbers; // 实际测线号，不是数组下标。
    std::vector<double> x,y;                         // 与振幅数组逐位置对应的坐标。
    std::vector<float> amplitudes;                   // 行优先：inlineIndex * crosslineNumbers.size() + crosslineIndex。
    double actualTime=Missing;                       // 实际取到的最近样点时间。
    std::string timeUnit;
    int nativeInlineCount=0, nativeCrosslineCount=0;
    bool decimated=false;
    SeismicReadStatistics statistics;
};

// 只报告已完整校验的分块。回调在发起服务调用的线程执行，不能直接操作GUI。
// 最终方法失败/取消时完整result为空；已显示的临时分块应由界面清除。
struct SeismicTimeSliceTile {
    int inlineOffset=0, crosslineOffset=0, totalInlineCount=0, totalCrosslineCount=0;
    int inlineCount=0, crosslineCount=0, completedTiles=0, totalTiles=0;
    double actualTime=Missing;
    std::string timeUnit;
    std::vector<float> amplitudes;              // 此块行优先，仅包含当前时间面。
    bool fromCache=false;
};
using TimeSliceTileCallback=std::function<void(const SeismicTimeSliceTile&)>;

// 对外的 C++ 数据服务入口。调用者无需直接访问 SQLite 或文件解析器。
// 统一用法：bool 表示调用是否成功；result 引用接收结果；失败时读取 error。
// 方法返回后，result 的数值已经在内存中，可直接计算/绘图；JSON 序列化是可选的后续步骤。
// 宿主进程先创建一个 QCoreApplication（Qt GUI 已有 QApplication，无需重复创建）。
// 每个调用线程各用一个 DataService，不要跨线程共享同一实例。
// 关闭和重开保留已提交修改；内存分配失败等 C++ 异常仍可能抛出。
class DataService {
public:
    DataService();
    ~DataService();
    DataService(const DataService&)=delete;
    DataService& operator=(const DataService&)=delete;
    bool createProject(const std::string& directory, const std::string& name, std::string& error);
    // directory 是包含 data.sqlite 的工区文件夹；路径字符串使用 UTF-8。
    bool openProject(const std::string& directory, std::string& error);
    // Read existing data only: no schema upgrades, imports or edits. A separate
    // instance is required for each worker thread, as for writable sessions.
    bool openProjectReadOnly(const std::string& directory, std::string& error);
    bool isReadOnly() const;
    // Consistent SQLite snapshot including committed WAL; destination must not exist.
    // Copies the database only; external source references retain their meaning.
    bool snapshotDatabase(const std::string& destination, std::string& error) const;
    void closeProject();
    std::string projectPath() const;
    std::string projectName() const;
    std::string projectCrs() const;
    std::string projectCrsUnit() const;
    bool setProjectCrs(const std::string& epsg, const std::string& unit, std::string& e);
    bool importDirectory(const std::string& directory, const ImportOptions& options, ImportReport& result, std::string& error, const ProgressCallback& progress={}, const CancelCheck& cancelled={});
    bool importFile(const std::string& path, const ImportOptions& options, ImportReport& result, std::string& error, const CancelCheck& cancelled={});
    // Persist a host-prepared, normalized model without reparsing or copying.
    // expectedSha256 binds it to readablePath; storedReference is interpreted
    // relative to this data project after its enclosing directory is published.
    // The caller owns parsing correctness and publication of that directory.
    bool importPreparedFile(const std::string& readablePath, const std::string& identityPath,
                            const std::string& storedReference, const std::string& expectedSha256,
                            const ParsedFile& model, long long& sourceId, std::string& error,
                            const CancelCheck& cancelled={});
    bool listWells(std::vector<WellSummary>& result, std::string& error) const;
    bool readWellInfo(long long wellId, WellInfo& result, std::string& error) const;
    // Basic fields only: one query, no per-well counts or payload decoding.
    bool listWellInfos(std::vector<WellInfoRecord>& result, std::string& error) const;
    bool saveWellInfo(long long wellId, const WellInfo& value, std::string& error);
    bool createWell(const WellInfo& value, long long& wellId, std::string& error);
    // 先列出资料卡片，供选择ID。wellId=0 表示不按井过滤，也包含待归井资料。
    // kind: curves曲线、intervals分层道、samples样品表、time_depth时深、trajectories测斜，
    // 或 markers、core_photos、attachments、horizons、maps、volumes、sections、tables。
    bool listData(const std::string& kind, long long wellId, std::vector<DataEntry>& result, std::string& error) const;
    // Filter in SQLite, without listing every other source in the work area.
    bool listSourceData(const std::string& kind, long long sourceId, std::vector<DataEntry>& result, std::string& error) const;
    bool readTable(const std::string& kind, long long id, long long offset, int limit, DataTable& result, std::string& error) const;
    // 真正读取曲线数组。范围含端点；topDepth/bottomDepth 为 Missing 时不限制相应端。
    // processed=true 读取处理值，false 读取原值；两种模式都返回 rawValues 与 quality。
    // ID来自 listData；同名曲线可能属于不同组，调用者需先选定来源/曲线组。
    bool readCurveSlice(long long curveId, double topDepth, double bottomDepth,
                        bool processed, CurveSlice& result, std::string& error) const;
    // 曲线组编号来自 DataEntry.parentId。完整返回共同深度/TVD/TVDSS轴、曲线及来源工作表。
    bool readLogSet(long long logSetId, bool processed, LogSet& result, std::string& error) const;
    bool cleanCurve(long long curveId, const CurveCleaningRule& rule, std::string& error);
    bool saveCurveUnit(long long curveId, const std::string& confirmedUnit, std::string& error); // confirm unknown unit; never relabel existing measurements
    bool updateCurveValue(long long curveId, long long sampleIndex, double value, std::string& error);
    bool resetCurve(long long curveId, std::string& error);
    // 返回与深度范围相交的层段，保留层段原顶底深，不把边界裁成请求值。
    bool readIntervals(long long trackId, double topDepth, double bottomDepth, std::vector<IntervalRecord>& result, std::string& error) const;
    bool readIntervalTrackInfo(long long trackId, IntervalTrack& result, std::string& error) const; // metadata only
    // 将同一口井、同一曲线组共享深度轴的曲线组合为特征表。
    // 只准备数据，不隐式插值、换单位或执行地质分类模型。
    bool prepareWellFeatures(long long wellId, const std::vector<long long>& curveIds, double topDepth, double bottomDepth, bool processed, WellFeatureTable& result, std::string& error) const;
    bool compareIntervals(const std::vector<long long>& trackIds, double topDepth, double bottomDepth, std::vector<IntervalTrackResult>& result, std::string& error) const;
    bool findMaps(const std::string& area, const std::string& type, const std::string& stratum, std::vector<GeologicalMap>& result, std::string& error) const;
    bool saveInterval(long long trackId, IntervalRecord& interval, std::string& error);
    bool deleteData(const std::string& kind, long long id, std::string& error); // DB records only; external files never deleted
    bool listSources(std::vector<SourceEntry>& result, std::string& error) const;
    bool listSourceIndexEntries(std::vector<SourceIndexEntry>& result, std::string& error) const;
    bool listIssues(bool unresolvedOnly, std::vector<IssueEntry>& result, std::string& error) const;
    bool resolveIssue(long long issueId, const std::string& resolution, std::string& error);
    bool reimportSource(long long sourceId, const ParseOptions& correctedMapping, ImportReport& result, std::string& error, const CancelCheck& cancelled={});
    bool sourcePath(long long sourceId, std::string& path, std::string& error) const;
    bool verifySource(long long sourceId, std::string& error) const; // SEG-Y uses size/mtime/header fingerprint; other sources SHA-256
    bool readDocument(long long sourceId, std::string& text, std::string& error) const;
    bool relocateSource(long long sourceId, const std::string& path, std::string& error);
    bool readSampleTable(long long tableId, SampleTable& result, std::string& error) const;
    // 返回完整时深点数组及深度/时间约定；保留负时间和未知单位，不自动换算。
    bool readTimeDepth(long long id, TimeDepthTable& result, std::string& error) const;
    bool assignTimeDepthWell(long long id, long long wellId, std::string& error); // 0 clears association; points and source remain unchanged
    bool readTrajectory(long long id, TrajectorySurvey& result, std::string& error) const;
    // 每个图道返回照片数组，路径已解析为可访问的完整路径；深度未知时为 Missing。
    bool readCorePhotoTrack(long long id, CorePhotoTrack& result, std::string& error) const;
    bool readAttachment(long long id, Attachment& result, std::string& error) const;
    bool readVolume(long long id, SeismicVolume& result, std::string& error) const; // metadata only; traces stays empty
    bool saveVolume(long long id, const SeismicVolume& value, std::string& error); // edits existing registration; never stores amplitudes
    bool readHorizon(long long id, long long offset, int limit, SeismicHorizon& result, std::string& error) const;
    bool saveHorizonInfo(long long id, const SeismicHorizon& value, std::string& error); // name/parent/conventions only
    bool readMap(long long id, GeologicalMap& result, std::string& error) const;
    bool saveMap(long long& id, const GeologicalMap& value, std::string& error);
    bool saveSection(long long& id, const SeismicSection& section, std::string& error);
    bool readSection(long long id, SeismicSection& result, std::string& error) const;
    // 快速核验外部SEG-Y。仅返回文件事实，不替用户确认CRS、TWT或时间基准。
    bool inspectSeismic(long long volumeId, SeismicFileInfo& result, std::string& error, const CancelCheck& cancelled={});
    // 测区平面坐标范围（四角点+包围盒）。volumeId 取 listData("volumes") 的编号，
    // 与 inspectSeismic/readSeismicLine 同一来源校验。返回 true 只表示调用成功：
    // 范围不可得属于文件事实，读 status 与 warnings，不要用 0 代替 Missing。
    // 角点坐标为抽样道头拟合值，按 fitRmsResidual/fitMaxResidual 判断可用精度；
    // 不自动投影、不换算单位、不外推缺道。cancelled 在打开与换算阶段协作生效。
    bool readSeismicCoordinateRange(long long volumeId, SeismicCoordinateRange& result,
                                    std::string& error, const CancelCheck& cancelled={}) const;
    // 工区平面范围：井口点最小外接矩形 ∪ 全部地震体范围矩形。
    // 井口取 listWellInfos 的实际坐标；缺坐标的井计数上报，不按 0 或井名补位。
    // 地震体逐个取 readSeismicCoordinateRange，给出范围的体参与并集；其余记入
    // volumesWithoutRange 与 warnings，不因单个体不可读而让整次调用失败。
    // 合并前要求两侧坐标系与单位已声明且一致；未声明或冲突时 merged 留空，
    // 由调用方决定先确认约定还是先做转换。返回 true 只表示查询完成。
    bool readProjectPlanarRange(ProjectPlanarRange& result, std::string& error,
                                const CancelCheck& cancelled={}) const;
    // 按文件位置读取单道，既不要求已知CRS，也不要求规则三维网格。
    bool readSeismicTrace(long long volumeId, long long traceIndex, const SeismicTraceReadOptions& options,
                          SeismicTraceResult& result, std::string& error, const CancelCheck& cancelled={});
    // direction为inline或crossline，lineNumber为真实测线号。直接看原生测线无需填写CRS。
    // 时间窗单位与返回结果一致；超出网格或格式不支持时明确报错，不遍历整个大文件。
    bool readSeismicLine(long long volumeId, const std::string& direction, int lineNumber, const SectionReadOptions& options,
                         SeismicSectionResult& result, std::string& error, const CancelCheck& cancelled={});
    // 原生IL/XL网格切片：时间按文件道头ms解释，不要求已确认CRS/TWT。
    // 用于文件预览；跨井/层位联合分析仍须确认坐标、时间类型和参考面。
    bool readSeismicTimeSlice(long long volumeId, double timeMs, const TimeSliceReadOptions& options,
                             SeismicTimeSliceResult& result, std::string& error, const CancelCheck& cancelled={}, const TimeSliceTileCallback& tileReady={});
    static void clearSeismicSliceCache();       // 清空进程内切片缓存，不改动来源文件。
    bool processSeismicTrace(const SeismicTraceResult& input, const SeismicProcessingOptions& options,
                            SeismicTraceResult& result, std::string& error, const CancelCheck& cancelled={}) const;
    bool processSeismicSection(const SeismicSectionResult& input, const SeismicProcessingOptions& options,
                              SeismicSectionResult& result, std::string& error, const CancelCheck& cancelled={}) const;
    bool seismicStatistics(const std::vector<float>& amplitudes, SeismicStatistics& result, std::string& error,
                           const CancelCheck& cancelled={}) const;
    bool extractSection(long long sectionId, const SectionReadOptions& options, SeismicSectionResult& result, std::string& error, const CancelCheck& cancelled={});
    // 直接用多个有序XY坐标提取剖面墙，无需先保存路径。坐标与时间约定必须与所属体一致。
    // fullResolution 时访问路径穿过的原生网格道；振幅取最近网格道，不插值、不复制地震体。
    // pathX/pathY/distances 描述墙体几何；x/y/traceIndexes 描述实际来源道；缺道保留NaN。
    bool extractSection(const SeismicSection& section, const SectionReadOptions& options, SeismicSectionResult& result, std::string& error, const CancelCheck& cancelled={});
    bool readTimeSlice(long long volumeId, double time, const TimeSliceReadOptions& options, SeismicTimeSliceResult& result, std::string& error, const CancelCheck& cancelled={}, const TimeSliceTileCallback& tileReady={});
    bool backupDatabase(const std::string& destination, std::string& error) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
