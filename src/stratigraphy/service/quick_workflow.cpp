#include "paleo_stratigraphy/quick_workflow.h"
#include "paleo_stratigraphy/conventions.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <map>
#include <limits>
#include <tuple>

namespace paleo::stratigraphy {
namespace {
QString q(const std::string& value){return QString::fromUtf8(value);}
std::string u(const QString& value){return value.toUtf8().toStdString();}
bool blocked(const std::vector<QuickIssue>& issues){return std::any_of(issues.begin(),issues.end(),[](const auto& i){return i.blocking;});}
void issue(std::vector<QuickIssue>& out,long long well,const std::string& field,const std::string& message,bool blocking,std::vector<long long> ids={}){out.push_back({well,field,message,blocking,std::move(ids)});}
std::vector<long long> ids(const std::vector<DataEntry>& values){std::vector<long long> out;for(const auto& v:values)out.push_back(v.id);return out;}
std::vector<DataEntry> resources(DataService& s,const std::string& kind,long long well,std::vector<QuickIssue>& issues,bool required){std::vector<DataEntry> rows;std::string error;if(!s.listData(kind,well,rows,error))issue(issues,well,"source",kind+": "+error,required);return rows;}
template<class Match>
long long choose(const std::vector<DataEntry>& all,long long preferred,Match match,bool required,long long well,const std::string& field,const std::string& label,std::vector<QuickIssue>& issues){
    if(preferred){if(std::any_of(all.begin(),all.end(),[&](const auto& r){return r.id==preferred;}))return preferred;
        issue(issues,well,field,label+"：原先指定的资料不存在或不属于此井，请重新选择",required,ids(all));return 0;}
    std::vector<DataEntry> candidates;for(const auto& row:all)if(match(row))candidates.push_back(row);
    if(candidates.size()==1)return candidates.front().id;
    issue(issues,well,field,label+(candidates.empty()?"：未找到匹配资料":"：存在多份候选，请选择采用的一份"),required,ids(candidates));return 0;
}
bool namedHorizon(const std::string& name,const std::string& role){
    // A complete role token, not a substring (T700 must never become T70).
    const QRegularExpression re(QString("(?:^|[^A-Z0-9])%1(?:$|[^A-Z0-9])").arg(q(role)));
    return re.match(q(name).trimmed().toUpper()).hasMatch();
}
bool manualCycle(const InterpretationCycle& c){return c.source=="manual"||c.source=="imported_interpretation"||c.status=="confirmed";}
const InterpretationTemplateSegment* findSegment(const InterpretationTemplate& t,const std::string& segment){
    const InterpretationTemplateSegment* found=nullptr;
    for(const auto& s:t.segments)if(s.segmentId==segment){if(found)return nullptr;found=&s;}
    return found;
}
bool usableSegment(const InterpretationTemplateSegment& s){
    const auto n=s.directions.size();
    if(!n||n>128||s.boundaryFractions.size()!=n+1||s.sequenceIdentities.size()!=n)return false;
    if(!s.parentGroups.empty()&&s.parentGroups.size()!=n)return false;
    if(!s.parentDirections.empty()&&s.parentDirections.size()!=n)return false;
    if(std::abs(s.boundaryFractions.front())>1e-8||std::abs(s.boundaryFractions.back()-1)>1e-8)return false;
    std::set<std::string> names;
    for(size_t i=0;i<n;++i){
        if(!std::isfinite(s.boundaryFractions[i])||!std::isfinite(s.boundaryFractions[i+1])||s.boundaryFractions[i+1]<=s.boundaryFractions[i]||(s.directions[i]!=1&&s.directions[i]!=-1))return false;
        if(s.sequenceIdentities[i].empty()||!names.insert(s.sequenceIdentities[i]).second)return false;
        if(!s.parentGroups.empty()&&(s.parentGroups[i]<0||(i&&s.parentGroups[i]<s.parentGroups[i-1])))return false;
        if(!s.parentDirections.empty()&&(s.parentDirections[i]<-1||s.parentDirections[i]>1))return false;
        if(i&&!s.parentGroups.empty()&&!s.parentDirections.empty()&&s.parentGroups[i]==s.parentGroups[i-1]&&s.parentDirections[i]!=s.parentDirections[i-1])return false;
    }
    return true;
}
bool hasSegment(const InterpretationTemplate& t,const std::string& segment){const auto* s=findSegment(t,segment);return s&&usableSegment(*s);}
bool approved(const InterpretationTemplate& t){return t.status=="approved"||t.status=="confirmed"||t.status=="adopted";}
bool protectedBoundary(const InterpretationBoundary& b){return b.locked||b.source=="manual"||b.source=="imported_interpretation"||b.status=="confirmed";}
void nameManualRoles(WellInterpretation& w){for(auto& b:w.boundaries)if(b.role=="horizon"&&(b.name=="T70"||b.name=="T72"||b.name=="T73"||b.name=="T80"))b.role=b.name;}
WellInterpretation quickManual(const CorrelationWellInput& input,const GeometryOptions& geometry){
    const auto resolved=resolveWellConventions(input,{},geometry,false);auto out=makeManualInterpretation(input,resolved.geometry);
    if(geometry.resolveFromSources){for(auto& b:out.boundaries){auto evidence=QJsonDocument::fromJson(QByteArray::fromStdString(b.evidenceJson)).object();evidence["autoConventions"]=QJsonDocument::fromJson(QByteArray::fromStdString(resolved.evidenceJson)).object();b.evidenceJson=QJsonDocument(evidence).toJson(QJsonDocument::Compact).toStdString();if(resolved.reviewRequired)b.status="review";}out.warnings.insert(out.warnings.end(),resolved.messages.begin(),resolved.messages.end());}return out;
}
bool sameSegment(const InterpretationTemplateSegment& a,const InterpretationTemplateSegment& b){
    if(a.segmentId!=b.segmentId||a.boundaryFractions.size()!=b.boundaryFractions.size()||a.directions!=b.directions||a.parentGroups!=b.parentGroups||a.parentDirections!=b.parentDirections||a.sequenceIdentities!=b.sequenceIdentities)return false;
    for(size_t i=0;i<a.boundaryFractions.size();++i)if(!std::isfinite(a.boundaryFractions[i])||std::abs(a.boundaryFractions[i]-b.boundaryFractions[i])>1e-10)return false;
    return true;
}
QJsonObject sourceAudit(const InterpretationTemplate& t,const std::string& segment){
    const auto document=QJsonDocument::fromJson(QByteArray::fromStdString(t.sourceReconciliationJson));
    if(!document.isObject())return {};
    QJsonObject found;for(const auto& row:document.object().value("segments").toArray())if(row.toObject().value("segmentId").toString()==q(segment)){
        if(!found.isEmpty())return {};found=row.toObject();
    }
    if(!found.value("regionalTopMd").isDouble()||!found.value("regionalBottomMd").isDouble()||found.value("sourceL4").toArray().isEmpty())return {};
    return found;
}
void verifyManualSource(const InterpretationTemplate& adopted,const InterpretationTemplate& extracted,const std::string& segment){
    if(adopted.referenceWellId!=extracted.referenceWellId)throw std::runtime_error("人工道所属井与标准模板不一致");
    if(adopted.sourceRevision.empty()||extracted.sourceRevision.empty()||adopted.sourceRevision!=extracted.sourceRevision)throw std::runtime_error("模板来源快照与当前输入不一致，须核对原始人工道及深度约定后再恢复");
    const auto a=std::find_if(adopted.segments.begin(),adopted.segments.end(),[&](const auto& s){return s.segmentId==segment;});
    const auto b=std::find_if(extracted.segments.begin(),extracted.segments.end(),[&](const auto& s){return s.segmentId==segment;});
    if(a==adopted.segments.end()||b==extracted.segments.end()||!sameSegment(*a,*b))throw std::runtime_error("原始人工道与已采用模板的层序结构不一致，请核对所用版本");
    // Fractions alone cannot prove an absolute manual interpretation: a shifted
    // or stretched interval has the same ratios. Compare original source IDs,
    // absolute depths, regional anchors and L3 parent mapping from extraction.
    const auto expected=sourceAudit(adopted,segment),actual=sourceAudit(extracted,segment);
    if(expected.isEmpty()||actual.isEmpty())throw std::runtime_error("模板缺少可核验的原始深度记录，不能仅按比例还原人工解释");
    if(expected!=actual)throw std::runtime_error("原始人工道的来源记录或绝对深度已变化，请核对所用版本");
}
std::string templateVersion(const InterpretationTemplate& t,const WellInterpretation& w){
    QJsonArray rows;for(const auto& s:t.segments){QJsonArray fractions,dirs,groups,parents,names;for(double x:s.boundaryFractions)fractions.append(x);for(int x:s.directions)dirs.append(x);for(int x:s.parentGroups)groups.append(x);for(int x:s.parentDirections)parents.append(x);for(const auto& x:s.sequenceIdentities)names.append(q(x));rows.append(QJsonObject{{"segment",q(s.segmentId)},{"fractions",fractions},{"directions",dirs},{"groups",groups},{"parentDirections",parents},{"identities",names}});}
    QJsonArray geometry;for(const auto& b:w.boundaries)geometry.append(QJsonObject{{"id",q(b.id)},{"md",b.md},{"role",q(b.role)}});
    const auto bytes=QJsonDocument(QJsonObject{{"input",q(w.input.inputFingerprint)},{"segments",rows},{"geometry",geometry},{"sourceReconciliation",q(t.sourceReconciliationJson)}}).toJson(QJsonDocument::Compact);
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().toStdString();
}
std::vector<long long> references(const std::vector<const InterpretationTemplate*>& ts){std::set<long long> unique;for(const auto* t:ts)unique.insert(t->referenceWellId);return {unique.begin(),unique.end()};}
int parentCount(const InterpretationTemplateSegment& s){return int(std::set<int>(s.parentGroups.begin(),s.parentGroups.end()).size());}
std::vector<int> normalizedGroups(const std::vector<int>& groups){std::map<int,int> ordinals;std::vector<int> result;for(int group:groups){if(!ordinals.contains(group))ordinals[group]=int(ordinals.size());result.push_back(ordinals[group]);}return result;}
bool manualCountConflict(const WellInterpretation& w,const InterpretationTemplateSegment& s,int& l3,int& l4){
    std::vector<const InterpretationCycle*> children;std::set<std::string> parents;
    for(const auto& c:w.cycles)if(c.segmentId==s.segmentId){if(c.level==3)parents.insert(c.id);else if(c.level==4)children.push_back(&c);}
    l3=int(parents.size());l4=int(children.size());
    if(l3!=parentCount(s)||l4!=int(s.directions.size()))return true;
    auto depth=[&](const std::string& id){for(const auto& b:w.boundaries)if(b.id==id)return b.md;return std::numeric_limits<double>::quiet_NaN();};
    for(const auto* c:children)if(!std::isfinite(depth(c->topBoundaryId)))return true;
    std::sort(children.begin(),children.end(),[&](auto* a,auto* b){return depth(a->topBoundaryId)<depth(b->topBoundaryId);});
    std::map<std::string,int> ordinals;std::vector<int> groups;
    for(const auto* c:children){
        if(s.parentGroups.empty()){if(!c->parentId.empty())return true;continue;}
        if(!parents.contains(c->parentId))return true;
        if(!ordinals.contains(c->parentId))ordinals[c->parentId]=int(ordinals.size());groups.push_back(ordinals[c->parentId]);
    }
    return groups!=normalizedGroups(s.parentGroups);
}
double recordedTolerance(const InterpretationTemplate& t){
    const auto audit=QJsonDocument::fromJson(QByteArray::fromStdString(t.sourceReconciliationJson)).object();
    const auto value=audit.value("toleranceM");
    if(!value.isDouble()||!std::isfinite(value.toDouble())||value.toDouble()<0||value.toDouble()>2)throw std::runtime_error("模板缺少有效的原始序位协调容差记录（业务上限 2 米）");
    return value.toDouble();
}
}
bool QuickInputPlan::ready()const{return !request.wells.empty()&&!blocked(issues);}
bool QuickTemplatePlan::ready()const{return !blocked(issues);}

QuickInputPlan planQuickInputs(DataService& service,const std::vector<long long>& wellIds,const CorrelationInputRequest& preferred){
    QuickInputPlan plan;plan.request.firstDepth=preferred.firstDepth;plan.request.lastDepth=preferred.lastDepth;plan.request.surfaceSearchRadius=preferred.surfaceSearchRadius;
    if(wellIds.empty())issue(plan.issues,0,"wells","请先选择至少一口井",true);
    std::set<long long> seen;
    for(long long id:wellIds){
        if(id<=0||!seen.insert(id).second){issue(plan.issues,id,"wells","井编号无效或重复",true);continue;}
        WellInfo info;std::string error;if(!service.readWellInfo(id,info,error)){issue(plan.issues,id,"source",error,true);continue;}
        CorrelationWellSelection selection;selection.wellId=id;
        auto saved=std::find_if(preferred.wells.begin(),preferred.wells.end(),[&](const auto& w){return w.wellId==id;});
        if(saved!=preferred.wells.end())selection=*saved;
        auto curves=resources(service,"curves",id,plan.issues,true);
        selection.curveId=choose(curves,selection.curveId,[](const auto& e){auto name=q(e.name).trimmed().toUpper();return name=="GR"||name=="GAMMA RAY"||name=="GAMMARAY"||name==QStringLiteral("自然伽马");},true,id,"curve","测井曲线",plan.issues);
        auto td=resources(service,"time_depth",id,plan.issues,true);
        selection.timeDepthId=choose(td,selection.timeDepthId,[](const auto&){return true;},true,id,"timeDepth","时深关系",plan.issues);
        bool positioned=false;
        if(selection.timeDepthId){TimeDepthTable relation;if(!service.readTimeDepth(selection.timeDepthId,relation,error))issue(plan.issues,id,"timeDepth",error,true);else positioned=std::count_if(relation.depthTimePairs.begin(),relation.depthTimePairs.end(),[](const auto& p){return std::isfinite(p.depthValue)&&std::isfinite(p.x)&&std::isfinite(p.y);})>=2;}
        auto trajectories=resources(service,"trajectories",id,plan.issues,!positioned&&!selection.confirmedVertical);
        const bool head=std::isfinite(info.wellheadX)&&std::isfinite(info.wellheadY);
        // A complete checkshot XY path needs no guessed straight-well fallback.
        if(selection.trajectoryId||!positioned)selection.trajectoryId=choose(trajectories,selection.trajectoryId,[](const auto&){return true;},!positioned&&!selection.confirmedVertical,id,"trajectory","井轨迹",plan.issues);
        QuickWellSummary summary{id,info.wellName,"unavailable"};
        if(positioned)summary.coordinateSource="timeDepthXYZ";
        else if(selection.trajectoryId&&head)summary.coordinateSource="trajectory";
        else if(selection.confirmedVertical&&head)summary.coordinateSource="confirmedVertical";
        else issue(plan.issues,id,"coordinates","未取得完整时深 XY 轨迹，也没有可用井口坐标及测斜/已确认直井依据",true);
        auto tracks=resources(service,"intervals",id,plan.issues,false);
        selection.intervalTrackId=choose(tracks,selection.intervalTrackId,[](const auto& e){auto name=q(e.name).trimmed().toUpper();return name.contains(QStringLiteral("四级"))||name=="L4"||name=="LEVEL4";},false,id,"manualL4","人工四级旋回",plan.issues);
        selection.manualLevel3TrackId=choose(tracks,selection.manualLevel3TrackId,[](const auto& e){auto name=q(e.name).trimmed().toUpper();return name.contains(QStringLiteral("三级"))||name=="L3"||name=="LEVEL3";},false,id,"manualL3","人工三级旋回",plan.issues);
        selection.formationTrackId=choose(tracks,selection.formationTrackId,[](const auto& e){auto name=q(e.name).trimmed();return !name.contains(QStringLiteral("三级"))&&!name.contains(QStringLiteral("四级"))&&(name==QStringLiteral("段")||name.contains(QStringLiteral("组段"))||name.contains(QStringLiteral("段划分"))||name.contains(QStringLiteral("段分层")));},false,id,"formation","人工组段",plan.issues);
        plan.request.wells.push_back(selection);plan.wellSummaries.push_back(std::move(summary));
    }
    auto horizons=resources(service,"horizons",0,plan.issues,true);
    std::set<long long> assigned;
    for(const std::string role:{"T70","T72","T73","T80"}){
        long long chosen=0;for(const auto& h:preferred.horizons)if(h.role==role){if(chosen&&chosen!=h.resourceId)issue(plan.issues,0,"horizon:"+role,role+" 有冲突的显式绑定",true);chosen=h.resourceId;}
        chosen=choose(horizons,chosen,[&](const auto& e){return namedHorizon(e.name,role);},true,0,"horizon:"+role,role+" 地震层位",plan.issues);
        if(chosen){if(!assigned.insert(chosen).second)issue(plan.issues,0,"horizon:"+role,"同一层位资源不能同时绑定为两个界面",true);plan.request.horizons.push_back({chosen,role});}
    }
    return plan;
}

QuickReferenceDiscovery discoverQuickReferences(DataService& service,const GeometryOptions& geometry,CancelCheck cancel,double sourceReconciliationTolerance){
    QuickReferenceDiscovery out;
    if(!std::isfinite(sourceReconciliationTolerance)||sourceReconciliationTolerance<0||sourceReconciliationTolerance>2){issue(out.issues,0,"manualTemplate","人工源序位协调容差须在 0–2 米内",true);return out;}
    std::vector<WellSummary> wells;std::string error;
    if(!service.listWells(wells,error)){issue(out.issues,0,"manualTemplate",error,true);return out;}
    std::sort(wells.begin(),wells.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    for(const auto& well:wells){
        if(cancel&&cancel())throw WorkflowCancelled();
        auto tracks=resources(service,"intervals",well.id,out.issues,false);
        // Wells with no interpretation tracks are ordinary targets, not errors.
        const auto isL4=[](const auto& e){const auto name=q(e.name).trimmed().toUpper();return name.contains(QStringLiteral("四级"))||name=="L4"||name=="LEVEL4";};
        const auto isL3=[](const auto& e){const auto name=q(e.name).trimmed().toUpper();return name.contains(QStringLiteral("三级"))||name=="L3"||name=="LEVEL3";};
        if(std::none_of(tracks.begin(),tracks.end(),isL4)&&std::none_of(tracks.begin(),tracks.end(),isL3))continue;
        CorrelationWellSelection selected;selected.wellId=well.id;
        selected.intervalTrackId=choose(tracks,0,isL4,false,well.id,"manualL4","标准井四级人工道",out.issues);
        selected.manualLevel3TrackId=choose(tracks,0,isL3,false,well.id,"manualL3","标准井三级人工道",out.issues);
        selected.formationTrackId=choose(tracks,0,[](const auto& e){const auto name=q(e.name).trimmed();return !name.contains(QStringLiteral("三级"))&&!name.contains(QStringLiteral("四级"))&&(name==QStringLiteral("段")||name.contains(QStringLiteral("组段"))||name.contains(QStringLiteral("段划分"))||name.contains(QStringLiteral("段分层")));},false,well.id,"formation","标准井组段人工道",out.issues);
        if(!selected.intervalTrackId||!selected.manualLevel3TrackId||!selected.formationTrackId)continue;
        const auto curves=resources(service,"curves",well.id,out.issues,false);
        selected.curveId=choose(curves,0,[](const auto& e){const auto name=q(e.name).trimmed().toUpper();return name=="GR"||name=="GAMMA RAY"||name=="GAMMARAY"||name==QStringLiteral("自然伽马");},false,well.id,"curve","标准井 GR 曲线",out.issues);
        if(!selected.curveId)continue;
        CorrelationInputRequest request;request.wells.push_back(selected);CorrelationInputSnapshot snapshot;
        if(!service.captureCorrelationInputs(request,snapshot,error,cancel)){if(cancel&&cancel())throw WorkflowCancelled();issue(out.issues,well.id,"manualTemplate","人工标准井快照不可用："+error,false);continue;}
        try{
            auto manual=quickManual(snapshot.wells.front(),geometry);nameManualRoles(manual);
            auto candidate=extractTemplate(manual,"quick-manual-"+std::to_string(well.id),"pending",sourceReconciliationTolerance);
            candidate.version=templateVersion(candidate,manual);candidate.name=well.name+" 人工标准层序（自动发现候选）";
            out.templates.push_back(std::move(candidate));out.manualReferences.push_back(std::move(manual));
        }catch(const std::exception& e){issue(out.issues,well.id,"manualTemplate",well.name+"：不能提取可验证标准层序："+e.what(),false);}
    }
    return out;
}

QuickTemplatePlan planQuickTemplates(const InterpretationDocument& document,const GeometryOptions& geometry,long long preferredReferenceWellId,const std::string& segmentFilter,double sourceReconciliationTolerance,const std::vector<WellInterpretation>& discoveredReferences){
    QuickTemplatePlan plan;
    if(!segmentFilter.empty()&&segmentFilter!="E12"&&segmentFilter!="E3"&&segmentFilter!="E4"){issue(plan.issues,0,"template","未知划分区间",true);return plan;}
    if(!std::isfinite(sourceReconciliationTolerance)||sourceReconciliationTolerance<0||sourceReconciliationTolerance>2){issue(plan.issues,0,"template","人工源序位协调容差须在 0–2 米内；原始解释深度不会移动",true);return plan;}
    if(document.wells.empty()){issue(plan.issues,0,"wells","请先获取井和界面深度",true);return plan;}
    const auto view=QJsonDocument::fromJson(QByteArray::fromStdString(document.viewStateJson)).object();
    const auto previousPlan=view.value("uniformTemplatePlan").toObject();
    std::map<std::string,QJsonObject> previousSegments;
    if(view.contains("uniformTemplatePlan")&&(!view.value("uniformTemplatePlan").isObject()||!previousPlan.value("segments").isArray())){issue(plan.issues,0,"template","已有分段模板绑定记录格式无效，未忽略该记录重新选井",true);return plan;}
    for(const auto& item:previousPlan.value("segments").toArray()){
        const auto row=item.toObject();const auto segment=row.value("segmentId").toString().toStdString();
        if((segment!="E12"&&segment!="E3"&&segment!="E4")||!previousSegments.emplace(segment,row).second){issue(plan.issues,0,"template","已有分段模板绑定含未知或重复区间，未自动重选",true);return plan;}
    }
    std::set<std::pair<std::string,std::string>> keys;
    for(const auto& t:document.templates)if(approved(t)&&!t.id.empty()&&!t.version.empty()&&keys.emplace(t.id,t.version).second)plan.candidates.push_back(t);
    const auto persisted=keys;
    for(const auto& w:document.wells){
        // Retain a deliberately adopted version; do not create duplicate versions
        // for the same well merely because the UI was opened again.
        if(std::any_of(plan.candidates.begin(),plan.candidates.end(),[&](const auto& t){return t.referenceWellId==w.wellId;}))continue;
        const bool authored=std::any_of(w.cycles.begin(),w.cycles.end(),manualCycle);
        if(!authored&&w.input.manualTrack.intervals.empty()&&w.input.manualLevel3Track.intervals.empty())continue;
        try{
            auto manual=authored?w:quickManual(w.input,geometry);nameManualRoles(manual);
            auto reference=extractTemplate(manual,"quick-manual-"+std::to_string(w.wellId),"pending",sourceReconciliationTolerance);
            if(reference.segments.empty())throw std::runtime_error("人工解释不含完整区间");
            reference.version=templateVersion(reference,manual);reference.name=w.name+" 人工标准层序";
            plan.candidates.push_back(std::move(reference));plan.manualReferences.push_back(std::move(manual));
        }catch(const std::exception& e){issue(plan.issues,w.wellId,"manualTemplate",w.name+"：人工标准解释暂不可用："+e.what(),false);}
    }
    // Choose the regional standard before looking at targets. Cluster membership
    // can no longer change the number or grouping of cycles from well to well.
    auto score=[&](const InterpretationTemplate& t){int coverage=0,grouped=0,audited=0;for(const std::string s:{"E12","E3","E4"})if(hasSegment(t,s)){++coverage;if(!findSegment(t,s)->parentGroups.empty())++grouped;if(!sourceAudit(t,s).isEmpty())++audited;}return std::tuple{coverage,grouped,audited,!t.sourceRevision.empty(),persisted.contains({t.id,t.version})};};
    auto better=[&](const auto* a,const auto* b){const auto sa=score(*a),sb=score(*b);if(sa!=sb)return sa>sb;return std::tie(a->referenceWellId,a->id,a->version)<std::tie(b->referenceWellId,b->id,b->version);};
    std::vector<const InterpretationTemplate*> pool;
    for(const auto& t:plan.candidates){
        if(!document.activeTemplateId.empty()){if(t.id==document.activeTemplateId)pool.push_back(&t);}
        else if(!preferredReferenceWellId||t.referenceWellId==preferredReferenceWellId)pool.push_back(&t);
    }
    if(!document.activeTemplateId.empty()&&pool.size()!=1){issue(plan.issues,0,"template",pool.empty()?"已指定的全区模板不存在或尚未批准，不能改用其他标准井":"已指定模板存在多个版本，须明确版本后才能统一执行",true,references(pool));return plan;}
    std::sort(pool.begin(),pool.end(),better);
    const auto* primary=pool.empty()?nullptr:pool.front();
    std::map<std::string,const InterpretationTemplate*> chosen;
    // Audit is a bounded union of the three regional bindings. A filtered run
    // updates only its segment and does not erase previous segments' bindings.
    auto retainedSegments=previousSegments;
    plan.usesRecommendation=document.activeTemplateId.empty();
    const std::string mode=!document.activeTemplateId.empty()?"active_template":preferredReferenceWellId?"preferred_reference":"auto_recommended";
    for(const std::string segment:{"E12","E3","E4"}){
        if(!segmentFilter.empty()&&segment!=segmentFilter)continue;
        const InterpretationTemplate* selected=nullptr;bool reused=false;
        if(document.activeTemplateId.empty()&&!preferredReferenceWellId&&previousSegments.contains(segment)){
            const auto& saved=previousSegments.at(segment);const auto id=saved.value("templateId").toString().toStdString(),version=saved.value("templateVersion").toString().toStdString();
            for(const auto* t:pool)if(t->id==id&&t->version==version){selected=t;break;}
            if(!selected||!hasSegment(*selected,segment)){issue(plan.issues,0,"template:"+segment,"已保存的 "+segment+" 分段模板或其精确版本不可用；保留原绑定，未悄悄改用另一标准井",true);continue;}
            const auto& s=*findSegment(*selected,segment);QJsonArray groups;for(int group:s.parentGroups)groups.append(group);
            const auto source=QJsonDocument::fromJson(QByteArray::fromStdString(selected->sourceReconciliationJson)).object();
            if(saved.value("referenceWellId").toDouble()!=double(selected->referenceWellId)||saved.value("level3Count").toInt(-1)!=parentCount(s)||saved.value("level4Count").toInt(-1)!=int(s.directions.size())||saved.value("parentGroups").toArray()!=groups||saved.value("sourceRevision").toString()!=q(selected->sourceRevision)||saved.value("sourceReconciliation").toObject()!=source){issue(plan.issues,0,"template:"+segment,"已保存的 "+segment+" 模板来源、原始深度或 L3/L4 组合与当前同编号版本不一致；保留原绑定，需核对来源",true);continue;}
            reused=true;
        }else{
            selected=primary&&hasSegment(*primary,segment)?primary:nullptr;
            if(!selected&&document.activeTemplateId.empty())for(const auto* t:pool)if(hasSegment(*t,segment)){selected=t;break;}
        }
        if(!selected)continue;
        chosen[segment]=selected;const auto& s=*findSegment(*selected,segment);
        QuickSegmentTemplatePlan summary;summary.segmentId=segment;summary.templateId=selected->id;summary.templateVersion=selected->version;summary.referenceWellId=selected->referenceWellId;summary.level3Count=parentCount(s);summary.level4Count=int(s.directions.size());summary.parentGroups=s.parentGroups;summary.selectionMode=reused?"reused_recommendation":mode;
        if(reused)summary.reason="沿用本方案此前保存的分段推荐及精确版本；该段所有目标井保持相同 L3/L4 数量和父子组合，仍非人工确认模板";
        else if(!document.activeTemplateId.empty())summary.reason="严格采用已指定全区模板；所有目标井同段沿用相同 L3/L4 数量和组合";
        else if(preferredReferenceWellId)summary.reason="采用指定参考井；尚未登记为全区统一模板，作为本次统一方案执行";
        else summary.reason=(selected==primary?"自动推荐覆盖完整段最多、L3 分组及来源记录最完整的标准模板":"主参考模板缺少此段，按相同完整度规则补选全区共用段模板")+std::string("；同分按井编号、模板编号和版本稳定选择，尚未登记统一模板");
        QJsonArray groups;for(int group:s.parentGroups)groups.append(group);
        retainedSegments[segment]=QJsonObject{{"segmentId",q(segment)},{"templateId",q(selected->id)},{"templateVersion",q(selected->version)},{"referenceWellId",double(selected->referenceWellId)},{"level3Count",summary.level3Count},{"level4Count",summary.level4Count},{"parentGroups",groups},{"selectionMode",q(summary.selectionMode)},{"reason",q(summary.reason)},{"sourceRevision",q(selected->sourceRevision)},{"sourceReconciliation",QJsonDocument::fromJson(QByteArray::fromStdString(selected->sourceReconciliationJson)).object()}};
        plan.segmentPlans.push_back(std::move(summary));
    }
    size_t usableSegments=0;
    for(const auto& w:document.wells)for(const std::string segment:{"E12","E3","E4"}){
        if(!segmentFilter.empty()&&segment!=segmentFilter)continue;
        const auto top=segment=="E12"?"T70":segment=="E3"?"T72":"T73";
        const auto bottom=segment=="E12"?"T72":segment=="E3"?"T73":"T80";
        auto a=std::find_if(w.boundaries.begin(),w.boundaries.end(),[&](const auto& b){return b.role==top;});
        auto b=std::find_if(w.boundaries.begin(),w.boundaries.end(),[&](const auto& p){return p.role==bottom;});
        if(a==w.boundaries.end()||b==w.boundaries.end()||!std::isfinite(a->md)||!std::isfinite(b->md)||a->md>=b->md){issue(plan.issues,w.wellId,"template:"+segment,"此区间缺少完整顶底界面，跳过该区间",false);continue;}
        const auto selected=chosen.find(segment);const auto* reference=selected==chosen.end()?nullptr:selected->second;
        if(!reference){issue(plan.issues,w.wellId,"template:"+segment,!document.activeTemplateId.empty()?"已指定全区模板缺少此段或此段结构无效，不能另选模板代替":"该区间没有可验证的人工标准层序，不能猜测 L3/L4 数量",true);continue;}
        if(std::any_of(w.cycles.begin(),w.cycles.end(),[&](const auto& c){return c.segmentId==segment&&manualCycle(c);})){++usableSegments;int l3=0,l4=0;const auto& s=*findSegment(*reference,segment);const bool conflict=manualCountConflict(w,s,l3,l4);plan.hasManualConflicts|=conflict;issue(plan.issues,w.wellId,"template:"+segment,conflict?"保留已有人工解释：本井 L3/L4="+std::to_string(l3)+"/"+std::to_string(l4)+"，与全区模板 "+std::to_string(parentCount(s))+"/"+std::to_string(s.directions.size())+" 的数量或父子组合不一致，需地质校核；不会自动覆盖":"保留该区间已有人工解释；其 L3/L4 数量和父子组合与本次模板一致",false);continue;}
        if(reference->referenceWellId==w.wellId){
            try{
                if(!w.cycles.empty()||std::any_of(w.boundaries.begin(),w.boundaries.end(),protectedBoundary))throw std::runtime_error("当前井已有解释或人工锁定界面，不能自动覆盖；请先核对并载入原始人工解释");
                const auto discovered=std::find_if(discoveredReferences.begin(),discoveredReferences.end(),[&](const auto& r){return r.wellId==w.wellId&&r.input.inputFingerprint==reference->sourceRevision;});
                const auto& input=discovered==discoveredReferences.end()?w.input:discovered->input;
                if(input.manualTrack.intervals.empty()&&input.manualLevel3Track.intervals.empty())throw std::runtime_error("当前输入没有标准井原始人工分层道，不能由模板比例生成原始层位");
                auto manual=quickManual(input,geometry);nameManualRoles(manual);
                const auto extracted=extractTemplate(manual,"verify-manual-source","1",recordedTolerance(*reference));verifyManualSource(*reference,extracted,segment);
                if(std::none_of(plan.manualReferences.begin(),plan.manualReferences.end(),[&](const auto& r){return r.wellId==w.wellId;}))plan.manualReferences.push_back(std::move(manual));
                ++usableSegments;issue(plan.issues,w.wellId,"manualReference:"+segment,"已核对原始人工分层道与标准模板，将恢复原始人工划分",false);
            }catch(const std::exception& e){issue(plan.issues,w.wellId,"manualReference:"+segment,w.name+"：标准井人工解释尚未恢复："+e.what(),true);}
            continue;
        }
        ++usableSegments;auto executable=*reference;if(executable.status=="adopted")executable.status="approved";plan.assignments.push_back({w.wellId,segment,std::move(executable)});
    }
    if(usableSegments==0)issue(plan.issues,0,"template","所选井和区间没有可执行的层序划分或可保留的人工解释，请先补齐界面及对应标准井资料",true);
    QJsonArray auditSegments;for(const std::string segment:{"E12","E3","E4"})if(retainedSegments.contains(segment))auditSegments.append(retainedSegments.at(segment));
    QJsonArray auditIssues;for(const auto& i:plan.issues)auditIssues.append(QJsonObject{{"wellId",double(i.wellId)},{"field",q(i.field)},{"message",q(i.message)},{"blocking",i.blocking}});
    plan.recommendationJson=QJsonDocument(QJsonObject{{"policy","regional_segment_template/v1"},{"selectionMode",q(mode)},{"registeredUnifiedTemplate",!document.activeTemplateId.empty()},{"usesRecommendation",plan.usesRecommendation},{"hasManualConflicts",plan.hasManualConflicts},{"sourceReconciliationToleranceM",sourceReconciliationTolerance},{"sourceDepthsModified",false},{"selectionOrder","complete segments; complete L3 groups; source audit; source revision; persisted approval; stable reference/id/version"},{"segments",auditSegments},{"issues",auditIssues}}).toJson(QJsonDocument::Compact).toStdString();
    return plan;
}
}
