#include "paleo_stratigraphy/time_depth_selection.h"
#include "paleo_stratigraphy/conventions.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace paleo::stratigraphy {
namespace {
constexpr double grMarginM=100.0;
const std::vector<std::string> roles{"T70","T72","T73","T80"};
QString q(const std::string& s){return QString::fromUtf8(s);}
QJsonObject json(const std::string& s){return QJsonDocument::fromJson(QByteArray::fromStdString(s)).object();}
std::string text(const QJsonObject& o){return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();}
QJsonArray strings(const std::vector<std::string>& values){QJsonArray result;for(const auto& s:values)result.append(q(s));return result;}
QJsonValue number(double value){return std::isfinite(value)?QJsonValue(value):QJsonValue(QJsonValue::Null);}
bool knownRole(const std::string& s){return std::find(roles.begin(),roles.end(),s)!=roles.end();}
std::string roleOf(const InterpretationBoundary& b){if(knownRole(b.role))return b.role;if(b.role=="horizon"&&knownRole(b.name))return b.name;return {};}
bool independent(const InterpretationBoundary& b){return b.source=="manual"||b.source=="imported_interpretation"||b.status=="confirmed";}
std::map<std::string,double> anchors(const std::vector<InterpretationBoundary>& boundaries){
    std::map<std::string,double> out;
    for(const auto& b:boundaries){const auto role=roleOf(b);if(role.empty()||!independent(b)||!std::isfinite(b.md))continue;
        const auto [it,inserted]=out.emplace(role,b.md);if(!inserted&&std::abs(it->second-b.md)>1e-6)throw std::invalid_argument("人工 "+role+" 存在不同深度，未用冲突锚点优选时深关系");}
    double previous=-INFINITY;for(const auto& role:roles)if(out.contains(role)){if(out[role]<=previous)throw std::invalid_argument("人工界面次序冲突，不能按残差删点后优选时深关系");previous=out[role];}
    return out;
}
std::vector<InterpretationBoundary> sourceAnchors(const CorrelationWellInput& source,const GeometryOptions& geometry){
    if(source.formationTrack.intervals.empty())return {};
    // Manual regional anchors are independent of the relation being scored.
    // Exclude cycle tracks to avoid unrelated source gaps affecting this read.
    auto input=source;input.manualTrack={};input.manualLevel3Track={};input.markers.clear();
    const auto resolved=resolveWellConventions(input,{},geometry,false);
    return makeManualInterpretation(input,resolved.geometry).boundaries;
}
QJsonObject candidateJson(const TimeDepthCandidateEvaluation& c){
    return {{"wellId",QString::number(c.wellId)},{"timeDepthId",QString::number(c.timeDepthId)},{"name",q(c.name)},
        {"usable",c.usable},{"selected",c.selected},{"explicitSelection",c.explicitSelection},
        {"interfaceRoles",strings(c.interfaceRoles)},{"anchorRoles",strings(c.anchorRoles)},
        {"anchorRmseM",number(c.anchorRmseM)},{"anchorMaxAbsM",number(c.anchorMaxAbsM)},
        {"observedInterfaces",c.observedInterfaces},{"extrapolatedInterfaces",c.extrapolatedInterfaces},
        {"totalExtrapolationM",c.totalExtrapolationM},{"reason",q(c.reason)},{"evidence",json(c.evidenceJson)}};
}
void checkpoint(const CancelCheck& cancel){if(cancel&&cancel())throw WorkflowCancelled();}
}

bool TimeDepthSelectionPlan::ready()const{return std::any_of(request.wells.begin(),request.wells.end(),[](const auto& w){return w.timeDepthId>0&&w.curveId>0;})&&std::none_of(issues.begin(),issues.end(),[](const auto& i){return i.wellId==0&&i.blocking;});}

TimeDepthCandidateEvaluation evaluateTimeDepthCandidate(const CorrelationInputSnapshot& snapshot,const GeometryOptions& geometry,const std::vector<InterpretationBoundary>& independentAnchors){
    TimeDepthCandidateEvaluation out;QJsonObject receipt{{"method","regional_time_depth_candidate/v1"},{"grEndMarginM",grMarginM},{"scoreMetric","unshifted_boundary_MD_residual_m"},{"timeOffsetFitted",false},{"sourceArraysModified",false}};
    try{
        if(snapshot.wells.size()!=1)throw std::invalid_argument("时深候选评估需要恰好一口井的输入快照");
        const auto& input=snapshot.wells.front();out.wellId=input.wellId;out.timeDepthId=input.timeDepthId;
        receipt["snapshotId"]=q(snapshot.id);receipt["inputFingerprint"]=q(input.inputFingerprint);receipt["sourceRefs"]=QJsonDocument::fromJson(QByteArray::fromStdString(input.sourceRefsJson)).array();
        if(input.log.axis.size()!=input.log.values.size()||(!input.log.quality.empty()&&input.log.quality.size()!=input.log.values.size()))throw std::invalid_argument("GR 深度、值或质量标志长度不一致");
        const auto resolved=resolveWellConventions(input,snapshot.horizons,geometry);receipt["autoConventions"]=json(resolved.evidenceJson);
        const auto unit=resolved.geometry.confirmedDepthUnit.empty()?input.log.axisUnit:resolved.geometry.confirmedDepthUnit;
        const double scale=unit=="m"||unit=="M"?1.:unit=="ft"?0.3048:0.;if(!scale)throw std::invalid_argument("有效 GR 深度单位不是已知米或英尺");
        for(size_t i=0;i<input.log.axis.size();++i)if(std::isfinite(input.log.axis[i])&&std::isfinite(input.log.values[i])&&(input.log.quality.empty()||!(input.log.quality[i]&(1|4)))){
            const double md=input.log.axis[i]*scale;if(!std::isfinite(out.grFirstMd)){out.grFirstMd=out.grLastMd=md;}else{out.grFirstMd=std::min(out.grFirstMd,md);out.grLastMd=std::max(out.grLastMd,md);}}
        if(!std::isfinite(out.grFirstMd)||out.grFirstMd>=out.grLastMd)throw std::invalid_argument("没有足够有效 GR 样点供候选范围核验");
        receipt["grFirstMd"]=out.grFirstMd;receipt["grLastMd"]=out.grLastMd;
        auto fixedAnchors=anchors(independentAnchors);
        if(fixedAnchors.empty())fixedAnchors=anchors(sourceAnchors(input,geometry));
        QJsonArray anchorAudit;for(const auto& [role,md]:fixedAnchors)anchorAudit.append(QJsonObject{{"role",q(role)},{"md",md}});receipt["independentAnchors"]=anchorAudit;
        std::vector<CorrelationHorizon> supported;for(const auto& h:snapshot.horizons)if(knownRole(h.role)&&std::find(resolved.excludedHorizonIds.begin(),resolved.excludedHorizonIds.end(),h.resourceId)==resolved.excludedHorizonIds.end())supported.push_back(h);
        if(supported.empty())throw std::invalid_argument("没有具有有效空间或时间支持的 T70/T72/T73/T80 层位");
        const auto intersections=intersectHorizons(input,supported,resolved.geometry);
        std::map<std::string,std::vector<InterpretationBoundary>> byRole;for(const auto& b:intersections.boundaries)byRole[roleOf(b)].push_back(b);
        QJsonArray excluded,selectedPoints,warnings;for(const auto& w:intersections.warnings)warnings.append(q(w));
        double previous=-INFINITY,sumSquares=0,maximum=0;
        for(const auto& role:roles){const auto found=byRole.find(role);if(found==byRole.end()||found->second.size()!=1){excluded.append(QJsonObject{{"role",q(role)},{"reason",found==byRole.end()?"no_supported_intersection":"multiple_intersections"}});continue;}
            auto b=found->second.front();if(!std::isfinite(b.md)||b.md<out.grFirstMd-grMarginM-1e-8||b.md>out.grLastMd+grMarginM+1e-8){excluded.append(QJsonObject{{"role",q(role)},{"reason","outside_valid_GR_ends_plus_100m"},{"md",number(b.md)}});continue;}
            if(b.md<=previous)throw std::invalid_argument("求交结果 T70/T72/T73/T80 深度次序冲突，未自动选取该时深关系");previous=b.md;
            auto evidence=json(b.evidenceJson);evidence["autoConventions"]=json(resolved.evidenceJson);b.evidenceJson=text(evidence);
            const auto extension=evidence.value("timeDepthExtrapolation").toObject();const bool extrapolated=extension.value("applied").toBool();
            if(extrapolated){++out.extrapolatedInterfaces;out.totalExtrapolationM+=std::max({0.,extension.value("timeDepthExtrapolationM").toDouble(),extension.value("trajectoryExtrapolationM").toDouble(),extension.value("outsideLogDistanceM").toDouble()});}else ++out.observedInterfaces;
            out.interfaceRoles.push_back(role);out.boundaries.push_back(b);QJsonObject point{{"role",q(role)},{"md",b.md},{"canonicalTwtMs",number(b.time)},{"extrapolated",extrapolated},{"grDistanceM",std::max({0.,out.grFirstMd-b.md,b.md-out.grLastMd})}};
            if(fixedAnchors.contains(role)){const double residual=b.md-fixedAnchors.at(role);out.anchorRoles.push_back(role);sumSquares+=residual*residual;maximum=std::max(maximum,std::abs(residual));point["independentAnchorMd"]=fixedAnchors.at(role);point["unshiftedResidualM"]=residual;}
            selectedPoints.append(point);
        }
        receipt["interfaces"]=selectedPoints;receipt["excluded"]=excluded;receipt["warnings"]=warnings;
        if(out.interfaceRoles.empty())throw std::invalid_argument("该时深关系未获得有效 GR 上下端各 100 米范围内的唯一界面交点");
        if(!out.anchorRoles.empty()){out.anchorRmseM=std::sqrt(sumSquares/double(out.anchorRoles.size()));out.anchorMaxAbsM=maximum;}
        out.usable=true;out.reason=out.anchorRoles.empty()?"可用；没有独立人工锚点，按覆盖与外推程度作待校核推荐":"可用；同界面范围内比较原始求交深度与独立人工锚点的未平移残差";
        if(out.anchorRoles.size()==1)out.reason+="；只有单界面锚点，不能据此证明整条时深曲线形态更优";
    }catch(const std::exception& e){out.usable=false;out.reason=e.what();receipt["error"]=q(out.reason);}
    out.evidenceJson=text(receipt);return out;
}

TimeDepthSelectionPlan selectTimeDepthCandidates(DataService& service,const CorrelationInputRequest& request,const GeometryOptions& geometry,CancelCheck cancel,const std::vector<WellInterpretation>& independentAnchors){
    TimeDepthSelectionPlan plan;plan.request=request;QJsonArray wellAudits;
    if(request.wells.empty())plan.issues.push_back({0,"timeDepth","请选择至少一口井",true,{}});
    checkpoint(cancel);std::vector<CorrelationTimeDepthPreference> savedPreferences;std::string preferenceError;
    if(!service.readCorrelationTimeDepthPreferences(savedPreferences,preferenceError)){plan.issues.push_back({0,"timeDepth","无法读取工区已有时深优选绑定，未绕过已有优选结果："+preferenceError,true,{}});plan.auditJson=text(QJsonObject{{"policy","regional_time_depth_selection/v1"},{"preferenceReadError",q(preferenceError)}});return plan;}
    std::set<long long> seen;
    for(auto& selection:plan.request.wells){checkpoint(cancel);const auto explicitId=selection.timeDepthId;
        if(selection.wellId<=0||!seen.insert(selection.wellId).second){plan.issues.push_back({0,"timeDepth","井编号无效或重复",true,{}});selection.timeDepthId=0;continue;}
        std::vector<DataEntry> entries;std::string error;if(!service.listData("time_depth",selection.wellId,entries,error)){plan.issues.push_back({selection.wellId,"timeDepth",error,true,{}});selection.timeDepthId=0;continue;}
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        const auto manual=std::find_if(independentAnchors.begin(),independentAnchors.end(),[&](const auto& w){return w.wellId==selection.wellId;});const std::vector<InterpretationBoundary> empty;
        const auto& fixed=manual==independentAnchors.end()?empty:manual->boundaries;
        const size_t begin=plan.candidates.size();
        for(const auto& entry:entries){checkpoint(cancel);auto one=request;one.wells={selection};one.wells[0].timeDepthId=entry.id;CorrelationInputSnapshot snapshot;TimeDepthCandidateEvaluation evaluation;
            if(service.captureCorrelationInputs(one,snapshot,error,cancel))evaluation=evaluateTimeDepthCandidate(snapshot,geometry,fixed);
            else{checkpoint(cancel);evaluation.wellId=selection.wellId;evaluation.timeDepthId=entry.id;evaluation.reason="候选输入不可用："+error;evaluation.evidenceJson=text(QJsonObject{{"captureError",q(error)}});}
            evaluation.name=entry.name;evaluation.explicitSelection=entry.id==explicitId;plan.candidates.push_back(std::move(evaluation));
        }
        const size_t end=plan.candidates.size();std::vector<size_t> eligible;for(size_t i=begin;i<end;++i)if(plan.candidates[i].usable)eligible.push_back(i);
        const auto prior=std::find_if(savedPreferences.begin(),savedPreferences.end(),[&](const auto& p){return p.wellId==selection.wellId;});const long long priorId=prior==savedPreferences.end()?0:prior->timeDepthId;
        size_t winner=end;std::vector<std::string> scope;std::string selectionBasis="automatic_same_mask_recommendation";
        if(explicitId){for(size_t i=begin;i<end;++i)if(plan.candidates[i].timeDepthId==explicitId&&plan.candidates[i].usable){winner=i;scope=plan.candidates[i].interfaceRoles;break;}}
        else if(priorId){for(size_t i=begin;i<end;++i)if(plan.candidates[i].timeDepthId==priorId){auto receipt=json(plan.candidates[i].evidenceJson);receipt["historicalSelectionEvidence"]=json(prior->evidenceJson);plan.candidates[i].evidenceJson=text(receipt);if(plan.candidates[i].usable){winner=i;scope=plan.candidates[i].interfaceRoles;selectionBasis="persisted_historical_preference";}break;}
            if(winner==end){std::string reason="资源缺失或不属于本井";for(size_t i=begin;i<end;++i)if(plan.candidates[i].timeDepthId==priorId)reason=plan.candidates[i].reason;plan.issues.push_back({selection.wellId,"timeDepth","原优选时深绑定当前不可用（"+reason+"），回退到当前候选同范围优选；原优选记录保留",false,{priorId}});}}
        if(!explicitId&&winner!=end){size_t bestCoverage=scope.size();for(auto i:eligible)bestCoverage=std::max(bestCoverage,plan.candidates[i].interfaceRoles.size());
            if(bestCoverage>scope.size()){plan.issues.push_back({selection.wellId,"timeDepth","历史覆盖不足：原优选当前支持 "+std::to_string(scope.size())+" 个界面，其他候选在空间支持及有效 GR 端点 100 米范围内可支持 "+std::to_string(bestCoverage)+" 个；本次优先采用覆盖更多界面的候选，原优选记录保留",false,{priorId}});winner=end;scope.clear();selectionBasis="automatic_after_historical_coverage_gap";}}
        if(!explicitId&&winner==end&&!eligible.empty()){
            // Scope selection is independent of the measured residual. Masks
            // are compared by coverage, independent anchor span, then geology.
            std::map<std::vector<std::string>,std::vector<size_t>> masks;for(auto i:eligible)masks[plan.candidates[i].interfaceRoles].push_back(i);
            auto anchorSpan=[&](const auto& mask){double lo=INFINITY,hi=-INFINITY;const auto receipt=json(plan.candidates[masks.at(mask).front()].evidenceJson);for(const auto& item:receipt.value("independentAnchors").toArray()){const auto a=item.toObject();const auto role=a.value("role").toString().toStdString();if(std::find(mask.begin(),mask.end(),role)!=mask.end()){lo=std::min(lo,a.value("md").toDouble());hi=std::max(hi,a.value("md").toDouble());}}return std::isfinite(lo)?hi-lo:0.;};
            for(const auto& [mask,indices]:masks)if(scope.empty()||mask.size()>scope.size()||(mask.size()==scope.size()&&(anchorSpan(mask)>anchorSpan(scope)+1e-8||(std::abs(anchorSpan(mask)-anchorSpan(scope))<=1e-8&&mask<scope))))scope=mask;
            const auto& same=masks.at(scope);std::vector<std::string> commonAnchors=plan.candidates[same.front()].anchorRoles;
            for(auto i:same)std::erase_if(commonAnchors,[&](const auto& role){return std::find(plan.candidates[i].anchorRoles.begin(),plan.candidates[i].anchorRoles.end(),role)==plan.candidates[i].anchorRoles.end();});
            // Different anchor sets are never compared with different RMSE
            // denominators. The evaluator normally derives the same fixed set.
            const bool comparable=!commonAnchors.empty()&&std::all_of(same.begin(),same.end(),[&](auto i){return plan.candidates[i].anchorRoles==commonAnchors;});
            auto better=[&](size_t a,size_t b){const auto& x=plan.candidates[a];const auto& y=plan.candidates[b];if(comparable){if(std::abs(x.anchorRmseM-y.anchorRmseM)>1e-8)return x.anchorRmseM<y.anchorRmseM;if(std::abs(x.anchorMaxAbsM-y.anchorMaxAbsM)>1e-8)return x.anchorMaxAbsM<y.anchorMaxAbsM;}return std::tie(x.extrapolatedInterfaces,x.totalExtrapolationM,x.timeDepthId)<std::tie(y.extrapolatedInterfaces,y.totalExtrapolationM,y.timeDepthId);};
            winner=*std::min_element(same.begin(),same.end(),better);
            if(!comparable)plan.issues.push_back({selection.wellId,"timeDepth","缺少所有候选共同覆盖的人工锚点；本次仅按层位覆盖、外推程度和稳定来源编号推荐，结果待校核",false,{}});
        }
        if(winner==end){selection.timeDepthId=0;std::string message=explicitId?"显式绑定的时深关系不存在或未通过几何/覆盖校验，未擅自改用其他来源":"没有通过空间、单调性和有效 GR 范围校验的时深关系";
            for(size_t i=begin;i<end;++i)if(!explicitId||plan.candidates[i].timeDepthId==explicitId)message+="；"+plan.candidates[i].name+"："+plan.candidates[i].reason;
            plan.issues.push_back({selection.wellId,"timeDepth",message,true,{}});}
        else{selection.timeDepthId=plan.candidates[winner].timeDepthId;plan.candidates[winner].selected=true;if(explicitId)selectionBasis="explicit_source_binding";plan.candidates[winner].reason=(explicitId?"保留显式来源绑定；":selectionBasis=="persisted_historical_preference"?"沿用已登记的历史多界面优选来源，原暂定等级和来源审计保留；":"同一界面覆盖范围内自动推荐；")+plan.candidates[winner].reason;
            if(scope.size()<4)plan.issues.push_back({selection.wellId,"timeDepth","所选时深关系当前支持 "+std::to_string(scope.size())+" 个目标界面；其他界面缺少唯一交点或超出有效 GR 端点 100 米，未强行补齐",false,{}});}
        QJsonArray candidates;for(size_t i=begin;i<end;++i){auto& c=plan.candidates[i];if(c.usable&&!c.selected)c.reason=explicitId?"未选：保留用户显式时深来源绑定；"+c.reason:selectionBasis=="persisted_historical_preference"?"未选：沿用当前仍可用的历史优选来源，不以不同轨迹下的 MD 残差推翻历史多界面优选；"+c.reason:c.interfaceRoles!=scope?"未选：界面覆盖范围优先级低于本次统一比较范围；"+c.reason:"未选：同范围内残差、外推程度或稳定来源编号排序靠后；"+c.reason;candidates.append(candidateJson(c));}
        wellAudits.append(QJsonObject{{"wellId",QString::number(selection.wellId)},{"selectedTimeDepthId",QString::number(selection.timeDepthId)},{"selectionBasis",q(selectionBasis)},{"explicitBinding",explicitId>0},{"comparisonScope",strings(scope)},{"historicalTimeDepthId",QString::number(priorId)},{"historicalSelectionEvidence",prior==savedPreferences.end()?QJsonObject{}:json(prior->evidenceJson)},{"candidates",candidates}});
    }
    QJsonArray issues;for(const auto& i:plan.issues)issues.append(QJsonObject{{"wellId",QString::number(i.wellId)},{"field",q(i.field)},{"message",q(i.message)},{"blocking",i.blocking}});
    checkpoint(cancel);plan.auditJson=text(QJsonObject{{"policy","regional_time_depth_selection/v1"},{"selectionOrder","explicit source binding; historical preference when coverage equals best available; interface count; independent anchor span; fixed geological order; same-mask unshifted MD residual; extrapolation extent; source ID"},{"scoreMetric","unshifted_boundary_MD_residual_m"},{"historicalTwtRmseReproduced",false},{"grEndMarginM",grMarginM},{"timeOffsetFitted",false},{"sourceArraysModified",false},{"wells",wellAudits},{"issues",issues}});return plan;
}
}
