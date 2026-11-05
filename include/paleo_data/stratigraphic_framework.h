#pragma once
// 统一地层层段体系：界面（Boundary）≠ 地层单元。
// 相邻两个有序界面构成一个父层间单元（Interval，如 T70–T71）；
// 每个父单元恰有四个有序、有厚度的子层段 S1–S4（默认顶→底编号）。
// 所有分析与成果归属于稳定 SubIntervalId；显示名不作主键。
// E12/E3/E4 等地层对比算法区段与 L3/L4 旋回只是算法内部/参考证据，
// 不是本体系的单元身份（见 docs/STRATIGRAPHIC_FRAMEWORK.md）。
#include "model.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace paleo::data {

// 强类型身份：不同种类的 ID 之间不能隐式互换。
template <class Tag>
struct StrongId {
    std::string value;
    bool empty() const { return value.empty(); }
    friend bool operator==(const StrongId& a, const StrongId& b) { return a.value == b.value; }
    friend bool operator!=(const StrongId& a, const StrongId& b) { return a.value != b.value; }
    friend bool operator<(const StrongId& a, const StrongId& b) { return a.value < b.value; }
};
using BoundaryId = StrongId<struct BoundaryIdTag>;
using IntervalId = StrongId<struct IntervalIdTag>;
using SubIntervalId = StrongId<struct SubIntervalIdTag>;
using InterpretationRevisionId = StrongId<struct InterpretationRevisionIdTag>;

constexpr int SubIntervalCount = 4;

// 层序界面：地震时间面、井中深度点、对比线或确认界面。没有厚度。
struct StratigraphicBoundary {
    BoundaryId id;
    std::string name;          // 显示名，如 T70；不作身份
    int order = 0;             // 顶→底严格递增的地层顺序（不是图像坐标）
    std::string kind = "horizon"; // horizon|marker|correlation|confirmed
    std::string source;        // 来源说明（导入资源、人工）
    long long seismicHorizonResourceId = 0; // 可选：关联的原始地震层位资源，原始数据不改
    std::string evidenceJson;
};

// 父层间单元：顶界面 + 相邻底界面。
struct StratigraphicInterval {
    IntervalId id;
    BoundaryId top, bottom;
    int order = 0;
    std::string status = "valid"; // valid|incomplete|invalid
    std::string legacySegment;    // 仅参考：所在旧对比算法区段（如 E12），不是身份
    std::string evidenceJson;
};

// 内部分界状态：derived=初始等比推导（provisional），confirmed=解释确认，pending=待解释。
struct SubIntervalDivision {
    IntervalId interval;
    std::string mode = "initial_equal"; // initial_equal|interpreted
    std::array<double, 3> fractions{0.25, 0.5, 0.75}; // 规范化层间坐标，0=顶 1=底
    std::array<std::string, 3> edgeStatus{"derived", "derived", "derived"};
    std::string basis = "equal-proportion 0/0.25/0.5/0.75/1 (provisional geometry only)";
};

struct StratigraphicSubInterval {
    SubIntervalId id;
    IntervalId interval;
    int index = 0; // 1..4，顶→底
    std::string status = "valid"; // valid|pending|missing|pinched_out
};

// 某口井上一个父单元的实际出现：各井深度不同但身份一致。
struct WellIntervalOccurrence {
    long long wellId = 0;
    IntervalId interval;
    std::string depthType = "MD", depthUnit = "m";
    double top = Missing, bottom = Missing; // 缺失为 NaN：不补造
    // 解释约束的内部三个分界深度；NaN 表示采用父单元 Division 比例（provisional）。
    std::array<double, 3> internal{Missing, Missing, Missing};
    std::array<std::string, 3> internalStatus{"derived", "derived", "derived"};
    std::array<bool, 3> locked{false, false, false};
    // complete|top_missing|not_penetrated|pinched_out|faulted|pending
    std::string status = "complete";
    std::string evidenceJson;
};

// 地层方案（一个修订即一个不可变快照）。
struct StratigraphicScheme {
    std::string id, name;
    InterpretationRevisionId revision, parentRevision;
    std::string projectId, created, author, note;
    std::vector<StratigraphicBoundary> boundaries;
    std::vector<StratigraphicInterval> intervals;
    std::vector<StratigraphicSubInterval> subIntervals;
    std::vector<SubIntervalDivision> divisions;
    std::vector<WellIntervalOccurrence> wellOccurrences;
};

struct StratigraphicSchemeSummary { std::string id, name; InterpretationRevisionId headRevision; std::string created; };
struct StratigraphicSchemeRevisionSummary { InterpretationRevisionId revision, parentRevision; std::string created, author, note; };

struct FrameworkIssue {
    std::string severity; // error|warning
    std::string code;     // duplicate_id|inverted|overlap|gap|missing_boundary|...
    std::string objectId;
    std::string message;
};

// 单井子层段实际深度范围；无法确定时 resolved=false 并给出原因。
struct WellSubIntervalRange {
    SubIntervalId subInterval;
    IntervalId interval;
    long long wellId = 0;
    int index = 0;
    std::string depthType, depthUnit;
    double top = Missing, bottom = Missing;
    bool resolved = false;
    bool provisional = true; // 任一端为 derived 内部面
    std::string reason;
};

// 地震层段有效掩膜：traces×samples，1=属于该子层段，0=不属于，2=待复核/缺失。
struct SeismicSubIntervalMask {
    std::size_t traceCount = 0, sampleCount = 0;
    std::vector<std::uint8_t> values;
    std::size_t insideCount = 0, reviewTraceCount = 0;
};

// 子层段成果登记（单井相/地震相/单因素/编图）。
struct SubIntervalResultBinding {
    std::string id;
    std::string kind;          // well_facies|seismic_facies|single_factor|map|composite
    SubIntervalId subInterval; // 空 + legacyLabel 表示历史 legacy/unresolved
    IntervalId interval;       // 父单元综合成果可只填 interval（aggregate=true）
    bool aggregate = false;
    std::string schemeId;
    InterpretationRevisionId schemeRevision;
    long long wellId = 0;
    std::string assetRef, inputFingerprint, legacyLabel;
    std::string status = "current"; // current|stale|review_required|legacy_unresolved|candidate
    std::string created, detailJson;
};

struct LegacyUnitResolution {
    std::string status; // resolved_candidate|legacy_unresolved
    IntervalId interval;
    SubIntervalId subInterval;
    std::string reason;
};

// 导入/建项期：由地震层位 + 时深关系自动建立井震统一地层框架与四等分小层（T70-T72 / T70-T72-S1..S4）。
struct SeismicStratigraphyBuildOptions {
    std::string schemeId = "default";
    std::string schemeName = "地震层序地层方案（四等分小层）";
    double searchRadiusMeters = 500.0; // 井轨迹/井口匹配层位网格的最大搜索半径（m）
    int subIntervalCount = SubIntervalCount; // 默认 4（S1..S4 四等分）
    bool autoDetectTimeSign = true;    // 自动识别负 TWT（如 Petrel checkshot 海拔以下为负）并转为正双程时
    bool materializeWellTracks = true; // 同步在井上生成 '地震层位' 与 '地震小层' 分层道及 '地震层位标志层'
    bool overwriteExistingTracks = true; // 发布本方案新修订的物化道；旧修订/用户道保留。false则保留已有本方案物化道。
};

struct SeismicStratigraphyBuildReport {
    std::string schemeId;
    InterpretationRevisionId revision;
    int horizonCount = 0;       // 参与排序建模的地震层位数（如 T30..Tg 共 14 个）
    int intervalCount = 0;      // 相邻层位构成的父层位单元数（如 T70-T72 等）
    int subIntervalCount = 0;   // 细化生成的四等分小层总数（intervalCount * 4）
    int matchedWellCount = 0;   // 成功通过时深表关联并生成层位/小层的井数
    int skippedWellCount = 0;   // 缺少时深表或不在层位覆盖范围内的井数
    std::vector<std::string> orderedHorizonNames; // 顶→底排序后的层位名列表
    std::vector<std::string> warnings;
};

// 测井地层道来源类型：统一支持地震派生小层/层位道与常规地质分层道。
enum class StratigraphicTrackSource {
    SeismicFramework,   // 地震层位/小层体系（StratigraphicScheme: T70-T72 或 T70-T72-S1..S4）
    WellIntervalTrack   // 井内分层道（tracks 表：'地震小层'、'地震层位'、'组'、'段' 或其他地层道）
};

struct StratigraphicTrackDescriptor {
    StratigraphicTrackSource source = StratigraphicTrackSource::WellIntervalTrack;
    std::string schemeId;       // SeismicFramework 时为方案 ID（如 "default"）
    std::string trackName;      // 道名称（如 "地震小层", "地震层位", "组", "段"）
    std::string trackType;      // 道类别
    std::vector<std::string> unitNames; // 该道下包含的层位/小层单元名称列表（顶→底排序）
    int wellCount = 0;          // 拥有该地层道的井数
};

struct WellStratigraphicSelector {
    StratigraphicTrackSource source = StratigraphicTrackSource::SeismicFramework;
    std::string schemeId = "default";
    std::string revision;       // 空表示当前头修订
    SubIntervalId subIntervalId;// 已保存方案的小层稳定 ID；显示名请用 unitName。
    IntervalId intervalId;      // 已保存方案的父层稳定 ID；与 subIntervalId 同填时须为其父层。
    std::string trackName;      // WellIntervalTrack 时的分层道名（如 "段"、"组"、"地震小层"）；空则匹配任意道
    std::string unitName;       // 层位/小层显示名（如 "T70-T72-S1"、"T70-T72"、"文昌组"、"文三段"）
    long long trackId = 0;      // WellIntervalTrack 的精确道 ID；0 仅允许无歧义的名称查询。
    long long intervalRecordId = 0; // 同一道可有同名单元，指定记录 ID 可消除歧义。
};

struct WellIntervalLogQuery {
    std::vector<long long> wellIds;        // 空表示查询工区全部有效井
    WellStratigraphicSelector stratigraphy;// 地层/小层选择器（支持地震地层道与其他地层道）
    std::vector<std::string> curveNames;   // 需提取的测井曲线名（如 {"GR","DT","RT","RHOB"}；空表示全部曲线）
    bool processed = true;                 // true 使用清洗/处理后曲线值
    bool includeAuxiliaryIntervals = true; // 同时返回该深度窗内的岩性/测井解释结论等辅助道切片
    double resampleStepMeters = 0.0;       // <=0 保留原深度轴；>0 按指定步长（m）对齐公共深度轴
    int normalizedSampleCount = 0;         // >0 时额外输出 [0,1] 相对深度重采样（如 64 点供神经网络输入）
    long long maxValues = 8000000;         // 所有井的原生/重采样输出数值总预算，1..64000000。
    int maxSamplesPerWell = 1000000;       // 单井任一结果轴样点上限，1..4000000。
    std::function<bool()> cancelled;       // 在调用线程检查；取消/预算失败不返回部分结果。
    int maxAuxiliaryTracks = 64;            // 所有井检视的辅助道总数，1..4096。
    long long maxAuxiliaryRecords = 100000; // 所有辅助道检视/输出的记录总数，1..1000000。
    long long maxAuxiliaryBytes = 16777216; // 辅助元数据/文本及记录工作集预算，1..67108864 字节。
};

struct IntervalRecord {
    long long id = 0;         // 此层段的数据库编号，修改或删除时使用。
    long long trackId = 0;    // 所属分层道编号；同一口井可以有多套不同分层解释。
    GeologicalInterval value; // 真正的层段数据：名称、顶深、底深、岩性/相等。
};

struct FeatureColumn {
    long long curveId = 0;
    std::string name, unit;
    std::vector<double> values; // 这条曲线在公共深度轴上的数值。
    std::vector<unsigned char> quality;
};

// 供后续算法使用的多曲线特征表：一个公共深度轴，多列曲线数值。
// columns[j].values[i] 表示第 j 条曲线在 depths[i] 处的数值。
struct WellFeatureTable {
    long long wellId = 0;
    std::string depthUnit;
    std::vector<double> depths;
    std::vector<FeatureColumn> columns;
};

struct IntervalTrackResult {
    long long trackId = 0, wellId = 0;
    std::string name;
    DepthInfo depthInfo;
    std::vector<IntervalRecord> intervals;
};

struct WellStratigraphicLogSlice {
    long long wellId = 0;
    std::string wellName;
    double wellheadX = Missing, wellheadY = Missing;

    // 命中的地层/小层范围信息
    std::string unitId;         // 如 "T70-T72-S1"
    std::string unitName;       // 如 "T70-T72-S1" 或 "文三段"
    std::string trackName;      // 来源道名称（如 "地震小层"、"段"）
    std::string depthType, depthUnit; // 未确认时保持未知；不得自动补成 MD/m。
    double topDepth = Missing, bottomDepth = Missing;
    double topTimeMs = Missing, bottomTimeMs = Missing; // 若具备时深表，同步给出顶底 TWT(ms)
    double thickness = Missing;
    bool resolved = false;
    std::string unresolvedReason;

    // 截取并对齐后的多曲线特征表（depths + columns[GR, DT, ...]）
    WellFeatureTable featureTable;
    // 若 normalizedSampleCount > 0，按相对深度 [0,1] 等距重采样的特征表
    WellFeatureTable normalizedFeatureTable;

    // 该层段深度窗内的辅助分层道记录（如岩性道、电测解释道、已有相标签）
    std::vector<IntervalTrackResult> auxiliaryTracks;
    long long trackId = 0, intervalRecordId = 0;
    std::string schemeId, schemeRevision, timeReference;
};

namespace framework {
// 由有序界面派生相邻父单元及其四个子层段；复用已有同顶底的单元 ID。
// 不把 N 个界面当 N 个单元：N 个有效界面恰得 N-1 个父单元。
void deriveUnits(StratigraphicScheme& scheme, const std::string& idSeed = {});
std::vector<FrameworkIssue> validate(const StratigraphicScheme& scheme);
bool hasErrors(const std::vector<FrameworkIssue>& issues);
std::vector<const StratigraphicSubInterval*> subIntervalsOf(const StratigraphicScheme& scheme, const IntervalId& interval);
const StratigraphicSubInterval* findSubInterval(const StratigraphicScheme& scheme, const SubIntervalId& id);
const StratigraphicInterval* findInterval(const StratigraphicScheme& scheme, const IntervalId& id);
const StratigraphicBoundary* findBoundary(const StratigraphicScheme& scheme, const BoundaryId& id);
// 兼容稳定 ID 与人类可读层位名（如 "T70-T72-S1"、"T70–T72/S1"、"T70-T72"）：
const StratigraphicInterval* resolveInterval(const StratigraphicScheme& scheme, const std::string& idOrName);
const StratigraphicSubInterval* resolveSubInterval(const StratigraphicScheme& scheme, const std::string& idOrName);
std::string displayName(const StratigraphicScheme& scheme, const IntervalId& id);
std::string displayName(const StratigraphicScheme& scheme, const SubIntervalId& id);
WellSubIntervalRange wellRange(const StratigraphicScheme& scheme, long long wellId, const SubIntervalId& id);
// 设置某井内部三个分界（解释约束）；拒绝逆序、越出父单元、重叠。
bool setWellInternalBoundaries(StratigraphicScheme& scheme, long long wellId, const IntervalId& interval,
                               const std::array<double, 3>& depths, const std::string& status, std::string& error);
// 逐道顶/底时间（与地震时间轴同单位同零点）→ 子层段掩膜。
// 只用有效顶底之间的样点；缺失/倒置/越界道置 2（待复核），绝不按全体固定时间四等分。
SeismicSubIntervalMask seismicMask(const std::vector<double>& topTime, const std::vector<double>& bottomTime,
                                   double sampleStart, double sampleInterval, std::size_t sampleCount,
                                   const SubIntervalDivision& division, int index);
// 两修订之间受影响的子层段（界面、分界、井出现变化）。
std::vector<SubIntervalId> impactedSubIntervals(const StratigraphicScheme& before, const StratigraphicScheme& after);
// 稳定缓存键片段：SubIntervalId + 影响该层段几何的内容指纹。
std::string subIntervalFingerprint(const StratigraphicScheme& scheme, const SubIntervalId& id);
// Direct MD consumers depend on this well's effective bounds, not other wells or seismic resource bindings.
std::string wellSubIntervalFingerprint(const StratigraphicScheme& scheme, const SubIntervalId& id, long long wellId);
// Time-window amplitude consumers depend on surface resource IDs and the two effective fractional edges, not MD occurrences.
std::string seismicSubIntervalFingerprint(const StratigraphicScheme& scheme, const SubIntervalId& id);
// well_facies + positive wellId and seismic_facies select the above contracts.
// Other/legacy kinds retain subIntervalFingerprint; aggregate combines the parent's children.
std::string resultGeometryFingerprint(const StratigraphicScheme& scheme, const SubIntervalResultBinding& binding);
// 历史单界面/显示名标签 → 候选关联。只有显式 "顶–底/Sk" 或 "顶–底" 且界面相邻时给候选，
// 单个界面名（如 "T70"）一律 legacy_unresolved，绝不默认归入 S1。
LegacyUnitResolution resolveLegacyLabel(const StratigraphicScheme& scheme, const std::string& label);
}

}
