// 统一地层层段体系契约测试：合成工区 T70/T71/T72 → 2 个父单元 → 8 个子层段。
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace paleo::data;
namespace fw = paleo::data::framework;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } } while (0)
static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

static StratigraphicScheme synthetic()
{
    StratigraphicScheme s;
    s.id = "scheme-a"; s.name = "合成方案";
    for (auto [name, order] : {std::pair{"T71", 2}, {"T70", 1}, {"T72", 3}}) { // 输入乱序：顺序只由 order 决定
        StratigraphicBoundary b; b.name = name; b.order = order; b.kind = "horizon"; s.boundaries.push_back(b);
    }
    fw::deriveUnits(s);
    // 两口井 T70/T71/T72 深度不同
    const auto& i1 = s.intervals.at(0).id; const auto& i2 = s.intervals.at(1).id;
    s.wellOccurrences.push_back({1, i1, "MD", "m", 1000, 1100});
    s.wellOccurrences.push_back({1, i2, "MD", "m", 1100, 1180});
    s.wellOccurrences.push_back({2, i1, "MD", "m", 1250, 1290});
    WellIntervalOccurrence np{2, i2, "MD", "m", 1290, Missing}; np.status = "not_penetrated";
    s.wellOccurrences.push_back(np);
    return s;
}

static void pureModel()
{
    auto s = synthetic();
    CHECK(s.boundaries.size() == 3);
    CHECK(s.intervals.size() == 2);       // 三个界面 ≠ 三个单元
    CHECK(s.subIntervals.size() == 8);
    CHECK(fw::validate(s).empty());
    CHECK(fw::displayName(s, s.intervals[0].id) == "T70\xE2\x80\x93T71");
    const auto subs = fw::subIntervalsOf(s, s.intervals[0].id);
    CHECK(subs.size() == 4);
    for (int k = 0; k < 4; ++k) CHECK(subs[k]->index == k + 1);
    CHECK(fw::displayName(s, subs[1]->id) == "T70\xE2\x80\x93T71/S2");
    // 类型区别：BoundaryId 与 IntervalId 不同类型（编译期）；ID 不是显示名
    CHECK(s.intervals[0].id.value != "T70" && s.intervals[0].id.value.find("T70") == std::string::npos);
    // 稳定：重新派生 ID 不变
    auto again = s; fw::deriveUnits(again);
    CHECK(again.subIntervals[5].id == s.subIntervals[5].id);
    // 初始等比、provisional；无重叠无间隙
    double prevBottom = 1000;
    for (int k = 0; k < 4; ++k) {
        auto r = fw::wellRange(s, 1, subs[k]->id);
        CHECK(r.resolved && r.provisional);
        CHECK(near(r.top, prevBottom)); CHECK(near(r.bottom - r.top, 25)); prevBottom = r.bottom;
    }
    CHECK(near(prevBottom, 1100));
    // 多井：身份一致、深度各异
    auto w2 = fw::wellRange(s, 2, subs[1]->id);
    CHECK(w2.resolved && near(w2.top, 1260) && near(w2.bottom, 1270));
    // 未钻穿：不补造
    auto s2 = fw::subIntervalsOf(s, s.intervals[1].id);
    auto miss = fw::wellRange(s, 2, s2[0]->id);
    CHECK(!miss.resolved && std::isnan(miss.top));
    // 解释约束不等厚
    std::string err;
    CHECK(fw::setWellInternalBoundaries(s, 1, s.intervals[0].id, {1010, 1060, 1065}, "confirmed", err));
    auto u = fw::wellRange(s, 1, subs[2]->id);
    CHECK(u.resolved && !u.provisional && near(u.top, 1060) && near(u.bottom, 1065));
    CHECK(!fw::setWellInternalBoundaries(s, 1, s.intervals[0].id, {1050, 1040, 1065}, "confirmed", err)); // 逆序
    CHECK(!fw::setWellInternalBoundaries(s, 1, s.intervals[0].id, {990, 1040, 1065}, "confirmed", err));  // 越界
    CHECK(fw::validate(s).empty());
    // 倒置、重叠、尖灭
    auto bad = s; bad.wellOccurrences[0].top = 1200;
    CHECK(fw::hasErrors(fw::validate(bad)));
    auto ov = s; ov.wellOccurrences[1].top = 1090;
    bool overlap = false; for (auto& i : fw::validate(ov)) overlap |= i.code == "overlap"; CHECK(overlap);
    auto pinch = s; pinch.wellOccurrences[2].bottom = 1250; pinch.wellOccurrences[2].status = "pinched_out";
    CHECK(!fw::hasErrors(fw::validate(pinch)));
    CHECK(!fw::wellRange(pinch, 2, subs[0]->id).resolved);
    auto gap = s; gap.wellOccurrences[1].top = 1105;
    bool g = false; for (auto& i : fw::validate(gap)) g |= i.code == "gap"; CHECK(g);
    gap.wellOccurrences[1].status = "faulted"; g = false; for (auto& i : fw::validate(gap)) g |= i.code == "gap"; CHECK(!g);
    // 顺序冲突 / 同名
    auto dup = synthetic(); dup.boundaries[0].order = 1; CHECK(fw::hasErrors(fw::validate(dup)));
    // 影响：只改 S2/S3 分界 → 只有 T70–T71 的 S2、S3 受影响
    auto before = synthetic(); auto after = before;
    after.divisions[0].fractions = {0.25, 0.6, 0.75}; after.divisions[0].mode = "interpreted";
    auto hit = fw::impactedSubIntervals(before, after);
    std::set<int> idx; for (auto& id : hit) { auto* x = fw::findSubInterval(after, id); CHECK(x->interval == after.intervals[0].id); idx.insert(x->index); }
    CHECK((idx == std::set<int>{2, 3}));
    CHECK(fw::subIntervalFingerprint(before, before.subIntervals[0].id) == fw::subIntervalFingerprint(after, after.subIntervals[0].id));
    // 历史标签：单界面不归入 S1
    auto l1 = fw::resolveLegacyLabel(before, "T70");
    CHECK(l1.status == "legacy_unresolved" && l1.subInterval.empty());
    auto l2 = fw::resolveLegacyLabel(before, "T70-T71/S2");
    CHECK(l2.status == "resolved_candidate" && l2.subInterval == fw::subIntervalsOf(before, before.intervals[0].id)[1]->id);
    CHECK(fw::resolveLegacyLabel(before, "T70-T72").status == "legacy_unresolved"); // E12 跨两个单元，不相邻
    CHECK(fw::resolveLegacyLabel(before, "D61").status == "legacy_unresolved");
}

static void seismicMask()
{
    SubIntervalDivision d;
    // 4 道：正常、倾斜、缺失顶、倒置；时间轴 0..196 ms，dt 4
    std::vector<double> top{100, 60, NAN, 150}, bottom{140, 180, 140, 120};
    std::size_t total = 0;
    std::vector<std::uint8_t> owner(4 * 50, 0);
    for (int n = 1; n <= 4; ++n) {
        auto m = fw::seismicMask(top, bottom, 0, 4, 50, d, n);
        CHECK(m.reviewTraceCount == 2);
        total += m.insideCount;
        for (std::size_t i = 0; i < m.values.size(); ++i) if (m.values[i] == 1) { CHECK(owner[i] == 0); owner[i] = std::uint8_t(n); }
        for (std::size_t k = 0; k < 50; ++k) { CHECK(m.values[2 * 50 + k] == 2); CHECK(m.values[3 * 50 + k] == 2); }
        for (std::size_t k = 0; k < 50; ++k) { const double t = 4.0 * k; if (t < 100 || t > 140) CHECK(m.values[k] == 0); }
    }
    // 道 0：100..140 共 11 个样点；道 1：60..180 共 31 个样点；无重叠无遗漏
    CHECK(total == 11 + 31);
    // 第二道 S1 = [60,90) 而不是全体固定时间四等分
    auto s1 = fw::seismicMask(top, bottom, 0, 4, 50, d, 1);
    CHECK(s1.values[50 + 15] == 1 && s1.values[50 + 23] == 0 && s1.values[50 + 22] == 1);
}

static void persistence()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    const std::string path = dir.path().toStdString() + "/area";
    std::string err;
    StratigraphicScheme committed; std::vector<FrameworkIssue> issues;
    {
        DataService ds; CHECK(ds.createProject(path, "合成工区", err));
        std::vector<StratigraphicSchemeSummary> none; CHECK(ds.listStratigraphicSchemes(none, err) && none.empty()); // 新/旧工程无方案
        StratigraphicScheme r; CHECK(!ds.readStratigraphicScheme("x", {}, r, err));
        auto bad = synthetic(); bad.wellOccurrences[0].top = 1500;
        CHECK(!ds.commitStratigraphicScheme(bad, {}, committed, issues, err)); // 失败不留半套数据
        CHECK(ds.listStratigraphicSchemes(none, err) && none.empty());
        CHECK(ds.commitStratigraphicScheme(synthetic(), {}, committed, issues, err));
        CHECK(!committed.revision.empty() && committed.subIntervals.size() == 8);
        // 成果登记：S2 单井相 + S1 单因素 + 父单元综合 + legacy
        auto subs = fw::subIntervalsOf(committed, committed.intervals[0].id);
        SubIntervalResultBinding b, out;
        b.kind = "well_facies"; b.schemeId = committed.id; b.subInterval = subs[1]->id; b.wellId = 1; b.assetRef = "asset:wf1";
        CHECK(ds.bindSubIntervalResult(b, out, err) && out.interval == committed.intervals[0].id);
        b.kind = "single_factor"; b.subInterval = subs[0]->id; b.wellId = 0; CHECK(ds.bindSubIntervalResult(b, out, err));
        b.kind = "map"; b.subInterval = fw::subIntervalsOf(committed, committed.intervals[1].id)[1]->id; CHECK(ds.bindSubIntervalResult(b, out, err));
        SubIntervalResultBinding agg; agg.kind = "composite"; agg.schemeId = committed.id; agg.interval = committed.intervals[0].id; agg.aggregate = true;
        CHECK(ds.bindSubIntervalResult(agg, out, err));
        SubIntervalResultBinding legacy; legacy.kind = "map"; legacy.schemeId = committed.id; legacy.legacyLabel = "T70";
        CHECK(ds.bindSubIntervalResult(legacy, out, err) && out.status == "legacy_unresolved");
        SubIntervalResultBinding none2; none2.kind = "map"; none2.schemeId = committed.id;
        CHECK(!ds.bindSubIntervalResult(none2, out, err)); // 新任务必须选有效子层段
        none2.subInterval.value = "T70"; CHECK(!ds.bindSubIntervalResult(none2, out, err)); // 显示名不是身份
    }
    // 关闭重开：身份不丢
    DataService ds; CHECK(ds.openProject(path, err));
    StratigraphicScheme head; CHECK(ds.readStratigraphicScheme(committed.id, {}, head, err));
    CHECK(head.revision == committed.revision);
    for (std::size_t k = 0; k < 8; ++k) CHECK(head.subIntervals[k].id == committed.subIntervals[k].id);
    auto subs = fw::subIntervalsOf(head, head.intervals[0].id);
    WellSubIntervalRange range; CHECK(ds.readWellSubIntervalRange(head.id, {}, 2, subs[1]->id, range, err) && range.resolved && near(range.top, 1260));
    // 预演影响
    auto edit = head; edit.divisions[0].fractions = {0.25, 0.6, 0.75};
    std::vector<SubIntervalId> hit; std::vector<SubIntervalResultBinding> hitResults;
    CHECK(ds.previewStratigraphicImpact(edit, hit, hitResults, err) && hit.size() == 2);
    CHECK(hitResults.size() == 2); // S2 单井相 + 父单元综合
    // 版本冲突
    StratigraphicScheme next; std::vector<FrameworkIssue> issues2;
    CHECK(!ds.commitStratigraphicScheme(edit, "stale-revision", next, issues2, err));
    CHECK(ds.commitStratigraphicScheme(edit, head.revision.value, next, issues2, err));
    std::vector<SubIntervalResultBinding> all; CHECK(ds.listSubIntervalResults(head.id, {}, all, err) && all.size() == 5);
    int stale = 0, current = 0, legacyCount = 0;
    for (auto& b : all) { stale += b.status == "stale"; current += b.status == "current"; legacyCount += b.status == "legacy_unresolved"; }
    CHECK(stale == 2 && current == 2 && legacyCount == 1); // 无关层段（S1、T71–T72/S2）不清空
    std::vector<SubIntervalResultBinding> s2; CHECK(ds.listSubIntervalResults(head.id, subs[1]->id, s2, err) && s2.size() == 1);
    // 回退：生成新头，内容等于历史修订，不混用
    StratigraphicScheme restored; CHECK(ds.restoreStratigraphicSchemeRevision(head.id, head.revision.value, next.revision.value, restored, err));
    CHECK(restored.parentRevision == next.revision && near(restored.divisions[0].fractions[1], 0.5));
    StratigraphicScheme old; CHECK(ds.readStratigraphicScheme(head.id, next.revision.value, old, err) && near(old.divisions[0].fractions[1], 0.6));
    std::vector<StratigraphicSchemeRevisionSummary> revs; CHECK(ds.listStratigraphicSchemeRevisions(head.id, revs, err) && revs.size() == 3);
    ds.closeProject();
    // 只读工程：可读不可写
    DataService ro; CHECK(ro.openProjectReadOnly(path, err));
    StratigraphicScheme roHead; CHECK(ro.readStratigraphicScheme(head.id, {}, roHead, err));
    CHECK(!ro.commitStratigraphicScheme(roHead, roHead.revision.value, next, issues2, err));
    SubIntervalResultBinding rb, ro2; rb.kind = "map"; rb.schemeId = head.id; rb.subInterval = subs[0]->id;
    CHECK(!ro.bindSubIntervalResult(rb, ro2, err));
}

static void legacyProjectWithoutFramework()
{
    QTemporaryDir dir; std::string err;
    const std::string path = dir.path().toStdString() + "/old";
    { DataService ds; CHECK(ds.createProject(path, "旧工程", err)); }
    DataService ro; CHECK(ro.openProjectReadOnly(path, err));
    std::vector<StratigraphicSchemeSummary> list; CHECK(ro.listStratigraphicSchemes(list, err) && list.empty());
    std::vector<SubIntervalResultBinding> r; CHECK(ro.listSubIntervalResults("x", {}, r, err) && r.empty());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    pureModel(); seismicMask(); persistence(); legacyProjectWithoutFramework();
    if (failures) { std::cerr << failures << " failures\n"; return 1; }
    std::cout << "stratigraphic_framework_tests passed\n";
    return 0;
}
