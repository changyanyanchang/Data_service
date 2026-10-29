#include "paleo_stratigraphy/workflow.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace paleo::stratigraphy;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f,const char* message){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,message);}
InterpretationBoundary mark(std::string name,double md){InterpretationBoundary b;b.id=name;b.name=name;b.role=name;b.md=b.originalMd=md;b.source="manual";b.locked=true;return b;}
InterpretationDocument fixture(){InterpretationDocument d;d.id="SYNTH-DOCUMENT";d.snapshot.id="SYNTH-SNAPSHOT";d.snapshot.fingerprint="synthetic-input-v1";d.profile.arOrders={5,9};d.profile.padding=15;d.profile.minimumThickness=10;
    WellInterpretation w;w.wellId=2;w.input.wellId=2;w.name="SYNTH-TARGET";w.input.inputFingerprint="synthetic-gr-2";w.input.log.id="GR";w.input.log.kind="GR";w.input.log.unit="API";
    for(int i=0;i<=700;++i){w.input.log.axis.push_back(i);w.input.log.values.push_back(60+15*std::tanh((i-160.)/3)-20*std::tanh((i-230.)/4)+15*std::tanh((i-360.)/3)-20*std::tanh((i-430.)/4)+2*std::sin(i*.5));}
    w.boundaries={mark("T70",100),mark("T72",300),mark("T73",500),mark("T80",650)};d.wells.push_back(w);d.snapshot.wells.push_back(w.input);return d;
}
InterpretationTemplate reference(std::string segment,std::string id,long long well){InterpretationTemplate t;t.id=id;t.version="1";t.referenceWellId=well;t.status="approved";
    InterpretationTemplateSegment s;s.segmentId=segment;s.boundaryFractions={0,.3,.65,1};s.directions={1,-1,1};s.parentDirections={-1,-1,-1};s.parentGroups={0,0,0};s.sequenceIdentities={segment+"-a",segment+"-b",segment+"-c"};t.segments.push_back(s);return t;}
void workflowTests(){
    auto d=fixture();auto other=InterpretationSeries{};other.id="old-E4";other.kind="PACE";other.segmentId="E4";other.status="stale";other.axis={501,502};other.values={7,8};other.parametersJson="{\"retained\":true}";d.wells[0].series.push_back(other);
    size_t progress=0;auto derived=deriveForSegments(d,{}, {},[&](const auto& p){++progress;check(p.total==1,"Single-segment total must be one");},"E12");
    check(derived.runs.back().status=="complete"&&progress==2,"Derived workflow must finish one transaction with progress");check(d.wells[0].series.size()==1&&d.runs.empty(),"Input document must remain unchanged");
    const auto& linked=derived.wells[0].series.back();check(linked.runId==derived.runs.back().id&&linked.parametersJson.find("algorithmRunId")!=std::string::npos,"Derived series must link directly to its persisted workflow run and retain the core run ID");
    const auto& preserved=derived.wells[0].series.front();check(preserved.id=="old-E4"&&preserved.status=="stale"&&preserved.values==other.values&&preserved.parametersJson==other.parametersJson,"Other-segment arrays, parameters and stale state must survive");
    check(derived.wells[0].input.log.values==d.wells[0].input.log.values&&derived.snapshot.wells[0].log.axis==d.snapshot.wells[0].log.axis,"Calculation must never rewrite input snapshots");
    std::vector<TemplateAssignment> choices{{2,"E12",reference("E12","template-A",11)},{2,"E3",reference("E3","template-B",12)}};
    auto divided=propagateTemplates(derived,choices,{});check(divided.runs.back().status=="complete"&&divided.wells[0].cycles.size()==8,"Distinct templates must propagate into the same well's two segments");check(validateInterpretation(divided.wells[0]).empty(),"Merged multi-segment interpretation must validate");
    check(std::count_if(divided.wells[0].series.begin(),divided.wells[0].series.end(),[](const auto& s){return s.kind=="FISCHER_CDMT";})==4,"Each accepted hierarchy must have actual L3 and L4 CDMT");
    check(divided.templates.size()==2&&divided.runs.back().parametersJson.find("template-B")!=std::string::npos,"Run must record per-segment reference assignments");
    auto e3=std::find_if(divided.wells[0].series.begin(),divided.wells[0].series.end(),[](const auto& s){return s.segmentId=="E3"&&s.kind=="PACE";});const auto e3Values=e3->values;const auto e3Run=e3->runId;
    auto filtered=propagateTemplates(divided,choices,{}, {}, {},"E12");auto e3After=std::find_if(filtered.wells[0].series.begin(),filtered.wells[0].series.end(),[](const auto& s){return s.segmentId=="E3"&&s.kind=="PACE";});check(e3After->values==e3Values&&e3After->runId==e3Run,"Filtering propagation must leave other-segment computations unchanged");
    auto dragged=divided;auto& editedWell=dragged.wells[0];auto fourth=std::find_if(editedWell.cycles.begin(),editedWell.cycles.end(),[](const auto& c){return c.segmentId=="E12"&&c.level==4;});const auto movedId=fourth->bottomBoundaryId;
    auto moving=std::find_if(editedWell.boundaries.begin(),editedWell.boundaries.end(),[&](const auto& b){return b.id==movedId;});
    auto oldCdmt=std::find_if(editedWell.series.begin(),editedWell.series.end(),[](const auto& s){return s.id=="FISCHER_CDMT-E12-L4";})->values;
    const auto e3Cdmt=*std::find_if(editedWell.series.begin(),editedWell.series.end(),[](const auto& s){return s.id=="FISCHER_CDMT-E3-L4";});
    moveBoundary(editedWell,movedId,moving->md+1);
    for(auto& c:editedWell.cycles)if(c.segmentId=="E12")c.source="manual";
    check(std::find_if(editedWell.series.begin(),editedWell.series.end(),[](const auto& s){return s.id=="FISCHER_CDMT-E12-L4";})->status=="stale","Internal manual drag must invalidate existing CDMT");
    auto refreshed=deriveForSegments(dragged,{}, {}, {},"E12");
    const auto refreshedCdmt=*std::find_if(refreshed.wells[0].series.begin(),refreshed.wells[0].series.end(),[](const auto& s){return s.id=="FISCHER_CDMT-E12-L4";});
    check(refreshedCdmt.status=="current"&&refreshedCdmt.values!=oldCdmt&&refreshedCdmt.runId==refreshed.runs.back().id,"Recalculation after manual edit must refresh actual cycle-thickness CDMT without template propagation");
    const auto untouchedCdmt=*std::find_if(refreshed.wells[0].series.begin(),refreshed.wells[0].series.end(),[](const auto& s){return s.id=="FISCHER_CDMT-E3-L4";});
    check(untouchedCdmt.values==e3Cdmt.values&&untouchedCdmt.runId==e3Cdmt.runId&&untouchedCdmt.status==e3Cdmt.status,"Another segment's Fischer state and values must remain unchanged");
    auto gap=dragged;auto& gapWell=gap.wells[0];auto next=std::find_if(gapWell.cycles.begin(),gapWell.cycles.end(),[&](const auto& c){return c.segmentId=="E12"&&c.level==4&&c.topBoundaryId==movedId;});
    auto separate=*std::find_if(gapWell.boundaries.begin(),gapWell.boundaries.end(),[&](const auto& b){return b.id==movedId;});separate.id="synthetic-manual-gap";separate.md+=.5;gapWell.boundaries.push_back(separate);next->topBoundaryId=separate.id;
    auto incomplete=deriveForSegments(gap,{}, {}, {},"E12");
    const auto kept=*std::find_if(incomplete.wells[0].series.begin(),incomplete.wells[0].series.end(),[](const auto& s){return s.id=="FISCHER_CDMT-E12-L4";});
    check(kept.status=="stale"&&kept.values==oldCdmt&&incomplete.runs.back().status=="partial","Nonconsecutive manual cycles must keep old Fischer values stale and mark the run partial");
    check(std::any_of(incomplete.wells[0].series.begin(),incomplete.wells[0].series.end(),[&](const auto& s){return s.segmentId=="E12"&&s.kind=="PACE"&&s.status=="current"&&s.runId==incomplete.runs.back().id;}),"A local CDMT failure must not discard valid GR derived curves");
    bool cancel=false;bool cancelled=false;try{deriveForSegments(d,{},[&]{return cancel;},[&](const auto& p){if(p.completed==1)cancel=true;});}catch(const WorkflowCancelled&){cancelled=true;}check(cancelled&&d.runs.empty()&&d.wells[0].series.size()==1,"Cancellation after work must return no partial document");
    rejects([&]{deriveForSegments(d,{}, {},[](const auto&){throw std::runtime_error("caller callback failure");});},"Progress callback exception must escape transaction");
    auto unknown=d;unknown.wells[0].input.log.axisUnit.clear();unknown.wells[0].input.log.axisKind.clear();GeometryOptions confirmation;confirmation.confirmedDepthUnit="m";confirmation.confirmedLogDepthType="MD";auto explicitRun=deriveForSegments(unknown,confirmation,{}, {},"E12");check(explicitRun.runs.back().status=="complete"&&explicitRun.wells[0].input.log.axisUnit.empty(),"Explicit source confirmations must affect only calculation copies");
    auto uppercase=d;uppercase.wells[0].input.log.axisUnit="M";auto aliasRun=deriveForSegments(uppercase,confirmation,{}, {},"E12");check(aliasRun.runs.back().status=="complete"&&aliasRun.wells[0].input.log.axisUnit=="M","Known equivalent unit alias must not be treated as conflicting metadata");
    confirmation.confirmedDepthUnit="ft";auto conflict=deriveForSegments(d,confirmation,{}, {},"E12");check(conflict.runs.back().status=="failed"&&conflict.wells[0].series.size()==1,"A true unit conflict must preserve existing results");
    auto missing=d;missing.wells[0].boundaries.erase(missing.wells[0].boundaries.begin()+2);auto partial=deriveForSegments(missing,{});check(partial.runs.back().status=="partial"&&!partial.wells[0].warnings.empty(),"Missing segment endpoints must be an explicit partial result");
    auto manual=divided;for(auto& c:manual.wells[0].cycles)if(c.segmentId=="E12")c.source="manual";auto protectedResult=propagateTemplates(manual,choices,{}, {}, {},"E12");check(protectedResult.runs.back().status=="failed"&&protectedResult.wells[0].cycles.front().source=="manual","Manual interpretation must not be silently overwritten");
    choices.push_back(choices.front());rejects([&]{propagateTemplates(d,choices,{});},"Duplicate well-segment assignments must be rejected before mutation");
}
void horizonWorkflowTests(){auto d=fixture();auto& w=d.wells[0];w.boundaries.clear();auto& in=w.input;in.depthInfo.depthType="MD";in.depthInfo.depthUnit="m";in.depthInfo.mdReference="KB";in.timeDepth.depthInfo=in.depthInfo;in.timeDepth.timeUnit="ms";in.timeDepth.timeType="TWT";in.timeDepth.timeReference="DATUM";in.timeDepth.coordinateSystem="SYNTH:LOCAL";in.timeDepth.coordinateUnit="m";in.timeDepth.depthTimePairs={{0,0,50,50},{700,1400,150,50}};
    CorrelationHorizon h;h.role="T70";h.resourceId=10;h.surface.coordinateSystem="SYNTH:LOCAL";h.surface.coordinateUnit="m";h.surface.timeUnit="ms";h.surface.timeType="TWT";h.surface.timeReference="DATUM";h.surface.points={{0,0,0,0,200},{1,0,200,0,200},{0,1,0,100,200},{1,1,200,100,200}};d.snapshot.horizons={h};
    auto out=calculateHorizons(d,{});check(out.runs.back().status=="complete"&&out.wells[0].boundaries.size()==1,"Unique supported horizon intersection must become a reviewable regional anchor");check(std::abs(out.wells[0].boundaries[0].md-100)<1e-6&&d.wells[0].boundaries.empty(),"Horizon workflow must use exact MD and preserve input document");
    d.wells[0].boundaries={mark("T70",110)};out=calculateHorizons(d,{});check(out.wells[0].boundaries.size()==2&&out.wells[0].boundaries[0].md==110,"Existing manual anchor must survive with a separate seismic candidate");check(out.wells[0].boundaries[1].role=="candidate:T70","Candidate must not collide with named active boundary lookup");
}
}
int main(){try{workflowTests();horizonWorkflowTests();std::cout<<"Stratigraphy workflow tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
