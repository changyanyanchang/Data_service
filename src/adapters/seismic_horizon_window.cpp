// 按地震层位/四等分小层（T70-T72 / T70-T72-S1..S4）读取逐道变时窗/变深窗地震数据：
// 每个空间点 (inline, crossline) / (x, y) 具有由顶底层位表面决定的独立时间与深度范围。
#include "paleo_data/service.h"
#include "segy_reader.h"
#include "store/project_store.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

namespace paleo::data {
using namespace detail;
namespace fw = framework;

namespace {

bool isFiniteVal(double v) { return std::isfinite(v); }

long long packIlXl(int il, int xl)
{
    return (static_cast<long long>(il) << 32) | static_cast<unsigned int>(xl);
}

struct HorizonSurfaceIndex {
    long long horizonId = 0;
    std::string name;
    double timeFactor = 1.0; // s -> 1000, ms -> 1
    int ilMin = std::numeric_limits<int>::max(), ilMax = std::numeric_limits<int>::min();
    int xlMin = std::numeric_limits<int>::max(), xlMax = std::numeric_limits<int>::min();
    std::unordered_map<long long, double> gridTwtMs; // (il, xl) -> positive TWT(ms)
    struct XYPt { int il = 0, xl = 0; double x = Missing, y = Missing, twtMs = Missing; };
    std::vector<XYPt> pts;

    bool load(const ProjectStore& st, long long hid,
              int filterIlMin, int filterIlMax, int filterXlMin, int filterXlMax,
              std::string& error)
    {
        horizonId = hid;
        QSqlQuery infoQuery;
        if (!st.exec("SELECT name, time_unit FROM horizons WHERE id=?", {hid}, error, &infoQuery) || !infoQuery.next()) {
            if (error.empty()) error = "地震层位不存在: " + std::to_string(hid);
            return false;
        }
        name = s(infoQuery.value(0).toString());
        const std::string tunit = s(infoQuery.value(1).toString().trimmed());
        timeFactor = (tunit == "s" || tunit == "S") ? 1000.0 : 1.0;

        QString sql = "SELECT inline_no, crossline_no, x, y, z FROM horizon_points WHERE horizon_id=?";
        QVariantList args{hid};
        if (filterIlMin <= filterIlMax && filterIlMin > std::numeric_limits<int>::min() / 2 &&
            filterIlMax < std::numeric_limits<int>::max() / 2) {
            sql += " AND inline_no BETWEEN ? AND ?";
            args << (filterIlMin - 4) << (filterIlMax + 4);
        }
        if (filterXlMin <= filterXlMax && filterXlMin > std::numeric_limits<int>::min() / 2 &&
            filterXlMax < std::numeric_limits<int>::max() / 2) {
            sql += " AND crossline_no BETWEEN ? AND ?";
            args << (filterXlMin - 4) << (filterXlMax + 4);
        }

        QSqlQuery qPts;
        if (!st.exec(sql, args, error, &qPts)) return false;
        while (qPts.next()) {
            const int il = qPts.value(0).toInt();
            const int xl = qPts.value(1).toInt();
            const double x = value(qPts.value(2));
            const double y = value(qPts.value(3));
            const double z = value(qPts.value(4));
            if (!isFiniteVal(z)) continue;
            const double twt = std::abs(z) * timeFactor;
            if (il != 0 || xl != 0) {
                gridTwtMs[packIlXl(il, xl)] = twt;
                ilMin = std::min(ilMin, il);
                ilMax = std::max(ilMax, il);
                xlMin = std::min(xlMin, xl);
                xlMax = std::max(xlMax, xl);
            }
            pts.push_back({il, xl, x, y, twt});
        }
        return !pts.empty();
    }

    double sampleAt(int il, int xl, double x, double y) const
    {
        auto it = gridTwtMs.find(packIlXl(il, xl));
        if (it != gridTwtMs.end()) return it->second;

        // 尝试周围 2 步以内的网格邻域插值
        double wSum = 0.0, vSum = 0.0;
        for (int dil = -2; dil <= 2; ++dil) {
            for (int dxl = -2; dxl <= 2; ++dxl) {
                if (dil == 0 && dxl == 0) continue;
                auto nit = gridTwtMs.find(packIlXl(il + dil, xl + dxl));
                if (nit != gridTwtMs.end()) {
                    const double w = 1.0 / double(dil * dil + dxl * dxl);
                    wSum += w;
                    vSum += w * nit->second;
                }
            }
        }
        if (wSum > 0.0) return vSum / wSum;

        // 若有平面 XY，按最近邻 IDW 插值
        if (isFiniteVal(x) && isFiniteVal(y) && !pts.empty()) {
            double bestD2 = std::numeric_limits<double>::infinity();
            double bestTwt = Missing;
            for (const auto& p : pts) {
                if (!isFiniteVal(p.x) || !isFiniteVal(p.y)) continue;
                const double dx = p.x - x, dy = p.y - y;
                const double d2 = dx * dx + dy * dy;
                if (d2 < bestD2) {
                    bestD2 = d2;
                    bestTwt = p.twtMs;
                }
            }
            if (bestD2 <= 500.0 * 500.0) return bestTwt;
        }
        return Missing;
    }
};

struct TdDepthConverter {
    struct WellCurve {
        double x = Missing, y = Missing;
        std::vector<std::pair<double, double>> twtToMd; // (twtMs, mdM)
    };
    std::vector<WellCurve> curves;

    void loadFromStore(const DataService& ds, const ProjectStore& st)
    {
        std::string ignoreErr;
        std::vector<WellInfoRecord> wells;
        if (!ds.listWellInfos(wells, ignoreErr)) return;
        for (const auto& w : wells) {
            QSqlQuery qTd;
            if (!st.exec("SELECT id FROM well_objects WHERE well_id=? AND kind='time_depth' ORDER BY id LIMIT 1",
                         {w.id}, ignoreErr, &qTd) || !qTd.next())
                continue;
            TimeDepthTable t;
            if (!ds.readTimeDepth(qTd.value(0).toLongLong(), t, ignoreErr)) continue;
            const double dScale = (t.depthInfo.depthUnit == "ft" || t.depthInfo.depthUnit == "FT") ? 0.3048 : 1.0;
            double maxAbsT = 0.0;
            for (const auto& p : t.depthTimePairs) {
                if (isFiniteVal(p.timeValue)) maxAbsT = std::max(maxAbsT, std::abs(p.timeValue));
            }
            const double tScale = (t.timeUnit == "s" || t.timeUnit == "S" || (t.timeUnit.empty() && maxAbsT > 0.0 && maxAbsT <= 15.0)) ? 1000.0 : 1.0;
            const double owt = (t.timeType == "OWT" || t.timeType == "owt") ? 2.0 : 1.0;

            WellCurve wc;
            wc.x = w.value.wellheadX;
            wc.y = w.value.wellheadY;
            for (const auto& p : t.depthTimePairs) {
                if (!isFiniteVal(p.depthValue) || !isFiniteVal(p.timeValue)) continue;
                const double md = p.depthValue * dScale;
                const double twt = std::abs(p.timeValue) * tScale * owt;
                if (md >= 0.0 && twt >= 0.0) {
                    wc.twtToMd.push_back({twt, md});
                    if (!isFiniteVal(wc.x) && isFiniteVal(p.x) && isFiniteVal(p.y)) {
                        wc.x = p.x;
                        wc.y = p.y;
                    }
                }
            }
            if (wc.twtToMd.size() < 2) continue;
            std::sort(wc.twtToMd.begin(), wc.twtToMd.end());
            curves.push_back(std::move(wc));
        }
    }

    static double evalSingle(const WellCurve& wc, double twtMs)
    {
        if (wc.twtToMd.size() < 2 || !isFiniteVal(twtMs)) return Missing;
        if (twtMs <= wc.twtToMd.front().first) {
            const auto& a = wc.twtToMd[0];
            const auto& b = wc.twtToMd[1];
            const double dt = b.first - a.first;
            return dt > 1e-6 ? std::max(0.0, a.second + (twtMs - a.first) * (b.second - a.second) / dt) : a.second;
        }
        if (twtMs >= wc.twtToMd.back().first) {
            const auto& a = wc.twtToMd[wc.twtToMd.size() - 2];
            const auto& b = wc.twtToMd.back();
            const double dt = b.first - a.first;
            return dt > 1e-6 ? (b.second + (twtMs - b.first) * (b.second - a.second) / dt) : b.second;
        }
        auto it = std::lower_bound(wc.twtToMd.begin(), wc.twtToMd.end(), std::make_pair(twtMs, -1.0));
        if (it == wc.twtToMd.begin()) return it->second;
        const auto& hi = *it;
        const auto& lo = *(it - 1);
        const double dt = hi.first - lo.first;
        return dt > 1e-6 ? (lo.second + (twtMs - lo.first) * (hi.second - lo.second) / dt) : lo.second;
    }

    double depthAt(double x, double y, double twtMs) const
    {
        if (curves.empty() || !isFiniteVal(twtMs)) return Missing;
        if (!isFiniteVal(x) || !isFiniteVal(y)) {
            double sum = 0.0;
            int cnt = 0;
            for (const auto& wc : curves) {
                const double d = evalSingle(wc, twtMs);
                if (isFiniteVal(d)) { sum += d; ++cnt; }
            }
            return cnt > 0 ? (sum / cnt) : Missing;
        }
        double wSum = 0.0, dSum = 0.0;
        for (const auto& wc : curves) {
            const double d = evalSingle(wc, twtMs);
            if (!isFiniteVal(d)) continue;
            if (!isFiniteVal(wc.x) || !isFiniteVal(wc.y)) continue;
            const double dx = wc.x - x, dy = wc.y - y;
            const double dist2 = dx * dx + dy * dy;
            if (dist2 <= 1.0) return d;
            const double w = 1.0 / dist2;
            wSum += w;
            dSum += w * d;
        }
        if (wSum > 0.0) return dSum / wSum;
        return evalSingle(curves.front(), twtMs);
    }
};

long long resolveHorizonIdByName(const ProjectStore& st, const std::string& name)
{
    if (name.empty()) return 0;
    std::string ignoreErr;
    QSqlQuery qH;
    if (!st.exec("SELECT h.id, COUNT(p.id) AS cnt FROM horizons h "
                 "LEFT JOIN horizon_points p ON p.horizon_id = h.id "
                 "WHERE h.name=? GROUP BY h.id ORDER BY cnt DESC, h.id ASC LIMIT 1",
                 {q(name)}, ignoreErr, &qH) || !qH.next())
        return 0;
    return qH.value(0).toLongLong();
}

int nearestLine(const std::vector<int>& axis, double value)
{
    const auto it = std::lower_bound(axis.begin(), axis.end(), value);
    if (it == axis.begin()) return *it;
    if (it == axis.end()) return axis.back();
    return (value - *(it - 1) <= *it - value) ? *(it - 1) : *it;
}

float sampleTraceLinear(const std::vector<float>& amps, double t0, double dt, double tMs)
{
    if (amps.empty() || !isFiniteVal(dt) || dt <= 0.0 || !isFiniteVal(tMs))
        return std::numeric_limits<float>::quiet_NaN();
    const double pos = (tMs - t0) / dt;
    if (pos < -1e-6 || pos > double(amps.size() - 1) + 1e-6)
        return std::numeric_limits<float>::quiet_NaN();
    const double clamped = std::clamp(pos, 0.0, double(amps.size() - 1));
    const size_t i0 = static_cast<size_t>(std::floor(clamped));
    const size_t i1 = std::min(i0 + 1, amps.size() - 1);
    const float v0 = amps[i0], v1 = amps[i1];
    if (!std::isfinite(v0) && !std::isfinite(v1)) return std::numeric_limits<float>::quiet_NaN();
    if (!std::isfinite(v0)) return v1;
    if (!std::isfinite(v1)) return v0;
    const float f = static_cast<float>(clamped - double(i0));
    return v0 + f * (v1 - v0);
}

} // namespace

bool DataService::readSeismicHorizonWindow(const SeismicHorizonWindowQuery& query,
                                           SeismicHorizonWindowResult& result,
                                           std::string& error,
                                           const CancelCheck& cancelled) const
{
    error.clear();
    result = {};
    auto& st = impl_->store;
    if (!st.db.isOpen()) { error = "请先打开工区"; return false; }

    auto isCancelled = [&]() {
        if (cancelled && cancelled()) { error = "地震层位窗口读取已取消"; return true; }
        return false;
    };
    if (isCancelled()) return false;

    // 1. 解析地震体 ID（volumeId <= 0 时自动选工区第一个地震体）
    long long volumeId = query.volumeId;
    if (volumeId <= 0) {
        std::vector<DataEntry> vols;
        if (!listData("volumes", 0, vols, error)) return false;
        if (vols.empty()) { error = "工区中没有已导入的地震体"; return false; }
        volumeId = vols.front().id;
    }
    SeismicVolume volume;
    if (!readVolume(volumeId, volume, error)) return false;
    result.volumeId = volumeId;

    // 2. 解析目标层位/四等分小层（T70-T72-S1..S4 或 T70-T72）及顶底层位资源 ID 与比例区间 [fTop, fBot]
    long long topHorizonId = query.target.topHorizonId;
    long long bottomHorizonId = query.target.bottomHorizonId;
    double fTop = 0.0, fBot = 1.0;
    int subIndex = query.target.subIndex;
    std::string unitDisplay = query.target.unitName;

    const std::string schemeId = query.target.schemeId.empty() ? "default" : query.target.schemeId;
    StratigraphicScheme scheme;
    std::string schemeErr;
    if (readStratigraphicScheme(schemeId, query.target.revision, scheme, schemeErr)) {
        result.schemeId = scheme.id;
        result.schemeRevision = scheme.revision;

        const StratigraphicSubInterval* sub = nullptr;
        const StratigraphicInterval* iv = nullptr;
        if (!query.target.subIntervalId.empty())
            sub = fw::resolveSubInterval(scheme, query.target.subIntervalId.value);
        if (!sub && !query.target.unitName.empty())
            sub = fw::resolveSubInterval(scheme, query.target.unitName);
        if (sub) {
            iv = fw::findInterval(scheme, sub->interval);
            subIndex = sub->index;
            result.subIntervalId = sub->id;
        } else {
            if (!query.target.intervalId.empty())
                iv = fw::resolveInterval(scheme, query.target.intervalId.value);
            if (!iv && !query.target.unitName.empty())
                iv = fw::resolveInterval(scheme, query.target.unitName);
            if (iv && subIndex >= 1 && subIndex <= SubIntervalCount) {
                for (const auto* s : fw::subIntervalsOf(scheme, iv->id)) {
                    if (s->index == subIndex) { sub = s; result.subIntervalId = s->id; break; }
                }
            }
        }

        if (iv) {
            result.intervalId = iv->id;
            const auto* tb = fw::findBoundary(scheme, iv->top);
            const auto* bb = fw::findBoundary(scheme, iv->bottom);
            if (tb) {
                if (topHorizonId <= 0 && tb->seismicHorizonResourceId > 0)
                    topHorizonId = tb->seismicHorizonResourceId;
                if (topHorizonId <= 0)
                    topHorizonId = resolveHorizonIdByName(st, tb->name);
            }
            if (bb) {
                if (bottomHorizonId <= 0 && bb->seismicHorizonResourceId > 0)
                    bottomHorizonId = bb->seismicHorizonResourceId;
                if (bottomHorizonId <= 0)
                    bottomHorizonId = resolveHorizonIdByName(st, bb->name);
            }
            if (tb && bb && unitDisplay.empty()) {
                unitDisplay = tb->name + "-" + bb->name + (subIndex >= 1 ? ("-S" + std::to_string(subIndex)) : "");
            }

            std::array<double, 5> edges{0.0, 0.25, 0.50, 0.75, 1.0};
            for (const auto& d : scheme.divisions) {
                if (d.interval == iv->id) {
                    edges = {0.0, d.fractions[0], d.fractions[1], d.fractions[2], 1.0};
                    break;
                }
            }
            if (subIndex >= 1 && subIndex <= SubIntervalCount) {
                fTop = edges[size_t(subIndex - 1)];
                fBot = edges[size_t(subIndex)];
            }
        }
    }

    // 若未建 StratigraphicScheme 或直接传了 "T70-T72-S1" / "T70-T72" 名称，支持直接按层位名解析
    if ((topHorizonId <= 0 || bottomHorizonId <= 0) && !query.target.unitName.empty()) {
        std::string label = query.target.unitName;
        for (const std::string dash : {"\xE2\x80\x93", "\xE2\x80\x94", "~"}) {
            for (size_t p; (p = label.find(dash)) != std::string::npos;) label.replace(p, dash.size(), "-");
        }
        if (label.size() > 3 && (label[label.size() - 3] == '-' || label[label.size() - 3] == '/') &&
            (label[label.size() - 2] == 'S' || label[label.size() - 2] == 's') &&
            label.back() >= '1' && label.back() <= '4') {
            if (subIndex <= 0) subIndex = label.back() - '0';
            label = label.substr(0, label.size() - 3);
        }
        const auto dashPos = label.find('-');
        if (dashPos != std::string::npos) {
            const std::string tName = label.substr(0, dashPos);
            const std::string bName = label.substr(dashPos + 1);
            if (topHorizonId <= 0) topHorizonId = resolveHorizonIdByName(st, tName);
            if (bottomHorizonId <= 0) bottomHorizonId = resolveHorizonIdByName(st, bName);
            if (subIndex >= 1 && subIndex <= SubIntervalCount && fTop == 0.0 && fBot == 1.0) {
                fTop = 0.25 * double(subIndex - 1);
                fBot = 0.25 * double(subIndex);
            }
        }
    } else if (subIndex >= 1 && subIndex <= SubIntervalCount && fTop == 0.0 && fBot == 1.0) {
        fTop = 0.25 * double(subIndex - 1);
        fBot = 0.25 * double(subIndex);
    }

    if (topHorizonId <= 0 || bottomHorizonId <= 0) {
        error = "无法解析目标层位或四等分小层的顶底地震层位资源";
        return false;
    }

    // 3. 根据查询域确定层位点预过滤范围并加载顶底层位网格
    int filterIlMin = std::numeric_limits<int>::min(), filterIlMax = std::numeric_limits<int>::max();
    int filterXlMin = std::numeric_limits<int>::min(), filterXlMax = std::numeric_limits<int>::max();
    if (query.domainKind == HorizonSpatialDomainKind::InlineSection) {
        filterIlMin = filterIlMax = query.fixedLineNumber;
        if (query.crosslineMin <= query.crosslineMax && query.crosslineMax > 0) {
            filterXlMin = query.crosslineMin;
            filterXlMax = query.crosslineMax;
        }
    } else if (query.domainKind == HorizonSpatialDomainKind::CrosslineSection) {
        filterXlMin = filterXlMax = query.fixedLineNumber;
        if (query.inlineMin <= query.inlineMax && query.inlineMax > 0) {
            filterIlMin = query.inlineMin;
            filterIlMax = query.inlineMax;
        }
    } else if (query.domainKind == HorizonSpatialDomainKind::InlineCrosslineBox) {
        if (query.inlineMin <= query.inlineMax && query.inlineMax > 0) {
            filterIlMin = query.inlineMin;
            filterIlMax = query.inlineMax;
        }
        if (query.crosslineMin <= query.crosslineMax && query.crosslineMax > 0) {
            filterXlMin = query.crosslineMin;
            filterXlMax = query.crosslineMax;
        }
    } else if (query.domainKind == HorizonSpatialDomainKind::ArbitraryTraceList && !query.explicitTraces.empty()) {
        filterIlMin = filterXlMin = std::numeric_limits<int>::max();
        filterIlMax = filterXlMax = std::numeric_limits<int>::min();
        for (const auto& [il, xl] : query.explicitTraces) {
            filterIlMin = std::min(filterIlMin, il);
            filterIlMax = std::max(filterIlMax, il);
            filterXlMin = std::min(filterXlMin, xl);
            filterXlMax = std::max(filterXlMax, xl);
        }
    }

    HorizonSurfaceIndex topSurf, botSurf;
    if (!topSurf.load(st, topHorizonId, filterIlMin, filterIlMax, filterXlMin, filterXlMax, error) ||
        !botSurf.load(st, bottomHorizonId, filterIlMin, filterIlMax, filterXlMin, filterXlMax, error))
        return false;

    if (unitDisplay.empty()) {
        unitDisplay = topSurf.name + "-" + botSurf.name + (subIndex >= 1 ? ("-S" + std::to_string(subIndex)) : "");
    }
    result.unitDisplayName = unitDisplay;
    result.subIndex = subIndex;
    result.fractionTop = fTop;
    result.fractionBottom = fBot;
    result.sampleMode = query.sampleMode;
    result.fixedSampleCount = (query.sampleMode == HorizonWindowSampleMode::ResampledFixedCount)
        ? std::max(1, query.resampledSampleCount)
        : 0;

    // 4. 打开 SEG-Y 读取器
    segy_detail::Reader reader;
    if (!reader.open(volume.dataFile, error, cancelled, true)) return false;
    const auto& facts = reader.info();
    const auto& inlines = reader.inlines();
    const auto& crosslines = reader.crosslines();
    if (inlines.empty() || crosslines.empty()) {
        error = "地震体缺少规则 Inline/Crossline 测线网格，无法按层位网格提取窗口";
        return false;
    }
    result.sampleIntervalMs = facts.sampleInterval;
    result.timeUnit = "ms";
    result.depthUnit = "m";

    // 5. 生成目标 (inline, crossline) 道列表
    std::vector<std::pair<int, int>> targetTraces;
    const int ilStride = std::max(1, query.inlineStride);
    const int xlStride = std::max(1, query.crosslineStride);

    if (query.domainKind == HorizonSpatialDomainKind::ArbitraryTraceList) {
        targetTraces = query.explicitTraces;
    } else if (query.domainKind == HorizonSpatialDomainKind::ArbitraryXYPath) {
        if (!reader.hasCoordinateMap()) {
            error = "地震体缺少可验证的平面 XY 仿射关系，无法按 XY 路径提取层位窗口";
            return false;
        }
        for (const auto& [x, y] : query.xyPoints) {
            double ilD = 0.0, xlD = 0.0;
            if (!reader.mapXY(x, y, ilD, xlD)) continue;
            const int il = nearestLine(inlines, ilD);
            const int xl = nearestLine(crosslines, xlD);
            if (targetTraces.empty() || targetTraces.back().first != il || targetTraces.back().second != xl)
                targetTraces.push_back({il, xl});
        }
    } else if (query.domainKind == HorizonSpatialDomainKind::InlineSection) {
        const int il = query.fixedLineNumber;
        const int xlLo = (query.crosslineMin <= query.crosslineMax && query.crosslineMax > 0) ? query.crosslineMin : crosslines.front();
        const int xlHi = (query.crosslineMin <= query.crosslineMax && query.crosslineMax > 0) ? query.crosslineMax : crosslines.back();
        for (size_t j = 0; j < crosslines.size(); j += size_t(xlStride)) {
            if (crosslines[j] >= xlLo && crosslines[j] <= xlHi)
                targetTraces.push_back({il, crosslines[j]});
        }
    } else if (query.domainKind == HorizonSpatialDomainKind::CrosslineSection) {
        const int xl = query.fixedLineNumber;
        const int ilLo = (query.inlineMin <= query.inlineMax && query.inlineMax > 0) ? query.inlineMin : inlines.front();
        const int ilHi = (query.inlineMin <= query.inlineMax && query.inlineMax > 0) ? query.inlineMax : inlines.back();
        for (size_t i = 0; i < inlines.size(); i += size_t(ilStride)) {
            if (inlines[i] >= ilLo && inlines[i] <= ilHi)
                targetTraces.push_back({inlines[i], xl});
        }
    } else {
        // FullHorizonGrid 或 InlineCrosslineBox
        int ilLo = std::max(inlines.front(), std::max(topSurf.ilMin, botSurf.ilMin));
        int ilHi = std::min(inlines.back(), std::min(topSurf.ilMax, botSurf.ilMax));
        int xlLo = std::max(crosslines.front(), std::max(topSurf.xlMin, botSurf.xlMin));
        int xlHi = std::min(crosslines.back(), std::min(topSurf.xlMax, botSurf.xlMax));
        if (query.domainKind == HorizonSpatialDomainKind::InlineCrosslineBox) {
            if (query.inlineMin <= query.inlineMax && query.inlineMax > 0) {
                ilLo = std::max(inlines.front(), query.inlineMin);
                ilHi = std::min(inlines.back(), query.inlineMax);
            }
            if (query.crosslineMin <= query.crosslineMax && query.crosslineMax > 0) {
                xlLo = std::max(crosslines.front(), query.crosslineMin);
                xlHi = std::min(crosslines.back(), query.crosslineMax);
            }
        }
        for (size_t i = 0; i < inlines.size(); i += size_t(ilStride)) {
            const int il = inlines[i];
            if (il < ilLo || il > ilHi) continue;
            for (size_t j = 0; j < crosslines.size(); j += size_t(xlStride)) {
                const int xl = crosslines[j];
                if (xl < xlLo || xl > xlHi) continue;
                targetTraces.push_back({il, xl});
            }
        }
    }

    if (targetTraces.empty()) {
        error = "指定空间范围内没有匹配的地震道";
        return false;
    }

    // 6. 可选：加载工区时深表用于每道 [windowTopDepthM, windowBottomDepthM] 换算
    TdDepthConverter tdConv;
    if (query.computeDepthRange) {
        tdConv.loadFromStore(*this, st);
    }

    // 7. 计算每道在目标层位/小层下的独立时间窗口 [windowTopTimeMs, windowBottomTimeMs] 与深度范围
    result.traces.resize(targetTraces.size());
    struct ValidTraceSpan {
        size_t resultIdx = 0;
        long long ordinal = -1;
        int sBegin = 0;
        int sEnd = 0; // inclusive
    };
    std::vector<ValidTraceSpan> validSpans;
    validSpans.reserve(targetTraces.size());

    const double t0 = facts.firstTime;
    const double dt = facts.sampleInterval;
    const int ns = facts.sampleCount;
    const double tEnd = t0 + double(std::max(0, ns - 1)) * dt;

    long long totalBudgetSamples = 0;
    const long long maxBudget = std::clamp(query.maxValues, 1LL, 64000000LL);

    for (size_t idx = 0; idx < targetTraces.size(); ++idx) {
        const auto [il, xl] = targetTraces[idx];
        auto& meta = result.traces[idx];
        meta.inlineNumber = il;
        meta.crosslineNumber = xl;
        meta.traceIndex = reader.traceIndex(il, xl);
        if (reader.hasCoordinateMap()) {
            reader.mapLine(il, xl, meta.x, meta.y);
        }

        const double topT = topSurf.sampleAt(il, xl, meta.x, meta.y);
        const double botT = botSurf.sampleAt(il, xl, meta.x, meta.y);
        meta.parentTopTimeMs = topT;
        meta.parentBottomTimeMs = botT;

        if (!isFiniteVal(topT) || !isFiniteVal(botT) || !(botT > topT)) {
            meta.status = 2; // 层位缺失、尖灭或顶底倒置
            ++result.reviewTraceCount;
            continue;
        }

        const double spanT = botT - topT;
        const double winTop = (topT + fTop * spanT) - std::max(0.0, query.padTopMs);
        const double winBot = (topT + fBot * spanT) + std::max(0.0, query.padBottomMs);
        meta.windowTopTimeMs = winTop;
        meta.windowBottomTimeMs = winBot;
        meta.thicknessTimeMs = winBot - winTop;

        if (query.computeDepthRange && !tdConv.curves.empty()) {
            meta.windowTopDepthM = tdConv.depthAt(meta.x, meta.y, winTop);
            meta.windowBottomDepthM = tdConv.depthAt(meta.x, meta.y, winBot);
            if (isFiniteVal(meta.windowTopDepthM) && isFiniteVal(meta.windowBottomDepthM) &&
                meta.windowBottomDepthM >= meta.windowTopDepthM) {
                meta.thicknessDepthM = meta.windowBottomDepthM - meta.windowTopDepthM;
            }
        }

        if (winBot < t0 || winTop > tEnd) {
            meta.status = 2;
            ++result.reviewTraceCount;
            continue;
        }

        if (meta.traceIndex < 0) {
            meta.status = 0; // 缺地震道
            continue;
        }

        const int sBegin = std::clamp(static_cast<int>(std::floor((std::max(winTop, t0) - t0) / dt)), 0, ns - 1);
        const int sEnd = std::clamp(static_cast<int>(std::ceil((std::min(winBot, tEnd) - t0) / dt)), sBegin, ns - 1);
        const int sCount = sEnd - sBegin + 1;

        meta.status = 1;
        meta.nativeSampleCount = sCount;
        meta.firstSampleTimeMs = t0 + double(sBegin) * dt;
        if (query.sampleMode == HorizonWindowSampleMode::NativeVariableSamples) {
            meta.sampleOffset = totalBudgetSamples;
            totalBudgetSamples += sCount;
        } else if (query.sampleMode == HorizonWindowSampleMode::ResampledFixedCount) {
            meta.sampleOffset = static_cast<long long>(idx) * result.fixedSampleCount;
            totalBudgetSamples = static_cast<long long>(targetTraces.size()) * result.fixedSampleCount;
        }
        if (totalBudgetSamples > maxBudget) {
            error = "层位窗口样点总数超过 maxValues 预算上限，请增大步长、缩小范围或提高预算";
            return false;
        }

        validSpans.push_back({idx, meta.traceIndex, sBegin, sEnd});
        ++result.validTraceCount;
    }

    // 预分配输出振幅数组
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    if (query.sampleMode == HorizonWindowSampleMode::NativeVariableSamples) {
        result.amplitudes.assign(static_cast<size_t>(totalBudgetSamples), qnan);
    } else if (query.sampleMode == HorizonWindowSampleMode::ResampledFixedCount) {
        result.amplitudes.assign(targetTraces.size() * static_cast<size_t>(result.fixedSampleCount), qnan);
    }

    if (validSpans.empty()) return true;

    // 8. 分批从 SEG-Y 读取各道在对应时间窗口内的波形，并计算变窗重采样与窗口属性
    constexpr size_t BatchLimit = 1024;
    segy_detail::BatchReadOptions batchOpts;
    batchOpts.ioMode = "auto";
    batchOpts.collectTimings = false;

    for (size_t start = 0; start < validSpans.size(); start += BatchLimit) {
        if (isCancelled()) return false;
        const size_t end = std::min(validSpans.size(), start + BatchLimit);
        int batchMinSample = ns - 1;
        int batchMaxSample = 0;
        std::vector<long long> ordinals;
        ordinals.reserve(end - start);
        for (size_t i = start; i < end; ++i) {
            batchMinSample = std::min(batchMinSample, validSpans[i].sBegin);
            batchMaxSample = std::max(batchMaxSample, validSpans[i].sEnd);
            ordinals.push_back(validSpans[i].ordinal);
        }
        const int batchSampleCount = batchMaxSample - batchMinSample + 1;

        segy_detail::TraceWindowBatch batch;
        SeismicReadStatistics batchStats;
        if (!reader.readTraceBatch(ordinals, batchMinSample, batchSampleCount, batchOpts,
                                   batch, batchStats, error, cancelled))
            return false;

        result.statistics.readCalls += batchStats.readCalls;
        result.statistics.bytesRead += batchStats.bytesRead;
        result.statistics.tracesRead += batchStats.tracesRead;

        const double batchT0 = t0 + double(batchMinSample) * dt;

        for (size_t localIdx = 0; localIdx < end - start; ++localIdx) {
            const auto& span = validSpans[start + localIdx];
            auto& meta = result.traces[span.resultIdx];
            if (localIdx < batch.x.size() && isFiniteVal(batch.x[localIdx]) && isFiniteVal(batch.y[localIdx])) {
                meta.x = batch.x[localIdx];
                meta.y = batch.y[localIdx];
            }

            const float* tracePtr = batch.amplitudes.data() + localIdx * static_cast<size_t>(batchSampleCount);
            const int localOffset = span.sBegin - batchMinSample;
            const int sCount = span.sEnd - span.sBegin + 1;

            // (a) 窗口振幅统计属性（RMS / 平均绝对振幅 / 峰值 / 谷值 / 能量）
            if (query.computeWindowAttributes && sCount > 0) {
                double sumSq = 0.0, sumAbs = 0.0;
                float maxPeak = -std::numeric_limits<float>::infinity();
                float minTrough = std::numeric_limits<float>::infinity();
                int finiteCnt = 0;
                for (int k = 0; k < sCount; ++k) {
                    const float v = tracePtr[localOffset + k];
                    if (!std::isfinite(v)) continue;
                    const double dv = static_cast<double>(v);
                    sumSq += dv * dv;
                    sumAbs += std::abs(dv);
                    maxPeak = std::max(maxPeak, v);
                    minTrough = std::min(minTrough, v);
                    ++finiteCnt;
                }
                if (finiteCnt > 0) {
                    meta.rmsAmplitude = static_cast<float>(std::sqrt(sumSq / double(finiteCnt)));
                    meta.meanAbsAmplitude = static_cast<float>(sumAbs / double(finiteCnt));
                    meta.maxPeakAmplitude = maxPeak;
                    meta.minTroughAmplitude = minTrough;
                    meta.energyAmplitude = static_cast<float>(sumSq);
                }
            }

            // (b) 拷贝原生变长样点或等比例重采样为固定 K 点
            if (query.sampleMode == HorizonWindowSampleMode::NativeVariableSamples) {
                std::copy(tracePtr + localOffset, tracePtr + localOffset + sCount,
                          result.amplitudes.begin() + meta.sampleOffset);
            } else if (query.sampleMode == HorizonWindowSampleMode::ResampledFixedCount) {
                const int K = result.fixedSampleCount;
                std::vector<float> sliceVec(tracePtr, tracePtr + batchSampleCount);
                float* dst = result.amplitudes.data() + span.resultIdx * static_cast<size_t>(K);
                for (int k = 0; k < K; ++k) {
                    const double frac = (K == 1) ? 0.5 : (double(k) / double(K - 1));
                    const double tSample = meta.windowTopTimeMs + frac * (meta.windowBottomTimeMs - meta.windowTopTimeMs);
                    dst[k] = sampleTraceLinear(sliceVec, batchT0, dt, tSample);
                }
            }
        }
    }

    return true;
}

} // namespace paleo::data
