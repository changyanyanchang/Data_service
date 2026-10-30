#include "paleo_stratigraphy/core.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace paleo::stratigraphy;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double actual,double expected){check(std::isfinite(actual)&&std::abs(actual-expected)<1e-7,"Known extrapolated depth differs");}
template<class F>void rejects(F f,const char* message){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,message);}
CorrelationWellInput well(){
    CorrelationWellInput w;w.wellId=1;w.log.axis={0,2000};w.log.values={50,60};w.depthInfo.depthType="MD";w.depthInfo.depthUnit="m";w.depthInfo.mdReference="SYNTH-KB";
    auto& td=w.timeDepth;td.depthInfo=w.depthInfo;td.coordinateSystem="SYNTH-XY";td.coordinateUnit="m";td.timeType="TWT";td.timeUnit="ms";td.timeReference="SYNTH-TIME";
    for(int i=0;i<=20;++i){const double md=50.*i;td.depthTimePairs.push_back({md,2*md,10+.1*md,50,Missing});}return w;
}
CorrelationHorizon horizon(double time){CorrelationHorizon h;h.resourceId=9;h.role="T80";auto& s=h.surface;s.coordinateSystem="SYNTH-XY";s.coordinateUnit="m";s.timeType="TWT";s.timeUnit="ms";s.timeReference="SYNTH-TIME";s.points={{0,0,0,0,time},{1,0,400,0,time},{0,1,0,100,time},{1,1,400,100,time}};return h;}
GeometryOptions enabled(){GeometryOptions o;o.allowBottomExtrapolation=true;return o;}
void sameInput(const CorrelationWellInput& a,const CorrelationWellInput& b){check(a.timeDepth.depthTimePairs.size()==b.timeDepth.depthTimePairs.size()&&a.positions.size()==b.positions.size(),"Original source point count changed");for(size_t i=0;i<a.timeDepth.depthTimePairs.size();++i){const auto& p=a.timeDepth.depthTimePairs[i];const auto& q=b.timeDepth.depthTimePairs[i];check(p.depthValue==q.depthValue&&p.timeValue==q.timeValue&&p.x==q.x&&p.y==q.y,"Original source values changed");}check(a.log.axis==b.log.axis&&a.log.values==b.log.values,"Original log changed");}
void knownSolution(){
    auto w=well();const auto before=w;auto h=horizon(2400);check(intersectHorizons(w,{h}).boundaries.empty(),"Default strict mode must not extrapolate");
    auto r=intersectHorizons(w,{h},enabled());check(r.boundaries.size()==1,"Expected one bottom extension pick");near(r.boundaries[0].md,1200);near(r.boundaries[0].time,2400);check(r.boundaries[0].status=="review"&&!r.boundaries[0].locked,"Extended picks require review, not automatic confirmation");
    const auto& audit=r.boundaries[0].evidenceJson;for(const auto* marker:{"\"timeDepthExtrapolation\":{","\"applied\":true","\"observedTimeDepthBottomMd\":1000","\"timeDepthExtrapolationM\":200","\"trajectoryExtrapolationM\":200","\"fitTopMd\":800","\"slopeMsPerM\":2","\"sourceValuesModified\":false","\"horizonSpaceExtrapolated\":false"})check(audit.find(marker)!=std::string::npos,"Missing quantitative extrapolation provenance");sameInput(w,before);
    auto observed=intersectHorizons(w,{horizon(1800)},enabled());near(observed.boundaries.at(0).md,900);check(observed.boundaries[0].evidenceJson.find("\"applied\":false")!=std::string::npos,"Observed-range pick must not be labelled extrapolated");
    auto negative=w;for(auto& p:negative.timeDepth.depthTimePairs)p.timeValue=-p.timeValue;auto o=enabled();o.wellTimeSign=-1;near(intersectHorizons(negative,{h},o).boundaries.at(0).md,1200);
}
void bounds(){
    auto w=well();auto o=enabled();o.maximumBottomExtrapolationM=100;check(intersectHorizons(w,{horizon(2400)},o).boundaries.empty(),"Configured extrapolation cap must be enforced");
    check(intersectHorizons(w,{horizon(3200)},enabled()).boundaries.empty(),"Default cap is 500m below the observed TD");
    w.log.axis.back()=1100;check(intersectHorizons(w,{horizon(2400)},enabled()).boundaries.empty(),"Extension cannot exceed the actual log axis");
    w=well();check(intersectHorizons(w,{horizon(-100)},enabled()).boundaries.empty(),"No shallow extrapolation is allowed");
    auto narrow=horizon(2400);narrow.surface.points[1].x=narrow.surface.points[3].x=115;auto clipped=intersectHorizons(w,{narrow},enabled());check(clipped.boundaries.empty(),"Projected XY outside horizon triangles cannot receive a fabricated pick");
    o=enabled();o.maximumBottomExtrapolationM=0;near(intersectHorizons(w,{horizon(3200)},o).boundaries.at(0).md,1600);o.maximumBottomExtrapolationM=-1;rejects([&]{intersectHorizons(w,{horizon(2400)},o);},"Negative extension cap is invalid");o.maximumBottomExtrapolationM=Missing;rejects([&]{intersectHorizons(w,{horizon(2400)},o);},"Nonfinite extension cap is forbidden");o=enabled();o.bottomExtrapolationFitWindowM=10;rejects([&]{intersectHorizons(w,{horizon(2400)},o);},"Too-small reliability window is invalid");
}
void unreliableTail(){
    auto w=well();w.timeDepth.depthTimePairs.back().timeValue+=500;auto result=intersectHorizons(w,{horizon(2700)},enabled());check(result.boundaries.empty()&&!result.warnings.empty(),"Strong terminal curvature must not become a falsely reliable linear extension");
    w=well();auto first=w.timeDepth.depthTimePairs.front(),last=w.timeDepth.depthTimePairs.back();w.timeDepth.depthTimePairs={first,last};result=intersectHorizons(w,{horizon(2400)},enabled());near(result.boundaries.at(0).md,1200);check(result.boundaries[0].evidenceJson.find("\"lowSupport\":true")!=std::string::npos&&!result.warnings.empty(),"Two TD samples must produce a low-support review candidate");
    w=well();w.timeDepth.depthTimePairs[19].timeValue=w.timeDepth.depthTimePairs[18].timeValue-1;rejects([&]{intersectHorizons(w,{horizon(2400)},enabled());},"Nonmonotonic observed TD is never repaired by extrapolation");
    w=well();w.timeDepth.depthTimePairs.back().x+=200;result=intersectHorizons(w,{horizon(2400)},enabled());check(result.boundaries.empty()&&!result.warnings.empty(),"Physically inconsistent terminal XY slope must not be extended");
    w=well();w.depthInfo.tvdReference="SYNTH-KB";w.timeDepth.depthInfo.depthType="TVD";w.timeDepth.depthInfo.tvdReference="SYNTH-KB";w.well.coordinateSystem="SYNTH-XY";w.well.coordinateUnit="m";w.positions={{0,10,50,0,Missing},{1000,110,50,1000,Missing}};
    result=intersectHorizons(w,{horizon(2400)},enabled());check(result.boundaries.empty()&&!result.warnings.empty(),"A TVD table must not be extended as though its axis were MD");
}
void useObservedDeeperTrajectory(){
    auto w=well();w.well.coordinateSystem="SYNTH-XY";w.well.coordinateUnit="m";w.positions={{0,10,50,Missing,Missing},{1000,110,50,Missing,Missing},{1500,130,50,Missing,Missing}};
    const auto before=w;auto r=intersectHorizons(w,{horizon(2400)},enabled());near(r.boundaries.at(0).md,1200);check(r.boundaries[0].evidenceJson.find("\"trajectoryExtrapolationM\":0")!=std::string::npos,"Observed deeper trajectory must be used without fictitious XY extension");sameInput(w,before);
    // A TD already covering deeper depths can use a bounded XY-only extension.
    for(int i=21;i<=30;++i)w.timeDepth.depthTimePairs.push_back({50.*i,100.*i,10+5.*i,50,Missing});w.positions.resize(2);auto xyOnly=intersectHorizons(w,{horizon(2400)},enabled());near(xyOnly.boundaries.at(0).md,1200);check(xyOnly.boundaries[0].evidenceJson.find("\"timeDepthExtrapolationM\":0")!=std::string::npos,"Existing TD values must not be replaced by a fitted time");
}
}
int main(){try{knownSolution();bounds();unreliableTail();useObservedDeeperTrajectory();std::cout<<"PASS bounded bottom TD/trajectory extrapolation: analytic depth, review evidence, source preservation and rejection limits\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
