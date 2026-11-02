// 按地震层位/四等分小层（T70-T72 / T70-T72-S1..S4）读取逐道变时窗/变深窗地震数据：
// 每个空间点 (inline, crossline) / (x, y) 具有由顶底层位表面决定的独立时间与深度范围。
#include "paleo_data/service.h"
#include "segy_reader.h"
#include "store/project_store.h"
#include "store/horizon_grid_sampler.h"
#include <memory>
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

struct HorizonSurfaceIndex {
    std::string name;double scale=1.;std::unique_ptr<GridSampler> grid;
    int ilMin=std::numeric_limits<int>::min(),ilMax=std::numeric_limits<int>::max();
    int xlMin=std::numeric_limits<int>::min(),xlMax=std::numeric_limits<int>::max();
    bool load(const DataService& ds,const ProjectStore& st,const SeismicVolume& volume,long long id,
              const SeismicHorizonWindowQuery& query,std::string& error){
        SeismicHorizon h;if(!ds.readHorizon(id,0,1,h,error))return false;
        if((h.parentVolumeId&&h.parentVolumeId!=query.volumeId&&query.volumeId>0)||
           h.coordinateSystem!=volume.geometry.coordinateSystem||h.coordinateUnit!=volume.geometry.coordinateUnit){
            error="层位与所选地震体的归属或坐标约定不同";return false;
        }
        if(!h.timeReference.empty()&&!volume.sampling.timeReference.empty()&&h.timeReference!=volume.sampling.timeReference){
            error="层位与地震体的时间基准不同";return false;
        }
        const auto unit=h.timeUnit.empty()?(volume.sampling.timeUnit.empty()?query.undeclaredTimeUnit:volume.sampling.timeUnit):h.timeUnit;
        const auto volumeType=volume.sampling.timeType.empty()?query.undeclaredTimeType:volume.sampling.timeType;
        const auto type=h.timeType.empty()?volumeType:h.timeType;
        if(volumeType!="TWT"||(unit!="ms"&&unit!="s")||(type!="TWT"&&type!="OWT")){
            error="地震层段读取需要明确的时间单位及 TWT 地震体约定";return false;
        }
        name=h.horizonName;scale=(unit=="s"?1000.:1.)*(type=="OWT"?2.:1.);
        grid=std::make_unique<GridSampler>(st,id,64);return grid->prepare(error);
    }
    double sampleAt(int il,int xl,std::string& error){return grid->sample(il,xl,error)*scale;}
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
    SeismicHorizonWindowResult next;
    auto& st = impl_->store;
    if (!st.db.isOpen()) { error = "请先打开工区"; return false; }

    auto isCancelled = [&]() {
        if (cancelled && cancelled()) { error = "地震层位窗口读取已取消"; return true; }
        return false;
    };
    if (isCancelled()) return false;

    if(!st.exec("BEGIN",{},error))return false;
    struct Snapshot{const ProjectStore& store;~Snapshot(){std::string ignored;store.exec("ROLLBACK",{},ignored);}}snapshot{st};
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
    next.volumeId = volumeId;

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
        next.schemeId = scheme.id;
        next.schemeRevision = scheme.revision;

        const StratigraphicSubInterval* sub = nullptr;
        const StratigraphicInterval* iv = nullptr;
        if (!query.target.subIntervalId.empty())
            sub = fw::resolveSubInterval(scheme, query.target.subIntervalId.value);
        if (!sub && !query.target.unitName.empty())
            sub = fw::resolveSubInterval(scheme, query.target.unitName);
        if (sub) {
            iv = fw::findInterval(scheme, sub->interval);
            subIndex = sub->index;
            next.subIntervalId = sub->id;
        } else {
            if (!query.target.intervalId.empty())
                iv = fw::resolveInterval(scheme, query.target.intervalId.value);
            if (!iv && !query.target.unitName.empty())
                iv = fw::resolveInterval(scheme, query.target.unitName);
            if (iv && subIndex >= 1 && subIndex <= SubIntervalCount) {
                for (const auto* s : fw::subIntervalsOf(scheme, iv->id)) {
                    if (s->index == subIndex) { sub = s; next.subIntervalId = s->id; break; }
                }
            }
        }

        if (iv) {
            next.intervalId = iv->id;
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

    // The same indexed, signed, null-preserving surface sampler as section overlays.
    HorizonSurfaceIndex topSurf,botSurf;
    auto boundQuery=query;boundQuery.volumeId=volumeId;
    if(!topSurf.load(*this,st,volume,topHorizonId,boundQuery,error)||
       !botSurf.load(*this,st,volume,bottomHorizonId,boundQuery,error))return false;

    if (unitDisplay.empty()) {
        unitDisplay = topSurf.name + "-" + botSurf.name + (subIndex >= 1 ? ("-S" + std::to_string(subIndex)) : "");
    }
    next.unitDisplayName = unitDisplay;
    next.subIndex = subIndex;
    next.fractionTop = fTop;
    next.fractionBottom = fBot;
    next.sampleMode = query.sampleMode;
    next.fixedSampleCount = (query.sampleMode == HorizonWindowSampleMode::ResampledFixedCount)
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
    next.sampleIntervalMs = facts.sampleInterval;
    next.timeUnit = "ms";
    next.depthUnit = "m";

    // 5. 生成目标 (inline, crossline) 道列表
    std::vector<std::pair<int, int>> targetTraces;
    const auto appendTarget=[&](int il,int xl){
      if(targetTraces.size()>=65536){error="层位窗口超过道数预算，请增大测线步长或缩小范围";return false;}
      targetTraces.push_back({il,xl});return true;
    };
    const int ilStride = std::max(1, query.inlineStride);
    const int xlStride = std::max(1, query.crosslineStride);

    if (query.domainKind == HorizonSpatialDomainKind::ArbitraryTraceList) {
        if(query.explicitTraces.size()>65536){error="显式地震道列表超过预算";return false;}
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
                if(!appendTarget(il,xl))return false;
        }
    } else if (query.domainKind == HorizonSpatialDomainKind::InlineSection) {
        const int il = query.fixedLineNumber;
        const int xlLo = (query.crosslineMin <= query.crosslineMax && query.crosslineMax > 0) ? query.crosslineMin : crosslines.front();
        const int xlHi = (query.crosslineMin <= query.crosslineMax && query.crosslineMax > 0) ? query.crosslineMax : crosslines.back();
        for (size_t j = 0; j < crosslines.size(); j += size_t(xlStride)) {
            if (crosslines[j] >= xlLo && crosslines[j] <= xlHi)
                if(!appendTarget(il,crosslines[j]))return false;
        }
    } else if (query.domainKind == HorizonSpatialDomainKind::CrosslineSection) {
        const int xl = query.fixedLineNumber;
        const int ilLo = (query.inlineMin <= query.inlineMax && query.inlineMax > 0) ? query.inlineMin : inlines.front();
        const int ilHi = (query.inlineMin <= query.inlineMax && query.inlineMax > 0) ? query.inlineMax : inlines.back();
        for (size_t i = 0; i < inlines.size(); i += size_t(ilStride)) {
            if (inlines[i] >= ilLo && inlines[i] <= ilHi)
                if(!appendTarget(inlines[i],xl))return false;
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
                if(!appendTarget(il,xl))return false;
            }
        }
    }

    if (targetTraces.empty()) {
        error = "指定空间范围内没有匹配的地震道";
        return false;
    }

    if(targetTraces.size()>65536||query.resampledSampleCount<1||query.resampledSampleCount>4096){
        error="层位窗口超过道数或重采样预算，请增大测线步长或缩小范围";return false;
    }
    // 6. 可选：加载工区时深表用于每道 [windowTopDepthM, windowBottomDepthM] 换算
    TdDepthConverter tdConv;
    if (query.computeDepthRange) {
        tdConv.loadFromStore(*this, st);
    }

    // 7. 计算每道在目标层位/小层下的独立时间窗口 [windowTopTimeMs, windowBottomTimeMs] 与深度范围
    next.traces.resize(targetTraces.size());
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
        auto& meta = next.traces[idx];
        meta.inlineNumber = il;
        meta.crosslineNumber = xl;
        meta.traceIndex = reader.traceIndex(il, xl);
        if (reader.hasCoordinateMap()) {
            reader.mapLine(il, xl, meta.x, meta.y);
        }

        const double topT = topSurf.sampleAt(il,xl,error);
        const double botT = botSurf.sampleAt(il,xl,error);
        if(!error.empty())return false;
        meta.parentTopTimeMs = topT;
        meta.parentBottomTimeMs = botT;

        if (!isFiniteVal(topT) || !isFiniteVal(botT) || !(botT > topT)) {
            meta.status = 2; // 层位缺失、尖灭或顶底倒置
            ++next.reviewTraceCount;
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
            ++next.reviewTraceCount;
            continue;
        }

        if (meta.traceIndex < 0) {
            meta.status = 0; // 缺地震道
            continue;
        }

        const int sBegin=std::clamp(int(std::ceil((std::max(winTop,t0)-t0)/dt)),0,ns-1);
        const double lastIndex=(std::min(winBot,tEnd)-t0)/dt;
        const bool exclusiveBottom=subIndex>=1&&subIndex<4&&winBot<=tEnd;
        const int sEnd=std::clamp(exclusiveBottom?int(std::ceil(lastIndex))-1:int(std::floor(lastIndex)),0,ns-1);
        if(sEnd<sBegin){meta.status=2;++next.reviewTraceCount;continue;}
        const int sCount = sEnd - sBegin + 1;

        meta.status = 1;
        meta.nativeSampleCount = sCount;
        meta.firstSampleTimeMs = t0 + double(sBegin) * dt;
        if (query.sampleMode == HorizonWindowSampleMode::NativeVariableSamples) {
            meta.sampleOffset = totalBudgetSamples;
            totalBudgetSamples += sCount;
        } else if (query.sampleMode == HorizonWindowSampleMode::ResampledFixedCount) {
            meta.sampleOffset = static_cast<long long>(idx) * next.fixedSampleCount;
            totalBudgetSamples = static_cast<long long>(targetTraces.size()) * next.fixedSampleCount;
        }
        if (totalBudgetSamples > maxBudget) {
            error = "层位窗口样点总数超过 maxValues 预算上限，请增大步长、缩小范围或提高预算";
            return false;
        }

        validSpans.push_back({idx, meta.traceIndex, sBegin, sEnd});
        ++next.validTraceCount;
    }

    // 预分配输出振幅数组
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    if (query.sampleMode == HorizonWindowSampleMode::NativeVariableSamples) {
        next.amplitudes.assign(static_cast<size_t>(totalBudgetSamples), qnan);
    } else if (query.sampleMode == HorizonWindowSampleMode::ResampledFixedCount) {
        next.amplitudes.assign(targetTraces.size() * static_cast<size_t>(next.fixedSampleCount), qnan);
    }

    if (validSpans.empty()){result=std::move(next);return true;}

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
        // Linear interpolation needs bracketing source samples; statistics and native output stay strictly inside the unit.
        if(query.sampleMode==HorizonWindowSampleMode::ResampledFixedCount){batchMinSample=std::max(0,batchMinSample-1);batchMaxSample=std::min(ns-1,batchMaxSample+1);}
        const int batchSampleCount = batchMaxSample - batchMinSample + 1;

        segy_detail::TraceWindowBatch batch;
        SeismicReadStatistics batchStats;
        if (!reader.readTraceBatch(ordinals, batchMinSample, batchSampleCount, batchOpts,
                                   batch, batchStats, error, cancelled))
            return false;

        next.statistics.readCalls += batchStats.readCalls;
        next.statistics.bytesRead += batchStats.bytesRead;
        next.statistics.tracesRead += batchStats.tracesRead;

        const double batchT0 = t0 + double(batchMinSample) * dt;

        for (size_t localIdx = 0; localIdx < end - start; ++localIdx) {
            const auto& span = validSpans[start + localIdx];
            auto& meta = next.traces[span.resultIdx];
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
                          next.amplitudes.begin() + meta.sampleOffset);
            } else if (query.sampleMode == HorizonWindowSampleMode::ResampledFixedCount) {
                const int K = next.fixedSampleCount;
                std::vector<float> sliceVec(tracePtr, tracePtr + batchSampleCount);
                float* dst = next.amplitudes.data() + span.resultIdx * static_cast<size_t>(K);
                for (int k = 0; k < K; ++k) {
                    const double frac = (K == 1) ? 0.5 : (double(k) / double(subIndex>=1&&subIndex<4?K:K-1));
                    const double tSample = meta.windowTopTimeMs + frac * (meta.windowBottomTimeMs - meta.windowTopTimeMs);
                    dst[k] = sampleTraceLinear(sliceVec, batchT0, dt, tSample);
                }
            }
        }
    }

    result=std::move(next);
    return true;
}

} // namespace paleo::data
