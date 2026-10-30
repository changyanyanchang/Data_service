// 多源资料模型：工区下的单井、地震数据与地质平面图平级；层位、二维剖面通过编号关联地震体。
// 本版定义当前范围内的完整逻辑数据组成；只用普通结构体、基础类型、string 和 vector。
// 数组描述逻辑数据组成；DataService 单独实现持久化与按需访问，不自动加载整个工区或地震体。
// 未知浮点数用 Missing（NaN），通过 std::isnan(value) 判断；0 是正常数值。
// 未知文本用空字符串。数值列的空单元格读为 Missing，写回时 Missing 保存为空单元格。
#pragma once

#include <string>
#include <vector>
#include <limits>
#include <cmath>

namespace paleo::data {

const double Missing = std::numeric_limits<double>::quiet_NaN();

// 一组数据实际使用的深度类型、单位和起算面；没有依据时保持空字符串。
// 曲线、分层等组独立保存；样品和照片采用井默认约定，测斜采用其中的 MD 约定。
// 导入时先核对单位和基准；不一致或未知时先记录问题，不能直接套用默认值。
struct DepthInfo {
    std::string depthType; // 主深度类型：MD、TVD、TVDSS；空表示未知
    std::string depthUnit; // 深度单位，如 m 或 ft；空表示未知
    std::string mdReference; // MD 起算面，如转盘面；空表示未知
    std::string tvdReference; // TVD 起算面；空表示未知
    std::string tvdssReference; // TVDSS 垂向基准，如平均海平面
    std::string tvdssDirection; // TVDSS 正方向：down、up；空表示未知
};

// 单井基础信息；坐标顺序约定 wellheadX 为东向或经度，wellheadY 为北向或纬度。
// 原始坐标轴序没有确认时，不直接写入 wellheadX/wellheadY，应先在导入问题中保留原值。
struct WellInfo {
    std::string wellName; // 井名，如 SYNTH-1
    std::string fieldName; // 油田或工区名称
    std::string wellType; // 井型，如直井、斜井或水平井
    double wellheadX = Missing; // 井口 X 坐标；缺失时为 Missing
    double wellheadY = Missing; // 井口 Y 坐标；缺失时为 Missing
    std::string coordinateSystem; // 坐标系名称或代码；空表示未知
    std::string coordinateUnit; // 井口 X/Y 坐标单位，如 m、ft 或 degree；空表示未知
    double totalDrilledDepth = Missing; // 报告中的总井深，采用 WellDataset.defaultDepthInfo
    std::string remark; // 其他需要保留的说明
};

// 同一组曲线共用的显式深度数组；深度类型与单位由所属 LogSet.depthInfo 给出。
// 附加深度数组允许为空；非空时须与 depthValues 等长，个别缺失值使用 Missing。
struct SamplingAxis {
    std::vector<double> depthValues; // 主深度数组，保留原始采样位置
    std::vector<double> tvdValues; // 来源提供的垂深，可整列缺失
    std::vector<double> tvdssValues; // 来源提供的海拔垂深，可整列缺失
    double sampleStep = Missing; // 经确认的采样步长摘要，非等距或未确认时为 Missing
};

// 一条测井曲线；sampleValues 与所属 LogSet.samplingAxis.depthValues 一一对应。
struct LogCurve {
    std::string curveName; // 曲线名称，如 GR、RT、AC
    std::string curveUnit; // 曲线单位，如 API；空表示未知
    std::vector<double> sampleValues; // 曲线数值，缺失采样点为 Missing
    std::string description; // 曲线含义或原始表头说明
};

// 一批具有共同深度轴的测井曲线；不同批次可建立多个 LogSet。
struct LogSet {
    std::string logSetName; // 曲线组名称，如综合测井
    DepthInfo depthInfo; // 本组数据的实际深度单位与基准
    SamplingAxis samplingAxis; // 所有曲线共用的采样深度
    std::vector<LogCurve> logCurves; // 各条测井曲线
    std::string sourceFile; // 原始 CSV、LAS 或 Excel 文件路径
    std::string sourceSheet; // 原始工作表名；非表格资料可为空
};

// 一个岩性、地层、沉积相或旋回层段；主顶底深遵循所属分层道的 DepthInfo。
// 不同层段可以同名；intervalId 在同一口井的全部 GeologicalInterval 中唯一。
struct Measurement;
struct GeologicalInterval {
    int intervalId = 0; // 层段编号，0 表示尚未分配
    std::string intervalName; // 层段名称或岩性描述
    int displayOrder = 0; // 显示顺序，不用作关联身份
    std::string faciesType; // 相类型
    int topUnconformity = -1; // -1未知，0否，1是
    int bottomUnconformity = -1; // -1未知，0否，1是
    int bottomNotPenetrated = -1; // 底部未钻穿
    std::vector<Measurement> properties; // 附加层段指标
    double topDepth = Missing; // 主深度类型下的顶深
    double bottomDepth = Missing; // 原记录止深；未钻穿时不能当作地层真实底界
    double topTvd = Missing; // 来源提供的顶界 TVD
    double bottomTvd = Missing; // 来源提供的底界 TVD
    double topTvdss = Missing; // 来源提供的顶界 TVDSS
    double bottomTvdss = Missing; // 来源提供的底界 TVDSS
    int sourceRow = 0; // 原始表格行号，1 起算；0 表示未知
};

// 一条地质分层道；不同解释方案或组、段等不同层级分别建道。
// 不同道可覆盖相同深度区间，不能仅凭顶底深相同就合并解释。
struct IntervalTrack {
    std::string trackName; // 道名称，如文昌组分层、岩性解释
    std::string trackType; // 道类型，如地层、岩性、沉积相、旋回
    DepthInfo depthInfo; // 本道实际使用的深度单位与基准
    std::vector<GeologicalInterval> intervals; // 本道各个层段
    std::string sourceFile; // 原始数据文件路径
    std::string sourceSheet; // 原始工作表名
};

// 一个界面点；markerId 在同一口井的全部 Marker 中唯一。
struct Marker {
    int markerId = 0; // 界面编号，0 表示尚未分配
    std::string markerName; // 标准层或界面名称
    double depthValue = Missing; // 主深度值，遵循所属 MarkerTrack.depthInfo
    double tvd = Missing; // 来源提供的 TVD
    double tvdss = Missing; // 来源提供的 TVDSS
    int sourceRow = 0; // 原始表格行号，1 起算；0 表示未知
};

// 一组标准层或界面点，使用共同深度约定。
struct MarkerTrack {
    std::string trackName; // 界面道名称
    DepthInfo depthInfo; // 本道的深度单位与基准
    std::vector<Marker> markers; // 各个界面点
    std::string sourceFile; // 原始文件路径
    std::string sourceSheet; // 原始工作表名
};

// 样品的一项分析结果；只保留指标名称、单位、可用数值和原始文字。
// 对于“<0.01”等限定值，不直接当作精确数值写入 numericValue，应保留 originalText。
struct Measurement {
    std::string measurementName; // 指标名称，如 SiO2、孢粉总数
    std::string measurementUnit; // 指标单位，如 %、ppm、粒；空表示未知
    double numericValue = Missing; // 明确的数值结果；文字、限定值或缺失时为 Missing
    std::string originalText; // 原始结果文字，如 12.5、<0.01、未检出或环境描述
};

// 一个样品对应一行分析记录，编号保留原报告写法，不要求全井唯一。
// 点样品的 topDepth 与 bottomDepth 相等；区间样品分别保存顶底深。
// 深度采用 WellDataset.defaultDepthInfo；来源不明的取样范围先留待核对。
struct SampleRecord {
    std::string sampleNumber; // 来源中的样品编号
    std::string sampleType; // 岩心、岩屑等
    std::string sourceRange; // 原单元格范围
    double topDepth = Missing; // 点样品深度，或区间样品顶深
    double bottomDepth = Missing; // 区间样品底深；点样品等于顶深，未知时为 Missing
    std::vector<Measurement> measurements; // 该样品的分析结果数组
};

// 一批样品分析结果；不同实验内容或不同批次建立不同的表。
// 通过表名区分内容和批次，原文件登记在 WellDataset.sourceFiles。
struct SampleTable {
    std::string tableName; // 分析表名称
    std::string sourceFile; // 来源文件
    std::string sourceSheet; // 来源工作表
    std::vector<SampleRecord> samples; // 本表的样品记录数组
};

// 一个测斜测点，只保存测量井深、井斜角和方位角这三项输入。
// measuredDepth 固定为 MD，使用井默认 depthUnit 和 mdReference，不随 depthType 改变。
struct SurveyPoint {
    double measuredDepth = Missing; // 测量井深 MD，采用井默认深度单位与 MD 起算面
    double inclinationDegrees = Missing; // 相对向下铅垂方向的井斜角，单位为度
    double azimuthDegrees = Missing; // 方位角，单位为度
};

// 一组测斜输入；轨迹计算另行实现，不在本组重复保存垂深和坐标结果。
// 原始报告保留在 WellDataset.sourceFiles；角度导入时核对并统一为度。
struct TrajectorySurvey {
    std::string surveyName; // 测斜表名称，如完钻测斜
    std::string azimuthReference; // 方位角相对真北、网格北或磁北；空表示未知
    std::vector<SurveyPoint> surveyPoints; // 测斜点数组
};

// 时深关系中的一个采样点，深度和时间约定由所属 TimeDepthTable 给出。
struct TimeDepthPoint {
    double depthValue = Missing; // 深度值
    double timeValue = Missing; // 对应的时间值
    // 来源提供的同一采样位置；不从井口坐标或图片补齐。旧的二列时深表保持 Missing。
    double x = Missing;
    double y = Missing;
    double z = Missing;
};

// 井级时深关系，放在 WellDataset 下与 wellbore 并列，供地震数据使用。
// 保留单程或双程时间及零点，避免混用。
struct TimeDepthTable {
    std::string relationName; // 时深关系表名称
    DepthInfo depthInfo; // 深度类型、单位与基准
    std::string timeUnit; // 时间单位，如 ms；空表示未知
    std::string timeType; // OWT 或 TWT；空表示未知
    std::string timeReference; // 时间零点或参考面；空表示未知
    std::vector<TimeDepthPoint> depthTimePairs; // 时深配对采样点
    std::string sourceFile; // 原始时深文件路径
    std::string sourceSheet; // 原始工作表名
    // Petrel checkshots 可同时提供沿井 X/Y/Z；Z 的单位、方向、基准独立于 MD。
    DepthInfo positionDepthInfo;
    std::string coordinateSystem;
    std::string coordinateUnit;
};

// 地震层位文件中的一行：INLINE、XLINE、X、Y、Z 时间。
// INLINE/XLINE 是实际测线编号，不是数组下标；两项均须确认后才形成有效层位点。
// 编号初始化为 0 只是 C++ 初值，不表示缺失；实际编号可以为 0。
struct SeismicHorizonPoint {
    int inlineNumber = 0; // INLINE 测线编号，如 2800
    int crosslineNumber = 0; // XLINE 测线编号，如 1527
    double x = Missing; // X 坐标，如 1000.0
    double y = Missing; // Y 坐标，如 2000.0
    double zTime = Missing; // Z 轴时间，如 1500.0；单位由所属层位说明
};

// 一个时间域地震层位，由多个平面位置及其 Z 时间组成。
// 坐标系、坐标单位、时间单位和单/双程含义依据来源填写，未知时保持空字符串。
// 在 SeismicData.horizons 中独立登记，用 parentVolumeId 指向所属地震体。
// parentVolumeId=0 表示尚未关联，不能按名称猜测；points 描述该层位的全部点记录。
// 坐标与时间约定保留原始资料实际含义；关联地震体前核对，不能直接覆盖为体数据的约定。
struct SeismicHorizon {
    std::string horizonName; // 地震层位名称，如 T30；由文件名或解释资料确认
    int parentVolumeId = 0; // 所属地震体编号，对应 SeismicVolume.volumeId；0 表示尚未关联
    std::string coordinateSystem; // X/Y 坐标系名称或代码；空表示未知
    std::string coordinateUnit; // X/Y 坐标单位，如 m、ft；空表示未知
    std::string timeUnit; // Z 时间单位，如 ms、s；空表示未知
    std::string timeType; // OWT（单程）或 TWT（双程）；空表示未知
    std::string timeReference; // Z 时间零点或参考面；空表示未知
    std::vector<SeismicHorizonPoint> points; // 层位点数组，每项对应 INLINE、XLINE、X、Y、Z 时间
    std::string sourceFile; // 原始层位文件相对路径，可指向工区共享文件
};

// 地震体的空间范围摘要；测线范围不表示其中每个网格位置都有道。
// traceCount > 0 时测线范围才有效；完整数据中，道数与测线范围须与 traces 一致。
struct SeismicGeometry {
    long long traceCount = -1; // 地震道总数；-1 表示尚未统计，0 表示空地震体
    int inlineMin = 0; // 最小 INLINE 编号
    int inlineMax = 0; // 最大 INLINE 编号
    int crosslineMin = 0; // 最小 XLINE 编号
    int crosslineMax = 0; // 最大 XLINE 编号
    std::string coordinateSystem; // 平面坐标系名称或代码；空表示未知
    std::string coordinateUnit; // 平面坐标单位；空表示未知
};

// 一期面向时间域、具有共同采样轴的地震体；不一致的道须明确处理后才作为规则体读取。
// 采样参数须核对实际道头，不能只由二进制文件头或测线范围推断。
struct SeismicSampling {
    int sampleCount = 0; // 每道采样点数，0 表示尚未确认共同采样轴
    double firstTime = Missing; // 第 0 个样点的时间
    double sampleInterval = Missing; // 相邻样点时间间隔，须大于 0
    std::string timeUnit; // firstTime 与 sampleInterval 共用的单位，如 ms
    std::string timeType; // OWT（单程）或 TWT（双程）；空表示未知
    std::string timeReference; // 时间零点或参考面；空表示未知
};

// 一条地震道：测线编号、平面位置与按时间排列的振幅序列。
// 原型范围是具有共同时间轴的三维叠后体；时间约定采用所属 SeismicVolume.sampling。
// 第 k 个振幅对应 firstTime + k * sampleInterval，amplitudes.size() 应等于 sampleCount。
// 原始道号允许重复或未知，不能代替 INLINE/XLINE 定位；缺失振幅用 float NaN，0 是有效值。
struct SeismicTrace {
    long long traceNumber = -1; // 来源中的道号；-1 表示未知，原始道号 0 仍有效
    int inlineNumber = 0; // 本道 INLINE 编号
    int crosslineNumber = 0; // 本道 XLINE 编号
    double x = Missing; // 本道 X 坐标，采用地震体的坐标单位与坐标系
    double y = Missing; // 本道 Y 坐标，采用地震体的坐标单位与坐标系
    std::vector<float> amplitudes; // 本道的振幅序列，按采样时间排列
};

// 地震体的完整逻辑定义：身份、来源、空间约定、时间约定和逐道振幅。
// traces 描述体数据组成；是否常驻内存以及如何读取属于后续访问层，不在本版规定。
struct SeismicVolume {
    int volumeId = 0; // 工区内唯一的地震体编号；0 表示尚未分配，保存后不随改名或排序改变
    std::string volumeName; // 地震体名称
    std::string dataFile; // 原始地震数据文件相对路径，如 SEG-Y 文件
    std::string fileFormat; // 原始数据格式，如 SEG-Y；空表示未知
    std::string amplitudeUnit; // 振幅单位或相对振幅说明；空表示未知
    SeismicGeometry geometry; // 道数、测线范围与平面坐标约定
    SeismicSampling sampling; // 共同的时间采样信息
    std::vector<SeismicTrace> traces; // 地震道数组，每道保存位置与振幅序列
};

// 二维地震剖面路径上的一个平面坐标点；这里的点是路径顶点，不一定是实际地震道位置。
// X/Y 的坐标系与单位由所属 SeismicSection 给出；不保存 Z，时间轴提取时从地震体取得。
struct SeismicSectionPoint {
    double x = Missing; // 路径点 X 坐标
    double y = Missing; // 路径点 Y 坐标
};

// 从地震体按需提取的二维剖面定义；只持久化名称、所属体、坐标约定与有序路径点。
// 两点定义直线剖面，多点按数组顺序连接为折线；不按 X/Y 排序，不自动闭合。
// 有效路径至少两个有限坐标点，连续点不能重合；未知坐标可在草稿中保留但不能参与提取。
// 保留路径自身的坐标约定，避免未关联或换体后坐标被重新解释；未知约定保持空字符串。
// 提取前核对所属体存在及坐标约定一致，或完成明确坐标转换；不能仅凭数值接近判断一致。
// 不保存振幅、剖面图片或地震道副本。沿线采样和读取由后续访问层执行，结果按需返回。
struct SeismicSection {
    std::string sectionName; // 二维剖面名称，如 A—A′ 连井剖面
    int parentVolumeId = 0; // 所属地震体编号；0 表示待关联，不能提取剖面
    std::string coordinateSystem; // 路径坐标系名称或代码；未知时为空
    std::string coordinateUnit; // 路径 X/Y 坐标单位，如 m；未知时为空
    std::vector<SeismicSectionPoint> pathPoints; // 按剖面起点到终点排列的路径坐标数组
};

// 地震数据总目录：地震体、层位与二维剖面分别保存；层位和剖面通过编号关联所属体。
// volumeId 使用已持久化的正整数，不用名称或数组下标代替；删除后不复用旧编号。
// parentVolumeId 非 0 时必须能在本目录中找到唯一地震体；0 保留为待关联状态。
// 删除地震体前先处理引用；导入另一工区有编号冲突时，体编号与层位、剖面关联一起重映射。
struct SeismicData {
    std::vector<SeismicVolume> volumes; // 地震体数据集合
    std::vector<SeismicHorizon> horizons; // 层位数据集合，每条记录说明所属地震体
    std::vector<SeismicSection> sections; // 二维地震剖面集合，仅保存定义与路径，显示时按需提取
};

// 一张岩心照片：每张照片都有自己的文件路径和对应深度范围。
// 深度遵循 WellDataset.defaultDepthInfo；未知时保留 Missing。
struct CorePhoto {
    std::string imagePath; // 这一张岩心照片的相对路径
    double topDepth = Missing; // 这一张照片对应的顶深
    double bottomDepth = Missing; // 这一张照片对应的底深
    std::string description; // 这一张照片的岩心描述或备注
};

// 一个岩心照片图道包含若干张照片，展示时按各张照片的深度排列。
struct CorePhotoTrack {
    std::string trackName; // 岩心照片道名称
    std::vector<CorePhoto> photos; // 照片对象数组，每张照片独立保存深度范围
};

// 原型只支持一个主井筒，相关业务数据直接放在对应集合中。
struct WellboreData {
    std::string wellboreName = "主井筒"; // 井筒显示名称
    std::vector<LogSet> logSets; // 测井曲线组
    std::vector<IntervalTrack> intervalTracks; // 地质分层道
    std::vector<MarkerTrack> markerTracks; // 标准层与界面道
    std::vector<SampleTable> sampleTables; // 样品分析表
    std::vector<TrajectorySurvey> trajectories; // 测斜资料组
    std::vector<CorePhotoTrack> corePhotoTracks; // 岩心照片道，每个道包含多张照片
};

// 附件文件独立保存在硬盘上；同一文件关联多个对象时可建立多条关联记录。
// targetId 必须结合 targetType 使用；目标对象改名或调序不改变其编号。
struct Attachment {
    std::string attachmentName; // 附件显示名称
    std::string attachmentType; // 照片、报告、图件等
    std::string filePath; // 相对于资料包根目录的路径
    std::string targetType = "well"; // well、interval 或 marker；样品报告作为井级附件，岩心照片由照片道管理
    int targetId = 0; // 井级附件为 0；其余填写对应对象的非零编号
};

// 单井资料的总对象：主井筒资料与时深关系并列，地震体及其层位独立于单井。
// sourceFiles 和 sharedFiles 仅登记路径，不把附件或工区大文件嵌入对象。
struct WellDataset {
    WellInfo wellInfo; // 井基础属性
    DepthInfo defaultDepthInfo; // 样品与照片共用的井深度约定；测斜采用其中的 MD 单位与起算面
    WellboreData wellbore; // 单个主井筒资料
    std::vector<TimeDepthTable> timeDepthRelations; // 井级时深关系，供地震标定及深度—时间转换使用
    std::vector<Attachment> attachments; // 照片、报告等附件关联
    std::vector<std::string> sourceFiles; // 原始资料文件路径清单
    std::vector<std::string> sharedFiles; // 引用工区公共资料的相对路径
};

// 一张工区级地质平面图；一张图可以对应多个地层，不属于某口井或某个地震体。
// mapId 在工区内唯一且为正整数，改名或排序不变；导入冲突时统一重编号，不用名称作身份。
// 地层名称使用完整名称，如“文昌组四段”；数组保留多个名称，未知时为空，不由图名自动确认。
// 所有路径相对于资料包根目录；直接导入 PNG 时 imagePath 与 sourceFile 可以相同。
// georeferenceStatus=1 表示配准资料已经核实，不代表当前应用已实现空间叠加。
// 状态为 1 时，坐标系与 georeferenceFile 必须明确，配准须对应当前 imagePath 的像素内容。
// 图片裁剪、旋转或替换后须重新核验配准；图上的经纬度刻度不能单独作为配准依据。
struct GeologicalMap {
    int mapId = 0; // 工区内图件编号；0 表示尚未分配，保存时分配稳定正整数
    std::string mapName; // 完整图名，如惠西南文昌组四段沉积相图
    std::string mapType; // 图件类型，如沉积相图、构造图；空表示未知
    std::string areaName; // 图件覆盖的地区名称，可与所属工区名称不同
    std::vector<std::string> stratigraphicNames; // 对应地层名称数组，如恩平组一段、恩平组二段
    std::string imagePath; // 用于预览的单张图片路径；保存路径，不保存像素数组
    std::string sourceFile; // 原始图片、报告或图集路径，可与 imagePath 相同
    std::string sourceLocation; // 在原资料中的图号、页号等；独立图片或未知时为空
    std::string coordinateSystem; // 已确认的图件坐标系名称或代码；未知时为空
    int georeferenceStatus = -1; // 配准资料状态：-1 未核实，0 无可用配准，1 配准资料已核实
    std::string georeferenceFile; // 配准资料路径，也可指向自带配准的图片；无依据时为空
    std::string description; // 图件内容、来源说明、图例摘要或其他备注
};

// 工区公共文件的登记信息；地震与地质平面图的业务属性由各自对象保存。
// 同一路径可以被多个对象引用，但不在 SharedFile 中维护第二份图件属性。
struct SharedFile {
    std::string fileName; // 公共资料名称
    std::string fileType; // 地震体、地震层位、沉积相图或工区报告
    std::string filePath; // 相对于资料包根目录的路径
    std::string description; // 单位、坐标系、配准状态等说明
};

// 工区级容器：单井资料、地震数据与地质平面图平级；地震体、层位和二维剖面在地震目录中管理。
struct ProjectData {
    std::string projectName; // 工区或项目名称
    std::vector<WellDataset> wells; // 工区内各口井的资料
    SeismicData seismicData; // 地震数据：地震体、层位与二维剖面集合
    std::vector<GeologicalMap> geologicalMaps; // 地质平面图集合，一项对应一张图及其来源信息
    std::vector<SharedFile> sharedFiles; // 工区公共文件登记
};

// 导入过程中需要人工核对的问题，不属于单井地质数据字段。
struct ImportIssue {
    std::string sourceFile; // 出现问题的源文件路径
    int sourceRow = 0; // 原始行号，1 起算；0 表示文件级或未知
    std::string problemDescription; // 冲突、缺失或格式问题的说明
    bool isResolved = false; // 是否已经核对并处理
};


} // namespace paleo::data
