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
        std::vector<std::pair<double, double>> twtToMd;
    };
    std::vector<WellCurve> curves;

    bool loadFromStore(const DataService& ds, const ProjectStore& st,
                       const SeismicVolume& volume, const SeismicHorizonWindowQuery& query,
                       SeismicHorizonWindowResult& result, const CancelCheck& cancelled,
                       std::string& error)
    {
        result.depthStatus = "unavailable";
        const auto warn = [&](const std::string& message) {
            if (result.depthWarnings.size() < 128) result.depthWarnings.push_back(message);
            else if (result.depthWarnings.size() == 128) result.depthWarnings.push_back("更多时深诊断已省略");
        };
        auto bindings = query.timeDepthBindings;
        if (bindings.empty()) {
            std::vector<CorrelationTimeDepthPreference> preferences;
            std::string why;
            if (!ds.readCorrelationTimeDepthPreferences(preferences, why)) {
                warn("无法读取已确认时深选择：" + why);return true;
            }
            for (const auto& preference : preferences)
                bindings.push_back({preference.wellId, preference.timeDepthId});
        }
        if (bindings.empty()) {warn("未指定时深关系，也没有已保存的时深优选；深度保持未知");return true;}
        if (bindings.size() > 10000) {error="时深关系选择超过10000口井预算";return false;}
        std::map<long long, long long> selected;
        for (const auto& binding : bindings)
            if (binding.wellId <= 0 || binding.timeDepthId <= 0 || !selected.emplace(binding.wellId,binding.timeDepthId).second) {
                error="时深关系选择须包含唯一正井编号及正关系编号";return false;
            }
        const auto volumeCrs=q(volume.geometry.coordinateSystem).trimmed();
        const auto volumeUnit=q(volume.geometry.coordinateUnit).trimmed().toLower();
        const auto volumeReference=q(volume.sampling.timeReference).trimmed();
        const auto volumeTimeType=q(volume.sampling.timeType.empty()?query.undeclaredTimeType:volume.sampling.timeType).trimmed().toUpper();
        const auto volumeTimeUnit=q(volume.sampling.timeUnit.empty()?query.undeclaredTimeUnit:volume.sampling.timeUnit).trimmed().toLower();
        if (volumeCrs.isEmpty() || (volumeUnit!="m" && volumeUnit!="ft") || volumeReference.isEmpty() ||
            volumeTimeType!="TWT" || (volumeTimeUnit!="ms" && volumeTimeUnit!="s")) {
            warn("地震体缺少深度换算所需的平面坐标或已确认时间单位/类型/基准");return true;
        }
        long long pointBudget=1000000;
        QString commonMdReference;
        bool referenceConflict=false;
        for (const auto& [wellId, relationId] : selected) {
            if (cancelled && cancelled()) {error="地震层位窗口读取已取消";return false;}
            const auto prefix="井 "+std::to_string(wellId)+" / 时深 "+std::to_string(relationId)+"：";
            QSqlQuery row;
            if (!st.exec("SELECT o.well_id,coalesce(s.active,0),json_array_length(o.payload,'$.points') "
                         "FROM well_objects o LEFT JOIN sources s ON s.id=o.source_id WHERE o.id=? AND o.kind='time_depth'",
                         {relationId},error,&row)) return false;
            if (!row.next() || row.value(0).toLongLong()!=wellId || !row.value(1).toBool()) {
                warn(prefix+"关系不存在、已停用或不属于指定井");continue;
            }
            const auto count=row.value(2).toLongLong();row.finish();
            if(count>pointBudget) {error="所选时深关系超过总计100万采样点预算，未截断IDW关系集合";return false;}
            if (count<2) {warn(prefix+"时深采样点不足");continue;}
            pointBudget-=count;
            TimeDepthTable table;WellInfo well;std::string why;
            if (!ds.readTimeDepth(relationId,table,why) || !ds.readWellInfo(wellId,well,why)) {warn(prefix+why);continue;}
            const auto depthType=q(table.depthInfo.depthType).trimmed().toUpper();
            const auto depthUnit=q(table.depthInfo.depthUnit).trimmed().toLower();
            const auto mdReference=q(table.depthInfo.mdReference).trimmed();
            const auto timeUnit=q(table.timeUnit).trimmed().toLower();
            const auto timeType=q(table.timeType).trimmed().toUpper();
            if (depthType!="MD" || (depthUnit!="m" && depthUnit!="ft") || mdReference.isEmpty() ||
                (timeUnit!="ms" && timeUnit!="s") || (timeType!="TWT" && timeType!="OWT") ||
                q(table.timeReference).trimmed()!=volumeReference || (!table.timeSign.empty() && table.timeSign!="negated")) {
                warn(prefix+"深度类型/单位/起算面、时间类型/单位/符号或时间基准未确认/不匹配");continue;
            }
            WellCurve curve;curve.x=well.wellheadX;curve.y=well.wellheadY;
            auto crs=q(well.coordinateSystem).trimmed();auto unit=q(well.coordinateUnit).trimmed().toLower();
            if (!isFiniteVal(curve.x) || !isFiniteVal(curve.y)) {
                crs=q(table.coordinateSystem).trimmed();unit=q(table.coordinateUnit).trimmed().toLower();
                for (const auto& point:table.depthTimePairs)
                    if (isFiniteVal(point.x) && isFiniteVal(point.y)) {curve.x=point.x;curve.y=point.y;break;}
            }
            if (!isFiniteVal(curve.x) || !isFiniteVal(curve.y) || crs!=volumeCrs || unit!=volumeUnit) {
                warn(prefix+"缺少与地震体一致的平面坐标约定");continue;
            }
            const double depthScale=depthUnit=="ft"?0.3048:1.;
            const double timeScale=(timeUnit=="s"?1000.:1.)*(timeType=="OWT"?2.:1.);
            bool valid=true;
            // readTimeDepth already applies a confirmed negated sign. Preserve the
            // returned signed values; never take abs or guess seconds from range.
            size_t pointIndex=0;
            for (const auto& point:table.depthTimePairs) {
                if((pointIndex++&4095)==0 && cancelled && cancelled()) {error="地震层位窗口读取已取消";return false;}
                const double md=point.depthValue*depthScale,twt=point.timeValue*timeScale;
                if (!isFiniteVal(md) || !isFiniteVal(twt)) {valid=false;break;}
                curve.twtToMd.push_back({twt,md});
            }
            std::sort(curve.twtToMd.begin(),curve.twtToMd.end());
            for (size_t i=1;i<curve.twtToMd.size();++i)
                if (curve.twtToMd[i].first<=curve.twtToMd[i-1].first || curve.twtToMd[i].second<=curve.twtToMd[i-1].second) valid=false;
            if (!valid || curve.twtToMd.size()<2) {warn(prefix+"含缺失、重复或非单调采样，不能建立唯一时深换算");continue;}
            if (commonMdReference.isEmpty()) commonMdReference=mdReference;
            else if (commonMdReference!=mdReference) referenceConflict=true;
            curves.push_back(std::move(curve));result.usedTimeDepthBindings.push_back({wellId,relationId});
        }
        if (referenceConflict) {
            curves.clear();result.usedTimeDepthBindings.clear();warn("所选井时深关系的MD起算面不一致，未混合插值");
        }
        if (!curves.empty()) {result.depthType="MD";result.depthReference=s(commonMdReference);}
        return true;
    }

    static double evalSingle(const WellCurve& curve, double twtMs)
    {
        if (curve.twtToMd.size()<2 || !isFiniteVal(twtMs) ||
            twtMs<curve.twtToMd.front().first || twtMs>curve.twtToMd.back().first) return Missing;
        const auto it=std::lower_bound(curve.twtToMd.begin(),curve.twtToMd.end(),std::make_pair(twtMs,-std::numeric_limits<double>::infinity()));
        if (it==curve.twtToMd.begin() || it->first==twtMs) return it->second;
        if (it==curve.twtToMd.end()) return Missing;
        const auto& hi=*it;const auto& lo=*(it-1);
        return lo.second+(twtMs-lo.first)*(hi.second-lo.second)/(hi.first-lo.first);
    }

    double depthAt(double x, double y, double twtMs, const CancelCheck& cancelled, std::string& error) const
    {
        if (curves.empty() || !isFiniteVal(twtMs) || !isFiniteVal(x) || !isFiniteVal(y)) return Missing;
        double weightSum=0.,depthSum=0.;
        for (size_t i=0;i<curves.size();++i) {
            if((i&127)==0 && cancelled && cancelled()) {error="地震层位窗口读取已取消";return Missing;}
            const auto& curve=curves[i];
            const double depth=evalSingle(curve,twtMs);if(!isFiniteVal(depth))continue;
            const double dx=curve.x-x,dy=curve.y-y,distanceSquared=dx*dx+dy*dy;
            if (!isFiniteVal(distanceSquared)) continue;
            if (distanceSquared<=1e-12) return depth;
            const double weight=1./distanceSquared;weightSum+=weight;depthSum+=weight*depth;
        }
        return weightSum>0.?depthSum/weightSum:Missing;
    }
};

bool resolveHorizonIdByName(const ProjectStore& st, const std::string& name, long long& id, std::string& error)
{
    id=0;
    if (name.empty()) return true;
    QSqlQuery qH;
    if (!st.exec("SELECT id FROM horizons WHERE name=? ORDER BY id LIMIT 2",{q(name)},error,&qH))return false;
    if (!qH.next()) return true;
    id=qH.value(0).toLongLong();
    if(qH.next()) {id=0;error="地震层位名称存在歧义，请指定准确的层位资源编号："+name;return false;}
    return true;
}

bool resolveSchemeName(const StratigraphicScheme& scheme, const std::string& name,
                       const StratigraphicSubInterval*& sub, const StratigraphicInterval*& interval,
                       std::string& error)
{
    size_t matches=0;
    for(const auto& candidate:scheme.subIntervals) {
        const auto* parent=fw::findInterval(scheme,candidate.interval);
        const auto* top=parent?fw::findBoundary(scheme,parent->top):nullptr;
        const auto* bottom=parent?fw::findBoundary(scheme,parent->bottom):nullptr;
        const auto base=top&&bottom?top->name+"-"+bottom->name:std::string();
        const auto index=std::to_string(candidate.index);
        if(candidate.id.value==name || fw::displayName(scheme,candidate.id)==name ||
           (!base.empty() && (name==base+"-S"+index || name==base+"-s"+index || name==base+"/S"+index || name==base+"/s"+index))) {
            sub=&candidate;++matches;
        }
    }
    for(const auto& candidate:scheme.intervals) {
        const auto* top=fw::findBoundary(scheme,candidate.top);
        const auto* bottom=fw::findBoundary(scheme,candidate.bottom);
        if(candidate.id.value==name || fw::displayName(scheme,candidate.id)==name ||
           (top&&bottom&&top->name+"-"+bottom->name==name)) {interval=&candidate;++matches;}
    }
    if(matches>1) {error="方案中的层段名称存在歧义，请使用强类型层段编号："+name;return false;}
    if(sub)interval=fw::findInterval(scheme,sub->interval);
    return true;
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

    if(query.target.topHorizonId<0 || query.target.bottomHorizonId<0 || query.target.subIndex<0 || query.target.subIndex>SubIntervalCount ||
       (query.computeDepthRange && (query.maxDepthEvaluations<1 || query.maxDepthEvaluations>64000000))) {
        error="层位资源编号、小层编号或深度计算预算无效";return false;
    }

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

    const bool fixedScheme=!query.target.schemeId.empty() || !query.target.revision.empty() ||
        !query.target.subIntervalId.empty() || !query.target.intervalId.empty();
    const std::string schemeId = query.target.schemeId.empty() ? "default" : query.target.schemeId;
    StratigraphicScheme scheme;
    std::string schemeErr;
    bool schemeTarget=false;
    if (fixedScheme || topHorizonId<=0 || bottomHorizonId<=0) {
        const bool foundScheme=readStratigraphicScheme(schemeId,query.target.revision,scheme,schemeErr);
        if(!foundScheme && fixedScheme) {error="指定方案或修订不可读取："+schemeErr;return false;}
        if(foundScheme) {
        const StratigraphicSubInterval* sub = nullptr;
        const StratigraphicInterval* iv = nullptr;
        if (!query.target.subIntervalId.empty()) {
            sub=fw::findSubInterval(scheme,query.target.subIntervalId);
            if(!sub) {error="指定小层编号不属于该方案修订";return false;}
            iv = fw::findInterval(scheme, sub->interval);
            if((!query.target.intervalId.empty() && query.target.intervalId!=sub->interval) ||
               (subIndex>0 && subIndex!=sub->index)) {error="小层编号与父层编号或小层序号不一致";return false;}
        } else if(!query.target.intervalId.empty()) {
            iv=fw::findInterval(scheme,query.target.intervalId);
            if(!iv) {error="指定父层编号不属于该方案修订";return false;}
        } else if(!query.target.unitName.empty() && !resolveSchemeName(scheme,query.target.unitName,sub,iv,error))return false;
        if (sub) {
            subIndex = sub->index;
            next.subIntervalId = sub->id;
        } else {
            if (iv && subIndex >= 1 && subIndex <= SubIntervalCount) {
                for (const auto* s : fw::subIntervalsOf(scheme, iv->id)) {
                    if (s->index == subIndex) { sub = s; next.subIntervalId = s->id; break; }
                }
            }
        }

        if(!iv && fixedScheme) {error="指定方案修订中未找到目标层段；请提供该修订的层段编号";return false;}
        if (iv) {
            schemeTarget=true;next.schemeId=scheme.id;next.schemeRevision=scheme.revision;
            next.intervalId = iv->id;
            const auto* tb = fw::findBoundary(scheme, iv->top);
            const auto* bb = fw::findBoundary(scheme, iv->bottom);
            if(!tb || !bb) {error="指定层段缺少顶底界面";return false;}
            if(fixedScheme && (tb->seismicHorizonResourceId<=0 || bb->seismicHorizonResourceId<=0)) {
                error="指定方案修订的界面尚未关联明确地震层位资源，未按名称回退";return false;
            }
            if((topHorizonId>0 && tb->seismicHorizonResourceId>0 && topHorizonId!=tb->seismicHorizonResourceId) ||
               (bottomHorizonId>0 && bb->seismicHorizonResourceId>0 && bottomHorizonId!=bb->seismicHorizonResourceId)) {
                error="显式顶底层位资源与方案修订中的界面关联冲突";return false;
            }
            if (tb) {
                if (topHorizonId <= 0 && tb->seismicHorizonResourceId > 0)
                    topHorizonId = tb->seismicHorizonResourceId;
                if (topHorizonId <= 0 && !resolveHorizonIdByName(st,tb->name,topHorizonId,error))return false;
            }
            if (bb) {
                if (bottomHorizonId <= 0 && bb->seismicHorizonResourceId > 0)
                    bottomHorizonId = bb->seismicHorizonResourceId;
                if (bottomHorizonId <= 0 && !resolveHorizonIdByName(st,bb->name,bottomHorizonId,error))return false;
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
    }

    // 若未建 StratigraphicScheme 或直接传了 "T70-T72-S1" / "T70-T72" 名称，支持直接按层位名解析
    if (!fixedScheme && !schemeTarget && (topHorizonId <= 0 || bottomHorizonId <= 0) && !query.target.unitName.empty()) {
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
            if (topHorizonId <= 0 && !resolveHorizonIdByName(st,tName,topHorizonId,error))return false;
            if (bottomHorizonId <= 0 && !resolveHorizonIdByName(st,bName,bottomHorizonId,error))return false;
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
    const long long maxBudget=std::clamp(query.maxValues,1LL,64000000LL);
    // Fixed output contains an N*K tensor even for missing/review traces. Bound
    // its allocation before loading depth relations or examining valid spans.
    if(query.sampleMode==HorizonWindowSampleMode::ResampledFixedCount &&
       static_cast<long long>(targetTraces.size())*next.fixedSampleCount>maxBudget) {
        error="层位窗口固定样点数组超过 maxValues 预算上限，请减少道数或重采样点数";return false;
    }
    // 6. 可选：加载工区时深表用于每道 [windowTopDepthM, windowBottomDepthM] 换算
    TdDepthConverter tdConv;
    if (query.computeDepthRange) {
        if(!tdConv.loadFromStore(*this,st,volume,query,next,cancelled,error))return false;
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
    long long remainingDepthEvaluations=query.maxDepthEvaluations;
    std::vector<bool> depthEvaluated;
    if(query.computeDepthRange)depthEvaluated.resize(targetTraces.size(),false);
    const auto convertDepth=[&](size_t idx) {
        auto& meta=next.traces[idx];
        if(tdConv.curves.empty() || depthEvaluated[idx] || !isFiniteVal(meta.x) || !isFiniteVal(meta.y))return true;
        // Each trace is evaluated once, after either a confirmed map or the
        // existing waveform batch supplies its actual trace-header position.
        if(tdConv.curves.size()>static_cast<size_t>(remainingDepthEvaluations/2)) {
            error="层位窗口深度换算超过 maxDepthEvaluations 预算，请缩小道范围或显式调整关系集合/预算";return false;
        }
        remainingDepthEvaluations-=2*static_cast<long long>(tdConv.curves.size());
        meta.windowTopDepthM=tdConv.depthAt(meta.x,meta.y,meta.windowTopTimeMs,cancelled,error);
        if(!error.empty())return false;
        meta.windowBottomDepthM=tdConv.depthAt(meta.x,meta.y,meta.windowBottomTimeMs,cancelled,error);
        if(!error.empty())return false;
        depthEvaluated[idx]=true;
        if(isFiniteVal(meta.windowTopDepthM) && isFiniteVal(meta.windowBottomDepthM) && meta.windowBottomDepthM>=meta.windowTopDepthM) {
            meta.thicknessDepthM=meta.windowBottomDepthM-meta.windowTopDepthM;meta.depthStatus=1;
        } else {meta.windowTopDepthM=Missing;meta.windowBottomDepthM=Missing;}
        return true;
    };

    for (size_t idx = 0; idx < targetTraces.size(); ++idx) {
        if(isCancelled())return false;
        const auto [il, xl] = targetTraces[idx];
        auto& meta = next.traces[idx];
        if(query.computeDepthRange)meta.depthStatus=2;
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

        if(query.computeDepthRange && !convertDepth(idx))return false;

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

    const auto publish=[&]() {
        if(isCancelled())return false;
        if(query.computeDepthRange){
            const auto resolved=std::count_if(next.traces.cbegin(),next.traces.cend(),[](const auto& trace){return trace.depthStatus==1;});
            next.depthStatus=resolved==0?"unavailable":static_cast<size_t>(resolved)==next.traces.size()&&next.depthWarnings.empty()?"available":"partial";
            if(resolved==0&&!tdConv.curves.empty()&&next.depthWarnings.size()<129)
                next.depthWarnings.push_back("窗口超出所选时深关系覆盖范围、缺少空间位置或缺失有效层位，深度保持未知");
        }
        result=std::move(next);return true;
    };
    if (validSpans.empty())return publish();

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
            if(isCancelled())return false;
            const auto& span = validSpans[start + localIdx];
            auto& meta = next.traces[span.resultIdx];
            if (localIdx < batch.x.size() && isFiniteVal(batch.x[localIdx]) && isFiniteVal(batch.y[localIdx])) {
                meta.x = batch.x[localIdx];
                meta.y = batch.y[localIdx];
            }
            if(query.computeDepthRange && !convertDepth(span.resultIdx))return false;

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

    return publish();
}

} // namespace paleo::data
