#include "paleo_stratigraphy/conventions.h"
#include "paleo_stratigraphy/workflow.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <iostream>
#include <functional>
#include <stdexcept>

using namespace paleo::data;
using namespace paleo::stratigraphy;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void rejects(const std::function<void()>& action,const char* message){bool failed=false;try{action();}catch(const std::invalid_argument&){failed=true;}check(failed,message);}
CorrelationWellInput well(long long id,bool negative=false){
    CorrelationWellInput w;w.wellId=id;w.well.wellName="SYNTH-"+std::to_string(id);w.well.wellheadX=50;w.well.wellheadY=50;w.well.coordinateSystem="SYNTH:LOCAL";w.well.coordinateUnit="m";
    w.depthInfo.depthType="MD";w.depthInfo.depthUnit="m";w.depthInfo.mdReference="KB";w.surveyDepthInfo=w.depthInfo;
    for(int i=0;i<=700;++i){w.log.axis.push_back(i);w.log.values.push_back(60+10*std::sin(i*.1));}w.log.axisKind="MD";w.log.axisUnit="m";
    auto& t=w.timeDepth;t.depthInfo=w.depthInfo;t.timeUnit="ms";t.timeType="TWT";t.timeReference="DATUM";t.coordinateSystem="SYNTH:LOCAL";t.coordinateUnit="m";
    const double sign=negative?-1.:1.;t.depthTimePairs={{0,sign*(-20),50,50},{350,sign*680,70,50},{700,sign*1380,90,50}};return w;
}
CorrelationHorizon horizon(){CorrelationHorizon h;h.resourceId=10;h.role="T70";auto& s=h.surface;s.coordinateSystem="SYNTH:LOCAL";s.coordinateUnit="m";s.timeUnit="ms";s.timeType="TWT";s.timeReference="DATUM";s.points={{0,0,0,0,200},{1,0,200,0,200},{0,1,0,100,200},{1,1,200,100,200}};return h;}
GeometryOptions policy(){GeometryOptions g;g.resolveFromSources=true;g.confirmedDepthUnit="m";g.confirmedLogDepthType="MD";g.confirmedDepthType="MD";g.confirmedWellTimeUnit="ms";g.confirmedHorizonTimeUnit="ms";g.policyEvidence="Explicit synthetic project conventions";return g;}
QJsonObject object(const std::string& json){return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();}
void declaredAndConflicts(){
    auto w=well(1);auto h=horizon();auto p=policy();auto r=resolveWellConventions(w,{h},p);check(!r.reviewRequired&&r.geometry.wellTimeSign==1,"fully declared data needs no provisional alignment");
    auto neg=well(2,true);auto n=resolveWellConventions(neg,{h},p);check(n.geometry.wellTimeSign==-1&&!n.reviewRequired,"negative time sign resolved separately per well");
    check(neg.timeDepth.depthTimePairs.front().timeValue==20&&neg.timeDepth.depthTimePairs.back().timeValue==-1380,"source values and sign crossing preserved; no abs");
    check(object(n.evidenceJson).value("entries").toArray().size()>0,"resolution has structured evidence");
    auto wrong=w;wrong.log.axisUnit="ft";rejects([&]{resolveWellConventions(wrong,{h},p);},"explicit unit conflicts must fail");
    auto badSurface=h;badSurface.surface.coordinateSystem="OTHER:CRS";rejects([&]{resolveWellConventions(w,{badSurface},p);},"known CRS conflict must fail");
    auto datum=w;datum.timeDepth.depthInfo.mdReference="MSL";rejects([&]{resolveWellConventions(datum,{h},p);},"known datum conflict must fail");
    auto sibling=h;sibling.resourceId=11;sibling.role="T72";sibling.surface.timeType.clear();rejects([&]{resolveWellConventions(w,{h,sibling},p);},"one horizon's TWT declaration does not confirm an undocumented sibling");
    sibling.surface.timeType="TWT";sibling.surface.timeUnit.clear();auto noTimePolicy=p;noTimePolicy.confirmedHorizonTimeUnit.clear();rejects([&]{resolveWellConventions(w,{h,sibling},noTimePolicy);},"one horizon's ms declaration does not confirm an undocumented sibling");
    noTimePolicy.allowProvisionalAlignment=true;check(resolveWellConventions(w,{h,sibling},noTimePolicy).reviewRequired,"sibling metadata inheritance is explicit provisional evidence");
    auto nonmono=w;nonmono.timeDepth.depthTimePairs[1].timeValue=1500;rejects([&]{resolveWellConventions(nonmono,{h},p);},"nonmonotonic time is not corrected by sign");
    auto duplicate=w;duplicate.timeDepth.depthTimePairs[1].depthValue=0;rejects([&]{resolveWellConventions(duplicate,{h},p);},"duplicate MD needs resolution");
    auto unknown=w;unknown.log.axisKind.clear();unknown.depthInfo.depthType.clear();auto inherited=resolveWellConventions(unknown,{h},p,false);check(inherited.geometry.confirmedLogDepthType=="MD"&&unknown.log.axisKind.empty(),"known project depth convention applies only to calculation");
}
void provisional(){
    auto w=well(1,true);auto h=horizon();auto p=policy();w.log.axisKind.clear();w.log.axisUnit.clear();w.depthInfo={};w.timeDepth.depthInfo.mdReference.clear();w.timeDepth.coordinateSystem.clear();w.timeDepth.timeReference.clear();w.timeDepth.timeUnit.clear();h.surface.coordinateSystem.clear();h.surface.coordinateUnit.clear();h.surface.timeUnit.clear();h.surface.timeType.clear();h.surface.timeReference.clear();
    rejects([&]{resolveWellConventions(w,{h},p);},"unknown cross-source alignment remains strict by default");
    p.allowProvisionalAlignment=true;auto r=resolveWellConventions(w,{h},p);check(r.reviewRequired&&r.geometry.confirmedTimeReference=="UNVERIFIED_PROJECT_TWT","unknown time datum is explicitly unverified");
    check(r.geometry.confirmedMdReference=="KB"&&r.geometry.wellTimeSign==-1,"candidate uses same-well datum and uniquely identified direction");
    check(r.geometry.allowIrregularSurface&&r.geometry.maximumSurfaceEdge==750,"candidate sparse surface policy has a bounded interpolation edge");
    check(w.timeDepth.timeReference.empty()&&w.depthInfo.depthUnit.empty()&&h.surface.coordinateSystem.empty(),"source metadata remains unknown after candidate resolution");
    auto shifted=w;shifted.well.wellheadX+=50000;rejects([&]{resolveWellConventions(shifted,{h},p);},"no nearby independent wellhead means no candidate CRS");
    auto distant=h;for(auto& x:distant.surface.points)x.x+=10000;rejects([&]{resolveWellConventions(w,{distant},p);},"far surface points cannot imply common coordinates");
    auto timeMismatch=h;for(auto& x:timeMismatch.surface.points)x.zTime=6000;rejects([&]{resolveWellConventions(w,{timeMismatch},p);},"disjoint time ranges cannot use uncalibrated zero-offset candidate");
    timeMismatch.resourceId=11;timeMismatch.role="T80";auto partial=resolveWellConventions(w,{h,timeMismatch},p);check(partial.excludedHorizonIds==std::vector<long long>{11},"a deep interface outside coverage does not block the covered shallow interface");
    auto depthOnly=w;depthOnly.timeDepth={};auto d=resolveWellConventions(depthOnly,{},p,false);check(d.geometry.confirmedLogDepthType=="MD","curve/manual depth resolution does not require time-depth or CRS");
    GeometryOptions strict;auto unchanged=resolveWellConventions(w,{h},strict);check(!unchanged.geometry.resolveFromSources&&unchanged.geometry.confirmedDepthUnit.empty(),"default path does not introduce assumptions");
}
void undeclaredSameWellDatum(){
    auto w=well(26);w.timeDepthId=126;w.depthInfo.mdReference.clear();w.surveyDepthInfo.mdReference.clear();w.timeDepth.depthInfo.mdReference.clear();
    w.sourceRefsJson="[{\"kind\":\"time_depth\",\"id\":\"126\",\"wellId\":\"26\",\"sourceId\":\"226\"}]";
    const auto raw=w.timeDepth.depthTimePairs;auto p=policy();const auto h=horizon();
    rejects([&]{resolveWellConventions(w,{h},p);},"strict policy still requires a real declared MD reference");
    p.allowProvisionalAlignment=true;const auto candidate=resolveWellConventions(w,{h},p);
    check(candidate.reviewRequired&&candidate.geometry.confirmedMdReference=="UNVERIFIED_SAME_WELL_MD","same-well MD/m with an SDK ownership receipt gets only an explicitly unverified candidate convention");
    bool receipt=false;for(const auto& entry:object(candidate.evidenceJson).value("entries").toArray()){const auto value=entry.toObject();if(value.value("field").toString()=="MD reference"&&value.value("value").toString()=="UNVERIFIED_SAME_WELL_MD"&&value.value("status").toString()=="provisional"&&value.value("source").toString().contains("timeDepthId=126"))receipt=true;}check(receipt,"unknown datum candidate has source binding and provisional audit");
    const auto found=intersectHorizons(w,{h},candidate.geometry);check(found.boundaries.size()==1&&std::abs(found.boundaries[0].md-110)<1e-6,"unknown named MD datum can compute a review candidate without moving source depth");
    check(w.depthInfo.mdReference.empty()&&w.timeDepth.depthInfo.mdReference.empty()&&w.surveyDepthInfo.mdReference.empty()&&w.timeDepth.depthTimePairs[1].depthValue==raw[1].depthValue&&w.timeDepth.depthTimePairs[1].timeValue==raw[1].timeValue,"raw unknown metadata and signed MD/time arrays remain unchanged");
    check(resolveWellConventions(w,{},p,false).geometry.confirmedMdReference=="UNVERIFIED_SAME_WELL_MD","depth-only reads of the same owned snapshot retain the same provisional convention");
    auto noOwnership=w;noOwnership.sourceRefsJson.clear();rejects([&]{resolveWellConventions(noOwnership,{h},p);},"resource IDs alone do not establish same-well source ownership");
    auto otherWell=w;otherWell.sourceRefsJson="[{\"kind\":\"time_depth\",\"id\":\"126\",\"wellId\":\"27\"}]";rejects([&]{resolveWellConventions(otherWell,{h},p);},"another well's time-depth source cannot receive same-well datum assumption");
    auto wrongUnit=w;wrongUnit.timeDepth.depthInfo.depthUnit="ft";rejects([&]{resolveWellConventions(wrongUnit,{h},p);},"unit mismatch is not hidden by the provisional datum");
    auto conflict=w;conflict.depthInfo.mdReference="KB";conflict.timeDepth.depthInfo.mdReference="MSL";rejects([&]{resolveWellConventions(conflict,{h},p);},"explicit conflicting depth origins still fail");
    auto known=w;known.surveyDepthInfo.mdReference="KB";check(resolveWellConventions(known,{h},p).geometry.confirmedMdReference=="KB","existing same-well KB inheritance takes priority over an unknown marker");
}
void sourcedCompanionWithoutWellhead(){
    // Mirrors the SDK schema of a well with raw coordinates retained only as an
    // unresolved remark: the wellhead is absent, while the selected checkshots
    // carry MD/metres and an independently verified companion CRS receipt.
    auto w=well(26,true);w.timeDepthId=1921;w.depthInfo={};w.log.axisKind.clear();w.log.axisUnit.clear();w.surveyDepthInfo={};w.surveyDepthInfo.depthUnit="m";
    w.well.wellheadX=w.well.wellheadY=Missing;w.well.coordinateSystem.clear();w.well.coordinateUnit.clear();w.timeDepth.depthInfo.mdReference.clear();w.timeDepth.coordinateSystem.clear();w.timeDepth.timeUnit.clear();w.timeDepth.timeReference.clear();
    QJsonObject binding{{"wellId","26"},{"timeDepthId","1921"},{"checkshotsSha256",QString(64,'a')}};
    QJsonObject verified{{"canonical_candidate_sha256_matches",true},{"checkshots_csv_exact_5columns_with_recorded_sign",true},{"companion_authorities",QJsonArray{"SIS,502498","EPSG,32650","SIS,550000"}}};
    QJsonObject evidence{{"coordinateSystem","EPSG:32650"},{"coordinateUnit","m"},{"reviewRequired",true},{"timeDatumConfirmed",false},{"sharedDepthDatumConfirmed",false},{"importer",binding},{"independentVerification",verified},{"sourceMetadata",QJsonObject{{"source_crs_companions",QJsonArray{"synthetic-source.crsmeta.xml"}}}}};
    auto refs=[&](const QJsonObject& value){return QJsonDocument(QJsonArray{QJsonObject{{"kind","curves"},{"id","424"},{"wellId","26"}},QJsonObject{{"kind","time_depth"},{"id","1921"},{"wellId","26"},{"sourceId","2199"},{"businessConventionEvidence",value}}}).toJson(QJsonDocument::Compact).toStdString();};
    w.sourceRefsJson=refs(evidence);auto h=horizon();h.surface.coordinateSystem.clear();h.surface.coordinateUnit.clear();h.surface.timeUnit.clear();h.surface.timeType.clear();h.surface.timeReference.clear();auto p=policy();
    rejects([&]{resolveWellConventions(w,{h},p);},"strict mode does not silently treat a business companion receipt as a confirmed source datum");
    p.allowProvisionalAlignment=true;const auto candidate=resolveWellConventions(w,{h},p);
    check(candidate.reviewRequired&&candidate.geometry.confirmedCoordinateSystem=="EPSG:32650"&&candidate.geometry.confirmedMdReference=="UNVERIFIED_SAME_WELL_MD","verified source-bound CRS allows same-well MD candidate without inventing a wellhead or KB");
    const auto result=intersectHorizons(w,{h},candidate.geometry);check(result.boundaries.size()==1&&std::abs(result.boundaries[0].md-110)<1e-6,"bound companion metadata reaches the actual intersection path");
    check(w.timeDepth.coordinateSystem.empty()&&w.timeDepth.depthInfo.mdReference.empty()&&!std::isfinite(w.well.wellheadX)&&h.surface.coordinateSystem.empty()&&w.timeDepth.depthTimePairs.back().timeValue==-1380,"source fields and negative signed arrays remain untouched");
    bool sourced=false;for(const auto& entry:object(candidate.evidenceJson).value("entries").toArray()){const auto row=entry.toObject();if(row.value("source").toString().contains("bound_time_depth_companion_receipt")&&row.value("status").toString()=="provisional")sourced=true;}check(sourced,"companion decision remains explicitly sourced and provisional");
    auto missing=w;missing.sourceRefsJson=refs({});rejects([&]{resolveWellConventions(missing,{h},p);},"coordinate numerical ranges alone cannot replace companion evidence");
    auto mismatch=evidence;auto wrongBinding=binding;wrongBinding["timeDepthId"]="1922";mismatch["importer"]=wrongBinding;auto other=w;other.sourceRefsJson=refs(mismatch);rejects([&]{resolveWellConventions(other,{h},p);},"receipt from another time-depth resource cannot be reused");
    auto unverifiable=evidence;auto otherAuthority=verified;otherAuthority["companion_authorities"]=QJsonArray{"EPSG,4326"};unverifiable["independentVerification"]=otherAuthority;other=w;other.sourceRefsJson=refs(unverifiable);rejects([&]{resolveWellConventions(other,{h},p);},"companion authority must support the claimed coordinate system");
    auto declaredConflict=w;declaredConflict.timeDepth.coordinateSystem="EPSG:4326";rejects([&]{resolveWellConventions(declaredConflict,{h},p);},"business evidence never overrides explicitly different source CRS");
    auto horizonConflict=h;horizonConflict.surface.coordinateSystem="EPSG:4326";rejects([&]{resolveWellConventions(w,{horizonConflict},p);},"conflicting declared horizon CRS remains a hard error");
}
void workflow(){
    InterpretationDocument doc;doc.id="SYNTHETIC";doc.snapshot.id="SYNTH-input";doc.snapshot.fingerprint="SYNTH-fingerprint";doc.snapshot.horizons={horizon()};doc.snapshot.wells={well(1),well(2,true)};
    auto p=policy();auto out=calculateHorizons(doc,p);check(out.runs.back().status=="complete"&&out.wells.size()==2,"mixed positive/negative well workflow succeeds independently");
    for(const auto& w:out.wells){check(w.boundaries.size()==1&&std::abs(w.boundaries.front().md-110)<1e-5,"same physical depth for opposite exported time signs");check(object(w.boundaries.front().evidenceJson).contains("autoConventions"),"boundary keeps business resolution audit");}
    check(object(out.runs.back().parametersJson).value("autoConventions").toArray().size()==2,"run keeps separate per-well resolution records");
    check(out.wells[1].input.timeDepth.depthTimePairs.front().timeValue==20&&doc.wells.empty(),"workflow preserves input source and caller document");
    auto draft=doc;draft.snapshot.wells.resize(1);auto& w=draft.snapshot.wells[0];w.timeDepth.coordinateSystem.clear();w.timeDepth.timeReference.clear();w.warnings={"曲线深度类型或单位尚未确认","时深表已有沿井位置点；其坐标或深度约定尚需确认，无需另行假定直井","T70局部层位仅按数值坐标提取候选；坐标系/单位尚未确认，不能直接自动落井"};auto& h=draft.snapshot.horizons[0].surface;h.coordinateSystem.clear();h.timeReference.clear();p.allowProvisionalAlignment=true;
    auto candidate=calculateHorizons(draft,p);check(candidate.runs.back().status=="complete"&&candidate.wells[0].boundaries.size()==1,"supported provisional calculation produces a draft");const auto& b=candidate.wells[0].boundaries[0];check(b.status=="review"&&object(b.evidenceJson)["autoConventions"].toObject()["reviewRequired"].toBool(),"candidate must remain visibly unverified");
    check(std::none_of(candidate.wells[0].warnings.begin(),candidate.wells[0].warnings.end(),[](const auto& x){return x=="曲线深度类型或单位尚未确认";})&&candidate.wells[0].input.warnings.size()==3,"resolved UI warnings do not mutate original input warnings");
    auto partialInput=draft;auto deep=partialInput.snapshot.horizons.front();deep.resourceId=11;deep.role="T80";for(auto& point:deep.surface.points)point.zTime=6000;partialInput.snapshot.horizons.push_back(deep);auto partial=calculateHorizons(partialInput,p);
    check(partial.runs.back().status=="partial"&&partial.wells[0].boundaries.size()==1,"a requested deep interface outside coverage produces partial, keeping supported shallow results");
    check(partial.runs.back().diagnosticsJson.find("T80")!=std::string::npos,"partial horizon run explains the missing requested interface");
}
void boundedPropagationAudit(){
    InterpretationDocument doc;doc.id="SYNTH-AUDIT";doc.snapshot.wells={well(1)};auto h1=horizon(),h2=horizon();h2.resourceId=11;h2.role="T72";for(auto& point:h2.surface.points)point.zTime=600;doc.snapshot.horizons={h1,h2};
    doc.snapshot.wells[0].timeDepth.timeReference.clear();for(auto& h:doc.snapshot.horizons)h.surface.timeReference.clear();auto p=policy();p.allowProvisionalAlignment=true;doc=calculateHorizons(doc,p);check(doc.wells[0].boundaries.size()==2,"candidate two-interface fixture");
    InterpretationTemplate t;t.id="synthetic-external";t.version="1";t.name="Authored test template";t.referenceWellId=999;t.status="approved";InterpretationTemplateSegment s;s.segmentId="E12";s.boundaryFractions={0,.5,1};s.directions={1,-1};s.parentGroups={0,0};s.parentDirections={1,1};s.sequenceIdentities={"E12-A","E12-B"};t.segments={s};doc.profile.arOrders={4};doc.profile.padding=10;
    std::vector<TemplateAssignment> assignments{{1,"E12",t}};size_t firstSize=0,lastSize=0;
    for(int i=0;i<5;++i){doc=propagateTemplates(doc,assignments,p,{}, {},"E12");check(doc.runs.back().status=="complete","repeated candidate propagation remains executable");lastSize=0;for(const auto& b:doc.wells[0].boundaries){lastSize+=b.evidenceJson.size();check(object(b.evidenceJson).value("autoConventions").toObject().value("reviewRequired").toBool(),"all child boundaries retain parent alignment review");}if(i==0)firstSize=lastSize;}
    check(lastSize<=firstSize*3,"repeated propagation evidence remains bounded rather than nesting audit history");
}
}
int main(){try{declaredAndConflicts();provisional();undeclaredSameWellDatum();sourcedCompanionWithoutWellhead();workflow();boundedPropagationAudit();std::cout<<"Convention resolution, independent time signs, source-bound companion CRS, unverified same-well MD, bounded candidate audit and immutable inputs passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
