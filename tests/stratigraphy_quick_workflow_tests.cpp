#include "paleo_stratigraphy/quick_workflow.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
using namespace paleo::stratigraphy;
namespace {
std::string u(const QString& value){return value.toUtf8().toStdString();}
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes){QFile f(path);check(f.open(QIODevice::WriteOnly),"write fixture");check(f.write(bytes)==bytes.size(),"complete fixture");}
void import(DataService& service,const QString& path,const QByteArray& bytes){write(path,bytes);std::string error;ImportReport report;check(service.importFile(u(path),{},report,error),error);check(report.failed==0&&report.imported+report.review>0,"fixture import produced no data");}
bool has(const std::vector<QuickIssue>& issues,const std::string& field,bool blocking){return std::any_of(issues.begin(),issues.end(),[&](const auto& i){return i.field==field&&i.blocking==blocking;});}
QByteArray las(const char* well,const char* curve,int offset=0){return QByteArray("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. ")+well+"\nNULL. -999.25\n~Curve\nMD.m\n"+curve+".API\n~Ascii\n100 "+QByteArray::number(10+offset)+"\n101 20\n102 30\n";}
QByteArray td(int offset=0){return QByteArray("# Petrel checkshots format\n# Unit in depth: m\n# Unit in X and Y direction: m\nVERSION 1\nBEGIN HEADER\nX\nY\nZ\nTWT picked\nMD\nWell\nEND HEADER\n10 20 -100 ")+QByteArray::number(-200-offset)+" 100 \"SYNTH-1\"\n11 21 -102 -204 102 \"SYNTH-1\"\n";}
void inputMatching(){
    QTemporaryDir temp;check(temp.isValid(),"temp directory");DataService service;std::string error;check(service.createProject(u(temp.path()+"/project"),"SYNTHETIC quick workflow",error),error);
    import(service,temp.path()+"/SYNTH-1.las",las("SYNTH-1","GR"));
    import(service,temp.path()+"/SYNTH-2.las",las("SYNTH-2","AC"));
    import(service,temp.path()+"/checkshots.txt",td());
    const QByteArray surface="INLINE : 1 XLINE : 1 10 20 100\nINLINE : 1 XLINE : 2 10 30 100\nINLINE : 2 XLINE : 1 20 20 100\n";
    int unique=0;for(const auto* role:{"T70","T72","SYNTH_T73","T80","T700"})import(service,temp.path()+"/"+role+".dat",surface+"INLINE : 2 XLINE : 2 20 30 "+QByteArray::number(101+unique++)+"\n");
    std::vector<WellSummary> wells;check(service.listWells(wells,error)&&wells.size()==2,"two wells");
    const auto one=std::find_if(wells.begin(),wells.end(),[](const auto& w){return w.name=="SYNTH-1";})->id;
    const auto two=std::find_if(wells.begin(),wells.end(),[](const auto& w){return w.name=="SYNTH-2";})->id;
    auto plan=planQuickInputs(service,{one});check(plan.ready(),"unique GR, time-depth XYZ and four roles must need no binding dialog");check(plan.request.wells.size()==1&&plan.request.horizons.size()==4,"all required bindings");
    check(plan.wellSummaries.front().coordinateSource=="timeDepthXYZ"&&!plan.request.wells.front().confirmedVertical,"XYZ path is not replaced by a guessed vertical well");
    check(has(plan.issues,"manualL4",false)&&has(plan.issues,"manualL3",false)&&has(plan.issues,"formation",false),"optional manual tracks do not block horizon acquisition");
    TimeDepthTable source;check(service.readTimeDepth(plan.request.wells.front().timeDepthId,source,error),error);check(source.timeUnit.empty()&&source.coordinateSystem.empty()&&source.timeReference.empty()&&source.depthTimePairs.front().timeValue==-200,"matching leaves unknown conventions and negative time values unchanged");
    check(!planQuickInputs(service,{}).ready()&&!planQuickInputs(service,{one,one}).ready(),"empty/duplicate well lists are not silently repaired");
    auto unsuitable=planQuickInputs(service,{two});check(!unsuitable.ready()&&has(unsuitable.issues,"curve",true)&&has(unsuitable.issues,"timeDepth",true),"a unique AC curve must not be labelled as GR");
    auto explicitRequest=plan.request;explicitRequest.surfaceSearchRadius=123;explicitRequest.wells[0].processed=false;
    import(service,temp.path()+"/SYNTH-1-extra.las",las("SYNTH-1","GR",7));
    import(service,temp.path()+"/checkshots-extra.txt",td(10));
    import(service,temp.path()+"/T70-copy.dat",surface+"INLINE : 2 XLINE : 2 20 30 150\n");
    auto ambiguous=planQuickInputs(service,{one});check(!ambiguous.ready()&&has(ambiguous.issues,"curve",true)&&has(ambiguous.issues,"timeDepth",true)&&has(ambiguous.issues,"horizon:T70",true),"multiple GR, time-depth and horizon candidates require an explicit choice");
    auto issue70=std::find_if(ambiguous.issues.begin(),ambiguous.issues.end(),[](const auto& i){return i.field=="horizon:T70";});check(issue70!=ambiguous.issues.end()&&issue70->candidateIds.size()==2,"T700 is not a T70 candidate");
    auto explicitPlan=planQuickInputs(service,{one},explicitRequest);check(explicitPlan.ready()&&explicitPlan.request.wells.front().curveId==plan.request.wells.front().curveId&&explicitPlan.request.wells.front().timeDepthId==plan.request.wells.front().timeDepthId,"previous explicit choices survive additional candidates");
    check(explicitPlan.request.surfaceSearchRadius==123&&!explicitPlan.request.wells.front().processed,"request options survive automation");
    auto wrongOwner=explicitRequest;std::vector<DataEntry> other;check(service.listData("curves",two,other,error)&&!other.empty(),"other curve");wrongOwner.wells[0].curveId=other.front().id;
    auto invalid=planQuickInputs(service,{one},wrongOwner);check(!invalid.ready()&&invalid.request.wells.front().curveId==0,"a resource from another well is not silently accepted or substituted");
    wrongOwner=explicitRequest;wrongOwner.horizons[1].resourceId=wrongOwner.horizons[0].resourceId;check(!planQuickInputs(service,{one},wrongOwner).ready(),"same surface cannot serve two roles");
}
InterpretationBoundary pick(const std::string& id,double md,const std::string& role={}){InterpretationBoundary p;p.id=id;p.name=role.empty()?id:role;p.role=role;p.md=p.originalMd=md;p.source="manual";p.status="confirmed";return p;}
WellInterpretation manualWell(long long id){WellInterpretation w;w.wellId=id;w.name="SYNTH-"+std::to_string(id);w.input.wellId=id;w.input.well.wellName=w.name;w.input.inputFingerprint="synthetic-"+std::to_string(id);w.boundaries={pick("top",100,"T70"),pick("mid",130),pick("bottom",200,"T72")};
    InterpretationCycle parent;parent.id="p";parent.name="L3";parent.segmentId="E12";parent.level=3;parent.topBoundaryId="top";parent.bottomBoundaryId="bottom";parent.direction=1;parent.source="manual";parent.status="confirmed";w.cycles.push_back(parent);
    InterpretationCycle first;first.id="a";first.name="L4-a";first.segmentId="E12";first.parentId="p";first.topBoundaryId="top";first.bottomBoundaryId="mid";first.direction=1;first.source="manual";first.status="confirmed";w.cycles.push_back(first);auto second=first;second.id="b";second.name="L4-b";second.topBoundaryId="mid";second.bottomBoundaryId="bottom";second.direction=-1;w.cycles.push_back(second);return w;
}
WellInterpretation targetWell(long long id){auto w=manualWell(id);w.cycles.clear();w.boundaries.erase(w.boundaries.begin()+1);for(auto& b:w.boundaries){b.source="seismic_horizon";b.status="review";b.locked=false;}return w;}
void templateMatching(){
    InterpretationDocument d;d.wells={manualWell(1),targetWell(2)};
    auto plan=planQuickTemplates(d,{},0,"E12");check(plan.ready()&&plan.candidates.size()==1&&plan.assignments.size()==1,"unique manual standard is automatically selected");check(plan.assignments.front().wellId==2&&plan.assignments.front().reference.referenceWellId==1,"standard itself remains manual");
    check(plan.assignments.front().reference.segments.front().boundaryFractions[1]==.3,"nonuniform real reference fractions retained");
    const auto version=plan.candidates.front().version;auto again=planQuickTemplates(d,{},0,"E12");check(again.candidates.front().version==version,"automatic template identities are reproducible");d.wells[0].boundaries[1].md=140;auto changed=planQuickTemplates(d,{},0,"E12");check(changed.candidates.front().version!=version,"editing the manual reference changes its template version");
    const auto strict=planQuickTemplates(d,{},0,"E12",0);check(strict.candidates.front().version!=changed.candidates.front().version,"different reconciliation audit cannot reuse an immutable template version");
    d.wells.push_back(manualWell(3));auto many=planQuickTemplates(d,{},0,"E12");check(many.ready()&&many.assignments.size()==1&&many.assignments.front().reference.referenceWellId==1,"multiple manual standards use a stable common regional recommendation");
    check(many.usesRecommendation&&many.segmentPlans.size()==1&&many.segmentPlans.front().level3Count==1&&many.segmentPlans.front().level4Count==2,"recommendation exposes real manual counts without claiming a registered standard");
    const auto recommendation=QJsonDocument::fromJson(QByteArray::fromStdString(many.recommendationJson)).object();check(!recommendation.value("registeredUnifiedTemplate").toBool()&&recommendation.value("sourceReconciliationToleranceM").toDouble()==2,"recommendation audit records unregistered status and business tolerance");
    auto selected=planQuickTemplates(d,{},3,"E12");check(selected.ready()&&selected.assignments.size()==1&&selected.assignments.front().reference.referenceWellId==3,"one explicit reference choice resolves eligible target segments");
    d.templates=many.candidates;d.activeTemplateId=d.templates.front().id;auto adopted=planQuickTemplates(d,{},0,"E12");check(adopted.ready()&&adopted.assignments.front().reference.id==d.activeTemplateId,"adopted version takes precedence");
    auto override=planQuickTemplates(d,{},3,"E12");check(override.ready()&&override.assignments.front().reference.id==d.activeTemplateId&&!override.usesRecommendation,"active regional template strictly takes precedence over reference preference");
    d.activeTemplateId.clear();InterpretationClustering groups;groups.id="adopted-groups";groups.members={{1,0,true},{2,1,false},{3,1,true}};d.clusterings={groups};d.activeClusteringId=groups.id;auto grouped=planQuickTemplates(d,{},0,"E12");check(grouped.ready()&&grouped.assignments.front().reference.referenceWellId==1,"target cluster no longer changes the regional template");
    std::reverse(d.wells.begin(),d.wells.end());std::reverse(d.templates.begin(),d.templates.end());auto reordered=planQuickTemplates(d,{},0,"E12");check(reordered.ready()&&reordered.assignments.front().reference.id==grouped.assignments.front().reference.id,"recommendation does not depend on input well/template order");
    d.clusterings.clear();d.activeClusteringId.clear();d.templates[0].status="draft";d.templates[1].status="draft";d.wells={targetWell(2)};auto drafts=planQuickTemplates(d,{},0,"E12");check(!drafts.ready()&&drafts.candidates.empty(),"unadopted draft templates are not used silently");
    check(!planQuickTemplates(d,{},0,"UNKNOWN").ready(),"invalid segment fails explicitly");
    auto invalid=manualWell(1);invalid.cycles[1].direction=0;d.templates.clear();d.wells={invalid,targetWell(2)};auto bad=planQuickTemplates(d,{},0,"E12");check(!bad.ready()&&has(bad.issues,"manualTemplate",false),"invalid reference direction is not inferred");
}
WellInterpretation sourceWell(long long id){
    auto w=targetWell(id);auto& input=w.input;DepthInfo depth;depth.depthType="MD";depth.depthUnit="m";depth.mdReference="SYNTHETIC_KB";
    input.formationTrackId=1;input.formationTrack.depthInfo=depth;input.formationTrack.trackName="段";GeologicalInterval member;member.intervalName="恩平一二段";member.topDepth=100;member.bottomDepth=200;input.formationTrack.intervals={member};
    input.intervalTrackId=2;input.manualTrack.trackName="四级";input.manualTrack.depthInfo=depth;
    GeologicalInterval a;a.intervalId=1;a.intervalName="正旋回";a.topDepth=100;a.bottomDepth=130;auto b=a;b.intervalId=2;b.intervalName="反旋回";b.topDepth=130;b.bottomDepth=200;input.manualTrack.intervals={a,b};
    input.manualLevel3TrackId=3;input.manualLevel3Track.trackName="三级";input.manualLevel3Track.depthInfo=depth;a.bottomDepth=200;input.manualLevel3Track.intervals={a};
    return w;
}
void sourceTrackTemplate(){
    InterpretationDocument doc;auto w=sourceWell(1);
    doc.wells={w,targetWell(2)};auto plan=planQuickTemplates(doc,{},0,"E12");check(plan.ready()&&plan.candidates.size()==1&&plan.manualReferences.size()==1&&plan.assignments.size()==1,"manual SDK tracks automatically produce a strict reference template");
    check(doc.wells.front().cycles.empty()&&plan.manualReferences.front().cycles.size()==3,"planner never overwrites the caller's current seismic framework");
    doc.wells.front().input.manualTrack.intervals[1].topDepth=130.5;auto gap=planQuickTemplates(doc,{},0,"E12",0);check(!gap.ready()&&gap.candidates.empty(),"explicit strict policy still rejects source gaps");
    auto reconciled=planQuickTemplates(doc,{},0,"E12");check(reconciled.ready()&&reconciled.candidates.size()==1,"business policy permits bounded source ordinal reconciliation");
    const auto audit=QJsonDocument::fromJson(QByteArray::fromStdString(reconciled.candidates.front().sourceReconciliationJson)).object();check(audit.value("toleranceM").toDouble()==2&&!audit.value("sourceDepthsModified").toBool(),"two-metre policy is explicit and original depths are retained");
    check(doc.wells.front().input.manualTrack.intervals[1].topDepth==130.5&&reconciled.manualReferences.front().input.manualTrack.intervals[1].topDepth==130.5,"source discrepancy is not erased");
    doc.wells.front().input.manualTrack.intervals[1].topDepth=132.1;check(!planQuickTemplates(doc,{},0,"E12").ready(),"source gaps beyond the business bound are refused");
    check(!planQuickTemplates(doc,{},0,"E12",3).ready(),"caller cannot silently enlarge the source reconciliation limit");
}
void registeredStandardRestoration(){
    InterpretationDocument original;original.wells={sourceWell(1),targetWell(2)};const auto extracted=planQuickTemplates(original,{},0,"E12");check(extracted.ready(),"initial source template");
    original.templates=extracted.candidates;original.activeTemplateId=original.templates.front().id;
    auto restored=planQuickTemplates(original,{},0,"E12");check(restored.ready()&&restored.manualReferences.size()==1&&restored.assignments.size()==1,"registered standard with empty displayed cycles restores the real source interpretation");
    check(restored.manualReferences.front().cycles.size()==3&&original.wells.front().cycles.empty(),"restore is a returned proposal, not a mutation");
    const auto& reference=restored.manualReferences.front();check(std::any_of(reference.boundaries.begin(),reference.boundaries.end(),[](const auto& b){return b.md==130&&b.source=="imported_interpretation"&&b.locked;}),"original nonuniform manual depth and protection preserved");
    check(std::any_of(reference.boundaries.begin(),reference.boundaries.end(),[](const auto& b){return b.role=="T70"&&b.name=="T70"&&b.md==100;}),"restored source anchor has the public role used by quick UI");
    auto onlyStandard=original;onlyStandard.wells.resize(1);auto restoreOnly=planQuickTemplates(onlyStandard,{},0,"E12");check(restoreOnly.ready()&&restoreOnly.assignments.empty()&&restoreOnly.manualReferences.size()==1,"restoring a single selected standard is useful work without self-propagation");
    auto absent=original;absent.wells[0].input.manualTrack.intervals.clear();absent.wells[0].input.manualLevel3Track.intervals.clear();auto missing=planQuickTemplates(absent,{},0,"E12");check(!missing.ready()&&missing.manualReferences.empty()&&has(missing.issues,"manualReference:E12",true),"template ratios must not fabricate missing original manual depths");
    auto unaudited=original;unaudited.templates.front().sourceReconciliationJson.clear();auto noReceipt=planQuickTemplates(unaudited,{},0,"E12");check(!noReceipt.ready()&&noReceipt.manualReferences.empty(),"a matching well ID and relative fractions alone do not authorize absolute reconstruction");
    auto shifted=original;for(auto* track:{&shifted.wells[0].input.formationTrack,&shifted.wells[0].input.manualTrack,&shifted.wells[0].input.manualLevel3Track})for(auto& row:track->intervals){row.topDepth+=50;row.bottomDepth+=50;}
    auto displaced=planQuickTemplates(shifted,{},0,"E12");check(!displaced.ready()&&displaced.manualReferences.empty(),"same ratios and even unchanged source fingerprint do not hide changed absolute manual depths");
    auto otherSource=original;otherSource.wells[0].input.intervalTrackId=99;auto switched=planQuickTemplates(otherSource,{},0,"E12");check(!switched.ready()&&switched.manualReferences.empty(),"different source track identity needs review even when numerical fractions match");
    auto changedStructure=original;changedStructure.templates[0].segments[0].directions[0]=-1;auto changed=planQuickTemplates(changedStructure,{},0,"E12");check(!changed.ready()&&changed.manualReferences.empty(),"registered template must match source polarity and grouping");
    auto locked=original;locked.wells[0].boundaries.front().locked=true;locked.wells[0].boundaries.front().md+=2;auto protectedPlan=planQuickTemplates(locked,{},0,"E12");check(!protectedPlan.ready()&&protectedPlan.manualReferences.empty(),"automatic restoration does not overwrite a current manually locked interface");
    auto newSnapshot=original;newSnapshot.wells[0].input.inputFingerprint="different-source-snapshot";auto changedSnapshot=planQuickTemplates(newSnapshot,{},0,"E12");check(!changedSnapshot.ready()&&changedSnapshot.manualReferences.empty(),"changed source metadata requires review even if absolute numerical depths still match");
}
void noExecutableSegments(){
    InterpretationDocument doc;doc.templates={extractTemplate(manualWell(9),"external-standard","1",0)};doc.wells={targetWell(2)};doc.wells.front().boundaries.clear();
    auto missing=planQuickTemplates(doc,{},0,"E12");check(!missing.ready()&&missing.assignments.empty()&&has(missing.issues,"template:E12",false)&&has(missing.issues,"template",true),"all missing interfaces produces an explicit no-work issue, not apparent success");
    doc.wells.push_back(targetWell(3));auto partial=planQuickTemplates(doc,{},0,"E12");check(partial.ready()&&partial.assignments.size()==1&&partial.assignments.front().wellId==3&&has(partial.issues,"template:E12",false),"one unavailable well does not block a complete target interval");
    doc.wells={targetWell(2)};doc.wells.front().boundaries.push_back(pick("T73",300,"T73"));auto wrongSegment=planQuickTemplates(doc,{},0,"E3");check(!wrongSegment.ready()&&wrongSegment.assignments.empty()&&has(wrongSegment.issues,"template:E3",true),"candidates for another segment cannot satisfy the requested segment");
    doc.wells={manualWell(1)};doc.templates.clear();auto manualOnly=planQuickTemplates(doc,{},0,"E12");check(manualOnly.ready()&&manualOnly.assignments.empty()&&!has(manualOnly.issues,"template",true),"an existing manual standard can still calculate curves while preserving its cycles");
}
InterpretationTemplate regionalTemplate(const std::string& id,long long reference,const std::vector<std::string>& segments){
    auto t=extractTemplate(manualWell(reference),id,"1",0);const auto base=t.segments.front();t.segments.clear();
    for(const auto& segment:segments){auto s=base;s.segmentId=segment;s.sequenceIdentities={segment+"-a",segment+"-b"};t.segments.push_back(s);}return t;
}
WellInterpretation regionalTarget(long long id){auto w=targetWell(id);auto a=pick("T73",300,"T73"),b=pick("T80",400,"T80");a.source=b.source="seismic_horizon";a.status=b.status="review";w.boundaries.push_back(a);w.boundaries.push_back(b);return w;}
void regionalUniformity(){
    InterpretationDocument doc;doc.wells={regionalTarget(20),regionalTarget(21)};
    doc.templates={regionalTemplate("partial",1,{"E12"}),regionalTemplate("complete",9,{"E12","E3","E4"})};
    InterpretationClustering groups;groups.id="groups";groups.members={{20,0,false},{21,1,false},{1,0,true},{9,1,true}};doc.activeClusteringId=groups.id;doc.clusterings={groups};
    auto plan=planQuickTemplates(doc);check(plan.ready()&&plan.assignments.size()==6&&plan.segmentPlans.size()==3,"all target segments are planned once from one regional reference");
    for(const auto& a:plan.assignments)check(a.reference.id=="complete"&&a.reference.referenceWellId==9,"complete common template takes priority over target cluster");
    for(const auto& summary:plan.segmentPlans)check(summary.level3Count==1&&summary.level4Count==2&&summary.parentGroups==std::vector<int>({0,0}),"all segment summaries use actual reference hierarchy");
    doc.templates.back().segments.pop_back();doc.templates.push_back(regionalTemplate("fallback",7,{"E4"}));
    auto fallback=planQuickTemplates(doc);check(fallback.ready()&&fallback.segmentPlans.size()==3,"missing primary segment gets one common secondary reference");
    for(const auto& a:fallback.assignments)check(a.reference.id==(a.segmentId=="E4"?"fallback":"complete"),"fallback is selected by segment, never by target well");
    doc.activeTemplateId="complete";auto incompleteActive=planQuickTemplates(doc);check(!incompleteActive.ready()&&has(incompleteActive.issues,"template:E4",true),"active missing segment cannot silently use another standard");
    doc.activeTemplateId="missing";check(!planQuickTemplates(doc).ready(),"missing active ID is not ignored");
    doc.activeTemplateId="complete";auto duplicate=doc.templates[1];duplicate.version="2";doc.templates.push_back(duplicate);check(!planQuickTemplates(doc).ready(),"active ID with multiple versions is explicit conflict");
    doc.templates.pop_back();doc.activeTemplateId.clear();doc.wells={manualWell(20),targetWell(21)};doc.templates={regionalTemplate("two-parents",9,{"E12"})};doc.templates[0].segments[0].parentGroups={0,1};
    auto conflict=planQuickTemplates(doc,{},0,"E12");check(conflict.ready()&&conflict.hasManualConflicts&&conflict.assignments.size()==1,"manual count mismatch is retained and reported without blocking other wells");
    check(doc.wells[0].cycles.size()==3&&doc.wells[0].boundaries[1].md==130,"planner does not rewrite manual interpretation");
    doc.activeTemplateId="two-parents";doc.templates[0].segments[0].parentGroups={0,0};auto same=planQuickTemplates(doc,{},0,"E12");check(same.ready()&&!same.hasManualConflicts,"matching manual hierarchy has no false conflict");
}
void persistentSegmentBindings(){
    InterpretationDocument doc;doc.wells={regionalTarget(20),regionalTarget(21)};
    doc.templates={regionalTemplate("e12-standard",1,{"E12"}),regionalTemplate("e3-standard",2,{"E3"}),regionalTemplate("e4-standard",3,{"E4"})};
    auto save=[&](const QuickTemplatePlan& plan){doc.viewStateJson=QJsonDocument(QJsonObject{{"uniformTemplatePlan",QJsonDocument::fromJson(QByteArray::fromStdString(plan.recommendationJson)).object()}}).toJson(QJsonDocument::Compact).toStdString();};
    int count=0;for(const std::string segment:{"E12","E3","E4"}){const auto plan=planQuickTemplates(doc,{},0,segment);check(plan.ready()&&plan.segmentPlans.size()==1&&plan.assignments.size()==2,"filtered run operates on only the requested segment");check(QJsonDocument::fromJson(QByteArray::fromStdString(plan.recommendationJson)).object().value("segments").toArray().size()==++count,"filtered audit retains all previously bound segments");save(plan);}
    doc.templates.push_back(regionalTemplate("new-complete-standard",9,{"E12","E3","E4"}));
    const auto reused=planQuickTemplates(doc);check(reused.ready()&&reused.segmentPlans.size()==3,"saved per-segment bindings remain executable");
    for(const auto& segment:reused.segmentPlans){check(segment.selectionMode=="reused_recommendation","reused recommendation is not relabelled as manual approval");check(segment.referenceWellId==(segment.segmentId=="E12"?1:segment.segmentId=="E3"?2:3),"new high-coverage candidate cannot silently replace previous segment references");}
    auto wrongVersion=doc;wrongVersion.templates[1].version="2";const auto stale=planQuickTemplates(wrongVersion,{},0,"E3");check(!stale.ready()&&has(stale.issues,"template:E3",true),"missing exact saved version blocks its segment instead of switching reference");
    check(planQuickTemplates(wrongVersion,{},0,"E12").ready(),"an unrequested segment conflict does not block a valid requested segment");
    auto changedSource=doc;auto audit=QJsonDocument::fromJson(QByteArray::fromStdString(changedSource.templates[0].sourceReconciliationJson)).object();audit["toleranceM"]=1;changedSource.templates[0].sourceReconciliationJson=QJsonDocument(audit).toJson(QJsonDocument::Compact).toStdString();check(!planQuickTemplates(changedSource,{},0,"E12").ready(),"changed source audit under the same immutable ID/version is rejected");
    auto changedGroups=doc;changedGroups.templates[0].segments[0].parentGroups={0,1};check(!planQuickTemplates(changedGroups,{},0,"E12").ready(),"changed hierarchy under a saved ID/version is rejected");
    auto explicitReference=planQuickTemplates(doc,{},9,"E12");check(explicitReference.ready()&&explicitReference.segmentPlans.front().referenceWellId==9&&QJsonDocument::fromJson(QByteArray::fromStdString(explicitReference.recommendationJson)).object().value("segments").toArray().size()==3,"explicit reference replaces only the requested binding while preserving other segments");
    doc.activeTemplateId="new-complete-standard";const auto active=planQuickTemplates(doc,{},0,"E12");check(active.ready()&&active.segmentPlans.front().referenceWellId==9&&active.segmentPlans.front().selectionMode=="active_template","explicit active template takes priority over a saved recommendation");
}
void discoverUnselectedReferences(){
    QTemporaryDir temp;check(temp.isValid(),"discovery temp");DataService service;std::string error;check(service.createProject(u(temp.path()+"/project"),"Synthetic discovery",error),error);
    import(service,temp.path()+"/SYNTH-1.las",las("SYNTH-1","GR"));import(service,temp.path()+"/SYNTH-2.las",las("SYNTH-2","GR"));
    import(service,temp.path()+"/manual.csv",QString::fromUtf8("井号,道名,层号,顶深,底深\nSYNTH-1,段,恩平一二段,100,200\nSYNTH-1,三级,正旋回,100,200\nSYNTH-1,四级,正旋回,100,130\nSYNTH-1,四级,反旋回,130,200\n").toUtf8());
    GeometryOptions geometry;geometry.resolveFromSources=true;geometry.confirmedDepthType="MD";geometry.confirmedLogDepthType="MD";geometry.confirmedDepthUnit="m";geometry.confirmedMdReference="SYNTH_KB";geometry.policyEvidence="Synthetic declared MD/metres and datum";
    auto found=discoverQuickReferences(service,geometry);check(found.templates.size()==1&&found.manualReferences.size()==1,"discovers an unselected manual standard via the public SDK without horizons/time-depth");
    check(found.manualReferences[0].input.timeDepthId==0&&found.manualReferences[0].input.timeDepth.depthTimePairs.empty(),"reference discovery does not require unrelated seismic geometry");
    std::vector<InterpretationTemplate> saved;check(service.listReferenceTemplates(saved,error)&&saved.empty(),"discovery does not silently register its recommendation");
    auto again=discoverQuickReferences(service,geometry);check(again.templates.size()==1&&again.templates[0].version==found.templates[0].version,"immutable captures keep stable reference identities for unchanged input");
    InterpretationDocument doc;doc.templates=found.templates;doc.wells={targetWell(99)};auto plan=planQuickTemplates(doc,geometry,0,"E12",2,found.manualReferences);check(plan.ready()&&plan.assignments.size()==1&&plan.assignments[0].reference.referenceWellId==found.templates[0].referenceWellId,"unselected discovered well supplies the common target template");
    doc.wells={targetWell(found.templates[0].referenceWellId)};doc.wells[0].input.inputFingerprint="different-target-with-time-depth";doc.activeTemplateId=found.templates[0].id;
    auto restored=planQuickTemplates(doc,geometry,0,"E12",2,found.manualReferences);check(restored.ready()&&restored.manualReferences.size()==1,"selected standard restores from its exact discovered source, not target seismic fingerprint");
    auto altered=found.manualReferences;altered[0].input.inputFingerprint="changed";check(!planQuickTemplates(doc,geometry,0,"E12",2,altered).ready(),"mismatched discovered source cannot bypass source revision validation");
    bool cancelled=false;try{discoverQuickReferences(service,geometry,[]{return true;});}catch(const WorkflowCancelled&){cancelled=true;}check(cancelled,"discovery honours cancellation");
    import(service,temp.path()+"/SYNTH-1-extra.las",las("SYNTH-1","GR",9));auto ambiguous=discoverQuickReferences(service,geometry);check(ambiguous.templates.empty()&&has(ambiguous.issues,"curve",false),"multiple GR resources are not chosen by list order");
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{inputMatching();templateMatching();sourceTrackTemplate();registeredStandardRestoration();noExecutableSegments();regionalUniformity();persistentSegmentBindings();discoverUnselectedReferences();std::cout<<"Quick workflow tests passed: regional templates, stable per-segment bindings, bounded reconciliation, source restoration and project-wide reference discovery\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
