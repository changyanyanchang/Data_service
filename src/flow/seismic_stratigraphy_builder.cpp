// 井震统一地层框架构建与按地层/小层查询测井道服务：
// 1. 导入期由地震层位 + 时深关系自动建立父层段（T70-T72）及四等分小层（T70-T72-S1..S4），并在井上落库地层道；
// 2. 提供按地震小层/层位或常规地质分层道提取测井曲线与辅助层段道的统一查询接口。
#include "paleo_data/service.h"
#include "store/project_store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace paleo::data {
using namespace detail;
namespace fw = framework;

namespace {

struct HorizonGroup {
    long long primaryId = 0;
    std::vector<long long> allIds;
    std::string name;
    std::string timeUnit;
    double meanTimeMs = Missing;
    long long pointCount = 0;
    std::vector<SeismicHorizonPoint> localPoints; // 井包围盒附近的层位点缓存
};

struct NormalizedTdPoint {
    double md = Missing;
    double twtMs = Missing;
    double x = Missing;
    double y = Missing;
};

struct WellTdCurve {
    long long wellId = 0;
    std::string wellName;
    double wellheadX = Missing;
    double wellheadY = Missing;
    std::vector<NormalizedTdPoint> points; // 按 md 严格递增排序
};

bool isFinite(double v) { return std::isfinite(v); }

// 线性插值：由 MD 求 TWT(ms)
double interpolateTwtAtMd(const WellTdCurve& td, double md)
{
    if (td.points.empty() || !isFinite(md)) return Missing;
    if (td.points.size() == 1) {
        return std::abs(md - td.points.front().md) <= 10.0 ? td.points.front().twtMs : Missing;
    }
    if (md < td.points.front().md) {
        const double span = td.points.back().md - td.points.front().md;
        if (td.points.front().md - md > std::max(50.0, span * 0.05)) return Missing;
        const auto& a = td.points[0];
        const auto& b = td.points[1];
        const double slope = (b.twtMs - a.twtMs) / (b.md - a.md);
        return a.twtMs + (md - a.md) * slope;
    }
    if (md > td.points.back().md) {
        const double span = td.points.back().md - td.points.front().md;
        if (md - td.points.back().md > std::max(50.0, span * 0.05)) return Missing;
        const auto& a = td.points[td.points.size() - 2];
        const auto& b = td.points.back();
        const double slope = (b.twtMs - a.twtMs) / (b.md - a.md);
        return b.twtMs + (md - b.md) * slope;
    }
    auto it = std::lower_bound(td.points.begin(), td.points.end(), md,
                               [](const NormalizedTdPoint& p, double d) { return p.md < d; });
    if (it == td.points.begin()) return it->twtMs;
    if (it == td.points.end()) return td.points.back().twtMs;
    const auto& hi = *it;
    const auto& lo = *(it - 1);
    const double denom = hi.md - lo.md;
    if (denom <= 1e-9) return lo.twtMs;
    const double t = (md - lo.md) / denom;
    return lo.twtMs + t * (hi.twtMs - lo.twtMs);
}

// 线性插值：由 TWT(ms) 求 MD(m)
double interpolateMdAtTwt(const WellTdCurve& td, double twtMs)
{
    if (td.points.empty() || !isFinite(twtMs)) return Missing;
    if (td.points.size() == 1) {
        return std::abs(twtMs - td.points.front().twtMs) <= 15.0 ? td.points.front().md : Missing;
    }
    const double minTwt = td.points.front().twtMs;
    const double maxTwt = td.points.back().twtMs;
    const double spanTwt = std::max(1.0, maxTwt - minTwt);
    // 允许在时深表首尾做小幅合理线性外推（例如浅部 T30 或底部 Tg 略超首末点 80 ms 内）
    const double marginMs = std::max(80.0, spanTwt * 0.06);
    if (twtMs < minTwt - marginMs || twtMs > maxTwt + marginMs) return Missing;
    if (twtMs <= minTwt) {
        const auto& a = td.points[0];
        const auto& b = td.points[1];
        const double dt = b.twtMs - a.twtMs;
        if (dt <= 1e-9) return a.md;
        const double md = a.md + (twtMs - a.twtMs) * (b.md - a.md) / dt;
        return md > 0.0 ? md : Missing;
    }
    if (twtMs >= maxTwt) {
        const auto& a = td.points[td.points.size() - 2];
        const auto& b = td.points.back();
        const double dt = b.twtMs - a.twtMs;
        if (dt <= 1e-9) return b.md;
        return b.md + (twtMs - b.twtMs) * (b.md - a.md) / dt;
    }
    auto it = std::lower_bound(td.points.begin(), td.points.end(), twtMs,
                               [](const NormalizedTdPoint& p, double t) { return p.twtMs < t; });
    if (it == td.points.begin()) return it->md;
    if (it == td.points.end()) return td.points.back().md;
    const auto& hi = *it;
    const auto& lo = *(it - 1);
    const double dt = hi.twtMs - lo.twtMs;
    if (dt <= 1e-9) return lo.md;
    const double f = (twtMs - lo.twtMs) / dt;
    return lo.md + f * (hi.md - lo.md);
}

// 在给定平面坐标 (x, y) 处对层位网格进行反距离加权 (IDW) 插值获取正 TWT(ms)
double sampleHorizonTwtAtXY(const HorizonGroup& h, double x, double y, double radiusMeters)
{
    if (h.localPoints.empty()) return Missing;
    if (!isFinite(x) || !isFinite(y)) {
        // 仅当全部层位点也无平面坐标（合成测试场景）时退回层位均值
        bool anyXY = false;
        for (const auto& p : h.localPoints) {
            if (isFinite(p.x) && isFinite(p.y)) { anyXY = true; break; }
        }
        return anyXY ? Missing : h.meanTimeMs;
    }
    const double r2Max = radiusMeters * radiusMeters;
    const double timeFactor = (h.timeUnit == "s" || h.timeUnit == "S") ? 1000.0 : 1.0;

    // 收集半径内最近的若干个点（最多取前 8 个最近邻做 IDW）
    struct NearPt { double d2; double twt; };
    std::vector<NearPt> nearPts;
    nearPts.reserve(16);
    for (const auto& p : h.localPoints) {
        if (!isFinite(p.x) || !isFinite(p.y) || !isFinite(p.zTime)) continue;
        const double dx = p.x - x;
        const double dy = p.y - y;
        const double d2 = dx * dx + dy * dy;
        if (d2 > r2Max) continue;
        const double twt = std::abs(p.zTime) * timeFactor;
        if (d2 <= 1.0) return twt; // 1m 内直接命中网格点
        nearPts.push_back({d2, twt});
    }
    if (nearPts.empty()) return Missing;
    if (nearPts.size() > 8) {
        std::nth_element(nearPts.begin(), nearPts.begin() + 8, nearPts.end(),
                         [](const NearPt& a, const NearPt& b) { return a.d2 < b.d2; });
        nearPts.resize(8);
    }
    double wSum = 0.0, vSum = 0.0;
    for (const auto& np : nearPts) {
        const double w = 1.0 / std::max(1e-6, np.d2);
        wSum += w;
        vSum += w * np.twt;
    }
    return wSum > 0.0 ? (vSum / wSum) : Missing;
}

// 求井轨迹（支持斜井随 MD 变化的 XY 或直井固定井口 XY）与地震层位面的交点 (MD, TWT)
bool intersectWellWithHorizon(const WellTdCurve& td, const HorizonGroup& h, double radiusMeters,
                              double& outMd, double& outTwtMs)
{
    outMd = Missing;
    outTwtMs = Missing;
    if (td.points.empty()) return false;

    // 检查该井时深点是否自带随深度变化的 3D 轨迹 (X, Y)
    bool hasTrajectoryXY = false;
    for (const auto& pt : td.points) {
        if (isFinite(pt.x) && isFinite(pt.y)) { hasTrajectoryXY = true; break; }
    }
    if (!hasTrajectoryXY) {
        const double twt = sampleHorizonTwtAtXY(h, td.wellheadX, td.wellheadY, radiusMeters);
        if (!isFinite(twt)) return false;
        const double md = interpolateMdAtTwt(td, twt);
        if (!isFinite(md)) return false;
        outMd = md;
        outTwtMs = twt;
        return true;
    }

    // 针对斜井轨迹：先用首末有效 XY 估算层位时间，再用轨迹点迭代收敛到 (X(MD), Y(MD)) 处的精确层位时间
    double refX = td.wellheadX, refY = td.wellheadY;
    for (const auto& pt : td.points) {
        if (isFinite(pt.x) && isFinite(pt.y)) { refX = pt.x; refY = pt.y; break; }
    }
    double twtGuess = sampleHorizonTwtAtXY(h, refX, refY, radiusMeters);
    if (!isFinite(twtGuess)) {
        // 尝试轨迹中点或末点
        const auto& mid = td.points[td.points.size() / 2];
        if (isFinite(mid.x) && isFinite(mid.y))
            twtGuess = sampleHorizonTwtAtXY(h, mid.x, mid.y, radiusMeters);
    }
    if (!isFinite(twtGuess)) return false;

    double mdGuess = interpolateMdAtTwt(td, twtGuess);
    if (!isFinite(mdGuess)) return false;

    // 2 次不动点迭代：由 mdGuess 插值轨迹 (x, y) -> 重取层位 TWT -> 重算 MD
    for (int iter = 0; iter < 2; ++iter) {
        auto it = std::lower_bound(td.points.begin(), td.points.end(), mdGuess,
                                   [](const NormalizedTdPoint& p, double d) { return p.md < d; });
        double xAtMd = refX, yAtMd = refY;
        if (it == td.points.begin()) {
            if (isFinite(it->x) && isFinite(it->y)) { xAtMd = it->x; yAtMd = it->y; }
        } else if (it == td.points.end()) {
            const auto& bk = td.points.back();
            if (isFinite(bk.x) && isFinite(bk.y)) { xAtMd = bk.x; yAtMd = bk.y; }
        } else {
            const auto& hi = *it;
            const auto& lo = *(it - 1);
            if (isFinite(lo.x) && isFinite(lo.y) && isFinite(hi.x) && isFinite(hi.y) && hi.md > lo.md) {
                const double f = (mdGuess - lo.md) / (hi.md - lo.md);
                xAtMd = lo.x + f * (hi.x - lo.x);
                yAtMd = lo.y + f * (hi.y - lo.y);
            } else if (isFinite(lo.x) && isFinite(lo.y)) {
                xAtMd = lo.x; yAtMd = lo.y;
            }
        }
        const double refinedTwt = sampleHorizonTwtAtXY(h, xAtMd, yAtMd, radiusMeters);
        if (!isFinite(refinedTwt)) break;
        const double refinedMd = interpolateMdAtTwt(td, refinedTwt);
        if (!isFinite(refinedMd)) break;
        twtGuess = refinedTwt;
        mdGuess = refinedMd;
    }
    outMd = mdGuess;
    outTwtMs = twtGuess;
    return true;
}

NormalizedTdPoint normalizeRawPoint(const TimeDepthPoint& raw, double depthScale, double timeScale, bool negateTime)
{
    NormalizedTdPoint pt;
    pt.md = isFinite(raw.depthValue) ? raw.depthValue * depthScale : Missing;
    if (isFinite(raw.timeValue)) {
        const double signedTwt = negateTime ? -raw.timeValue : raw.timeValue;
        pt.twtMs = signedTwt * timeScale;
    }
    pt.x = raw.x;
    pt.y = raw.y;
    return pt;
}

bool extractWellTdCurve(const DataService& ds, const ProjectStore& st, long long wellId,
                        const std::string& wellName, double wellheadX, double wellheadY,
                        long long preferredTdId, bool autoDetectSign, WellTdCurve& out)
{
    out = {};
    out.wellId = wellId;
    out.wellName = wellName;
    out.wellheadX = wellheadX;
    out.wellheadY = wellheadY;

    std::string err;
    QSqlQuery qTd;
    if (!st.exec("SELECT id FROM well_objects WHERE well_id=? AND kind='time_depth' ORDER BY id",
                 {wellId}, err, &qTd))
        return false;
    std::vector<long long> candidateIds;
    while (qTd.next()) candidateIds.push_back(qTd.value(0).toLongLong());

    // 若该井名下暂无已归井时深表，检查未归井时深表是否按井名匹配
    if (candidateIds.empty() && !wellName.empty()) {
        QSqlQuery qUnassigned;
        if (st.exec("SELECT id, name FROM well_objects WHERE (well_id IS NULL OR well_id<=0) AND kind='time_depth' ORDER BY id",
                    {}, err, &qUnassigned)) {
            while (qUnassigned.next()) {
                const QString rName = qUnassigned.value(1).toString().trimmed();
                if (rName.compare(q(wellName).trimmed(), Qt::CaseInsensitive) == 0)
                    candidateIds.push_back(qUnassigned.value(0).toLongLong());
            }
        }
    }
    if (candidateIds.empty()) return false;

    // 优先使用首选时深表，否则选择有效点最多的时深表
    TimeDepthTable bestTable;
    size_t bestValidCount = 0;
    for (long long tdId : candidateIds) {
        TimeDepthTable t;
        if (!ds.readTimeDepth(tdId, t, err)) continue;
        size_t validCount = 0;
        for (const auto& p : t.depthTimePairs) {
            if (isFinite(p.depthValue) && isFinite(p.timeValue)) ++validCount;
        }
        if (tdId == preferredTdId && validCount >= 2) {
            bestTable = std::move(t);
            bestValidCount = validCount;
            break;
        }
        if (validCount > bestValidCount) {
            bestValidCount = validCount;
            bestTable = std::move(t);
        }
    }
    if (bestValidCount < 2) return false;

    const double depthScale = (bestTable.depthInfo.depthUnit == "ft" || bestTable.depthInfo.depthUnit == "FT") ? 0.3048 : 1.0;
    int negCount = 0, posCount = 0;
    double maxAbsTime = 0.0;
    for (const auto& p : bestTable.depthTimePairs) {
        if (!isFinite(p.depthValue) || !isFinite(p.timeValue)) continue;
        if (p.timeValue < -1e-6) ++negCount;
        else if (p.timeValue > 1e-6) ++posCount;
        maxAbsTime = std::max(maxAbsTime, std::abs(p.timeValue));
    }
    // 识别时间单位（ms vs s）与正负号约定（Petrel 导出海拔以下常为负值）
    double timeScale = 1.0;
    if (bestTable.timeUnit == "s" || bestTable.timeUnit == "S" || (bestTable.timeUnit.empty() && maxAbsTime > 0.0 && maxAbsTime <= 15.0))
        timeScale = 1000.0;
    const bool negateTime = autoDetectSign && (negCount > posCount);
    const double owtFactor = (bestTable.timeType == "OWT" || bestTable.timeType == "owt") ? 2.0 : 1.0;

    std::vector<NormalizedTdPoint> rawPts;
    rawPts.reserve(bestTable.depthTimePairs.size());
    for (const auto& p : bestTable.depthTimePairs) {
        auto np = normalizeRawPoint(p, depthScale, timeScale * owtFactor, negateTime);
        if (isFinite(np.md) && isFinite(np.twtMs) && np.md >= 0.0 && np.twtMs >= 0.0)
            rawPts.push_back(np);
    }
    if (rawPts.size() < 2) return false;

    std::sort(rawPts.begin(), rawPts.end(),
              [](const NormalizedTdPoint& a, const NormalizedTdPoint& b) { return a.md < b.md; });

    // 过滤重复深度与非单调噪声点，保证 (md, twtMs) 严格递增
    for (const auto& pt : rawPts) {
        if (out.points.empty()) {
            out.points.push_back(pt);
            continue;
        }
        auto& prev = out.points.back();
        if (std::abs(pt.md - prev.md) <= 1e-4) {
            prev.twtMs = 0.5 * (prev.twtMs + pt.twtMs);
            if (!isFinite(prev.x) && isFinite(pt.x)) { prev.x = pt.x; prev.y = pt.y; }
            continue;
        }
        if (pt.twtMs > prev.twtMs + 1e-5) {
            out.points.push_back(pt);
        }
    }
    // 若井口无坐标但时深表首点含 XY，则补充为井参考 XY
    if ((!isFinite(out.wellheadX) || !isFinite(out.wellheadY)) && !out.points.empty()) {
        for (const auto& pt : out.points) {
            if (isFinite(pt.x) && isFinite(pt.y)) {
                out.wellheadX = pt.x;
                out.wellheadY = pt.y;
                break;
            }
        }
    }
    return out.points.size() >= 2;
}

} // namespace

bool DataService::buildSeismicStratigraphicFramework(const SeismicStratigraphyBuildOptions& options,
                                                     SeismicStratigraphyBuildReport& report,
                                                     std::string& error,
                                                     const CancelCheck& cancelled)
{
    error.clear();
    report = {};
    auto& st = impl_->store;
    if (!st.db.isOpen()) { error = "请先打开工区"; return false; }
    if (st.readOnly) { error = "只读工区不能构建或写入地震地层框架"; return false; }

    auto isCancelled = [&]() {
        if (cancelled && cancelled()) { error = "已取消地震地层框架构建"; return true; }
        return false;
    };
    if (isCancelled()) return false;

    // 1. 读取工区全部地震层位并按平均双程时（浅→深）排序
    QSqlQuery qHor;
    if (!st.exec("SELECT h.id, h.name, h.time_unit, "
                 "AVG(CASE WHEN p.z IS NOT NULL THEN ABS(p.z) END), "
                 "COUNT(p.id) "
                 "FROM horizons h LEFT JOIN horizon_points p ON p.horizon_id = h.id "
                 "GROUP BY h.id ORDER BY h.id",
                 {}, error, &qHor))
        return false;

    std::map<std::string, HorizonGroup> byName;
    while (qHor.next()) {
        const long long hid = qHor.value(0).toLongLong();
        const std::string hname = s(qHor.value(1).toString().trimmed());
        const std::string tunit = s(qHor.value(2).toString().trimmed());
        const double avgAbsZ = value(qHor.value(3));
        const long long cnt = qHor.value(4).toLongLong();
        if (hname.empty() || cnt <= 0 || !isFinite(avgAbsZ)) continue;

        const double factor = (tunit == "s" || tunit == "S") ? 1000.0 : 1.0;
        const double meanMs = avgAbsZ * factor;
        auto& grp = byName[hname];
        grp.name = hname;
        grp.allIds.push_back(hid);
        if (cnt > grp.pointCount) {
            grp.primaryId = hid;
            grp.timeUnit = tunit;
            grp.meanTimeMs = meanMs;
            grp.pointCount = cnt;
        }
    }

    if (byName.size() < 2) {
        error = "工区有效地震层位少于 2 个，无法建立相邻层间单元与四等分小层";
        return false;
    }

    std::vector<HorizonGroup> orderedHorizons;
    orderedHorizons.reserve(byName.size());
    for (auto& kv : byName) orderedHorizons.push_back(std::move(kv.second));
    std::sort(orderedHorizons.begin(), orderedHorizons.end(),
              [](const HorizonGroup& a, const HorizonGroup& b) {
                  if (std::abs(a.meanTimeMs - b.meanTimeMs) > 1e-3)
                      return a.meanTimeMs < b.meanTimeMs;
                  return a.name < b.name;
              });

    report.horizonCount = static_cast<int>(orderedHorizons.size());
    for (const auto& h : orderedHorizons) report.orderedHorizonNames.push_back(h.name);

    // 2. 提取工区各井的时深关系与井轨迹 XY 包围盒
    std::vector<CorrelationTimeDepthPreference> tdPrefs;
    std::string ignoredErr;
    readCorrelationTimeDepthPreferences(tdPrefs, ignoredErr);
    std::map<long long, long long> prefByWell;
    for (const auto& p : tdPrefs) prefByWell[p.wellId] = p.timeDepthId;

    std::vector<WellInfoRecord> wells;
    if (!listWellInfos(wells, error)) return false;

    std::vector<WellTdCurve> wellCurves;
    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();

    auto expandBbox = [&](double x, double y) {
        if (isFinite(x) && isFinite(y)) {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    };

    for (const auto& w : wells) {
        if (isCancelled()) return false;
        WellTdCurve curve;
        const long long prefId = prefByWell.count(w.id) ? prefByWell[w.id] : 0;
        if (extractWellTdCurve(*this, st, w.id, w.value.wellName, w.value.wellheadX, w.value.wellheadY,
                               prefId, options.autoDetectTimeSign, curve)) {
            expandBbox(curve.wellheadX, curve.wellheadY);
            for (const auto& pt : curve.points) expandBbox(pt.x, pt.y);
            wellCurves.push_back(std::move(curve));
        } else {
            ++report.skippedWellCount;
        }
    }

    // 3. 在井轨迹包围盒 (±searchRadiusMeters) 内加载各层位点，避免全量加载百万级远端层位点
    const double radius = std::max(10.0, options.searchRadiusMeters);
    const bool hasValidBbox = std::isfinite(minX) && std::isfinite(maxX) && minX <= maxX;
    for (auto& h : orderedHorizons) {
        if (isCancelled()) return false;
        for (long long hid : h.allIds) {
            QSqlQuery qPts;
            if (hasValidBbox) {
                if (!st.exec("SELECT inline_no, crossline_no, x, y, z FROM horizon_points "
                             "WHERE horizon_id=? AND ((x BETWEEN ? AND ? AND y BETWEEN ? AND ?) OR (x IS NULL AND y IS NULL))",
                             {hid, minX - radius, maxX + radius, minY - radius, maxY + radius}, error, &qPts))
                    return false;
            } else {
                if (!st.exec("SELECT inline_no, crossline_no, x, y, z FROM horizon_points WHERE horizon_id=? LIMIT 50000",
                             {hid}, error, &qPts))
                    return false;
            }
            while (qPts.next()) {
                SeismicHorizonPoint pt;
                pt.inlineNumber = qPts.value(0).toInt();
                pt.crosslineNumber = qPts.value(1).toInt();
                pt.x = value(qPts.value(2));
                pt.y = value(qPts.value(3));
                pt.zTime = value(qPts.value(4));
                h.localPoints.push_back(pt);
            }
        }
    }

    // 4. 组装 StratigraphicScheme（使用可读稳定标识：T70、T70-T72、T70-T72-S1..S4）
    StratigraphicScheme scheme;
    scheme.id = options.schemeId.empty() ? "default" : options.schemeId;
    scheme.name = options.schemeName.empty() ? "地震层序地层方案（四等分小层）" : options.schemeName;
    scheme.author = "DataService::buildSeismicStratigraphicFramework";
    scheme.note = "由工区地震层位与井时深关系自动建立（含四等分小层 S1..S4）";

    for (size_t i = 0; i < orderedHorizons.size(); ++i) {
        StratigraphicBoundary b;
        b.id = BoundaryId{orderedHorizons[i].name};
        b.name = orderedHorizons[i].name;
        b.order = static_cast<int>(i) + 1;
        b.kind = "horizon";
        b.source = "seismic_horizon";
        b.seismicHorizonResourceId = orderedHorizons[i].primaryId;
        scheme.boundaries.push_back(std::move(b));
    }

    for (size_t i = 0; i + 1 < orderedHorizons.size(); ++i) {
        const std::string& topName = orderedHorizons[i].name;
        const std::string& botName = orderedHorizons[i + 1].name;
        StratigraphicInterval iv;
        iv.id = IntervalId{topName + "-" + botName};
        iv.top = BoundaryId{topName};
        iv.bottom = BoundaryId{botName};
        iv.order = static_cast<int>(i) + 1;
        iv.status = "valid";
        scheme.intervals.push_back(iv);

        for (int sIdx = 1; sIdx <= SubIntervalCount; ++sIdx) {
            StratigraphicSubInterval sub;
            sub.id = SubIntervalId{iv.id.value + "-S" + std::to_string(sIdx)};
            sub.interval = iv.id;
            sub.index = sIdx;
            sub.status = "valid";
            scheme.subIntervals.push_back(sub);
        }
        SubIntervalDivision div;
        div.interval = iv.id;
        scheme.divisions.push_back(div);
    }

    // 5. 求取每口井在各层位面上的交点 (MD, TWT)，生成井层段出现记录与井内分层道
    struct WellResolvedHorizonPick {
        std::string horizonName;
        int order = 0;
        double md = Missing;
        double twtMs = Missing;
    };
    struct WellResolvedIntervalPick {
        std::string intervalId;
        std::string topName;
        std::string bottomName;
        double topMd = Missing;
        double bottomMd = Missing;
        double topTwtMs = Missing;
        double bottomTwtMs = Missing;
        std::array<double, 3> internalMd{Missing, Missing, Missing};
        std::array<double, 3> internalTwtMs{Missing, Missing, Missing};
    };
    std::map<long long, std::vector<WellResolvedHorizonPick>> picksByWell;
    std::map<long long, std::vector<WellResolvedIntervalPick>> intervalsByWell;

    for (const auto& wc : wellCurves) {
        if (isCancelled()) return false;
        std::vector<WellResolvedHorizonPick> hpicks(orderedHorizons.size());
        for (size_t i = 0; i < orderedHorizons.size(); ++i) {
            hpicks[i].horizonName = orderedHorizons[i].name;
            hpicks[i].order = static_cast<int>(i) + 1;
            intersectWellWithHorizon(wc, orderedHorizons[i], radius, hpicks[i].md, hpicks[i].twtMs);
        }

        std::vector<WellResolvedIntervalPick> ipicks;
        for (size_t i = 0; i + 1 < orderedHorizons.size(); ++i) {
            const auto& topPick = hpicks[i];
            const auto& botPick = hpicks[i + 1];
            const IntervalId ivId = scheme.intervals[i].id;
            if (!isFinite(topPick.md) && !isFinite(botPick.md)) continue;

            if (isFinite(topPick.md) && isFinite(botPick.md)) {
                if (!(botPick.md > topPick.md + 1e-3)) continue; // 跳过倒置或零厚度异常
                WellIntervalOccurrence occ;
                occ.wellId = wc.wellId;
                occ.interval = ivId;
                occ.depthType = "MD";
                occ.depthUnit = "m";
                occ.top = topPick.md;
                occ.bottom = botPick.md;
                for (int k = 0; k < 3; ++k) {
                    const double frac = 0.25 * double(k + 1);
                    occ.internal[size_t(k)] = topPick.md + frac * (botPick.md - topPick.md);
                    occ.internalStatus[size_t(k)] = "derived";
                }
                occ.status = "complete";
                scheme.wellOccurrences.push_back(occ);

                WellResolvedIntervalPick wp;
                wp.intervalId = ivId.value;
                wp.topName = topPick.horizonName;
                wp.bottomName = botPick.horizonName;
                wp.topMd = topPick.md;
                wp.bottomMd = botPick.md;
                wp.topTwtMs = topPick.twtMs;
                wp.bottomTwtMs = botPick.twtMs;
                wp.internalMd = occ.internal;
                for (int k = 0; k < 3; ++k) {
                    const double frac = 0.25 * double(k + 1);
                    wp.internalTwtMs[size_t(k)] = isFinite(topPick.twtMs) && isFinite(botPick.twtMs)
                        ? (topPick.twtMs + frac * (botPick.twtMs - topPick.twtMs))
                        : interpolateTwtAtMd(wc, occ.internal[size_t(k)]);
                }
                ipicks.push_back(wp);
            }
        }

        if (!ipicks.empty()) {
            ++report.matchedWellCount;
            picksByWell[wc.wellId] = std::move(hpicks);
            intervalsByWell[wc.wellId] = std::move(ipicks);
        } else {
            ++report.skippedWellCount;
        }
    }

    // 6. 读取是否已有同 ID 方案头修订，提交地层方案快照
    std::string expectedHead;
    std::vector<StratigraphicSchemeSummary> existingSchemes;
    if (listStratigraphicSchemes(existingSchemes, ignoredErr)) {
        for (const auto& s : existingSchemes) {
            if (s.id == scheme.id) {
                expectedHead = s.headRevision.value;
                break;
            }
        }
    }

    StratigraphicScheme committed;
    std::vector<FrameworkIssue> issues;
    if (!commitStratigraphicScheme(scheme, expectedHead, committed, issues, error))
        return false;

    report.schemeId = committed.id;
    report.revision = committed.revision;
    report.intervalCount = static_cast<int>(committed.intervals.size());
    report.subIntervalCount = static_cast<int>(committed.subIntervals.size());
    for (const auto& iss : issues) {
        report.warnings.push_back(iss.code + ": " + iss.message);
    }

    // 7. 在每口匹配井上落库 '地震层位'（T70-T72）与 '地震小层'（T70-T72-S1..S4）分层道及 '地震层位标志层'
    if (options.materializeWellTracks && !intervalsByWell.empty()) {
        Transaction tx(st, error);
        if (!tx.active) return false;

        DepthInfo mdInfo;
        mdInfo.depthType = "MD";
        mdInfo.depthUnit = "m";
        const QString depthText = json(depthJson(mdInfo));

        for (const auto& [wellId, ipicks] : intervalsByWell) {
            if (isCancelled()) return false;
            if (options.overwriteExistingTracks) {
                if (!st.exec("DELETE FROM tracks WHERE well_id=? AND name IN ('地震层位','地震小层')",
                             {wellId}, error))
                    return false;
                if (!st.exec("DELETE FROM well_objects WHERE well_id=? AND kind='markers' AND name='地震层位标志层'",
                             {wellId}, error))
                    return false;
            }

            // (a) 写入 '地震层位' 分层道（T70-T72 等）
            QSqlQuery qInsTrack;
            if (!st.exec("INSERT INTO tracks(well_id,source_id,name,type,depth_json,sheet) VALUES(?,NULL,?,?,?,?)",
                         {wellId, QStringLiteral("地震层位"), QStringLiteral("地震层位"),
                          depthText, QStringLiteral("seismic_framework")},
                         error, &qInsTrack))
                return false;
            const long long horizonTrackId = qInsTrack.lastInsertId().toLongLong();

            int ordinal = 0;
            for (const auto& ip : ipicks) {
                std::vector<Measurement> props{
                    {"topTwtMs", "ms", ip.topTwtMs, ""},
                    {"bottomTwtMs", "ms", ip.bottomTwtMs, ""},
                    {"schemeId", "", Missing, committed.id},
                    {"intervalId", "", Missing, ip.intervalId}
                };
                if (!st.exec("INSERT INTO intervals(track_id,ordinal,original_id,name,top,bottom,top_tvd,bottom_tvd,top_ss,bottom_ss,facies,top_unc,bottom_unc,not_penetrated,properties,source_row) "
                             "VALUES(?,?,?,?,?,?,NULL,NULL,NULL,NULL,'',0,0,0,?,?)",
                             {horizonTrackId, ordinal, ordinal + 1, q(ip.intervalId),
                              number(ip.topMd), number(ip.bottomMd),
                              json(measurementsJson(props)), ordinal + 1},
                             error))
                    return false;
                ++ordinal;
            }

            // (b) 写入 '地震小层' 分层道（T70-T72-S1..S4 四等分小层）
            QSqlQuery qInsSubTrack;
            if (!st.exec("INSERT INTO tracks(well_id,source_id,name,type,depth_json,sheet) VALUES(?,NULL,?,?,?,?)",
                         {wellId, QStringLiteral("地震小层"), QStringLiteral("地震小层"),
                          depthText, QStringLiteral("seismic_framework")},
                         error, &qInsSubTrack))
                return false;
            const long long subTrackId = qInsSubTrack.lastInsertId().toLongLong();

            int subOrdinal = 0;
            for (const auto& ip : ipicks) {
                const std::array<double, 5> edgesMd{
                    ip.topMd, ip.internalMd[0], ip.internalMd[1], ip.internalMd[2], ip.bottomMd};
                const std::array<double, 5> edgesTwt{
                    ip.topTwtMs, ip.internalTwtMs[0], ip.internalTwtMs[1], ip.internalTwtMs[2], ip.bottomTwtMs};
                for (int sIdx = 1; sIdx <= SubIntervalCount; ++sIdx) {
                    const std::string subName = ip.intervalId + "-S" + std::to_string(sIdx);
                    const double sTopMd = edgesMd[size_t(sIdx - 1)];
                    const double sBotMd = edgesMd[size_t(sIdx)];
                    const double sTopTwt = edgesTwt[size_t(sIdx - 1)];
                    const double sBotTwt = edgesTwt[size_t(sIdx)];
                    std::vector<Measurement> props{
                        {"topTwtMs", "ms", sTopTwt, ""},
                        {"bottomTwtMs", "ms", sBotTwt, ""},
                        {"schemeId", "", Missing, committed.id},
                        {"intervalId", "", Missing, ip.intervalId},
                        {"subIntervalId", "", Missing, subName},
                        {"subIndex", "", double(sIdx), std::to_string(sIdx)}
                    };
                    if (!st.exec("INSERT INTO intervals(track_id,ordinal,original_id,name,top,bottom,top_tvd,bottom_tvd,top_ss,bottom_ss,facies,top_unc,bottom_unc,not_penetrated,properties,source_row) "
                                 "VALUES(?,?,?,?,?,?,NULL,NULL,NULL,NULL,'',0,0,0,?,?)",
                                 {subTrackId, subOrdinal, subOrdinal + 1, q(subName),
                                  number(sTopMd), number(sBotMd),
                                  json(measurementsJson(props)), subOrdinal + 1},
                                 error))
                        return false;
                    ++subOrdinal;
                }
            }

            // (c) 写入 '地震层位标志层' (markers)
            const auto hpIt = picksByWell.find(wellId);
            if (hpIt != picksByWell.end()) {
                QJsonArray markerPoints;
                int mId = 1;
                for (const auto& hp : hpIt->second) {
                    if (!isFinite(hp.md)) continue;
                    markerPoints.append(QJsonObject{
                        {"id", mId},
                        {"name", q(hp.horizonName)},
                        {"depth", hp.md},
                        {"tvd", hp.md},
                        {"tvdss", QJsonValue::Null},
                        {"row", hp.order}
                    });
                    ++mId;
                }
                if (!markerPoints.isEmpty()) {
                    const QJsonObject payload{
                        {"depth", depthJson(mdInfo)},
                        {"points", markerPoints}
                    };
                    if (!st.exec("INSERT INTO well_objects(well_id,source_id,kind,name,payload) VALUES(?,NULL,'markers',?,?)",
                                 {wellId, QStringLiteral("地震层位标志层"), json(payload)}, error))
                        return false;
                }
            }
        }

        if (!tx.commit(error)) return false;
    }

    return true;
}

bool DataService::listAvailableStratigraphicTracks(std::vector<StratigraphicTrackDescriptor>& tracks,
                                                   std::string& error) const
{
    error.clear();
    tracks.clear();
    auto& st = impl_->store;
    if (!st.db.isOpen()) { error = "请先打开工区"; return false; }

    // 1. 列出统一地层方案中的地震小层与地震层位体系
    std::vector<StratigraphicSchemeSummary> schemes;
    if (!listStratigraphicSchemes(schemes, error)) return false;
    for (const auto& sum : schemes) {
        StratigraphicScheme scheme;
        std::string ignoreErr;
        if (!readStratigraphicScheme(sum.id, {}, scheme, ignoreErr)) continue;
        std::set<long long> schemeWells;
        for (const auto& occ : scheme.wellOccurrences) {
            if (isFinite(occ.top) && isFinite(occ.bottom) && occ.bottom > occ.top)
                schemeWells.insert(occ.wellId);
        }

        StratigraphicTrackDescriptor subDesc;
        subDesc.source = StratigraphicTrackSource::SeismicFramework;
        subDesc.schemeId = scheme.id;
        subDesc.trackName = "地震小层";
        subDesc.trackType = "seismic_subinterval";
        subDesc.wellCount = static_cast<int>(schemeWells.size());
        for (const auto& iv : scheme.intervals) {
            const auto* t = fw::findBoundary(scheme, iv.top);
            const auto* b = fw::findBoundary(scheme, iv.bottom);
            const std::string base = (t && b) ? (t->name + "-" + b->name) : iv.id.value;
            for (const auto* sub : fw::subIntervalsOf(scheme, iv.id)) {
                subDesc.unitNames.push_back(base + "-S" + std::to_string(sub->index));
            }
        }
        tracks.push_back(std::move(subDesc));

        StratigraphicTrackDescriptor ivDesc;
        ivDesc.source = StratigraphicTrackSource::SeismicFramework;
        ivDesc.schemeId = scheme.id;
        ivDesc.trackName = "地震层位";
        ivDesc.trackType = "seismic_interval";
        ivDesc.wellCount = static_cast<int>(schemeWells.size());
        for (const auto& iv : scheme.intervals) {
            const auto* t = fw::findBoundary(scheme, iv.top);
            const auto* b = fw::findBoundary(scheme, iv.bottom);
            ivDesc.unitNames.push_back((t && b) ? (t->name + "-" + b->name) : iv.id.value);
        }
        tracks.push_back(std::move(ivDesc));
    }

    // 2. 列出井内分层道（tracks 表：包含组、段、地震小层、地震层位及其他分层道）
    QSqlQuery qTracks;
    if (!st.exec("SELECT name, COALESCE(type,''), COUNT(DISTINCT well_id) "
                 "FROM tracks WHERE name IS NOT NULL AND name<>'' "
                 "GROUP BY name, COALESCE(type,'') ORDER BY MIN(id)",
                 {}, error, &qTracks))
        return false;

    while (qTracks.next()) {
        StratigraphicTrackDescriptor desc;
        desc.source = StratigraphicTrackSource::WellIntervalTrack;
        desc.trackName = s(qTracks.value(0).toString());
        desc.trackType = s(qTracks.value(1).toString());
        desc.wellCount = qTracks.value(2).toInt();

        QSqlQuery qUnits;
        if (!st.exec("SELECT i.name FROM intervals i JOIN tracks t ON t.id = i.track_id "
                     "WHERE t.name=? AND i.name IS NOT NULL AND i.name<>'' "
                     "GROUP BY i.name ORDER BY AVG(i.top), MIN(i.ordinal), MIN(i.id)",
                     {q(desc.trackName)}, error, &qUnits))
            return false;
        while (qUnits.next()) {
            desc.unitNames.push_back(s(qUnits.value(0).toString()));
        }
        if (!desc.unitNames.empty())
            tracks.push_back(std::move(desc));
    }
    return true;
}

bool DataService::queryWellLogsByStratigraphy(const WellIntervalLogQuery& query,
                                              std::vector<WellStratigraphicLogSlice>& results,
                                              std::string& error) const
{
    error.clear();
    results.clear();
    auto& st = impl_->store;
    if (!st.db.isOpen()) { error = "请先打开工区"; return false; }

    // 1. 确定目标井列表
    std::vector<WellInfoRecord> allWells;
    if (!listWellInfos(allWells, error)) return false;
    std::map<long long, WellInfo> wellMap;
    for (const auto& w : allWells) wellMap[w.id] = w.value;

    std::vector<long long> targetWells = query.wellIds;
    if (targetWells.empty()) {
        targetWells.reserve(allWells.size());
        for (const auto& w : allWells) targetWells.push_back(w.id);
    }

    // 2. 若选择地震地层体系（或传了 subIntervalId / intervalId），预加载 StratigraphicScheme
    const auto& sel = query.stratigraphy;
    StratigraphicScheme scheme;
    bool hasScheme = false;
    const std::string schemeId = sel.schemeId.empty() ? "default" : sel.schemeId;
    if (sel.source == StratigraphicTrackSource::SeismicFramework ||
        !sel.subIntervalId.empty() || !sel.intervalId.empty() ||
        sel.trackName == "地震小层" || sel.trackName == "地震层位") {
        std::string ignoreErr;
        hasScheme = readStratigraphicScheme(schemeId, sel.revision, scheme, ignoreErr);
    }

    const StratigraphicSubInterval* matchedSub = nullptr;
    const StratigraphicInterval* matchedIv = nullptr;
    if (hasScheme) {
        if (!sel.subIntervalId.empty())
            matchedSub = fw::resolveSubInterval(scheme, sel.subIntervalId.value);
        if (!matchedSub && !sel.unitName.empty())
            matchedSub = fw::resolveSubInterval(scheme, sel.unitName);
        if (!matchedSub && !sel.intervalId.empty())
            matchedIv = fw::resolveInterval(scheme, sel.intervalId.value);
        if (!matchedSub && !matchedIv && !sel.unitName.empty())
            matchedIv = fw::resolveInterval(scheme, sel.unitName);
    }

    // 读取时深首选设置以便填充 [topTimeMs, bottomTimeMs]
    std::vector<CorrelationTimeDepthPreference> tdPrefs;
    std::string ignoredErr;
    readCorrelationTimeDepthPreferences(tdPrefs, ignoredErr);
    std::map<long long, long long> prefByWell;
    for (const auto& p : tdPrefs) prefByWell[p.wellId] = p.timeDepthId;

    // 准备请求的曲线名集合（不区分大小写）
    std::vector<QString> wantedCurves;
    for (const auto& cn : query.curveNames) {
        if (!cn.empty()) wantedCurves.push_back(q(cn).trimmed());
    }
    auto matchesWantedCurve = [&](const std::string& cname) {
        if (wantedCurves.empty()) return true;
        const QString qc = q(cname).trimmed();
        for (const auto& w : wantedCurves) {
            if (qc.compare(w, Qt::CaseInsensitive) == 0) return true;
        }
        return false;
    };

    // 线性重采样辅助函数
    auto resampleFeatureTable = [](const WellFeatureTable& src, const std::vector<double>& targetDepths,
                                   const std::vector<double>& outputAxis) -> WellFeatureTable {
        WellFeatureTable dst;
        dst.wellId = src.wellId;
        dst.depthUnit = src.depthUnit;
        dst.depths = outputAxis;
        if (src.depths.empty() || targetDepths.empty()) return dst;

        for (const auto& col : src.columns) {
            FeatureColumn outCol;
            outCol.curveId = col.curveId;
            outCol.name = col.name;
            outCol.unit = col.unit;
            outCol.values.assign(targetDepths.size(), Missing);
            outCol.quality.assign(targetDepths.size(), 1);

            for (size_t i = 0; i < targetDepths.size(); ++i) {
                const double d = targetDepths[i];
                auto it = std::lower_bound(src.depths.begin(), src.depths.end(), d);
                if (it == src.depths.begin()) {
                    if (std::abs(*it - d) <= 1.0 && col.quality[0] == 0 && isFinite(col.values[0])) {
                        outCol.values[i] = col.values[0];
                        outCol.quality[i] = 0;
                    }
                } else if (it == src.depths.end()) {
                    const size_t last = src.depths.size() - 1;
                    if (std::abs(src.depths[last] - d) <= 1.0 && col.quality[last] == 0 && isFinite(col.values[last])) {
                        outCol.values[i] = col.values[last];
                        outCol.quality[i] = 0;
                    }
                } else {
                    const size_t hi = static_cast<size_t>(it - src.depths.begin());
                    const size_t lo = hi - 1;
                    const bool okLo = col.quality[lo] == 0 && isFinite(col.values[lo]);
                    const bool okHi = col.quality[hi] == 0 && isFinite(col.values[hi]);
                    if (okLo && okHi && src.depths[hi] > src.depths[lo]) {
                        const double f = (d - src.depths[lo]) / (src.depths[hi] - src.depths[lo]);
                        outCol.values[i] = col.values[lo] + f * (col.values[hi] - col.values[lo]);
                        outCol.quality[i] = 0;
                    } else if (okLo) {
                        outCol.values[i] = col.values[lo];
                        outCol.quality[i] = 0;
                    } else if (okHi) {
                        outCol.values[i] = col.values[hi];
                        outCol.quality[i] = 0;
                    }
                }
            }
            dst.columns.push_back(std::move(outCol));
        }
        return dst;
    };

    // 3. 逐井解析地层顶底深度并截取测井数据
    for (long long wellId : targetWells) {
        WellStratigraphicLogSlice slice;
        slice.wellId = wellId;
        if (auto it = wellMap.find(wellId); it != wellMap.end()) {
            slice.wellName = it->second.wellName;
            slice.wellheadX = it->second.wellheadX;
            slice.wellheadY = it->second.wellheadY;
        }

        // (a) 优先从 StratigraphicScheme 解析地震小层或父层位深度
        if (matchedSub) {
            const auto range = fw::wellRange(scheme, wellId, matchedSub->id);
            const auto* iv = fw::findInterval(scheme, matchedSub->interval);
            const auto* t = iv ? fw::findBoundary(scheme, iv->top) : nullptr;
            const auto* b = iv ? fw::findBoundary(scheme, iv->bottom) : nullptr;
            const std::string readable = (t && b)
                ? (t->name + "-" + b->name + "-S" + std::to_string(matchedSub->index))
                : matchedSub->id.value;
            slice.unitId = matchedSub->id.value;
            slice.unitName = readable;
            slice.trackName = "地震小层";
            if (range.resolved && isFinite(range.top) && isFinite(range.bottom) && range.bottom > range.top) {
                slice.depthType = range.depthType.empty() ? "MD" : range.depthType;
                slice.depthUnit = range.depthUnit.empty() ? "m" : range.depthUnit;
                slice.topDepth = range.top;
                slice.bottomDepth = range.bottom;
                slice.thickness = range.bottom - range.top;
                slice.resolved = true;
            } else {
                slice.unresolvedReason = range.reason;
            }
        } else if (matchedIv) {
            const auto* t = fw::findBoundary(scheme, matchedIv->top);
            const auto* b = fw::findBoundary(scheme, matchedIv->bottom);
            const std::string readable = (t && b) ? (t->name + "-" + b->name) : matchedIv->id.value;
            slice.unitId = matchedIv->id.value;
            slice.unitName = readable;
            slice.trackName = "地震层位";
            for (const auto& occ : scheme.wellOccurrences) {
                if (occ.wellId == wellId && occ.interval == matchedIv->id) {
                    if (isFinite(occ.top) && isFinite(occ.bottom) && occ.bottom > occ.top) {
                        slice.depthType = occ.depthType.empty() ? "MD" : occ.depthType;
                        slice.depthUnit = occ.depthUnit.empty() ? "m" : occ.depthUnit;
                        slice.topDepth = occ.top;
                        slice.bottomDepth = occ.bottom;
                        slice.thickness = occ.bottom - occ.top;
                        slice.resolved = true;
                    } else {
                        slice.unresolvedReason = "父层位在该井顶底缺失或未钻穿";
                    }
                    break;
                }
            }
            if (!slice.resolved && slice.unresolvedReason.empty())
                slice.unresolvedReason = "该井无此地震层位记录";
        }

        // (b) 若未在 StratigraphicScheme 命中（或用户请求的是常规分层道，如 '段'/'组'/'地震小层'），查询 tracks/intervals 表
        if (!slice.resolved) {
            std::string targetUnit = sel.unitName;
            if (targetUnit.empty() && !sel.subIntervalId.empty()) targetUnit = sel.subIntervalId.value;
            if (targetUnit.empty() && !sel.intervalId.empty()) targetUnit = sel.intervalId.value;

            if (!targetUnit.empty()) {
                QString sql = "SELECT t.name, t.depth_json, MIN(i.top), MAX(i.bottom) "
                              "FROM intervals i JOIN tracks t ON t.id = i.track_id "
                              "WHERE t.well_id=? AND i.name=? AND i.top IS NOT NULL AND i.bottom IS NOT NULL AND i.bottom > i.top";
                QVariantList args{wellId, q(targetUnit)};
                if (!sel.trackName.empty()) {
                    sql += " AND (t.name=? OR t.type=?)";
                    args << q(sel.trackName) << q(sel.trackName);
                }
                sql += " GROUP BY t.id ORDER BY t.id LIMIT 1";
                QSqlQuery qIv;
                if (!st.exec(sql, args, error, &qIv)) return false;
                if (qIv.next() && !qIv.value(2).isNull() && !qIv.value(3).isNull()) {
                    const DepthInfo di = readDepth(object(qIv.value(1).toString()));
                    slice.trackName = s(qIv.value(0).toString());
                    slice.unitId = targetUnit;
                    slice.unitName = targetUnit;
                    slice.depthType = di.depthType.empty() ? "MD" : di.depthType;
                    slice.depthUnit = di.depthUnit.empty() ? "m" : di.depthUnit;
                    slice.topDepth = qIv.value(2).toDouble();
                    slice.bottomDepth = qIv.value(3).toDouble();
                    slice.thickness = slice.bottomDepth - slice.topDepth;
                    slice.resolved = true;
                    slice.unresolvedReason.clear();
                } else if (slice.unresolvedReason.empty()) {
                    slice.unitName = targetUnit;
                    slice.trackName = sel.trackName;
                    slice.unresolvedReason = "该井未找到层位/分层单元: " + targetUnit;
                }
            } else if (slice.unresolvedReason.empty()) {
                slice.unresolvedReason = "未指定要查询的地层或小层名称";
            }
        }

        // 若该层段已解析顶底深度，补充时深转换顶底时间、测井特征表及辅助层段道
        if (slice.resolved) {
            WellTdCurve tdCurve;
            const long long prefId = prefByWell.count(wellId) ? prefByWell[wellId] : 0;
            if (extractWellTdCurve(*this, st, wellId, slice.wellName, slice.wellheadX, slice.wellheadY,
                                   prefId, true, tdCurve)) {
                slice.topTimeMs = interpolateTwtAtMd(tdCurve, slice.topDepth);
                slice.bottomTimeMs = interpolateTwtAtMd(tdCurve, slice.bottomDepth);
            }

            // 查找该井匹配的曲线 ID（优先选用匹配曲线最多的 log_set）
            std::vector<DataEntry> allCurves;
            if (!listData("curves", wellId, allCurves, error)) return false;
            std::map<long long, std::vector<long long>> curvesBySet;
            for (const auto& entry : allCurves) {
                if (matchesWantedCurve(entry.name))
                    curvesBySet[entry.parentId].push_back(entry.id);
            }
            long long bestSetId = 0;
            size_t bestSetCount = 0;
            for (const auto& [setId, cids] : curvesBySet) {
                if (cids.size() > bestSetCount) {
                    bestSetCount = cids.size();
                    bestSetId = setId;
                }
            }
            if (bestSetId > 0) {
                std::string featErr;
                WellFeatureTable rawTable;
                if (prepareWellFeatures(wellId, curvesBySet[bestSetId], slice.topDepth, slice.bottomDepth,
                                        query.processed, rawTable, featErr)) {
                    if (query.resampleStepMeters > 0.0 && !rawTable.depths.empty()) {
                        std::vector<double> uniformDepths;
                        for (double d = slice.topDepth; d <= slice.bottomDepth + 1e-9; d += query.resampleStepMeters)
                            uniformDepths.push_back(d);
                        slice.featureTable = resampleFeatureTable(rawTable, uniformDepths, uniformDepths);
                    } else {
                        slice.featureTable = std::move(rawTable);
                    }

                    if (query.normalizedSampleCount > 0 && !slice.featureTable.depths.empty()) {
                        const int K = query.normalizedSampleCount;
                        std::vector<double> targetDepths(size_t(K), 0.0);
                        std::vector<double> relAxis(size_t(K), 0.0);
                        for (int k = 0; k < K; ++k) {
                            const double f = (K == 1) ? 0.5 : (double(k) / double(K - 1));
                            relAxis[size_t(k)] = f;
                            targetDepths[size_t(k)] = slice.topDepth + f * (slice.bottomDepth - slice.topDepth);
                        }
                        slice.normalizedFeatureTable = resampleFeatureTable(slice.featureTable, targetDepths, relAxis);
                    }
                }
            }

            if (query.includeAuxiliaryIntervals) {
                std::vector<DataEntry> trackEntries;
                if (!listData("intervals", wellId, trackEntries, error)) return false;
                std::vector<long long> trackIds;
                trackIds.reserve(trackEntries.size());
                for (const auto& te : trackEntries) trackIds.push_back(te.id);
                if (!trackIds.empty()) {
                    std::string cmpErr;
                    compareIntervals(trackIds, slice.topDepth, slice.bottomDepth, slice.auxiliaryTracks, cmpErr);
                }
            }
        }

        results.push_back(std::move(slice));
    }

    return true;
}

} // namespace paleo::data
