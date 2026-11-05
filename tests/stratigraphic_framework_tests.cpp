// 统一地层层段体系契约测试：合成工区 T70/T71/T72 → 2 个父单元 → 8 个子层段。
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
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
    std::string invalidated;
    for (const auto& b : all) if (b.kind == "single_factor") invalidated = b.id;
    CHECK(!invalidated.empty());
    CHECK(!ds.invalidateSubIntervalResult({}, err));
    CHECK(ds.invalidateSubIntervalResult(invalidated, err));
    CHECK(ds.invalidateSubIntervalResult(invalidated, err)); // idempotent, preserves identity/history
    ds.closeProject();
    // 只读工程：可读不可写
    DataService ro; CHECK(ro.openProjectReadOnly(path, err));
    StratigraphicScheme roHead; CHECK(ro.readStratigraphicScheme(head.id, {}, roHead, err));
    CHECK(!ro.commitStratigraphicScheme(roHead, roHead.revision.value, next, issues2, err));
    SubIntervalResultBinding rb, ro2; rb.kind = "map"; rb.schemeId = head.id; rb.subInterval = subs[0]->id;
    CHECK(!ro.bindSubIntervalResult(rb, ro2, err));
    CHECK(!ro.invalidateSubIntervalResult(invalidated, err));
    CHECK(ro.listSubIntervalResults(head.id, {}, all, err) && all.size() == 5);
    bool retained = false;
    for (const auto& b : all) if (b.id == invalidated) retained = b.status == "stale";
    CHECK(retained);
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

static void consumerScopedInvalidation()
{
    QTemporaryDir dir;std::string error;DataService service;
    CHECK(service.createProject(dir.filePath("consumer-model").toStdString(),"consumer invalidation",error));
    StratigraphicScheme original;std::vector<FrameworkIssue> issues;
    CHECK(service.commitStratigraphicScheme(synthetic(),{},original,issues,error));
    const auto subs=fw::subIntervalsOf(original,original.intervals.front().id);
    const auto bind=[&](const std::string& id,const std::string& kind,long long well,int index,const StratigraphicScheme& scheme) {
        SubIntervalResultBinding binding,stored;binding.id=id;binding.kind=kind;binding.wellId=well;binding.schemeId=scheme.id;
        binding.schemeRevision=scheme.revision;binding.subInterval=subs[size_t(index-1)]->id;binding.assetRef="frozen:"+id;
        CHECK(service.bindSubIntervalResult(binding,stored,error));
    };
    bind("well-1","well_facies",1,2,original);bind("well-2","well_facies",2,2,original);
    bind("seismic-2","seismic_facies",0,2,original);bind("seismic-4","seismic_facies",0,4,original);
    bind("legacy-global","single_factor",0,2,original);
    auto other=synthetic();other.id="other-scheme";StratigraphicScheme otherHead;
    CHECK(service.commitStratigraphicScheme(other,{},otherHead,issues,error));bind("other-result","seismic_facies",0,2,otherHead);
    const auto status=[&](const std::string& scheme,const std::string& id) {
        std::vector<SubIntervalResultBinding> rows;CHECK(service.listSubIntervalResults(scheme,{},rows,error));
        const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.id==id;});
        CHECK(found!=rows.end());return found==rows.end()?std::string():found->status;
    };
    auto changed=original;
    for(auto& occurrence:changed.wellOccurrences)if(occurrence.wellId==1&&occurrence.interval==changed.intervals.front().id)
        occurrence.internal[1]=1060;
    CHECK(fw::wellSubIntervalFingerprint(original,subs[1]->id,1)!=fw::wellSubIntervalFingerprint(changed,subs[1]->id,1));
    CHECK(fw::wellSubIntervalFingerprint(original,subs[1]->id,2)==fw::wellSubIntervalFingerprint(changed,subs[1]->id,2));
    CHECK(fw::seismicSubIntervalFingerprint(original,subs[1]->id)==fw::seismicSubIntervalFingerprint(changed,subs[1]->id));
    StratigraphicScheme wellHead;CHECK(service.commitStratigraphicScheme(changed,original.revision.value,wellHead,issues,error));
    CHECK(status(original.id,"well-1")=="stale"&&status(original.id,"well-2")=="current");
    CHECK(status(original.id,"seismic-2")=="current"&&status(original.id,"legacy-global")=="stale");
    auto rebound=wellHead;
    for(auto& boundary:rebound.boundaries)if(boundary.id==rebound.intervals.front().top)boundary.seismicHorizonResourceId=987;
    CHECK(fw::subIntervalFingerprint(wellHead,subs[1]->id)==fw::subIntervalFingerprint(rebound,subs[1]->id));
    CHECK(fw::seismicSubIntervalFingerprint(wellHead,subs[1]->id)!=fw::seismicSubIntervalFingerprint(rebound,subs[1]->id));
    std::vector<SubIntervalId> affected;std::vector<SubIntervalResultBinding> outputs;
    CHECK(service.previewStratigraphicImpact(rebound,affected,outputs,error));CHECK(affected.size()==4&&outputs.size()==2);
    StratigraphicScheme surfaceHead;CHECK(service.commitStratigraphicScheme(rebound,wellHead.revision.value,surfaceHead,issues,error));
    CHECK(status(original.id,"well-2")=="current"&&status(original.id,"seismic-2")=="stale");
    CHECK(status(otherHead.id,"other-result")=="current");
    bind("current-s2","seismic_facies",0,2,surfaceHead);bind("current-s4","seismic_facies",0,4,surfaceHead);
    bind("explicit-well-1","well_facies",1,2,surfaceHead);
    auto divided=surfaceHead;divided.divisions.front().fractions={.25,.6,.75};
    StratigraphicScheme divisionHead;CHECK(service.commitStratigraphicScheme(divided,surfaceHead.revision.value,divisionHead,issues,error));
    CHECK(status(original.id,"current-s2")=="stale"&&status(original.id,"current-s4")=="current");
    CHECK(status(original.id,"explicit-well-1")=="current"); // its explicit MD edge overrides the changed fraction.
    StratigraphicScheme historical;CHECK(service.readStratigraphicScheme(original.id,original.revision.value,historical,error));
    CHECK(fw::wellSubIntervalFingerprint(original,subs[1]->id,1)==fw::wellSubIntervalFingerprint(historical,subs[1]->id,1));
    CHECK(fw::seismicSubIntervalFingerprint(original,subs[1]->id)==fw::seismicSubIntervalFingerprint(historical,subs[1]->id));
    service.closeProject();CHECK(service.openProjectReadOnly(dir.filePath("consumer-model").toStdString(),error));
    CHECK(status(original.id,"current-s2")=="stale"&&status(original.id,"well-2")=="stale");
    CHECK(status(original.id,"current-s4")=="current"&&status(otherHead.id,"other-result")=="current");
}

static void writeSegyFixture(const QString& path)
{
    auto u16 = [](QByteArray& bytes, int offset, quint16 value) { qToBigEndian(value, bytes.data() + offset); };
    auto u32 = [](QByteArray& bytes, int offset, quint32 value) { qToBigEndian(value, bytes.data() + offset); };
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    QByteArray text(3200, ' ');
    const QByteArray label("C01 SEISMIC HORIZON WINDOW TEST");
    std::memcpy(text.data(), label.constData(), size_t(label.size()));
    file.write(text);
    // 200 samples, dt = 2000 us (2 ms), firstTime = 1000 ms -> 1000..1398 ms
    QByteArray binary(400, '\0');
    u16(binary, 16, 2000);
    u16(binary, 20, 200);
    u16(binary, 24, 5); // IEEE float32
    u16(binary, 300, 0x100);
    u16(binary, 302, 1);
    file.write(binary);
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            QByteArray head(240, '\0');
            u32(head, 0, quint32(i * 2 + j + 1));
            u16(head, 70, 1);
            u16(head, 88, 1);
            u16(head, 108, 1000); // delay time 1000 ms
            u16(head, 114, 200);
            u16(head, 116, 2000);
            u32(head, 180, quint32(500000 + j * 100));
            u32(head, 184, quint32(4000000 + i * 100));
            u32(head, 188, quint32(100 + i * 2));
            u32(head, 192, quint32(200 + j * 2));
            file.write(head);
            QByteArray samples(200 * 4, '\0');
            for (int s = 0; s < 200; ++s) {
                const float amp = float((i + 1) * 100.0 + (j + 1) * 10.0 + s * 0.5);
                quint32 bits = 0;
                std::memcpy(&bits, &amp, 4);
                u32(samples, s * 4, bits);
            }
            file.write(samples);
        }
    }
}

static void seismicStratigraphyAndQueryApis()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    const std::string projPath = dir.path().toStdString() + "/hy_area";
    std::string err;
    DataService ds;
    CHECK(ds.createProject(projPath, "HY井震联合测试工区", err));

    // 1. 准备含 3 个倾斜地震层位 (T70, T72, T80)、带负 TWT 的时深关系、测井曲线 (GR, DT) 与地质分层道 (组/段) 的数据包
    ParsedFile parsed;
    parsed.kind = "synthetic";parsed.status="parsed";
    for (auto [hname, baseTwt] : {std::pair{"T80", 1320.0}, {"T70", 1040.0}, {"T72", 1160.0}}) {
        SeismicHorizon h;
        h.horizonName = hname;
        h.coordinateSystem = "EPSG:32650";
        h.coordinateUnit = "m";
        h.timeUnit = "ms";
        h.timeType = "TWT";
        h.timeReference = "fixture-datum";
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                // 随 (i, j) 倾斜，使每个空间点拥有不同的层位时间与深度范围
                const double dipMs = i * 16.0 + j * 8.0;
                h.points.push_back({100 + i * 2, 200 + j * 2, 500000.0 + j * 100.0, 4000000.0 + i * 100.0, baseTwt + dipMs});
            }
        }
        parsed.horizons.push_back(std::move(h));
    }

    WellDataset well;
    well.wellInfo.wellName = "HZ19-1-1";
    well.wellInfo.wellheadX = 500000.0;
    well.wellInfo.wellheadY = 4000000.0;
    well.wellInfo.coordinateSystem = "EPSG:32650";
    well.wellInfo.coordinateUnit = "m";
    well.defaultDepthInfo.depthType = "MD";
    well.defaultDepthInfo.depthUnit = "m";
    well.defaultDepthInfo.mdReference = "KB";

    // Petrel 风格负值 TWT 时深关系：1000 ms -> 1500 m, 1400 ms -> 2300 m（斜率 2.0 m/ms）
    TimeDepthTable td;
    td.relationName = "HZ19-1-1_TDR";
    td.depthInfo.depthType = "MD";
    td.depthInfo.depthUnit = "m";
    td.depthInfo.mdReference = "KB";
    td.timeUnit = "ms";
    td.timeType = "TWT";
    td.timeReference = "fixture-datum";
    td.timeSign = "negated";
    td.depthTimePairs = {
        {1500.0, -1000.0, 500000.0, 4000000.0, -1500.0},
        {1900.0, -1200.0, 500000.0, 4000000.0, -1900.0},
        {2300.0, -1400.0, 500000.0, 4000000.0, -2300.0}
    };
    well.timeDepthRelations.push_back(std::move(td));

    // 测井曲线组：1500..2200 m，步长 2 m
    LogSet ls;
    ls.logSetName = "常规测井";
    ls.depthInfo.depthType = "MD";
    ls.depthInfo.depthUnit = "m";
    ls.depthInfo.mdReference = "KB";
    ls.samplingAxis.sampleStep = 2.0;
    LogCurve gr{"GR", "API", {}, "自然伽马"}, dt{"DT", "us/ft", {}, "声波时差"};
    for (double d = 1500.0; d <= 2200.0; d += 2.0) {
        ls.samplingAxis.depthValues.push_back(d);
        gr.sampleValues.push_back(40.0 + 0.05 * (d - 1500.0));
        dt.sampleValues.push_back(95.0 - 0.02 * (d - 1500.0));
    }
    ls.logCurves.push_back(std::move(gr));
    ls.logCurves.push_back(std::move(dt));
    well.wellbore.logSets.push_back(std::move(ls));

    // 常规地质分层道（段：文三段 1560..1840 m）
    IntervalTrack segTrack;
    segTrack.trackName = "段";
    segTrack.trackType = "地层";
    segTrack.depthInfo.depthType = "MD";
    segTrack.depthInfo.depthUnit = "m";
    segTrack.depthInfo.mdReference = "KB";
    GeologicalInterval segIv;
    segIv.intervalName = "文三段";
    segIv.topDepth = 1560.0;
    segIv.bottomDepth = 1840.0;
    segTrack.intervals.push_back(segIv);
    well.wellbore.intervalTracks.push_back(std::move(segTrack));
    parsed.wells.push_back(std::move(well));

    const QString dummySrc = dir.path() + "/synthetic.xml";
    { QFile f(dummySrc); CHECK(f.open(QIODevice::WriteOnly)); f.write("synthetic"); }
    SourceEntry src;
    long long srcId = 0;
    // 用 importFile 导入 SEG-Y，并写入井/层位/时深
    const QString segyPath = dir.path() + "/cube.sgy";
    writeSegyFixture(segyPath);
    ImportOptions impOpts; impOpts.copySources = false;
    ImportReport impRep;
    CHECK(ds.importFile(segyPath.toUtf8().toStdString(), impOpts, impRep, err));
    std::vector<DataEntry> volumeEntries;CHECK(ds.listData("volumes",0,volumeEntries,err)&&volumeEntries.size()==1);
    SeismicVolume volume;CHECK(ds.readVolume(volumeEntries.front().id,volume,err));
    volume.sampling.timeType="TWT";volume.sampling.timeReference="fixture-datum";
    CHECK(ds.saveVolume(volumeEntries.front().id,volume,err));

    // 借用内部 ProjectStore 保存合成井与层位（或通过 SQL/importPreparedFile）
    // 计算 dummySrc 的 SHA-256 以调用 importPreparedFile
    const bool imported=ds.importPreparedFile(dummySrc.toUtf8().toStdString(), dummySrc.toUtf8().toStdString(),
                                dummySrc.toUtf8().toStdString(),
                                QCryptographicHash::hash(QByteArray("synthetic"),QCryptographicHash::Sha256).toHex().toStdString(),
                                parsed, srcId, err);
    CHECK(imported);if(!imported){std::cerr<<"framework fixture import: "<<err<<"\n";return;}

    // 2. 调用 buildSeismicStratigraphicFramework 自动建立井震统一地层方案与四等分小层
    SeismicStratigraphyBuildOptions buildOpts;
    SeismicStratigraphyBuildReport buildRep;
    CHECK(ds.buildSeismicStratigraphicFramework(buildOpts, buildRep, err));
    CHECK(buildRep.horizonCount == 3);
    CHECK((buildRep.orderedHorizonNames == std::vector<std::string>{"T70", "T72", "T80"}));
    CHECK(buildRep.intervalCount == 2);
    CHECK(buildRep.subIntervalCount == 8);
    CHECK(buildRep.matchedWellCount == 1);

    // 验证井上已生成 '地震层位'（T70-T72, T72-T80）与 '地震小层'（T70-T72-S1..S4, T72-T80-S1..S4）
    std::vector<WellSummary> wells;
    CHECK(ds.listWells(wells, err) && wells.size() == 1);
    const long long wellId = wells.front().id;
    WellPlotData plotData;
    CHECK(ds.readWellPlotData(wellId, {}, plotData, err));
    bool foundSeismicIvTrack = false, foundSeismicSubTrack = false;
    for (const auto& trk : plotData.intervals) {
        if (trk.value.trackName == "地震层位") {
            foundSeismicIvTrack = true;
            CHECK(trk.value.intervals.size() == 2);
            // T70 (1040 ms -> 1580 m), T72 (1160 ms -> 1820 m)
            CHECK(trk.value.intervals[0].intervalName == "T70-T72");
            CHECK(near(trk.value.intervals[0].topDepth, 1580.0));
            CHECK(near(trk.value.intervals[0].bottomDepth, 1820.0));
        } else if (trk.value.trackName == "地震小层") {
            foundSeismicSubTrack = true;
            CHECK(trk.value.intervals.size() == 8);
            // T70-T72 (1580..1820 m, 厚 240 m) 四等分每小层 60 m：S1=1580..1640, S2=1640..1700
            CHECK(trk.value.intervals[0].intervalName == "T70-T72-S1");
            CHECK(near(trk.value.intervals[0].topDepth, 1580.0) && near(trk.value.intervals[0].bottomDepth, 1640.0));
            CHECK(trk.value.intervals[1].intervalName == "T70-T72-S2");
            CHECK(near(trk.value.intervals[1].topDepth, 1640.0) && near(trk.value.intervals[1].bottomDepth, 1700.0));
        }
    }
    CHECK(foundSeismicIvTrack && foundSeismicSubTrack);

    // 3. 验证 listAvailableStratigraphicTracks 与 queryWellLogsByStratigraphy（双轨：地震小层道 + 常规地质分层道）
    std::vector<StratigraphicTrackDescriptor> availTracks;
    CHECK(ds.listAvailableStratigraphicTracks(availTracks, err));
    CHECK(availTracks.size() >= 3);

    // 3a. 查询地震小层 T70-T72-S2（1640..1700 m）
    StratigraphicScheme builtScheme;CHECK(ds.readStratigraphicScheme(buildRep.schemeId,buildRep.revision.value,builtScheme,err));
    const auto* selectedSub=fw::resolveSubInterval(builtScheme,"T70-T72-S2");CHECK(selectedSub);if(!selectedSub)return;
    WellIntervalLogQuery logQ;
    logQ.stratigraphy.source = StratigraphicTrackSource::SeismicFramework;
    logQ.stratigraphy.subIntervalId = selectedSub->id;
    logQ.curveNames = {"GR", "DT"};
    logQ.normalizedSampleCount = 16;
    std::vector<WellStratigraphicLogSlice> logSlices;
    CHECK(ds.queryWellLogsByStratigraphy(logQ, logSlices, err) && logSlices.size() == 1);
    CHECK(logSlices[0].resolved);
    CHECK(logSlices[0].unitName == "T70-T72-S2");
    CHECK(near(logSlices[0].topDepth, 1640.0) && near(logSlices[0].bottomDepth, 1700.0));
    CHECK(near(logSlices[0].topTimeMs, 1070.0) && near(logSlices[0].bottomTimeMs, 1100.0));
    CHECK(logSlices[0].featureTable.columns.size() == 2);
    CHECK(!logSlices[0].featureTable.depths.empty());
    CHECK(near(logSlices[0].featureTable.depths.front(), 1640.0));
    CHECK(near(logSlices[0].featureTable.depths.back(), 1698.0));
    CHECK(logSlices[0].normalizedFeatureTable.depths.size() == 16);

    // 3b. 查询其他地层道：段 / 文三段（1560..1840 m）
    WellIntervalLogQuery geoQ;
    geoQ.stratigraphy.source = StratigraphicTrackSource::WellIntervalTrack;
    geoQ.stratigraphy.trackName = "段";
    geoQ.stratigraphy.unitName = "文三段";
    geoQ.curveNames = {"GR"};
    std::vector<WellStratigraphicLogSlice> geoSlices;
    CHECK(ds.queryWellLogsByStratigraphy(geoQ, geoSlices, err) && geoSlices.size() == 1);
    CHECK(geoSlices[0].resolved && geoSlices[0].trackName == "段" && geoSlices[0].unitName == "文三段");
    CHECK(near(geoSlices[0].topDepth, 1560.0) && near(geoSlices[0].bottomDepth, 1840.0));
    CHECK(geoSlices[0].featureTable.columns.size() == 1);

    // 4. 验证 readSeismicHorizonWindow：每个空间点具有不同的时间窗与深度窗
    SeismicHorizonWindowQuery winQ;
    winQ.undeclaredTimeUnit="ms";winQ.undeclaredTimeType="TWT";
    winQ.target.subIntervalId = selectedSub->id; // S2 = [0.25, 0.50], actual persisted ID.
    winQ.domainKind = HorizonSpatialDomainKind::FullHorizonGrid;
    winQ.sampleMode = HorizonWindowSampleMode::ResampledFixedCount;
    winQ.resampledSampleCount = 12;
    winQ.computeWindowAttributes = true;
    winQ.computeDepthRange = true;
    std::vector<DataEntry> timeDepths;CHECK(ds.listData("time_depth",wellId,timeDepths,err)&&timeDepths.size()==1);
    winQ.timeDepthBindings={{wellId,timeDepths.front().id}};
    SeismicHorizonWindowResult winRes;
    CHECK(ds.readSeismicHorizonWindow(winQ, winRes, err));
    CHECK(winRes.validTraceCount == 4);
    CHECK(winRes.traces.size() == 4);
    CHECK(winRes.amplitudes.size() == 4 * 12);
    // 点 0 (IL=100, XL=200): T70=1040, T72=1160 -> S2=[1070, 1100] ms -> 深度 [1640, 1700] m
    // 点 3 (IL=102, XL=202): T70=1064, T72=1184 -> S2=[1094, 1124] ms -> 深度 [1688, 1748] m
    CHECK(near(winRes.traces[0].windowTopTimeMs, 1070.0) && near(winRes.traces[0].windowBottomTimeMs, 1100.0));
    CHECK(near(winRes.traces[3].windowTopTimeMs, 1094.0) && near(winRes.traces[3].windowBottomTimeMs, 1124.0));
    CHECK(winRes.traces[0].windowTopDepthM < winRes.traces[3].windowTopDepthM);
    CHECK(std::isfinite(winRes.traces[0].rmsAmplitude) && winRes.traces[0].rmsAmplitude > 0.0f);
}

static void strictSelectionAndBudgets()
{
    QTemporaryDir temp;CHECK(temp.isValid());std::string error;DataService ds;
    CHECK(ds.createProject(temp.filePath("model").toUtf8().toStdString(),"strict queries",error));
    const auto source=temp.filePath("synthetic.txt");const QByteArray bytes="strict-stratigraphy-fixture";
    {QFile file(source);CHECK(file.open(QIODevice::WriteOnly));CHECK(file.write(bytes)==bytes.size());}
    ParsedFile parsed;parsed.kind="test";parsed.status="parsed";WellDataset well;well.wellInfo.wellName="A";
    well.wellInfo.wellheadX=500000;well.wellInfo.wellheadY=4000000;well.wellInfo.coordinateSystem="EPSG:32650";well.wellInfo.coordinateUnit="m";
    well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit="m";well.defaultDepthInfo.mdReference="KB";
    TimeDepthTable td;td.relationName="checks";td.depthInfo=well.defaultDepthInfo;td.timeUnit="ms";td.timeType="TWT";td.timeReference="fixture";
    td.depthTimePairs={{100,0,500000,4000000},{200,16,500000,4000000}};well.timeDepthRelations.push_back(td);
    LogSet logs;logs.logSetName="logs";logs.depthInfo=well.defaultDepthInfo;logs.samplingAxis.depthValues={100,150,200};
    LogCurve curve;curve.curveName="GR";curve.curveUnit="API";curve.sampleValues={10,20,30};logs.logCurves.push_back(curve);well.wellbore.logSets.push_back(logs);
    auto bareDepth=well.defaultDepthInfo;bareDepth.mdReference.clear();auto bareLogs=logs;bareLogs.logSetName="bare-md-logs";bareLogs.depthInfo=bareDepth;
    well.wellbore.logSets.push_back(bareLogs);
    const auto track=[&](const std::string& name,const std::string& unit,DepthInfo depth,double top,double bottom) {
        IntervalTrack result;result.trackName=name;result.trackType="stratigraphy";result.depthInfo=depth;
        GeologicalInterval interval;interval.intervalName=unit;interval.topDepth=top;interval.bottomDepth=bottom;result.intervals.push_back(interval);
        return result;
    };
    well.wellbore.intervalTracks.push_back(track("地震小层","USER",well.defaultDepthInfo,100,150));
    well.wellbore.intervalTracks.push_back(track("ordinary","T1-T2-S1",well.defaultDepthInfo,100,200));
    well.wellbore.intervalTracks.push_back(track("bare-md","BARE",bareDepth,100,200));
    well.wellbore.intervalTracks.push_back(track("unknown","UNKNOWN",{},100,200));
    auto feet=well.defaultDepthInfo;feet.depthUnit="ft";well.wellbore.intervalTracks.push_back(track("feet","FEET",feet,100,200));
    auto tvd=well.defaultDepthInfo;tvd.depthType="TVD";tvd.tvdReference="KB";well.wellbore.intervalTracks.push_back(track("tvd","TVD",tvd,100,200));
    auto other=well.defaultDepthInfo;other.mdReference="OTHER";well.wellbore.intervalTracks.push_back(track("other","OTHER",other,100,200));
    well.wellbore.intervalTracks.push_back(track("duplicate","DUP",well.defaultDepthInfo,100,150));
    well.wellbore.intervalTracks.push_back(track("duplicate","DUP",well.defaultDepthInfo,150,200));
    auto repeated=track("repeated","REPEAT",well.defaultDepthInfo,100,125);
    repeated.intervals.push_back(repeated.intervals.front());repeated.intervals.back().topDepth=150;repeated.intervals.back().bottomDepth=175;
    well.wellbore.intervalTracks.push_back(repeated);
    auto auxOrder=track("aux-order","LOWER",well.defaultDepthInfo,150,175);
    auxOrder.intervals.push_back(auxOrder.intervals.front());auxOrder.intervals.back().intervalName="UPPER";
    auxOrder.intervals.back().topDepth=100;auxOrder.intervals.back().bottomDepth=125;
    well.wellbore.intervalTracks.push_back(auxOrder);parsed.wells.push_back(well);
    for(int i=0;i<2;++i){SeismicHorizon h;h.horizonName=i==0?"T1":"T2";h.coordinateSystem="EPSG:32650";h.coordinateUnit="m";
        h.timeUnit="ms";h.timeType="TWT";h.timeReference="fixture";h.points.push_back({100,200,500000,4000000,double(2+i*4)});parsed.horizons.push_back(h);}
    long long sourceId=0;
    const bool imported=ds.importPreparedFile(source.toUtf8().toStdString(),source.toUtf8().toStdString(),source.toUtf8().toStdString(),
        QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().toStdString(),parsed,sourceId,error);
    CHECK(imported);if(!imported){std::cerr<<error<<'\n';return;}
    std::vector<WellSummary> wells;CHECK(ds.listWells(wells,error)&&wells.size()==1);if(wells.empty())return;const auto wellId=wells.front().id;
    std::vector<DataEntry> before;CHECK(ds.listData("intervals",wellId,before,error));
    SeismicStratigraphyBuildOptions options;options.schemeId="scheme-A";SeismicStratigraphyBuildReport a,b;
    CHECK(ds.buildSeismicStratigraphicFramework(options,a,error));CHECK(a.matchedWellCount==1);
    std::vector<DataEntry> afterA;CHECK(ds.listData("intervals",wellId,afterA,error));
    for(const auto& original:before)CHECK(std::any_of(afterA.begin(),afterA.end(),[&](const auto& entry){return entry.id==original.id;}));
    options.schemeId="scheme-B";CHECK(ds.buildSeismicStratigraphicFramework(options,b,error));
    std::vector<DataEntry> afterB;CHECK(ds.listData("intervals",wellId,afterB,error));
    for(const auto& original:afterA)CHECK(std::any_of(afterB.begin(),afterB.end(),[&](const auto& entry){return entry.id==original.id;}));
    options.schemeId="scheme-A";SeismicStratigraphyBuildReport nextA;CHECK(ds.buildSeismicStratigraphicFramework(options,nextA,error));
    CHECK(nextA.revision!=a.revision);std::vector<DataEntry> afterNext;CHECK(ds.listData("intervals",wellId,afterNext,error));
    for(const auto& original:afterB)CHECK(std::any_of(afterNext.begin(),afterNext.end(),[&](const auto& entry){return entry.id==original.id;}));
    StratigraphicScheme old;CHECK(ds.readStratigraphicScheme("scheme-A",a.revision.value,old,error));
    const auto faultConnection="stratigraphy_fault_"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto faultDb=QSqlDatabase::addDatabase("QSQLITE",faultConnection);faultDb.setDatabaseName(temp.filePath("model/data.sqlite"));CHECK(faultDb.open());
        QSqlQuery fault(faultDb);
        CHECK(fault.exec("CREATE TRIGGER reject_framework_track BEFORE INSERT ON tracks WHEN NEW.source_id IS NULL BEGIN SELECT RAISE(ABORT,'injected materialization failure'); END"));
        fault.finish();SeismicStratigraphyBuildReport failed;
        CHECK(!ds.buildSeismicStratigraphicFramework(options,failed,error));CHECK(failed.schemeId.empty()&&failed.revision.empty());
        StratigraphicScheme preserved;CHECK(ds.readStratigraphicScheme("scheme-A",{},preserved,error));CHECK(preserved.revision==nextA.revision);
        std::vector<DataEntry> preservedTracks;CHECK(ds.listData("intervals",wellId,preservedTracks,error));CHECK(preservedTracks.size()==afterNext.size());
        for(const auto& original:afterNext)CHECK(std::any_of(preservedTracks.begin(),preservedTracks.end(),[&](const auto& entry){return entry.id==original.id;}));
        CHECK(fault.exec("DROP TRIGGER reject_framework_track"));
    }
    QSqlDatabase::removeDatabase(faultConnection);
    // Independent source declarations must remain independent, including missing
    // MD/time datums. A preferred second relation must never borrow the first's.
    ParsedFile additional;additional.kind="test";additional.status="parsed";WellDataset tdWell;
    tdWell.wellInfo=well.wellInfo;tdWell.defaultDepthInfo=well.defaultDepthInfo;
    auto missingMd=td;missingMd.relationName="missing-md-reference";missingMd.depthInfo.mdReference.clear();
    auto missingTime=td;missingTime.relationName="missing-time-reference";missingTime.timeReference.clear();
    tdWell.timeDepthRelations={missingMd,missingTime};
    tdWell.wellbore.intervalTracks.push_back(track("independent-auxiliary","AUX",well.defaultDepthInfo,100,200));
    additional.wells.push_back(tdWell);
    const auto tdSource=temp.filePath("independent-time-depth.txt");const QByteArray tdBytes="independent-time-depth-fixture";
    {QFile file(tdSource);CHECK(file.open(QIODevice::WriteOnly));CHECK(file.write(tdBytes)==tdBytes.size());}
    long long tdSourceId=0;CHECK(ds.importPreparedFile(tdSource.toUtf8().toStdString(),tdSource.toUtf8().toStdString(),tdSource.toUtf8().toStdString(),
        QCryptographicHash::hash(tdBytes,QCryptographicHash::Sha256).toHex().toStdString(),additional,tdSourceId,error));
    std::vector<DataEntry> tdEntries;CHECK(ds.listData("time_depth",wellId,tdEntries,error));
    const auto relationId=[&](const std::string& name){const auto it=std::find_if(tdEntries.begin(),tdEntries.end(),[&](const auto& entry){return entry.name==name;});return it==tdEntries.end()?0LL:it->id;};
    const auto prefer=[&](long long id){return ds.saveCorrelationTimeDepthPreferences({{wellId,id,"{\"basis\":\"explicit fixture selection\"}"}},error);};
    CHECK(relationId("checks")>0&&relationId("missing-md-reference")>0&&relationId("missing-time-reference")>0);
    CHECK(prefer(relationId("checks")));
    std::vector<WellStratigraphicLogSlice> results;
    WellIntervalLogQuery query;query.wellIds={wellId};query.includeAuxiliaryIntervals=false;
    query.stratigraphy.source=StratigraphicTrackSource::WellIntervalTrack;query.stratigraphy.trackName="ordinary";query.stratigraphy.unitName="T1-T2-S1";
    query.stratigraphy.schemeId="scheme-A";query.stratigraphy.subIntervalId={"T1-T2-S1"};query.stratigraphy.intervalId={"T1-T2"};
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.size()==1);
    if(results.empty())return;CHECK(results.front().resolved&&near(results.front().topDepth,100)&&near(results.front().bottomDepth,200));
    CHECK(results.front().featureTable.depths.size()==3);CHECK(results.front().trackId>0);
    CHECK(near(results.front().topTimeMs,0)&&near(results.front().bottomTimeMs,16));
    for(const auto* name:{"missing-md-reference","missing-time-reference"}) {
        CHECK(prefer(relationId(name)));CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.front().resolved);
        CHECK(results.front().featureTable.depths.size()==3&&std::isnan(results.front().topTimeMs)&&std::isnan(results.front().bottomTimeMs));
    }
    CHECK(prefer(relationId("checks")));
    const auto ordinaryId=results.front().trackId;
    query.stratigraphy.trackName="bare-md";query.stratigraphy.unitName="BARE";
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.front().resolved&&results.front().featureTable.depths.size()==3);
    CHECK(std::isnan(results.front().topTimeMs)&&std::isnan(results.front().bottomTimeMs));
    query.stratigraphy.trackName="ordinary";query.stratigraphy.unitName="T1-T2-S1";
    query.stratigraphy.source=StratigraphicTrackSource::SeismicFramework;query.stratigraphy.revision="missing-revision";
    CHECK(!ds.queryWellLogsByStratigraphy(query,results,error));CHECK(results.empty());
    query.stratigraphy.revision=a.revision.value;
    const auto* actualSub=fw::resolveSubInterval(old,"T1-T2-S1");CHECK(actualSub);if(!actualSub)return;
    const auto subDisplay=fw::displayName(old,actualSub->id),parentDisplay=fw::displayName(old,actualSub->interval);
    CHECK(subDisplay!=actualSub->id.value&&parentDisplay!=actualSub->interval.value);
    query.stratigraphy.subIntervalId={subDisplay};
    CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty()); // a different display form is not a typed ID.
    query.stratigraphy.subIntervalId={"missing-subinterval"};query.stratigraphy.intervalId=actualSub->interval;
    CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty()); // no invalid-sub -> valid-parent fallback.
    query.stratigraphy.subIntervalId=actualSub->id;query.stratigraphy.intervalId={"conflicting-parent"};
    CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty());
    query.stratigraphy.subIntervalId={};query.stratigraphy.intervalId={parentDisplay};
    CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty());
    query.stratigraphy.subIntervalId=actualSub->id;query.stratigraphy.intervalId=actualSub->interval;
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.size()==1);
    CHECK(results.front().resolved&&near(results.front().topDepth,112.5)&&near(results.front().bottomDepth,118.75));CHECK(results.front().schemeRevision==a.revision.value);
    query.stratigraphy.subIntervalId={};query.stratigraphy.intervalId={};
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.front().resolved); // unambiguous display-name compatibility.
    StratigraphicScheme ambiguous;ambiguous.id="scheme-label-ambiguity";
    for(const auto& [name,order]:{std::pair{"A",1},std::pair{"B-C",2},std::pair{"A-B",3},std::pair{"C",4}}) {
        StratigraphicBoundary boundary;boundary.name=name;boundary.order=order;ambiguous.boundaries.push_back(boundary);
    }
    StratigraphicScheme committedAmbiguous;std::vector<FrameworkIssue> ambiguityIssues;
    CHECK(ds.commitStratigraphicScheme(ambiguous,{},committedAmbiguous,ambiguityIssues,error));
    query.stratigraphy.schemeId=ambiguous.id;query.stratigraphy.revision=committedAmbiguous.revision.value;query.stratigraphy.unitName="A-B-C";
    CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty());
    query.stratigraphy.schemeId="scheme-A";
    query.stratigraphy.source=StratigraphicTrackSource::WellIntervalTrack;query.stratigraphy.revision.clear();
    for(auto names:{std::pair{"unknown","UNKNOWN"},std::pair{"feet","FEET"},std::pair{"tvd","TVD"},std::pair{"other","OTHER"},std::pair{"duplicate","DUP"}}) {
        query.stratigraphy.trackName=names.first;query.stratigraphy.unitName=names.second;
        CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.size()==1);CHECK(!results.front().resolved);CHECK(results.front().featureTable.depths.empty());
        if(std::string(names.first)=="unknown")CHECK(results.front().depthType.empty()&&results.front().depthUnit.empty());
    }
    const auto duplicate=std::find_if(before.begin(),before.end(),[](const auto& entry){return entry.name=="duplicate";});CHECK(duplicate!=before.end());if(duplicate==before.end())return;
    query.stratigraphy.trackId=duplicate->id;query.stratigraphy.trackName="renamed-display";
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.front().resolved);CHECK(results.front().trackId==duplicate->id);
    query.stratigraphy.trackId=0;query.stratigraphy.trackName="repeated";query.stratigraphy.unitName="REPEAT";
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&!results.front().resolved);
    const auto repeatedEntry=std::find_if(before.begin(),before.end(),[](const auto& entry){return entry.name=="repeated";});CHECK(repeatedEntry!=before.end());if(repeatedEntry==before.end())return;
    std::vector<IntervalRecord> records;CHECK(ds.readIntervals(repeatedEntry->id,Missing,Missing,records,error)&&records.size()==2);if(records.empty())return;
    query.stratigraphy.intervalRecordId=records.front().id;query.stratigraphy.unitName.clear();
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.front().resolved);CHECK(near(results.front().bottomDepth,125));
    query.stratigraphy.intervalRecordId=0;query.stratigraphy.trackId=ordinaryId;query.stratigraphy.unitName="T1-T2-S1";
    query.resampleStepMeters=1e-300;CHECK(!ds.queryWellLogsByStratigraphy(query,results,error));CHECK(results.empty());
    query.resampleStepMeters=0;query.normalizedSampleCount=std::numeric_limits<int>::max();CHECK(!ds.queryWellLogsByStratigraphy(query,results,error));CHECK(results.empty());
    query.normalizedSampleCount=0;query.maxValues=2;CHECK(!ds.queryWellLogsByStratigraphy(query,results,error));CHECK(results.empty());
    query.maxValues=8000000;query.resampleStepMeters=1;query.maxSamplesPerWell=2;CHECK(!ds.queryWellLogsByStratigraphy(query,results,error));CHECK(results.empty());
    query.resampleStepMeters=25;query.maxSamplesPerWell=100;query.normalizedSampleCount=4;query.maxValues=12;
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.front().resolved);
    CHECK((results.front().featureTable.depths==std::vector<double>{100,125,150,175,200}));CHECK(results.front().normalizedFeatureTable.depths.size()==4);
    CHECK(results.front().normalizedFeatureTable.depthUnit=="1");
    query.maxValues=11;CHECK(!ds.queryWellLogsByStratigraphy(query,results,error));CHECK(results.empty());
    query.maxValues=8000000;int cancellationChecks=0;query.cancelled=[&]{return ++cancellationChecks>1;};
    CHECK(!ds.queryWellLogsByStratigraphy(query,results,error));CHECK(results.empty());query.cancelled={};
    query.resampleStepMeters=0;query.normalizedSampleCount=0;query.includeAuxiliaryIntervals=true;
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.front().resolved);
    std::vector<long long> auxiliaryIds;for(const auto& group:results.front().auxiliaryTracks)auxiliaryIds.push_back(group.trackId);
    CHECK(!auxiliaryIds.empty()&&std::is_sorted(auxiliaryIds.begin(),auxiliaryIds.end()));
    std::vector<IntervalTrackResult> expectedAuxiliary;CHECK(ds.compareIntervals(auxiliaryIds,100,200,expectedAuxiliary,error));
    CHECK(expectedAuxiliary.size()==results.front().auxiliaryTracks.size());
    for(size_t i=0;i<expectedAuxiliary.size()&&i<results.front().auxiliaryTracks.size();++i) {
        const auto& expected=expectedAuxiliary[i];const auto& actual=results.front().auxiliaryTracks[i];
        CHECK(expected.name==actual.name&&expected.trackId==actual.trackId&&expected.wellId==actual.wellId);
        CHECK(expected.intervals.size()==actual.intervals.size());
        for(size_t j=0;j<expected.intervals.size()&&j<actual.intervals.size();++j) {
            CHECK(expected.intervals[j].id==actual.intervals[j].id&&expected.intervals[j].value.intervalName==actual.intervals[j].value.intervalName);
        }
    }
    query.maxAuxiliaryTracks=1;CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty());query.maxAuxiliaryTracks=64;
    query.maxAuxiliaryRecords=1;CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty());query.maxAuxiliaryRecords=100000;
    query.maxAuxiliaryBytes=1;CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty());query.maxAuxiliaryBytes=16777216;
    const auto metadataConnection="stratigraphy_metadata_"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto metadataDb=QSqlDatabase::addDatabase("QSQLITE",metadataConnection);metadataDb.setDatabaseName(temp.filePath("model/data.sqlite"));CHECK(metadataDb.open());
        QSqlQuery metadata(metadataDb);CHECK(metadata.prepare("SELECT options FROM sources WHERE id=?"));metadata.addBindValue(tdSourceId);CHECK(metadata.exec()&&metadata.next());
        const auto originalOptions=metadata.value(0);metadata.finish();
        CHECK(metadata.prepare("UPDATE sources SET options=? WHERE id=?"));
        metadata.addBindValue(QString("{\"padding\":\"")+QString(262144,'x')+"\"}");metadata.addBindValue(tdSourceId);CHECK(metadata.exec());metadata.finish();
        query.maxAuxiliaryBytes=131072;
        CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty()&&error.find("辅助分层")!=std::string::npos);
        CHECK(metadata.prepare("UPDATE sources SET options=? WHERE id=?"));metadata.addBindValue(originalOptions);metadata.addBindValue(tdSourceId);CHECK(metadata.exec());metadata.finish();
        query.maxAuxiliaryBytes=16777216;
    }
    QSqlDatabase::removeDatabase(metadataConnection);
    query.curveNames={"NO-MATCH"};cancellationChecks=0;query.cancelled=[&]{return ++cancellationChecks==4;};
    CHECK(!ds.queryWellLogsByStratigraphy(query,results,error)&&results.empty()&&cancellationChecks==4);query.cancelled={};query.curveNames.clear();
    query.includeAuxiliaryIntervals=false;
    CHECK(ds.verifySource(sourceId,error));ds.closeProject();CHECK(ds.openProjectReadOnly(temp.filePath("model").toUtf8().toStdString(),error));
    query.resampleStepMeters=0;query.normalizedSampleCount=0;
    CHECK(ds.queryWellLogsByStratigraphy(query,results,error)&&results.front().resolved&&results.front().trackId==ordinaryId);
    QFile file(source);CHECK(file.open(QIODevice::ReadOnly)&&file.readAll()==bytes);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    pureModel(); seismicMask(); persistence(); legacyProjectWithoutFramework();
    consumerScopedInvalidation();
    seismicStratigraphyAndQueryApis();
    strictSelectionAndBudgets();
    if (failures) { std::cerr << failures << " failures\n"; return 1; }
    std::cout << "stratigraphic_framework_tests passed\n";
    return 0;
}
