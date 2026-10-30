#include "paleo_stratigraphy/conventions.h"
#include "../core/internal.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <set>
#include <tuple>

namespace paleo::stratigraphy {
namespace {
using namespace detail;
QString q(const std::string& s){return QString::fromUtf8(s);}
struct Resolver {
    ConventionResolution out;
    QJsonArray entries;
    explicit Resolver(const GeometryOptions& g){out.geometry=g;}
    void record(const std::string& field,const std::string& value,const std::string& source,bool inferred=false){
        entries.append(QJsonObject{{"field",q(field)},{"value",q(value)},{"source",q(source)},{"status",inferred?"provisional":"supported"}});
        if(inferred)out.reviewRequired=true;
    }
    void review(const std::string& message){out.reviewRequired=true;out.messages.push_back(message);}
    void merge(std::string& value,const std::string& source,const std::string& label){
        if(source.empty())return;auto normalized=metadataToken(source,label);
        if(!value.empty()&&metadataToken(value,label)!=normalized)throw std::invalid_argument(label+"：工区规则与来源声明冲突，未自动覆盖");
        value=normalized;
    }
    ConventionResolution finish(long long id){
        QJsonArray messages;for(const auto& m:out.messages)messages.append(q(m));
        QJsonArray excluded;for(auto resource:out.excludedHorizonIds)excluded.append(QString::number(resource));
        out.evidenceJson=QJsonDocument(QJsonObject{{"resolverVersion",1},{"wellId",QString::number(id)},{"policyEvidence",q(out.geometry.policyEvidence)},{"reviewRequired",out.reviewRequired},{"entries",entries},{"messages",messages},{"excludedHorizonIds",excluded}}).toJson(QJsonDocument::Compact).toStdString();return out;
    }
};
void known(std::string& field,const std::string& value,Resolver& r,const std::string& label,const std::string& provenance){
    if(value.empty())return;r.merge(field,value,label);r.record(label,field,provenance);
}
double canonicalScale(const std::string& unit){auto u=unitToken(unit);if(u=="ms")return 1;if(u=="s")return 1000;throw std::invalid_argument("时间单位缺少有效来源或工区规则");}
void requireField(const std::string& field,const std::string& label){if(field.empty())throw std::invalid_argument(label+"缺少来源依据，无法自动判断");}
bool hasSameWellTimeDepthBinding(const CorrelationWellInput& w){
    if(w.wellId<=0||w.timeDepthId<=0)return false;
    const auto refs=QJsonDocument::fromJson(QByteArray::fromStdString(w.sourceRefsJson));if(!refs.isArray())return false;
    auto id=[](const QJsonValue& value){return value.isString()?value.toString().toLongLong():value.isDouble()?value.toInteger():qlonglong(0);};
    bool matched=false;
    for(const auto& entry:refs.array()){const auto ref=entry.toObject();if(ref.value("kind").toString()!="time_depth"||id(ref.value("id"))!=w.timeDepthId)continue;
        if(id(ref.value("wellId"))!=w.wellId)return false;matched=true;}
    return matched;
}
QJsonObject boundCompanionEvidence(const CorrelationWellInput& w){
    if(!hasSameWellTimeDepthBinding(w))return {};
    auto id=[](const QJsonValue& value){return value.isString()?value.toString().toLongLong():value.isDouble()?value.toInteger():qlonglong(0);};
    for(const auto& entry:QJsonDocument::fromJson(QByteArray::fromStdString(w.sourceRefsJson)).array()){
        const auto ref=entry.toObject();if(ref.value("kind").toString()!="time_depth"||id(ref.value("id"))!=w.timeDepthId||id(ref.value("wellId"))!=w.wellId)continue;
        const auto evidence=ref.value("businessConventionEvidence").toObject(),binding=evidence.value("importer").toObject();
        if(id(binding.value("wellId"))!=w.wellId||id(binding.value("timeDepthId"))!=w.timeDepthId||binding.value("checkshotsSha256").toString().isEmpty())continue;
        const auto verified=evidence.value("independentVerification").toObject(),source=evidence.value("sourceMetadata").toObject();
        if(!verified.value("canonical_candidate_sha256_matches").toBool()||!verified.value("checkshots_csv_exact_5columns_with_recorded_sign").toBool()||source.value("source_crs_companions").toArray().isEmpty())continue;
        const auto crs=QString::fromStdString(metadataToken(evidence.value("coordinateSystem").toString().toStdString(),"Coordinate system"));
        bool matchingAuthority=false;for(const auto& authority:verified.value("companion_authorities").toArray()){
            auto declared=authority.toString().trimmed().toUpper();declared.replace(',',':');if(declared==crs&&declared.startsWith("EPSG:"))matchingAuthority=true;
        }
        if(matchingAuthority)return evidence;
    }
    return {};
}
}

ConventionResolution resolveWellConventions(const CorrelationWellInput& w,const std::vector<CorrelationHorizon>& horizons,const GeometryOptions& requested,bool spatial){
    Resolver r(requested);if(!requested.resolveFromSources)return r.finish(w.wellId);
    auto& g=r.out.geometry;
    // Values already in requested are an explicit, scoped business policy.
    for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"Log depth basis",g.confirmedLogDepthType},{"Depth unit",g.confirmedDepthUnit},{"Well time unit",g.confirmedWellTimeUnit},{"Horizon time unit",g.confirmedHorizonTimeUnit}})if(!pair.second.empty())r.record(pair.first,pair.second,"explicit_project_policy");
    known(g.confirmedDepthUnit,w.depthInfo.depthUnit,r,"Depth unit","selected_log_group");
    known(g.confirmedDepthUnit,w.log.axisUnit,r,"Depth unit","selected_log_axis");
    known(g.confirmedLogDepthType,w.depthInfo.depthType,r,"Log depth basis","selected_log_group");
    known(g.confirmedLogDepthType,w.log.axisKind,r,"Log depth basis","selected_log_axis");
    if(g.confirmedDepthUnit.empty())known(g.confirmedDepthUnit,w.surveyDepthInfo.depthUnit,r,"Depth unit","same_well_default");
    if(g.confirmedLogDepthType.empty())known(g.confirmedLogDepthType,w.surveyDepthInfo.depthType,r,"Log depth basis","same_well_default");
    requireField(g.confirmedDepthUnit,"测井深度单位");requireField(g.confirmedLogDepthType,"测井深度类型");
    if(g.confirmedLogDepthType!="MD")throw std::invalid_argument("当前算法需要 MD 曲线，不会把 TVD/TVDSS 改名为 MD");
    if(spatial)known(g.confirmedDepthType,w.timeDepth.depthInfo.depthType,r,"Time-depth depth basis","selected_time_depth");
    if(g.confirmedDepthType.empty())g.confirmedDepthType=g.confirmedLogDepthType;
    // Only inputs required by this computation participate. Manual tracks are
    // validated by makeManualInterpretation when the caller extracts a template.
    std::vector<const DepthInfo*> depths{&w.depthInfo};
    if(spatial)depths.push_back(&w.timeDepth.depthInfo);
    bool missingMd=false;for(const auto* d:depths){if(d->mdReference.empty())missingMd=true;else known(g.confirmedMdReference,d->mdReference,r,"MD reference","selected_source");}
    if(g.confirmedMdReference.empty()&&requested.allowProvisionalAlignment&&!w.surveyDepthInfo.mdReference.empty()){
        g.confirmedMdReference=w.surveyDepthInfo.mdReference;r.record("MD reference",g.confirmedMdReference,"same_well_default_inheritance",true);
        r.review("同井缺失的 MD 起算面暂沿用井资料的 "+g.confirmedMdReference+"；跨来源对应关系待校核。");
    }else if(missingMd&&!g.confirmedMdReference.empty()&&requested.confirmedMdReference.empty()){
        if(!requested.allowProvisionalAlignment)throw std::invalid_argument("部分资料未声明 MD 起算面，不能自动确认跨来源相同");
        r.record("MD reference",g.confirmedMdReference,"matching_known_source_applied_to_unknown_source",true);r.review("部分资料的 MD 起算面未声明，暂采用同井已有说明，结果待校核。");
    }
    if(g.confirmedMdReference.empty()&&requested.allowProvisionalAlignment&&g.confirmedLogDepthType=="MD"&&unitToken(g.confirmedDepthUnit)=="m"&&axisToken(w.timeDepth.depthInfo.depthType)=="MD"&&unitToken(w.timeDepth.depthInfo.depthUnit)=="m"&&w.timeDepth.depthInfo.mdReference.empty()&&hasSameWellTimeDepthBinding(w)){
        // This is only a named calculation convention for source-owned MD
        // arrays. It neither asserts a KB datum nor shifts any original depth.
        g.confirmedMdReference="UNVERIFIED_SAME_WELL_MD";
        r.record("MD reference",g.confirmedMdReference,"same_well_sdk_time_depth_binding; wellId="+std::to_string(w.wellId)+"; timeDepthId="+std::to_string(w.timeDepthId)+"; explicit_MD_metres; datum_undeclared",true);
        r.review("测井与时深资料均为同井 MD 米，但未查到 KB 等起算面说明；暂按同井 MD 数值对齐生成候选，起点一致性仍待校核，未执行深度平移。");
    }
    if(!spatial)return r.finish(w.wellId);
    requireField(g.confirmedMdReference,"MD 起算面");
    if(!w.timeDepth.depthInfo.depthUnit.empty()&&unitToken(w.timeDepth.depthInfo.depthUnit)!=unitToken(g.confirmedDepthUnit))throw std::invalid_argument("测井与时深深度单位不同，需单独换算");
    known(g.confirmedWellTimeUnit,w.timeDepth.timeUnit,r,"Well time unit","selected_time_depth");
    known(g.confirmedWellTimeType,w.timeDepth.timeType,r,"Well time type","selected_time_depth");
    requireField(g.confirmedWellTimeType,"时深时间类型");const auto scale=canonicalScale(g.confirmedWellTimeUnit);
    std::vector<std::pair<double,double>> times;for(const auto& p:w.timeDepth.depthTimePairs)if(finite(p.depthValue)&&finite(p.timeValue))times.emplace_back(p.depthValue,p.timeValue);
    std::sort(times.begin(),times.end());if(times.size()<2)throw std::invalid_argument("有效时深点不足，不能判断时间方向");
    int sign=0;for(size_t i=1;i<times.size();++i){if(times[i].first<=times[i-1].first||times[i].second==times[i-1].second)throw std::invalid_argument("时深样点存在重复深度或时间，不能自动选择方向");int s=times[i].second>times[i-1].second?1:-1;if(sign&&sign!=s)throw std::invalid_argument("时深关系非单调，不能通过取绝对值修复");sign=s;}
    g.wellTimeSign=double(sign);r.record("wellTimeSign",sign>0?"1":"-1","unique_monotonic_direction_in_MD");
    const double tmin=sign*times.front().second*scale+g.wellTimeOffsetMs,tmax=sign*times.back().second*scale+g.wellTimeOffsetMs;
    bool xyNear=false;double nearest=INFINITY;std::vector<const TimeDepthPoint*> positioned;
    for(const auto& p:w.timeDepth.depthTimePairs)if(finite(p.x)&&finite(p.y)&&finite(p.depthValue)){positioned.push_back(&p);if(finite(w.well.wellheadX)&&finite(w.well.wellheadY))nearest=std::min(nearest,std::hypot(p.x-w.well.wellheadX,p.y-w.well.wellheadY));}
    xyNear=positioned.size()>=2&&nearest<=100.0;
    known(g.confirmedCoordinateUnit,w.timeDepth.coordinateUnit,r,"Coordinate unit","selected_time_depth_xyz");
    known(g.confirmedCoordinateSystem,w.timeDepth.coordinateSystem,r,"Coordinate system","selected_time_depth_xyz");
    bool companionSupported=false;
    if(requested.allowProvisionalAlignment){const auto evidence=boundCompanionEvidence(w);if(!evidence.isEmpty()){
        // A saved source-bound companion receipt can supply the calculation
        // convention when the independently registered wellhead is unavailable.
        // It is never written back as if declared in the original checkshots.
        r.merge(g.confirmedCoordinateSystem,evidence.value("coordinateSystem").toString().toStdString(),"Coordinate system");
        r.merge(g.confirmedCoordinateUnit,evidence.value("coordinateUnit").toString().toStdString(),"Coordinate unit");
        companionSupported=true;
        r.record("Coordinate system",g.confirmedCoordinateSystem,"bound_time_depth_companion_receipt; wellId="+std::to_string(w.wellId)+"; timeDepthId="+std::to_string(w.timeDepthId)+"; authority="+g.confirmedCoordinateSystem,true);
        r.review("时深坐标系依据该井该资源的已核验 companion 文件记录用于计算；原文件未声明的坐标字段保持未知，跨资料对齐仍为待校核候选。");
    }}
    if(g.confirmedCoordinateSystem.empty()&&requested.allowProvisionalAlignment&&xyNear&&!w.well.coordinateSystem.empty()&&unitToken(w.well.coordinateUnit)=="m"&&unitToken(g.confirmedCoordinateUnit)=="m"){
        g.confirmedCoordinateSystem=w.well.coordinateSystem;r.record("Coordinate system",g.confirmedCoordinateSystem,"time_depth_XY_within_100m_of_declared_wellhead",true);
        r.review("时深沿井坐标与已知井口位置接近，暂沿用井口坐标系；这是数值一致性依据，非坐标系确证。");
    }
    requireField(g.confirmedCoordinateSystem,"沿井平面坐标系");requireField(g.confirmedCoordinateUnit,"沿井平面坐标单位");
    known(g.confirmedTimeReference,w.timeDepth.timeReference,r,"Time reference","selected_time_depth");
    for(const auto& h:horizons){
        const auto& s=h.surface;known(g.confirmedHorizonTimeUnit,s.timeUnit,r,"Horizon time unit","horizon:"+h.role);
        known(g.confirmedHorizonTimeType,s.timeType,r,"Horizon time type","horizon:"+h.role);
        known(g.confirmedCoordinateSystem,s.coordinateSystem,r,"Coordinate system","horizon:"+h.role);
        known(g.confirmedCoordinateUnit,s.coordinateUnit,r,"Coordinate unit","horizon:"+h.role);
        known(g.confirmedTimeReference,s.timeReference,r,"Time reference","horizon:"+h.role);
    }
    if(g.confirmedHorizonTimeType.empty()&&requested.allowProvisionalAlignment&&g.confirmedWellTimeType=="TWT"){
        g.confirmedHorizonTimeType="TWT";r.record("Horizon time type","TWT","provisional_match_to_time_depth_TWT",true);
    }
    requireField(g.confirmedHorizonTimeType,"层位时间类型");const auto hs=canonicalScale(g.confirmedHorizonTimeUnit);
    for(const auto& h:horizons){const auto& s=h.surface;
        for(const auto& item:std::vector<std::tuple<std::string,std::string,std::string,std::string>>{{s.timeUnit,requested.confirmedHorizonTimeUnit,g.confirmedHorizonTimeUnit,"time unit"},{s.timeType,requested.confirmedHorizonTimeType,g.confirmedHorizonTimeType,"time type"}}){
            const auto& [source,explicitValue,effective,label]=item;if(!source.empty()||!explicitValue.empty())continue;
            if(!requested.allowProvisionalAlignment)throw std::invalid_argument(h.role+" 未声明 "+label+"，不能把另一层位的约定当作本层位已确认");
            r.record(h.role+" "+label,effective,"provisional_inheritance_from_other_selected_source",true);
        }
        const bool unknownXY=(s.coordinateSystem.empty()&&requested.confirmedCoordinateSystem.empty())||(s.coordinateUnit.empty()&&requested.confirmedCoordinateUnit.empty());const bool unknownTime=s.timeReference.empty()||w.timeDepth.timeReference.empty();
        if(unknownXY){
            if(!requested.allowProvisionalAlignment||(!xyNear&&!companionSupported)||unitToken(g.confirmedCoordinateUnit)!="m")throw std::invalid_argument(h.role+" 的坐标缺少可用于候选对齐的依据");
            double xlo=INFINITY,xhi=-INFINITY,ylo=INFINITY,yhi=-INFINITY;for(const auto* t:positioned){xlo=std::min(xlo,t->x);xhi=std::max(xhi,t->x);ylo=std::min(ylo,t->y);yhi=std::max(yhi,t->y);}
            bool nearby=false;for(const auto& p:s.points){if(!finite(p.x)||!finite(p.y)||p.x<xlo-3000||p.x>xhi+3000||p.y<ylo-3000||p.y>yhi+3000)continue;for(const auto* t:positioned)if(std::hypot(p.x-t->x,p.y-t->y)<=3000){nearby=true;break;}if(nearby)break;}
            if(!nearby){r.out.excludedHorizonIds.push_back(h.resourceId);r.out.messages.push_back(h.role+" 与沿井轨迹没有邻近层位点，跳过此界面；其他有覆盖的界面继续计算。");r.record(h.role,"excluded","no_local_coordinate_support");continue;}
            r.record(h.role+" Coordinate system",g.confirmedCoordinateSystem,"local_points_within_3000m_and_matching_numerical_XY",true);
        }
        if(unknownTime&&requested.confirmedTimeReference.empty()){
            if(!requested.allowProvisionalAlignment)throw std::invalid_argument("层位与时深的共同时间零点缺少来源依据");
            double low=INFINITY,high=-INFINITY;for(const auto& p:s.points)if(finite(p.zTime)){double t=p.zTime*hs*g.horizonTimeSign+g.horizonTimeOffsetMs;low=std::min(low,t);high=std::max(high,t);}
            if(!finite(low)||(high<tmin&&!requested.allowTopExtrapolation)||(low>tmax&&!requested.allowBottomExtrapolation)){r.out.excludedHorizonIds.push_back(h.resourceId);r.out.messages.push_back(h.role+" 时间范围超出当前时深表覆盖，跳过此界面；不外推补齐未覆盖的地层。");r.record(h.role,"excluded","no_observed_time_overlap");continue;}
            r.record(h.role+" Time reference","UNVERIFIED_PROJECT_TWT",(low>tmax||high<tmin)?"unverified_zero_offset_alignment; end_extrapolation_requested":"unverified_zero_offset_alignment; overlapping_TWT_ranges_only",true);
        }
    }
    if(!horizons.empty()&&r.out.excludedHorizonIds.size()==horizons.size())throw std::invalid_argument("所选层位均缺少当前井的空间或时深支持，无法生成落井候选");
    if(g.confirmedTimeReference.empty()){
        if(!requested.allowProvisionalAlignment)throw std::invalid_argument("共同时间零点缺少来源依据");
        g.confirmedTimeReference="UNVERIFIED_PROJECT_TWT";
    }
    if(requested.allowProvisionalAlignment){g.allowIrregularSurface=true;if(!finite(g.maximumSurfaceEdge))g.maximumSurfaceEdge=750.;r.record("surfaceInterpolation","irregular_points; max_edge_m="+number(g.maximumSurfaceEdge),"explicit_candidate_policy; interpolation_only_no_extrapolation");}
    if(r.out.reviewRequired)r.review("自动对齐采用了尚未标定的跨资料约定，仅生成待校核草稿；时间零点未校正，仍需地质校核。");
    return r.finish(w.wellId);
}
}
