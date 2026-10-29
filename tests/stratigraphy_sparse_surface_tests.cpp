#include "paleo_stratigraphy/core.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace paleo::stratigraphy;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double actual,double expected){check(std::isfinite(actual)&&std::abs(actual-expected)<1e-7,"Known plane intersection depth differs");}
template<class F>void rejects(F f,const char* message){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,message);}
bool warns(const HorizonIntersectionResult& result,const std::string& text){return std::any_of(result.warnings.begin(),result.warnings.end(),[&](const auto& warning){return warning.find(text)!=std::string::npos;});}
CorrelationWellInput well(double x,double y){
    CorrelationWellInput w;w.wellId=1;w.log.axis={0,1000};w.log.values={50,60};w.depthInfo.depthType="MD";w.depthInfo.depthUnit="m";w.depthInfo.mdReference="SYNTH-KB";
    w.timeDepth.depthInfo=w.depthInfo;w.timeDepth.coordinateSystem="SYNTHETIC-XY";w.timeDepth.coordinateUnit="m";w.timeDepth.timeUnit="ms";w.timeDepth.timeType="TWT";w.timeDepth.timeReference="SYNTH-TIME";
    w.timeDepth.depthTimePairs={{0,0,x,y,Missing},{1000,2000,x,y,Missing}};return w;
}
CorrelationHorizon surface(){CorrelationHorizon h;h.resourceId=1;h.role="T70";h.surface.coordinateSystem="SYNTHETIC-XY";h.surface.coordinateUnit="m";h.surface.timeUnit="ms";h.surface.timeType="TWT";h.surface.timeReference="SYNTH-TIME";return h;}
GeometryOptions irregular(double edge=300){GeometryOptions o;o.allowIrregularSurface=true;o.maximumSurfaceEdge=edge;return o;}
void threePointPlane(){
    auto h=surface();h.surface.points={{101,501,0,0,1000},{150,503,100,0,1100},{103,550,0,100,1200}};auto w=well(20,30);const auto original=h.surface.points;
    auto r=intersectHorizons(w,{h},irregular());check(r.boundaries.size()==1,"Three source points should form an exact planar TIN");near(r.boundaries[0].md,540);check(r.boundaries[0].evidenceJson.find("delaunay_max_edge/v1")!=std::string::npos,"TIN choice must be in boundary provenance");check(h.surface.points.size()==original.size()&&h.surface.points[1].zTime==1100,"Original points must not change");
    auto outside=intersectHorizons(well(80,80),{h},irregular());check(outside.boundaries.empty(),"Convex-hull exterior must not be extrapolated");
    rejects([&]{intersectHorizons(w,{h},irregular(50));},"All long-edge triangles must be rejected");
    rejects([&]{intersectHorizons(w,{h},irregular(Missing));},"TIN requires an explicit edge limit");rejects([&]{intersectHorizons(w,{h},irregular(0));},"Zero edge limit is invalid");
    rejects([&]{intersectHorizons(w,{h});},"Opt-out must not triangulate arbitrary missing adjacent grid cells");
    auto dup=h;dup.surface.points.push_back({999,999,0,0,1000});near(intersectHorizons(w,{dup},irregular()).boundaries.at(0).md,540);
    dup.surface.points.back().zTime=1001;rejects([&]{intersectHorizons(w,{dup},irregular());},"Conflicting values at duplicate XY must not be averaged");
    dup=h;dup.surface.points.push_back({101,501,4,0,1000});rejects([&]{intersectHorizons(w,{dup},irregular());},"Conflicting source grid identity must be rejected");
    auto bad=h;bad.surface.coordinateSystem="OTHER";rejects([&]{intersectHorizons(w,{bad},irregular());},"TIN cannot bypass source CRS checks");bad=h;bad.surface.points.front().x=std::numeric_limits<double>::infinity();rejects([&]{intersectHorizons(w,{bad},irregular());},"Infinite XY must be rejected before triangulation");
    bad=h;for(size_t i=0;i<bad.surface.points.size();++i){bad.surface.points[i].x=double(i);bad.surface.points[i].y=double(i);}rejects([&]{intersectHorizons(w,{bad},irregular());},"Collinear points cannot define a surface");
    bad=h;bad.surface.points.resize(2);rejects([&]{intersectHorizons(w,{bad},irregular());},"Two points cannot define a surface");
    auto translated=h;for(auto& p:translated.surface.points){p.x+=300000;p.y+=2360000;}near(intersectHorizons(well(300020,2360030),{translated},irregular()).boundaries.at(0).md,540);
}
void sparseLinesAndDefaultHole(){
    // Full interpretation lines at indices 0,1,3,4. The missing column is an
    // explicit hole in default mode, and an opt-in supported spacing in TIN.
    auto h=surface();for(int i:{0,1,3,4})for(int j=0;j<3;++j)h.surface.points.push_back({i,j,50.*i,50.*j,1000.});auto w=well(100,50);
    auto defaultResult=intersectHorizons(w,{h});check(defaultResult.boundaries.empty(),"Default grid mode must preserve a missing column");
    near(intersectHorizons(w,{h},irregular(125)).boundaries.at(0).md,500);
    auto limited=intersectHorizons(w,{h},irregular(80));check(limited.boundaries.empty(),"Edge cap must preserve a larger unsupported gap even in TIN mode");
    auto moving=w;moving.timeDepth.depthTimePairs={{0,0,75,20,Missing},{1000,2000,125,80,Missing}};near(intersectHorizons(moving,{h},irregular(125)).boundaries.at(0).md,500);
    for(auto& point:h.surface.points)point.zTime=3000;check(intersectHorizons(w,{h},irregular(125)).boundaries.empty(),"TIN must not extrapolate the observed time-depth range");
}
void readableMissingSupport(){
    auto h=surface();h.surface.points={{0,0,0,0,1000},{1,0,200,0,1000},{0,1,0,200,1000},{1,1,200,200,1000}};
    auto missing=surface();missing.role="T73";
    // A missing local surface is not repaired by confirming metadata; retain
    // another independently supported horizon from the same request.
    missing.surface.timeUnit.clear();missing.surface.coordinateSystem.clear();
    auto absent=intersectHorizons(well(50,50),{missing,h},irregular());
    check(absent.boundaries.size()==1&&absent.boundaries.front().role=="T70","An empty local horizon must not erase independently supported interfaces");
    check(warns(absent,"T73：局部无有效层位点"),"Empty local horizon needs a readable no-points diagnosis");
    auto blank=surface();blank.surface.points={{1,1,0,0,Missing},{2,2,100,100,Missing}};
    auto noValues=intersectHorizons(well(50,50),{blank},irregular());check(noValues.boundaries.empty()&&warns(noValues,"局部无有效层位点"),"All-missing horizon time values must remain a no-points diagnosis");
    auto exterior=intersectHorizons(well(400,400),{h},irregular());
    check(exterior.boundaries.empty()&&warns(exterior,"不能围合成受支持的面")&&!warns(exterior,"当前时深与曲线允许范围"),"Spatial exterior must not be mislabeled as a time/GR limit");

    auto gap=surface();gap.role="T73";for(int i:{0,1,10,11})for(int j=0;j<3;++j)gap.surface.points.push_back({i,j,100.*i,100.*j,1000.});
    const auto sourceCount=gap.surface.points.size();const auto options=irregular(750);
    auto longGap=intersectHorizons(well(500,100),{gap},options);
    check(longGap.boundaries.empty()&&warns(longGap,"井轨迹处为层位支持空白")&&warns(longGap,"当前上限 750 m"),"Long-edge rejection must identify the source blank and unchanged 750 m policy");
    check(!warns(longGap,"当前时深与曲线允许范围"),"Zero spatial support must not ask for a deeper time-depth range");
    check(gap.surface.points.size()==sourceCount&&options.maximumSurfaceEdge==750,"Diagnostics may not insert source points or relax the spatial cap");
    near(intersectHorizons(well(50,100),{gap},options).boundaries.at(0).md,500);
    bool allRejected=false;try{intersectHorizons(well(50,50),{h},irregular(50));}catch(const std::invalid_argument& e){allRejected=std::string(e.what()).find("T70：局部层位三角边均超过上限 50")!=std::string::npos;}
    check(allRejected,"All-long local triangles remain rejected with a layer-specific readable reason");

    auto timeMiss=h;for(auto& point:timeMiss.surface.points)point.zTime=2500;
    auto limits=irregular();limits.allowTopExtrapolation=true;limits.allowBottomExtrapolation=true;limits.maximumTopExtrapolationM=0;limits.maximumBottomExtrapolationM=0;limits.maximumLogExtrapolationM=100;
    auto temporal=intersectHorizons(well(50,50),{timeMiss},limits);
    check(temporal.boundaries.empty()&&warns(temporal,"已有层位空间支持")&&warns(temporal,"当前时深与曲线允许范围内没有交点")&&warns(temporal,"上/下端余量 100/100 m"),"A time/curve-limited miss must report valid spatial support and the actual allowed margin");
    auto partial=well(50,50);partial.timeDepth.depthTimePairs.back().x=300;
    auto mixed=intersectHorizons(partial,{timeMiss},irregular());
    check(mixed.boundaries.empty()&&warns(mixed,"部分井轨迹有层位空间支持，但空间覆盖不完整"),"Partial spatial coverage must not be presented as a purely temporal failure");
}
}
int main(){try{threePointPlane();sparseLinesAndDefaultHole();readableMissingSupport();std::cout<<"PASS opt-in sparse TIN: exact plane, bounded support, source blanks, time/GR limits, duplicate conflicts and default holes\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
