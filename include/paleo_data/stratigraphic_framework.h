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
// 历史单界面/显示名标签 → 候选关联。只有显式 "顶–底/Sk" 或 "顶–底" 且界面相邻时给候选，
// 单个界面名（如 "T70"）一律 legacy_unresolved，绝不默认归入 S1。
LegacyUnitResolution resolveLegacyLabel(const StratigraphicScheme& scheme, const std::string& label);
}

}
