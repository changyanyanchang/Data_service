// 井震统一地层框架构建与按地层/小层查询测井道服务：
// 1. 导入期由地震层位 + 时深关系自动建立父层段（T70-T72）及四等分小层（T70-T72-S1..S4），并在井上落库地层道；
// 2. 提供按地震小层/层位或常规地质分层道提取测井曲线与辅助层段道的统一查询接口。
#include "paleo_data/service.h"
#include "store/project_store.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
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
    DepthInfo depthInfo;
    std::string timeReference;
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
                        long long preferredTdId, bool autoDetectSign, WellTdCurve& out,
                        bool strictSemantics = false)
{
    out = {};
    out.wellId = wellId;
    out.wellName = wellName;
    out.wellheadX = wellheadX;
    out.wellheadY = wellheadY;

    std::string err;
    QSqlQuery qTd;
    QString candidateSql="SELECT id FROM well_objects WHERE well_id=? AND kind='time_depth'";
    QVariantList candidateArgs{wellId};
    if(strictSemantics && preferredTdId) {candidateSql+=" AND id=?";candidateArgs.push_back(preferredTdId);}
    candidateSql+=" ORDER BY id";
    if(strictSemantics)candidateSql+=" LIMIT 2";
    if (!st.exec(candidateSql,candidateArgs, err, &qTd))
        return false;
    std::vector<long long> candidateIds;
    while (qTd.next()) candidateIds.push_back(qTd.value(0).toLongLong());

    // 若该井名下暂无已归井时深表，检查未归井时深表是否按井名匹配
    if (!strictSemantics && candidateIds.empty() && !wellName.empty()) {
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
    if (strictSemantics && ((!preferredTdId && candidateIds.size()!=1) ||
        (preferredTdId && std::find(candidateIds.begin(),candidateIds.end(),preferredTdId)==candidateIds.end()))) return false;

    // 优先使用首选时深表，否则选择有效点最多的时深表
    TimeDepthTable bestTable;
    size_t bestValidCount = 0;
    for (long long tdId : candidateIds) {
        if(strictSemantics && preferredTdId && tdId!=preferredTdId)continue;
        TimeDepthTable t;
        if (!ds.readTimeDepth(tdId, t, err)) continue;
        if(strictSemantics && (t.depthInfo.depthType!="MD" ||
            (t.depthInfo.depthUnit!="m" && t.depthInfo.depthUnit!="ft") ||
            q(t.depthInfo.mdReference).trimmed().isEmpty() ||
            (t.timeUnit!="ms" && t.timeUnit!="s") || (t.timeType!="TWT" && t.timeType!="OWT") ||
            q(t.timeReference).trimmed().isEmpty() || (!t.timeSign.empty() && t.timeSign!="negated")))continue;
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
    out.depthInfo=bestTable.depthInfo;out.depthInfo.depthUnit="m";out.timeReference=bestTable.timeReference;

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
    if (bestTable.timeUnit == "s" || bestTable.timeUnit == "S" || (!strictSemantics && bestTable.timeUnit.empty() && maxAbsTime > 0.0 && maxAbsTime <= 15.0))
        timeScale = 1000.0;
    const bool negateTime = autoDetectSign && (negCount > posCount);
    const double owtFactor = (bestTable.timeType == "OWT" || bestTable.timeType == "owt") ? 2.0 : 1.0;

    std::vector<NormalizedTdPoint> rawPts;
    rawPts.reserve(bestTable.depthTimePairs.size());
    for (const auto& p : bestTable.depthTimePairs) {
        auto np = normalizeRawPoint(p, depthScale, timeScale * owtFactor, negateTime);
        if (isFinite(np.md) && isFinite(np.twtMs) && np.md >= 0.0 && (strictSemantics || np.twtMs >= 0.0))
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
    struct ResetFailedReport {
        SeismicStratigraphyBuildReport& report;bool published=false;
        ~ResetFailedReport(){if(!published)report={};}
    } reportState{report};
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
    Transaction publication(st,error);
    if(!publication.active || isCancelled())return false;
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
        for (const auto& [wellId, ipicks] : intervalsByWell) {
            if (isCancelled()) return false;
            DepthInfo mdInfo;mdInfo.depthType="MD";mdInfo.depthUnit="m";
            QSqlQuery wellDepth;
            if(!st.exec("SELECT depth_json FROM wells WHERE id=?",{wellId},error,&wellDepth))return false;
            if(wellDepth.next())mdInfo.mdReference=readDepth(object(wellDepth.value(0).toString())).mdReference;
            wellDepth.finish();const QString depthText=json(depthJson(mdInfo));
            // Published revisions and user edits retain their object IDs. Never
            // identify generated ownership from a display name or legacy sheet.
            const QJsonObject generated{{"generator","paleo.seismic_stratigraphy"},{"schema",1},
                {"schemeId",q(committed.id)},{"schemeRevision",q(committed.revision.value)}};
            const QString generatedSheet=json(generated);
            QSqlQuery existing;
            QString existingSql="SELECT 1 FROM tracks WHERE well_id=? AND source_id IS NULL AND ";
            QVariantList existingArgs{wellId};
            if(options.overwriteExistingTracks) {existingSql+="sheet=? LIMIT 1";existingArgs<<generatedSheet;}
            else {
                existingSql+="json_extract(CASE WHEN json_valid(sheet) THEN sheet ELSE '{}' END,'$.generator')=? "
                             "AND json_extract(CASE WHEN json_valid(sheet) THEN sheet ELSE '{}' END,'$.schemeId')=? LIMIT 1";
                existingArgs<<QStringLiteral("paleo.seismic_stratigraphy")<<q(committed.id);
            }
            if(!st.exec(existingSql,existingArgs,error,&existing))return false;
            const bool alreadyMaterialized=existing.next();existing.finish();
            if(alreadyMaterialized)continue;

            // (a) 写入 '地震层位' 分层道（T70-T72 等）
            QSqlQuery qInsTrack;
            if (!st.exec("INSERT INTO tracks(well_id,source_id,name,type,depth_json,sheet) VALUES(?,NULL,?,?,?,?)",
                         {wellId, QStringLiteral("地震层位"), QStringLiteral("地震层位"),
                          depthText, generatedSheet},
                         error, &qInsTrack))
                return false;
            const long long horizonTrackId = qInsTrack.lastInsertId().toLongLong();

            int ordinal = 0;
            for (const auto& ip : ipicks) {
                std::vector<Measurement> props{
                    {"topTwtMs", "ms", ip.topTwtMs, ""},
                    {"bottomTwtMs", "ms", ip.bottomTwtMs, ""},
                    {"schemeId", "", Missing, committed.id},
                    {"schemeRevision", "", Missing, committed.revision.value},
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
                          depthText, generatedSheet},
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
                        {"schemeRevision", "", Missing, committed.revision.value},
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
                        {"tvd", QJsonValue::Null}, // MD alone does not establish TVD for a deviated well.
                        {"tvdss", QJsonValue::Null},
                        {"row", hp.order}
                    });
                    ++mId;
                }
                if (!markerPoints.isEmpty()) {
                    const QJsonObject payload{
                        {"depth", depthJson(mdInfo)},
                        {"points", markerPoints},
                        {"generated",generated}
                    };
                    if (!st.exec("INSERT INTO well_objects(well_id,source_id,kind,name,payload) VALUES(?,NULL,'markers',?,?)",
                                 {wellId, QStringLiteral("地震层位标志层"), json(payload)}, error))
                        return false;
                }
            }
        }

    }
    if(isCancelled() || !publication.commit(error))return false;
    reportState.published=true;return true;
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

    if(query.maxValues<=0 || query.maxValues>64000000 || query.maxSamplesPerWell<=0 || query.maxSamplesPerWell>4000000 ||
       query.normalizedSampleCount<0 || query.normalizedSampleCount>query.maxSamplesPerWell ||
       query.maxAuxiliaryTracks<=0 || query.maxAuxiliaryTracks>4096 ||
       query.maxAuxiliaryRecords<=0 || query.maxAuxiliaryRecords>1000000 ||
       query.maxAuxiliaryBytes<=0 || query.maxAuxiliaryBytes>67108864 ||
       !isFinite(query.resampleStepMeters) || query.wellIds.size()>65536 || query.curveNames.size()>64 ||
       query.stratigraphy.trackId<0 || query.stratigraphy.intervalRecordId<0) {
        error="地层曲线查询参数或采样预算无效";return false;
    }
    bool cancelledSeen=false;
    const auto stopped=[&]{
        if(cancelledSeen || (query.cancelled && query.cancelled())) {cancelledSeen=true;error="已取消地层曲线查询";return true;}return false;
    };
    if(stopped())return false;
    if(!st.exec("BEGIN",{},error))return false;
    struct ReadSnapshot {ProjectStore& store;~ReadSnapshot(){std::string ignored;store.exec("ROLLBACK",{},ignored);}} snapshot{st};
    std::vector<WellStratigraphicLogSlice> pending;
    long long usedValues=0;
    long long auxiliaryTracks=0,auxiliaryRecords=0,auxiliaryBytes=0;
    const auto chargeAuxiliaryBytes=[&](long long bytes) {
        if(bytes<0 || bytes>query.maxAuxiliaryBytes-auxiliaryBytes) {
            error="辅助分层元数据或记录超过字节预算";return false;
        }
        auxiliaryBytes+=bytes;return true;
    };
    const auto charge=[&](size_t rows,size_t columns) {
        if(rows>size_t(query.maxSamplesPerWell) || columns>64 ||
           (columns && rows>size_t((query.maxValues-usedValues)/static_cast<long long>(columns)))) {
            error="地层曲线结果超过采样预算，请缩小井集、深度窗或曲线数";return false;
        }
        usedValues+=static_cast<long long>(rows)*static_cast<long long>(columns);return true;
    };
    const auto knownDepth=[&](const DepthInfo& d){
        return (d.depthType=="MD"||d.depthType=="TVD"||d.depthType=="TVDSS") &&
            (d.depthUnit=="m"||d.depthUnit=="ft") &&
            (d.depthType!="MD"||!d.mdReference.empty()||query.stratigraphy.source==StratigraphicTrackSource::WellIntervalTrack) &&
            (d.depthType!="TVD"||!d.tvdReference.empty()) &&
            (d.depthType!="TVDSS"||!d.tvdssReference.empty()) &&
            (d.depthType!="TVDSS"||d.tvdssDirection=="up"||d.tvdssDirection=="down");
    };
    const auto sameDepth=[&](const DepthInfo& a,const DepthInfo& b) {
        if(!knownDepth(a)||!knownDepth(b)||a.depthType!=b.depthType||a.depthUnit!=b.depthUnit)return false;
        if(a.depthType=="MD")return a.mdReference==b.mdReference;
        if(a.depthType=="TVD")return a.tvdReference==b.tvdReference;
        if(a.depthType=="TVDSS")return a.tvdssReference==b.tvdssReference && a.tvdssDirection==b.tvdssDirection;
        return false;
    };

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
    if(targetWells.size()>65536){error="地层曲线查询井数超过预算";return false;}

    // Explicit source controls resolution. A fixed revision never falls back to
    // another scheme or to current materialized rows with a similar name.
    const auto& sel = query.stratigraphy;
    StratigraphicScheme scheme;
    bool hasScheme = false;
    const std::string schemeId = sel.schemeId.empty() ? "default" : sel.schemeId;
    if(sel.source!=StratigraphicTrackSource::SeismicFramework && sel.source!=StratigraphicTrackSource::WellIntervalTrack) {
        error="未知的分层来源类型";return false;
    }
    if (sel.source == StratigraphicTrackSource::SeismicFramework)
        if(!(hasScheme=readStratigraphicScheme(schemeId,sel.revision,scheme,error)))return false;

    const StratigraphicSubInterval* matchedSub = nullptr;
    const StratigraphicInterval* matchedIv = nullptr;
    if (hasScheme) {
        if (!sel.subIntervalId.empty()) {
            matchedSub=fw::findSubInterval(scheme,sel.subIntervalId);
            if(!matchedSub) {error="指定小层编号不属于该方案修订";return false;}
            if(!sel.intervalId.empty() && sel.intervalId!=matchedSub->interval) {
                error="指定小层编号与父层编号冲突";return false;
            }
        } else if(!sel.intervalId.empty()) {
            matchedIv=fw::findInterval(scheme,sel.intervalId);
            if(!matchedIv) {error="指定父层编号不属于该方案修订";return false;}
        } else if(!sel.unitName.empty()) {
            const auto normalize=[](QString label) {
                label.replace(QChar(0x2013),'-');label.replace(QChar(0x2014),'-');label.replace('~','-');
                label.replace("/S","-S");label.replace("/s","-S");label.replace("-s","-S");return label;
            };
            const auto label=normalize(q(sel.unitName));size_t matches=0;
            for(const auto& candidate:scheme.subIntervals) {
                if(candidate.id.value==sel.unitName || normalize(q(fw::displayName(scheme,candidate.id)))==label) {
                    matchedSub=&candidate;++matches;
                }
            }
            for(const auto& candidate:scheme.intervals) {
                if(candidate.id.value==sel.unitName || normalize(q(fw::displayName(scheme,candidate.id)))==label) {
                    matchedIv=&candidate;++matches;
                }
            }
            if(matches>1) {error="方案修订中的层段名称存在歧义，请指定准确层段编号";return false;}
        }
        if(!matchedSub && !matchedIv) {error="指定方案修订中没有该层段或小层";return false;}
    }

    // 读取时深首选设置以便填充 [topTimeMs, bottomTimeMs]
    std::vector<CorrelationTimeDepthPreference> tdPrefs;
    std::string ignoredErr;
    if(!readCorrelationTimeDepthPreferences(tdPrefs,error))return false;
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
    auto resampleFeatureTable = [&](const WellFeatureTable& src, const std::vector<double>& targetDepths,
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
                if((i&4095)==0 && stopped())return WellFeatureTable{};
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
        if(stopped())return false;
        WellStratigraphicLogSlice slice;
        slice.wellId = wellId;
        DepthInfo rangeDepth;
        if(hasScheme) {
            slice.schemeId=scheme.id;slice.schemeRevision=scheme.revision.value;
            QSqlQuery depth;
            if(!st.exec("SELECT depth_json FROM wells WHERE id=?",{wellId},error,&depth))return false;
            if(depth.next())rangeDepth=readDepth(object(depth.value(0).toString()));
        }
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
                slice.depthType = range.depthType;
                slice.depthUnit = range.depthUnit;
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
                        slice.depthType = occ.depthType;
                        slice.depthUnit = occ.depthUnit;
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
        if (sel.source==StratigraphicTrackSource::WellIntervalTrack) {
            std::string targetUnit = sel.unitName;
            if (targetUnit.empty() && !sel.subIntervalId.empty()) targetUnit = sel.subIntervalId.value;
            if (targetUnit.empty() && !sel.intervalId.empty()) targetUnit = sel.intervalId.value;

            if (!targetUnit.empty() || sel.intervalRecordId>0) {
                QString sql = "SELECT t.id, t.name, MIN(i.top), MAX(i.bottom),COUNT(i.id),MIN(i.id),MIN(i.name) "
                              "FROM intervals i JOIN tracks t ON t.id = i.track_id "
                              "WHERE t.well_id=? AND i.top IS NOT NULL AND i.bottom IS NOT NULL AND i.bottom > i.top";
                QVariantList args{wellId};
                if(sel.intervalRecordId<=0){sql+=" AND i.name=?";args<<q(targetUnit);}
                if(sel.trackId>0){sql+=" AND t.id=?";args<<sel.trackId;}
                else if (!sel.trackName.empty()) {
                    sql += " AND (t.name=? OR t.type=?)";
                    args << q(sel.trackName) << q(sel.trackName);
                }
                if(sel.intervalRecordId>0){sql+=" AND i.id=?";args<<sel.intervalRecordId;}
                sql += " GROUP BY t.id ORDER BY t.id LIMIT 2";
                QSqlQuery qIv;
                if (!st.exec(sql, args, error, &qIv)) return false;
                if (qIv.next() && !qIv.value(2).isNull() && !qIv.value(3).isNull()) {
                    const auto trackId=qIv.value(0).toLongLong();
                    const auto recordId=qIv.value(5).toLongLong();
                    const auto count=qIv.value(4).toLongLong();
                    const auto actualUnit=s(qIv.value(6).toString());
                    slice.trackName = s(qIv.value(1).toString());
                    slice.topDepth=qIv.value(2).toDouble();slice.bottomDepth=qIv.value(3).toDouble();
                    const bool ambiguous=count!=1 || qIv.next();qIv.finish();
                    IntervalTrack info;if(!readIntervalTrackInfo(trackId,info,error))return false;
                    rangeDepth=info.depthInfo;slice.trackId=trackId;slice.intervalRecordId=recordId;
                    slice.unitId = "interval-record:"+std::to_string(recordId);
                    slice.unitName = actualUnit;
                    slice.depthType = rangeDepth.depthType;
                    slice.depthUnit = rangeDepth.depthUnit;
                    slice.thickness = slice.bottomDepth - slice.topDepth;
                    slice.resolved = !ambiguous;
                    slice.unresolvedReason=ambiguous?"分层名称对应多条道或记录，请指定实际道和记录编号":std::string();
                } else if (slice.unresolvedReason.empty()) {
                    slice.unitName = targetUnit;
                    slice.trackName = sel.trackName;
                    slice.unresolvedReason = "该井未找到层位/分层单元: " + targetUnit;
                }
            } else if (slice.unresolvedReason.empty()) {
                slice.unresolvedReason = "未指定要查询的地层或小层名称";
            }
        }

        rangeDepth.depthType=slice.depthType;rangeDepth.depthUnit=slice.depthUnit;
        if(slice.resolved && !knownDepth(rangeDepth)) {
            slice.resolved=false;slice.unresolvedReason="分层深度类型或单位尚未确认，未截取曲线";
        }

        // 若该层段已解析顶底深度，补充时深转换顶底时间、测井特征表及辅助层段道
        if (slice.resolved) {
            WellTdCurve tdCurve;
            const long long prefId = prefByWell.count(wellId) ? prefByWell[wellId] : 0;
            if (!q(rangeDepth.mdReference).trimmed().isEmpty() &&
                extractWellTdCurve(*this, st, wellId, slice.wellName, slice.wellheadX, slice.wellheadY,
                                   prefId, false, tdCurve,true)) {
                DepthInfo metres=rangeDepth;metres.depthUnit="m";
                if(sameDepth(metres,tdCurve.depthInfo) && (rangeDepth.depthUnit=="m" || rangeDepth.depthUnit=="ft")) {
                    const double scale=rangeDepth.depthUnit=="ft"?0.3048:1.0;
                    slice.topTimeMs = interpolateTwtAtMd(tdCurve, slice.topDepth*scale);
                    slice.bottomTimeMs = interpolateTwtAtMd(tdCurve, slice.bottomDepth*scale);
                    slice.timeReference=tdCurve.timeReference;
                }
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
                if(stopped())return false;
                QSqlQuery axis;
                if(!st.exec("SELECT l.depth_json,l.sample_count,s.kind,s.options,s.original_path FROM log_sets l LEFT JOIN sources s ON s.id=l.source_id WHERE l.id=? AND l.well_id=?",
                    {setId,wellId},error,&axis))return false;
                if(!axis.next())continue;
                auto depth=readDepth(object(axis.value(0).toString()));
                if(axis.value(2).toString()=="workbook" && object(axis.value(3).toString())["comprehensiveWellMetreDepth"].toBool()) {
                    if(depth.depthType.empty())depth.depthType="MD";if(depth.depthUnit.empty())depth.depthUnit="m";
                } else if(depth.depthType.empty() && axis.value(2).toString()=="logs" &&
                    QFileInfo(axis.value(4).toString()).suffix().compare("las",Qt::CaseInsensitive)==0)depth.depthType="MD";
                if(!sameDepth(rangeDepth,depth))continue;
                const auto sourceSamples=axis.value(1).toLongLong();
                if(sourceSamples<0 || cids.size()>64) {
                    error="候选曲线组超过解码预算，请缩小曲线集合";return false;
                }
                if (cids.size() > bestSetCount) {
                    bestSetCount = cids.size();
                    bestSetId = setId;
                }
            }
            if(!curvesBySet.empty() && !bestSetId) {
                slice.resolved=false;slice.unresolvedReason="分层道与曲线组的深度类型、单位或起算面不一致，未截取曲线";
            }
            if (bestSetId > 0) {
                std::string featErr;
                WellFeatureTable rawTable;
                CurveReadOptions readOptions;readOptions.cancelled=stopped;
                readOptions.maxSamples=std::min<long long>(query.maxSamplesPerWell,(query.maxValues-usedValues)/static_cast<long long>(curvesBySet[bestSetId].size()));
                if (prepareWellFeatures(wellId, curvesBySet[bestSetId], slice.topDepth, slice.bottomDepth,
                                        query.processed, rawTable, featErr, readOptions)) {
                    if(stopped() || !charge(rawTable.depths.size(),rawTable.columns.size()))return false;
                    if((query.resampleStepMeters>0 || query.normalizedSampleCount>0) &&
                       std::adjacent_find(rawTable.depths.begin(),rawTable.depths.end(),[](double a,double b){return b<=a;})!=rawTable.depths.end()) {
                        error="重采样需要严格递增的深度轴，未排序或合并原始样点";return false;
                    }
                    const bool exclusiveBottom=matchedSub&&matchedSub->index<SubIntervalCount;
                    if(exclusiveBottom){
                        while(!rawTable.depths.empty()&&rawTable.depths.back()>=slice.bottomDepth){
                            const auto index=rawTable.depths.size()-1;rawTable.depths.pop_back();
                            for(auto &column:rawTable.columns){if(column.values.size()>index)column.values.resize(index);if(column.quality.size()>index)column.quality.resize(index);}
                        }
                    }
                    if (query.resampleStepMeters > 0.0 && !rawTable.depths.empty()) {
                        const double stepInAxis=query.resampleStepMeters*(rangeDepth.depthUnit=="ft"?1.0/0.3048:1.0);
                        if(!isFinite(stepInAxis) || slice.topDepth+stepInAxis<=slice.topDepth) {
                            error="重采样步长无法推进深度轴";return false;
                        }
                        const long double steps=(static_cast<long double>(slice.bottomDepth)-slice.topDepth)/stepInAxis;
                        const long double count=exclusiveBottom?std::ceil(steps):std::floor(steps)+1;
                        if(!std::isfinite(count) || count<1 || count>query.maxSamplesPerWell) {
                            error="重采样样点数超过单井预算";return false;
                        }
                        const auto samples=static_cast<size_t>(count);
                        if(!charge(samples,rawTable.columns.size()))return false;
                        std::vector<double> uniformDepths;
                        uniformDepths.reserve(samples);
                        for(size_t i=0;i<samples;++i) {
                            if((i&4095)==0 && stopped())return false;
                            const double d=slice.topDepth+double(i)*stepInAxis;
                            if(exclusiveBottom?d>=slice.bottomDepth:d>slice.bottomDepth)break;
                            uniformDepths.push_back(d);
                        }
                        slice.featureTable = resampleFeatureTable(rawTable, uniformDepths, uniformDepths);
                    } else {
                        slice.featureTable = std::move(rawTable);
                    }

                    if (query.normalizedSampleCount > 0 && !slice.featureTable.depths.empty()) {
                        const int K = query.normalizedSampleCount;
                        if(!charge(size_t(K),slice.featureTable.columns.size()))return false;
                        std::vector<double> targetDepths(size_t(K), 0.0);
                        std::vector<double> relAxis(size_t(K), 0.0);
                        for (int k = 0; k < K; ++k) {
                            if((k&4095)==0 && stopped())return false;
                            const double f = (K == 1) ? 0.5 : (double(k) / double(exclusiveBottom?K:K-1));
                            relAxis[size_t(k)] = f;
                            targetDepths[size_t(k)] = slice.topDepth + f * (slice.bottomDepth - slice.topDepth);
                        }
                        slice.normalizedFeatureTable = resampleFeatureTable(slice.featureTable, targetDepths, relAxis);
                        slice.normalizedFeatureTable.depthUnit="1";
                    }
                    if(stopped())return false;
                } else {error=featErr;return false;}
            }

            if (query.includeAuxiliaryIntervals && slice.resolved) {
                // Bound inspected work as well as emitted records. Do not call
                // compareIntervals: it enumerates every project track and reads
                // every matching interval before the caller can enforce a limit.
                QSqlQuery tracks;
                if(!st.exec("SELECT t.id,coalesce(length(CAST(t.name AS BLOB)),0)+coalesce(length(CAST(t.type AS BLOB)),0)+"
                            "coalesce(length(CAST(t.depth_json AS BLOB)),0)+coalesce(length(CAST(t.sheet AS BLOB)),0)+"
                            "coalesce(length(CAST(s.stored_path AS BLOB)),0)+coalesce(length(CAST(s.options AS BLOB)),0)+coalesce(length(CAST(s.kind AS BLOB)),0) "
                            "FROM tracks t LEFT JOIN sources s ON s.id=t.source_id WHERE t.well_id=? "
                            "ORDER BY t.id LIMIT ?",{wellId,query.maxAuxiliaryTracks-auxiliaryTracks+1},error,&tracks))return false;
                while(tracks.next()) {
                    if(stopped())return false;
                    if(++auxiliaryTracks>query.maxAuxiliaryTracks) {error="辅助分层道检视数量超过预算";return false;}
                    if(!chargeAuxiliaryBytes(256+tracks.value(1).toLongLong()))return false;
                    const auto trackId=tracks.value(0).toLongLong();
                    IntervalTrack info;if(!readIntervalTrackInfo(trackId,info,error))return false;
                    if(!sameDepth(rangeDepth,info.depthInfo))continue;
                    const auto generated=object(q(info.sourceSheet));
                    if(generated["generator"].toString()=="paleo.seismic_stratigraphy" &&
                       (sel.source!=StratigraphicTrackSource::SeismicFramework || generated["schemeId"].toString()!=q(scheme.id) ||
                        generated["schemeRevision"].toString()!=q(scheme.revision.value)))continue;
                    IntervalTrackResult group;group.trackId=trackId;group.wellId=wellId;group.name=info.trackName;group.depthInfo=info.depthInfo;
                    // The track/depth index provides bounded streaming without
                    // sorting an unbounded source window. Restore display order
                    // after the bounded records have been selected.
                    const auto textLimit=std::min(1048576LL,query.maxAuxiliaryBytes-auxiliaryBytes);
                    QSqlQuery rows;
                    if(!st.exec("SELECT id,track_id,ordinal,original_id,"
                                "CASE WHEN coalesce(length(CAST(name AS BLOB)),0)<=? THEN name END,"
                                "top,bottom,top_tvd,bottom_tvd,top_ss,bottom_ss,"
                                "CASE WHEN coalesce(length(CAST(facies AS BLOB)),0)<=? THEN facies END,"
                                "top_unc,bottom_unc,not_penetrated,"
                                "CASE WHEN coalesce(length(CAST(properties AS BLOB)),0)<=? THEN properties END,source_row,"
                                "coalesce(length(CAST(name AS BLOB)),0)+coalesce(length(CAST(facies AS BLOB)),0)+coalesce(length(CAST(properties AS BLOB)),0) "
                                "FROM intervals WHERE track_id=? ORDER BY top,bottom,id LIMIT ?",
                                {textLimit,textLimit,textLimit,trackId,query.maxAuxiliaryRecords-auxiliaryRecords+1},error,&rows))return false;
                    while(rows.next()) {
                        if(stopped())return false;
                        if(++auxiliaryRecords>query.maxAuxiliaryRecords) {error="辅助分层记录检视数量超过预算";return false;}
                        const auto textBytes=rows.value(17).toLongLong();
                        if(textBytes>textLimit || !chargeAuxiliaryBytes(128+textBytes)) {
                            if(error.empty())error="单条辅助分层文本超过1 MiB或剩余字节预算";
                            return false;
                        }
                        const auto top=value(rows.value(5)),bottom=value(rows.value(6));
                        if((isFinite(bottom)&&bottom<slice.topDepth)||(isFinite(top)&&top>slice.bottomDepth))continue;
                        IntervalRecord record;record.id=rows.value(0).toLongLong();record.trackId=rows.value(1).toLongLong();
                        auto& interval=record.value;interval.displayOrder=rows.value(2).toInt();interval.intervalId=rows.value(3).toInt();
                        interval.intervalName=s(rows.value(4).toString());interval.topDepth=top;interval.bottomDepth=bottom;
                        interval.topTvd=value(rows.value(7));interval.bottomTvd=value(rows.value(8));
                        interval.topTvdss=value(rows.value(9));interval.bottomTvdss=value(rows.value(10));
                        interval.faciesType=s(rows.value(11).toString());interval.topUnconformity=rows.value(12).toInt();
                        interval.bottomUnconformity=rows.value(13).toInt();interval.bottomNotPenetrated=rows.value(14).toInt();
                        const auto properties=QJsonDocument::fromJson(rows.value(15).toString().toUtf8()).array();
                        if(properties.size()>4096 || !chargeAuxiliaryBytes(static_cast<long long>(properties.size())*sizeof(Measurement))) {
                            if(error.empty())error="单条辅助分层属性超过4096项预算";
                            return false;
                        }
                        interval.properties=readMeasurements(properties);
                        interval.sourceRow=rows.value(16).toInt();group.intervals.push_back(std::move(record));
                    }
                    std::sort(group.intervals.begin(),group.intervals.end(),[](const auto& a,const auto& b) {
                        return std::tie(a.value.displayOrder,a.id)<std::tie(b.value.displayOrder,b.id);
                    });
                    slice.auxiliaryTracks.push_back(std::move(group));
                }
            }
        }

        pending.push_back(std::move(slice));
    }

    if(stopped())return false;
    results=std::move(pending);return true;
}

} // namespace paleo::data
