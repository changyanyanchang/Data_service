#include "paleo_stratigraphy/core.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace paleo::stratigraphy;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double a,double b){check(std::isfinite(a)&&std::abs(a-b)<1e-7,"Analytic endpoint intersection differs");}
template<class F>void rejects(F action,const char* message){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}check(rejected,message);}
bool sameNumber(double a,double b){return a==b||(std::isnan(a)&&std::isnan(b));}
CorrelationWellInput well(){
    CorrelationWellInput w;w.wellId=3;w.depthInfo.depthType="MD";w.depthInfo.depthUnit="m";w.depthInfo.mdReference="SYNTH-KB";
    auto& log=w.log;log.id=log.kind=log.name="GR";log.unit="API";
    for(int z=0;z<=3000;z+=10){log.axis.push_back(z);log.values.push_back(50.);log.quality.push_back(z<500||z>2500?1:0);}
    log.values.front()=Missing;
    auto& td=w.timeDepth;td.depthInfo=w.depthInfo;td.coordinateSystem="SYNTH-XY";td.coordinateUnit="m";
    td.timeUnit="ms";td.timeType="TWT";td.timeReference="SYNTH-TIME";
    for(int md=1000;md<=1500;md+=100)td.depthTimePairs.push_back({double(md),100.+2.*md,200.+.02*md,100.,Missing});
    return w;
}
CorrelationHorizon horizon(double md,std::string role="T70",long long id=10){
    CorrelationHorizon h;h.role=role;h.resourceId=id;auto& s=h.surface;
    s.coordinateSystem="SYNTH-XY";s.coordinateUnit="m";s.timeUnit="ms";s.timeType="TWT";s.timeReference="SYNTH-TIME";
    const double t=100.+2.*md;s.points={{0,0,0,0,t},{1,0,400,0,t},{0,1,0,200,t},{1,1,400,200,t}};return h;
}
GeometryOptions enabled(){GeometryOptions o;o.allowTopExtrapolation=true;o.allowBottomExtrapolation=true;
    o.maximumTopExtrapolationM=0;o.maximumBottomExtrapolationM=0;o.maximumLogExtrapolationM=100;return o;}
void has(const InterpretationBoundary& b,const std::string& text){check(b.evidenceJson.find(text)!=std::string::npos,"Missing endpoint audit evidence");}
void unchanged(const CorrelationWellInput& w,const CorrelationWellInput& before){
    check(w.log.axis==before.log.axis&&w.log.quality==before.log.quality&&w.log.values.size()==before.log.values.size(),"GR axis or validity changed");
    for(size_t i=0;i<w.log.values.size();++i)check(sameNumber(w.log.values[i],before.log.values[i]),"GR values were filled or changed");
    check(w.timeDepth.depthTimePairs.size()==before.timeDepth.depthTimePairs.size(),"Source TD points were appended");
    for(size_t i=0;i<w.timeDepth.depthTimePairs.size();++i){const auto& a=w.timeDepth.depthTimePairs[i];const auto& b=before.timeDepth.depthTimePairs[i];
        check(a.depthValue==b.depthValue&&a.timeValue==b.timeValue&&a.x==b.x&&a.y==b.y&&sameNumber(a.z,b.z),"Source TD or trajectory values changed");}
}
void fourBoundariesAndMargins(){
    auto w=well();const auto before=w;const std::vector<CorrelationHorizon> horizons{
        horizon(400,"T70",10),horizon(900,"T72",11),horizon(2000,"T73",12),horizon(2600,"T80",13)};
    check(intersectHorizons(w,horizons).boundaries.empty(),"Default core must retain strict observed TD coverage");
    const auto out=intersectHorizons(w,horizons,enabled());check(out.boundaries.size()==4,"All four covered surfaces within effective GR plus 100 m must be found");
    const double expected[]={400,900,2000,2600};for(size_t i=0;i<out.boundaries.size();++i){const auto& b=out.boundaries[i];near(b.md,expected[i]);
        check(b.status=="review"&&!b.locked,"Endpoint intersections must remain review candidates");
        has(b,"\"timeDepthExtrapolation\":{");has(b,"\"applied\":true");has(b,"\"effectiveLogTopMd\":500");has(b,"\"effectiveLogBottomMd\":2500");
        has(b,"\"sourceValuesModified\":false");has(b,"\"grValuesExtrapolated\":false");has(b,"\"horizonSpaceExtrapolated\":false");}
    has(out.boundaries[0],"\"side\":\"top\"");has(out.boundaries[0],"\"outsideLogDistanceM\":100");
    has(out.boundaries[1],"\"outsideLogDistanceM\":0");has(out.boundaries[3],"\"side\":\"bottom\"");
    has(out.boundaries[3],"\"timeDepthExtrapolationM\":1100");has(out.boundaries[3],"\"outsideLogDistanceM\":100");
    check(intersectHorizons(w,{horizon(399.9),horizon(2600.1)},enabled()).boundaries.empty(),"The 100 m GR margin must be a hard boundary");
    auto noMargin=enabled();noMargin.maximumLogExtrapolationM=0;
    check(intersectHorizons(w,{horizon(450),horizon(2550)},noMargin).boundaries.empty(),"Explicit zero GR margin must prohibit outside-GR picks");
    unchanged(w,before);
}
void independentSidesAndValidAxis(){
    auto w=well();auto top=enabled();top.allowBottomExtrapolation=false;
    auto out=intersectHorizons(w,{horizon(900),horizon(2000)},top);check(out.boundaries.size()==1,"Top-only permission must not extend the bottom");near(out.boundaries[0].md,900);
    auto bottom=enabled();bottom.allowTopExtrapolation=false;
    out=intersectHorizons(w,{horizon(900),horizon(2000)},bottom);check(out.boundaries.size()==1,"Bottom-only permission must not extend the top");near(out.boundaries[0].md,2000);
    w.log.quality[50]=4;w.log.quality[250]=4;
    out=intersectHorizons(w,{horizon(410),horizon(2590)},enabled());check(out.boundaries.size()==2,"Unstable GR endpoints must move the valid bounds inward");
    has(out.boundaries.front(),"\"effectiveLogTopMd\":510");has(out.boundaries.back(),"\"effectiveLogBottomMd\":2490");
    check(intersectHorizons(w,{horizon(405),horizon(2595)},enabled()).boundaries.empty(),"Invalid leading/trailing GR samples must not enlarge the 100 m margin");
    w=well();std::fill(w.log.quality.begin(),w.log.quality.end(),1);
    rejects([&]{intersectHorizons(w,{horizon(900)},enabled());},"All-missing GR cannot define an extrapolation extent");
    w.log.quality[100]=0;rejects([&]{intersectHorizons(w,{horizon(900)},enabled());},"A single valid GR depth is not a curve extent");
}
void sparseTiesAndSign(){
    auto w=well();w.timeDepth.depthTimePairs={w.timeDepth.depthTimePairs.front(),w.timeDepth.depthTimePairs.back()};
    auto out=intersectHorizons(w,{horizon(900),horizon(2000)},enabled());check(out.boundaries.size()==2,"Two monotonic ties suffice for both endpoint slopes");
    for(const auto& b:out.boundaries){has(b,"\"fitSamples\":2");has(b,"\"lowSupport\":true");}
    check(!out.warnings.empty(),"Two-tie fits need a review warning");
    for(auto& point:w.timeDepth.depthTimePairs)point.timeValue=-point.timeValue;
    auto sign=enabled();sign.wellTimeSign=-1;out=intersectHorizons(w,{horizon(900),horizon(2000)},sign);near(out.boundaries[0].md,900);near(out.boundaries[1].md,2000);
    rejects([&]{intersectHorizons(w,{horizon(900)},enabled());},"Negative source TWT requires explicit sign policy, not implicit abs");
    w=well();w.timeDepth.depthTimePairs[2].timeValue=w.timeDepth.depthTimePairs[1].timeValue-1;
    rejects([&]{intersectHorizons(w,{horizon(900)},enabled());},"Nonmonotonic TD must not be repaired by endpoint extrapolation");
}
void trajectoryAndObservedTime(){
    auto w=well();w.timeDepth.depthTimePairs.front().x+=200;
    auto out=intersectHorizons(w,{horizon(900),horizon(2000)},enabled());check(out.boundaries.size()==1,"Unphysical top XY tangent must not disable a valid bottom tangent");near(out.boundaries[0].md,2000);
    w=well();auto outside=horizon(900);outside.surface.points[0].x=outside.surface.points[2].x=220;
    check(intersectHorizons(w,{outside},enabled()).boundaries.empty(),"Extended XY outside the horizon must not receive spatial extrapolation");
    w.timeDepth.depthTimePairs.clear();for(int md=0;md<=3000;md+=100)w.timeDepth.depthTimePairs.push_back({double(md),100.+2.*md,200.+.02*md,100.,Missing});
    const auto before=w;out=intersectHorizons(w,{horizon(450),horizon(1200)},enabled());check(out.boundaries.size()==2,"Observed TD may support a pick outside GR when explicitly allowed");
    has(out.boundaries[0],"\"outsideLogDistanceM\":50");has(out.boundaries[0],"\"timeDepthExtrapolationM\":0");has(out.boundaries[0],"\"trajectoryExtrapolationM\":0");has(out.boundaries[0],"\"applied\":true");
    has(out.boundaries[1],"\"applied\":false");near(out.boundaries[1].time,2500);unchanged(w,before);
}
void invalidOptionsAndCaps(){
    auto w=well();auto o=enabled();o.maximumTopExtrapolationM=100;
    auto out=intersectHorizons(w,{horizon(900),horizon(800)},o);check(out.boundaries.size()==1,"An optional top distance cap must still be respected");near(out.boundaries[0].md,900);
    for(double value:{-1.,Missing}){o=enabled();o.maximumLogExtrapolationM=value;rejects([&]{intersectHorizons(w,{horizon(900)},o);},"Invalid GR margin must be rejected");
        o=enabled();o.maximumTopExtrapolationM=value;rejects([&]{intersectHorizons(w,{horizon(900)},o);},"Invalid top cap must be rejected");}
}
}
int main(){try{fourBoundariesAndMargins();independentSidesAndValidAxis();sparseTiesAndSign();trajectoryAndObservedTime();invalidOptionsAndCaps();
    std::cout<<"PASS endpoint TD extrapolation: four analytic surfaces, effective GR +/-100m, signs, support, bounds and immutable sources\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
