#include "paleo_stratigraphy/time_depth_selection.h"
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
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
std::string u(const QString& s){return s.toUtf8().toStdString();}
QJsonObject object(const std::string& s){return QJsonDocument::fromJson(QByteArray::fromStdString(s)).object();}
GeometryOptions policy(){GeometryOptions g;g.resolveFromSources=true;g.confirmedDepthType=g.confirmedLogDepthType="MD";g.confirmedDepthUnit="m";g.confirmedMdReference="KB";g.confirmedCoordinateSystem="SYNTH:LOCAL";g.confirmedCoordinateUnit="m";g.confirmedWellTimeUnit=g.confirmedHorizonTimeUnit="ms";g.confirmedWellTimeType=g.confirmedHorizonTimeType="TWT";g.confirmedTimeReference="DATUM";g.policyEvidence="Synthetic explicit project conventions";return g;}
CorrelationInputSnapshot snapshot(){
    CorrelationInputSnapshot s;s.id="synthetic";CorrelationWellInput w;w.wellId=1;w.timeDepthId=11;w.inputFingerprint="source-v1";w.well.wellName="SYNTH-1";w.well.wellheadX=w.well.wellheadY=50;w.well.coordinateSystem="SYNTH:LOCAL";w.well.coordinateUnit="m";w.depthInfo.depthType="MD";w.depthInfo.depthUnit="m";w.depthInfo.mdReference="KB";w.surveyDepthInfo=w.depthInfo;w.log.axisKind="MD";w.log.axisUnit="m";
    for(int md=0;md<=1000;++md){w.log.axis.push_back(md);w.log.values.push_back(50+10*std::sin(md*.04));}
    w.timeDepth.depthInfo=w.depthInfo;w.timeDepth.timeType="TWT";w.timeDepth.timeUnit="ms";w.timeDepth.timeReference="DATUM";w.timeDepth.coordinateSystem="SYNTH:LOCAL";w.timeDepth.coordinateUnit="m";
    for(int md=0;md<=1000;md+=50)w.timeDepth.depthTimePairs.push_back({double(md),double(md*2),50,50});s.wells={w};
    int ordinal=0;for(const std::string role:{"T70","T72","T73","T80"}){CorrelationHorizon h;h.role=role;h.resourceId=100+ordinal;auto& surface=h.surface;surface.coordinateSystem="SYNTH:LOCAL";surface.coordinateUnit="m";surface.timeType="TWT";surface.timeUnit="ms";surface.timeReference="DATUM";const double time=200.*(++ordinal);surface.points={{0,0,0,0,time},{1,0,100,0,time},{0,1,0,100,time},{1,1,100,100,time}};s.horizons.push_back(h);}return s;
}
std::vector<InterpretationBoundary> controls(){std::vector<InterpretationBoundary> out;int i=0;for(const std::string role:{"T70","T72","T73","T80"}){InterpretationBoundary b;b.role=role;b.name=role;b.md=100.*(++i);b.source="manual";b.status="confirmed";out.push_back(b);}return out;}
void evaluation(){
    auto s=snapshot();const auto original=s.wells[0].timeDepth.depthTimePairs;
    auto result=evaluateTimeDepthCandidate(s,policy(),controls());check(result.usable&&result.interfaceRoles.size()==4&&result.anchorRoles.size()==4&&result.anchorRmseM<1e-8,"known source supports all four interfaces and independent anchors");
    check(result.observedInterfaces==4&&result.extrapolatedInterfaces==0,"observed and extrapolated interfaces distinguished");
    check(object(result.evidenceJson).value("scoreMetric").toString()=="unshifted_boundary_MD_residual_m"&&!object(result.evidenceJson).value("timeOffsetFitted").toBool(),"metric cannot be mistaken for historical TWT RMSE or fitted offsets");
    auto withFormation=s;withFormation.wells[0].formationTrack.depthInfo=withFormation.wells[0].depthInfo;GeologicalInterval member;member.intervalName="恩平一二段";member.topDepth=100;member.bottomDepth=200;withFormation.wells[0].formationTrack.intervals={member};auto unconfirmed=controls();for(auto& b:unconfirmed){b.source="seismic_horizon";b.status="review";}auto fromSource=evaluateTimeDepthCandidate(withFormation,policy(),unconfirmed);check(fromSource.usable&&fromSource.anchorRoles.size()==2&&fromSource.anchorRmseM<1e-8,"nonmanual computed boundaries do not hide independent source formation anchors");
    for(auto& p:s.wells[0].timeDepth.depthTimePairs)p.timeValue=-p.timeValue;
    auto negative=evaluateTimeDepthCandidate(s,policy(),controls());check(negative.usable&&negative.anchorRmseM<1e-8&&s.wells[0].timeDepth.depthTimePairs.back().timeValue==-2000,"negative source direction resolved without source mutation");
    s.wells[0].timeDepth.depthTimePairs[5].timeValue=2000;check(!evaluateTimeDepthCandidate(s,policy()).usable,"nonmonotonic candidate rejected rather than repaired by absolute values");
    s=snapshot();s.horizons[0].surface.coordinateSystem="OTHER";check(!evaluateTimeDepthCandidate(s,policy()).usable,"explicit coordinate conflict rejects candidate");
    s=snapshot();auto reversed=controls();reversed[1].md=50;check(!evaluateTimeDepthCandidate(s,policy(),reversed).usable,"conflicting independent anchors are not selectively dropped to improve score");
    s=snapshot();for(auto& p:s.horizons.back().surface.points)p.zTime=300;check(!evaluateTimeDepthCandidate(s,policy()).usable,"computed interfaces with inverted stratigraphic order rejected");
    s=snapshot();auto p=policy();p.allowTopExtrapolation=p.allowBottomExtrapolation=true;p.maximumLogExtrapolationM=100;p.maximumTopExtrapolationM=p.maximumBottomExtrapolationM=0;
    s.wells[0].log.quality.assign(s.wells[0].log.axis.size(),4);for(size_t i=200;i<=400;++i)s.wells[0].log.quality[i]=0;
    for(auto& point:s.horizons.back().surface.points)point.zTime=1000;
    auto edges=evaluateTimeDepthCandidate(s,p);check(edges.usable&&edges.interfaceRoles.size()==4&&edges.grFirstMd==200&&edges.grLastMd==400,"100-m allowance uses first/last valid GR values, not padded array bounds");
    check(edges.extrapolatedInterfaces==2&&std::abs(edges.totalExtrapolationM-200)<1e-6,"outside-GR edge candidates have quantified review evidence");
    for(auto& point:s.horizons.back().surface.points)point.zTime=1000.2;auto outside=evaluateTimeDepthCandidate(s,p);check(outside.usable&&outside.interfaceRoles.size()==3&&std::find(outside.interfaceRoles.begin(),outside.interfaceRoles.end(),"T80")==outside.interfaceRoles.end(),"a boundary more than 100m from valid GR is not forced into the result");
    check(s.wells[0].timeDepth.depthTimePairs.size()==original.size()&&s.wells[0].log.quality.front()==4,"evaluation preserves source time-depth and GR quality arrays");
}
void import(DataService& service,const QString& path,const QByteArray& bytes){QFile file(path);check(file.open(QIODevice::WriteOnly),"write fixture");check(file.write(bytes)==bytes.size(),"complete fixture");file.close();ImportReport report;std::string error;check(service.importFile(u(path),{},report,error),error);check(report.failed==0&&report.imported+report.review>0,"fixture not parsed as data: "+u(path)+"; imported="+std::to_string(report.imported)+", review="+std::to_string(report.review)+", failed="+std::to_string(report.failed));}
QByteArray relation(double shift,int last=1000){QByteArray result="# Petrel checkshots format\n# Unit in depth: m\n# Unit in X and Y direction: m\nVERSION 1\nBEGIN HEADER\nX\nY\nZ\nTWT picked\nMD\nWell\nEND HEADER\n";for(int md=0;md<=last;md+=50)result+="50 50 "+QByteArray::number(-md)+' '+QByteArray::number(-(2*md+shift))+' '+QByteArray::number(md)+" \"SYNTH-1\"\n";return result;}
void sdkSelection(){
    QTemporaryDir temp;check(temp.isValid(),"temporary project");DataService service;std::string error;check(service.createProject(u(temp.path()+"/project"),"Synthetic time-depth selection",error),error);
    QByteArray las="~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nMD.m\nGR.API\n~Ascii\n";for(int md=0;md<=1000;++md)las+=QByteArray::number(md)+' '+QByteArray::number(50+std::sin(md*.1))+'\n';import(service,temp.path()+"/SYNTH-1.las",las);
    import(service,temp.path()+"/wrong.txt",relation(100));import(service,temp.path()+"/exact.txt",relation(0));import(service,temp.path()+"/partial.txt",relation(0,300));
    int ordinal=0;for(const std::string role:{"T70","T72","T73","T80"}){const auto time=QByteArray::number(200*(++ordinal));QByteArray rows;for(int i=0;i<2;++i)for(int j=0;j<2;++j)rows+="INLINE : "+QByteArray::number(i)+" XLINE : "+QByteArray::number(j)+' '+QByteArray::number(i*100)+' '+QByteArray::number(j*100)+' '+time+'\n';import(service,temp.path()+"/"+QString::fromStdString(role)+".dat",rows);}
    std::vector<WellSummary> wells;check(service.listWells(wells,error)&&wells.size()==1,"single well");const auto id=wells[0].id;
    std::vector<DataEntry> curves,relations,horizons;check(service.listData("curves",id,curves,error)&&curves.size()==1,error);check(service.listData("time_depth",id,relations,error)&&relations.size()==3,"three time-depth candidates");check(service.listData("horizons",0,horizons,error)&&horizons.size()==4,error);
    CorrelationInputRequest request;CorrelationWellSelection w;w.wellId=id;w.curveId=curves[0].id;request.wells={w};for(const auto& h:horizons)for(const std::string role:{"T70","T72","T73","T80"})if(h.name.find(role)!=std::string::npos)request.horizons.push_back({h.id,role});
    check(request.horizons.size()==4,"horizon role bindings");WellInterpretation manual;manual.wellId=id;manual.boundaries=controls();
    long long exact=0,wrong=0,partial=0;for(const auto& entry:relations){TimeDepthTable td;check(service.readTimeDepth(entry.id,td,error),error);if(td.depthTimePairs.back().depthValue<400)partial=entry.id;else if(td.depthTimePairs.front().timeValue==0)exact=entry.id;else wrong=entry.id;}check(exact&&wrong&&partial,"candidate identities from source values");
    auto autoPlan=selectTimeDepthCandidates(service,request,policy(),{}, {manual});check(autoPlan.ready()&&autoPlan.request.wells[0].timeDepthId==exact&&autoPlan.candidates.size()==3,"same full-coverage mask selects the unshifted independently matching relation");
    check(!object(autoPlan.auditJson).value("historicalTwtRmseReproduced").toBool(),"MD residual scoring is explicitly distinct from old experiment");
    const auto partialResult=std::find_if(autoPlan.candidates.begin(),autoPlan.candidates.end(),[&](const auto& c){return c.timeDepthId==partial;});check(partialResult!=autoPlan.candidates.end()&&partialResult->interfaceRoles.size()==3&&!partialResult->selected,"zero residual on fewer interfaces cannot beat the full mask");
    check(service.saveCorrelationTimeDepthPreferences({{id,partial,"{\"grade\":\"暂定首选\",\"selectionType\":\"multi_interface\"}"}},error),error);
    auto coverageUpgrade=selectTimeDepthCandidates(service,request,policy(),{}, {manual});check(coverageUpgrade.ready()&&coverageUpgrade.request.wells[0].timeDepthId==exact,"a historical three-interface source yields to a supported four-interface source");
    check(std::any_of(coverageUpgrade.issues.begin(),coverageUpgrade.issues.end(),[](const auto& i){return i.message.find("历史覆盖不足")!=std::string::npos;}),"historical coverage override is explicitly audited");
    auto explicitPartial=request;explicitPartial.wells[0].timeDepthId=partial;check(selectTimeDepthCandidates(service,explicitPartial,policy(),{}, {manual}).request.wells[0].timeDepthId==partial,"explicit user binding remains highest priority even if another candidate covers more interfaces");
    const auto prior=CorrelationTimeDepthPreference{id,wrong,"{\"grade\":\"暂定首选\",\"selectedCandidateId\":\"historical-source\",\"selectionType\":\"multi_interface\"}"};check(service.saveCorrelationTimeDepthPreferences({prior},error),error);
    auto historical=selectTimeDepthCandidates(service,request,policy(),{}, {manual});check(historical.ready()&&historical.request.wells[0].timeDepthId==wrong,"usable historical multi-interface preference is not overturned by current MD residual");
    const auto historicalAudit=object(historical.auditJson).value("wells").toArray()[0].toObject();check(historicalAudit.value("selectionBasis").toString()=="persisted_historical_preference"&&historicalAudit.value("historicalSelectionEvidence").toObject().value("grade").toString()==QStringLiteral("暂定首选"),"historical grade and decision basis retained");
    auto explicitRequest=request;explicitRequest.wells[0].timeDepthId=exact;auto explicitPlan=selectTimeDepthCandidates(service,explicitRequest,policy(),{}, {manual});check(explicitPlan.ready()&&explicitPlan.request.wells[0].timeDepthId==exact,"explicit source binding takes priority over historical preference");
    explicitRequest.wells[0].timeDepthId=999999;auto invalid=selectTimeDepthCandidates(service,explicitRequest,policy(),{}, {manual});check(!invalid.ready()&&invalid.request.wells[0].timeDepthId==0,"invalid explicit binding is not silently replaced");
    auto multiple=request;auto unavailable=w;unavailable.wellId=99999;multiple.wells.push_back(unavailable);auto mixed=selectTimeDepthCandidates(service,multiple,policy(),{}, {manual});check(mixed.ready()&&mixed.request.wells[0].timeDepthId==wrong&&mixed.request.wells[1].timeDepthId==0,"one unavailable well does not block the valid selected wells");
    TimeDepthTable unchanged;check(service.readTimeDepth(wrong,unchanged,error)&&unchanged.depthTimePairs.front().timeValue==-100,"selection never fits a hidden time offset or changes samples");
    check(service.deleteData("time_depth",wrong,error),error);auto stale=selectTimeDepthCandidates(service,request,policy(),{}, {manual});check(stale.ready()&&stale.request.wells[0].timeDepthId==exact,"stale historical source degrades per well to the current same-mask recommendation");
    check(std::any_of(stale.issues.begin(),stale.issues.end(),[](const auto& i){return !i.blocking&&i.message.find("原优选时深绑定当前不可用")!=std::string::npos;}),"historical fallback reason is explicit and old provenance remains in audit");
    bool cancelled=false;try{selectTimeDepthCandidates(service,request,policy(),[]{return true;});}catch(const WorkflowCancelled&){cancelled=true;}check(cancelled,"selection observes cancellation");
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{evaluation();sdkSelection();std::cout<<"Time-depth selection tests passed: source binding, same-mask scoring, historical preference, geometry rejection and GR edge allowance\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
