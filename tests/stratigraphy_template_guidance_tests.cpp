#include "paleo_stratigraphy/core.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace paleo::stratigraphy;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double actual,double expected,const char* message){check(std::isfinite(actual)&&std::abs(actual-expected)<1e-8,message);}
template<class F>void rejects(F action,const char* message){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}check(rejected,message);}
InterpretationBoundary marker(std::string id,std::string name,double md){
    InterpretationBoundary b;b.id=id;b.name=name;b.role=name;b.md=b.originalMd=md;
    b.source="imported_interpretation";b.status="confirmed";b.locked=true;
    b.evidenceJson="{\"sourceRow\":7,\"original\":true}";return b;
}
WellInterpretation reference(){
    WellInterpretation w;w.wellId=1;w.name="SYNTH-REFERENCE";
    w.boundaries={marker("r0","T70",100),marker("r1","cycle-a",130),
                  marker("r2","cycle-b",170),marker("r3","cycle-c",240),marker("r4","T72",300)};
    for(int i=0;i<4;++i){InterpretationCycle c;c.id="source-l4-"+std::to_string(i);c.name=c.id;
        c.segmentId="E12";c.topBoundaryId=w.boundaries[i].id;c.bottomBoundaryId=w.boundaries[i+1].id;
        c.direction=i%2?-1:1;c.source="imported_interpretation";c.status="confirmed";
        c.parentId="source-l3-"+std::to_string(i/2);w.cycles.push_back(c);}
    for(int i=0;i<2;++i){InterpretationCycle c;c.id="source-l3-"+std::to_string(i);c.name=c.id;
        c.segmentId="E12";c.level=3;c.topBoundaryId=w.boundaries[2*i].id;
        c.bottomBoundaryId=w.boundaries[2*i+2].id;c.direction=i?-1:1;
        c.source="imported_interpretation";c.status="confirmed";w.cycles.push_back(c);}
    return w;
}
WellInterpretation target(){
    WellInterpretation w;w.wellId=2;w.name="SYNTH-TARGET";w.input.wellId=2;
    w.boundaries={marker("t0","T70",100),marker("t4","T72",500)};
    auto& log=w.input.log;log.id=log.name=log.kind="GR";log.unit="API";
    log.inputFingerprint="unchanged-synthetic-source";
    for(int md=0;md<=600;++md){log.axis.push_back(md);log.values.push_back(50.);}
    return w;
}
CorrelationAlgorithmProfile profile(){
    CorrelationAlgorithmProfile p;p.arOrders={5,9};p.padding=20;p.minimumThickness=15;
    return p;
}
void sourceUnchanged(const WellInterpretation& actual,const WellInterpretation& original){
    check(actual.input.log.axis==original.input.log.axis&&actual.input.log.values==original.input.log.values&&
          actual.input.log.inputFingerprint==original.input.log.inputFingerprint,"Input curve source must remain unchanged");
    check(actual.boundaries.size()==original.boundaries.size()&&actual.cycles.size()==original.cycles.size(),"Source interpretation must not be replaced");
    for(size_t i=0;i<actual.boundaries.size();++i){const auto& a=actual.boundaries[i];const auto& b=original.boundaries[i];
        check(a.id==b.id&&a.md==b.md&&a.originalMd==b.originalMd&&a.source==b.source&&a.status==b.status&&
              a.locked==b.locked&&a.evidenceJson==b.evidenceJson,"Source boundary provenance must remain unchanged");}
    for(size_t i=0;i<actual.cycles.size();++i){const auto& a=actual.cycles[i];const auto& b=original.cycles[i];
        check(a.id==b.id&&a.parentId==b.parentId&&a.source==b.source&&a.status==b.status&&
              a.topBoundaryId==b.topBoundaryId&&a.bottomBoundaryId==b.bottomBoundaryId,"Source cycle provenance must remain unchanged");}
}
void flatCurveAndHierarchy(){
    auto ref=reference();const auto originalRef=ref;auto t=extractTemplate(ref,"SYNTH-GUIDED","1");
    check(t.segments.at(0).parentGroups==std::vector<int>({0,0,1,1}),"Reference must retain two explicit L3 parents");
    auto w=target();const auto original=w;auto p=profile();
    check(!p.allowTemplateGuidedCandidates,"Strict core must remain the default");
    rejects([&]{divideSegment(w,t,"E12",p);},"Constant GR must fail without explicitly enabled template guidance");
    p.allowTemplateGuidedCandidates=true;auto out=divideSegment(w,t,"E12",p);
    check(out.events.empty(),"Constant GR must not be reclassified as observed features");
    check(out.boundaries.size()==5&&out.cycles.size()==6,"Template must produce exactly four L4 and two L3 cycles");
    const double expected[]={100,160,240,380,500};
    for(size_t i=0;i<out.boundaries.size();++i)near(out.boundaries[i].md,expected[i],"Template fractions must map exactly to unequal target intervals");
    for(size_t i=1;i+1<out.boundaries.size();++i){const auto& b=out.boundaries[i];
        check(b.status=="review"&&!b.locked&&b.source=="algorithm","Template-only boundaries must remain unlocked review candidates");
        check(b.evidenceJson.find("\"event\":{\"method\":\"template_position_prior/v1\"")!=std::string::npos&&
              b.evidenceJson.find("\"observedFeature\":false")!=std::string::npos,"Template prior must carry a nested explicit weak-evidence marker");}
    check(!out.warnings.empty(),"Template-only propagation must request review");
    std::vector<const InterpretationCycle*> l3,l4;
    for(const auto& c:out.cycles)(c.level==3?l3:l4).push_back(&c);
    check(l3.size()==2&&l4.size()==4,"L3 and L4 counts must match the approved template");
    for(size_t i=0;i<l4.size();++i){check(l4[i]->parentId==l3[i/2]->id,"L4 must retain its explicit template parent group");
        check(l4[i]->templateIdentity==t.segments[0].sequenceIdentities[i]&&l4[i]->direction==t.segments[0].directions[i],"L4 ordinal identity and polarity must survive propagation");}
    check(l3[0]->direction==1&&l3[1]->direction==-1,"L3 polarity must come from its explicit standard-well parent");
    check(l3[0]->bottomBoundaryId==l3[1]->topBoundaryId&&l3[0]->bottomBoundaryId==out.boundaries[2].id,"L3 shared interface must be the same L4 boundary object");
    auto generated=w;generated.boundaries=out.boundaries;generated.cycles=out.cycles;
    check(validateInterpretation(generated).empty(),"Template hierarchy must satisfy all interpretation invariants");
    sourceUnchanged(w,original);sourceUnchanged(ref,originalRef);
    check(out.boundaries.front().source==original.boundaries.front().source&&
          out.boundaries.front().evidenceJson==original.boundaries.front().evidenceJson,"Existing frame source must survive exact copying");
}
void observedEventsWin(){
    auto w=target();for(size_t i=0;i<w.input.log.axis.size();++i){const double z=w.input.log.axis[i];
        w.input.log.values[i]=50.+20.*std::tanh((z-180.)/3.)-25.*std::tanh((z-360.)/4.);}
    const auto original=w;auto p=profile();p.usePace=p.useResidual=p.usePhase=false;
    auto events=detectCycleEvents({w.input.log},100,500,p);
    std::vector<double> chosen;
    for(double center:{180.,360.}){const CycleEvent* best=nullptr;
        for(const auto& e:events)if(std::abs(e.md-center)<50.&&(!best||e.score>best->score))best=&e;
        if(best)chosen.push_back(best->md);}
    check(chosen.size()>=2,"Synthetic GR must provide at least two separated observed events");
    InterpretationTemplate t;t.id="OBSERVED-COLLISION";t.version="1";t.referenceWellId=1;t.status="approved";
    InterpretationTemplateSegment s;s.segmentId="E12";s.boundaryFractions={0,(chosen[0]-100)/400.,(chosen[1]-100)/400.,1};
    s.directions={1,-1,1};s.parentGroups={0,0,1};s.parentDirections={1,1,-1};s.sequenceIdentities={"A","B","C"};t.segments.push_back(s);
    p.allowTemplateGuidedCandidates=true;auto out=divideSegment(w,t,"E12",p);
    for(size_t i=1;i+1<out.boundaries.size();++i){const auto& b=out.boundaries[i];
        check(b.evidenceJson.find("multiscale_observed_curve_events/v1")!=std::string::npos&&
              b.evidenceJson.find("template_position_prior/v1")==std::string::npos,"Observed events must beat coincident weak template priors");
        near(b.md,chosen[i-1],"Observed event should retain its measured candidate depth");}
    sourceUnchanged(w,original);
}
void protectionsRemain(){
    auto ref=reference();auto t=extractTemplate(ref,"PROTECTIONS","1");auto p=profile();p.allowTemplateGuidedCandidates=true;
    auto low=target();for(size_t i=0;i<low.input.log.axis.size();++i)if(i>=100&&i<=500&&i%3==0)low.input.log.values[i]=Missing;
    rejects([&]{divideSegment(low,t,"E12",p);},"Template guidance must not bypass less than 80 percent observed coverage");
    auto gap=target();for(int i=156;i<=164;++i)gap.input.log.values[i]=Missing;
    rejects([&]{divideSegment(gap,t,"E12",p);},"A template prior falling inside a GR gap must not be injected as observed support");
    auto otherGap=target();for(int i=201;i<=210;++i)otherGap.input.log.values[i]=Missing;
    auto gapResult=divideSegment(otherGap,t,"E12",p);
    for(const auto& series:gapResult.usedSeries){auto at=std::lower_bound(series.axis.begin(),series.axis.end(),205.);
        check(at!=series.axis.end()&&*at==205.&&!std::isfinite(series.values[size_t(at-series.axis.begin())]),"Template propagation must preserve missing GR spans in all derived curves");}
    auto tooThin=p;tooThin.minimumThickness=110;
    rejects([&]{divideSegment(target(),t,"E12",tooThin);},"Template guidance must still enforce minimum cycle thickness");
    auto locked=target();for(int i=0;i<4;++i)locked.boundaries.push_back(marker("protected-"+std::to_string(i),"protected",150+80*i));
    rejects([&]{divideSegment(locked,t,"E12",p);},"Too many locked interfaces must not be dropped to satisfy a template count");
    auto shortLog=target();shortLog.input.log.axis.resize(450);shortLog.input.log.values.resize(450);
    rejects([&]{divideSegment(shortLog,t,"E12",p);},"Template guidance must not extend the GR curve to an uncovered frame");
    auto quality=target();quality.input.log.quality.assign(quality.input.log.axis.size(),0);
    for(int i=100;i<=220;++i)quality.input.log.quality[i]=1;
    rejects([&]{divideSegment(quality,t,"E12",p);},"Finite GR values marked missing must not count as observed support");
}
}
int main(){try{flatCurveAndHierarchy();observedEventsWin();protectionsRemain();
    std::cout<<"stratigraphy template guidance: hierarchy, weak evidence, source preservation and rejection checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
