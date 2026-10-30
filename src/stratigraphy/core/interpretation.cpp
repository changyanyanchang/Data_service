#include "internal.h"
#include <set>
#include <unordered_map>

namespace paleo::stratigraphy {
using namespace detail;
namespace {
std::vector<const InterpretationCycle*> orderedCycles(const WellInterpretation& w,const std::string& segment,int level){std::vector<const InterpretationCycle*> out;for(const auto& c:w.cycles)if(c.segmentId==segment&&c.level==level)out.push_back(&c);std::sort(out.begin(),out.end(),[&](auto a,auto b){return boundary(w,a->topBoundaryId).md<boundary(w,b->topBoundaryId).md;});return out;}
double windowMedian(const InterpretationSeries& s,double a,double b){std::vector<double> x;auto it=std::lower_bound(s.axis.begin(),s.axis.end(),a);for(size_t i=it-s.axis.begin();i<s.axis.size()&&s.axis[i]<=b;++i)if(valid(s,i))x.push_back(s.values[i]);return x.size()>=2?median(x):Missing;}
}
std::vector<CycleEvent> detectCycleEvents(const std::vector<InterpretationSeries>& series,double top,double bottom,const CorrelationAlgorithmProfile& p){
    checkProfile(p);if(!finite(top)||!finite(bottom)||bottom<=top)throw std::invalid_argument("Invalid event interval");
    const InterpretationSeries* raw=nullptr;for(const auto& s:series)if(s.kind=="GR"||s.name=="GR"||s.id=="GR"){raw=&s;break;}if(!raw&&!series.empty())raw=&series.front();if(!raw)throw std::invalid_argument("No curves supplied for event detection");checkAxis(*raw);
    double step=p.resample?p.sampleStep:std::max(.05,median([&](){std::vector<double> d;for(size_t i=1;i<raw->axis.size();++i)d.push_back(raw->axis[i]-raw->axis[i-1]);return d;}()));step=std::max(step,(bottom-top)/12000.);
    std::vector<double> grid;for(double z=top+p.minimumThickness;z<=bottom-p.minimumThickness;z+=step)grid.push_back(z);if(grid.empty())return {};
    std::vector<double> total(grid.size(),0),weights(grid.size(),0);std::vector<std::vector<double>> components;
    for(const auto& s:series){if(s.axis.empty())continue;checkAxis(s);double weight=0;if(&s==raw)weight=1.;else if(s.kind=="PACE"&&p.usePace)weight=p.paceWeight/double(std::max<size_t>(1,p.arOrders.size()));else if(s.kind=="FISCHER_GR_RESIDUAL"&&p.useResidual)weight=p.residualWeight;else if(s.kind=="COSINE_PHASE_MEDIAN"&&p.usePhase)weight=p.phaseWeight;if(weight==0)continue;
        std::vector<double> energy(grid.size(),Missing);for(size_t i=0;i<grid.size();++i){double z=grid[i];if(!finite(sample(*raw,z,p.maxGap)))continue;double best=0;bool observed=false;for(double radius:{std::max(2.*step,3.),std::max(4.*step,8.),std::max(6.*step,15.)}){double left=windowMedian(s,z-radius,z-step),right=windowMedian(s,z+step,z+radius),center=sample(s,z,p.maxGap);if(finite(left)&&finite(right)&&finite(center)){best=std::max(best,std::abs(right-left)+.5*std::abs(left+right-2*center));observed=true;}}if(observed)energy[i]=best;}
        std::vector<double> finiteEnergy;for(double v:energy)if(finite(v))finiteEnergy.push_back(v);if(finiteEnergy.empty())continue;std::sort(finiteEnergy.begin(),finiteEnergy.end());double scale=finiteEnergy[std::min(finiteEnergy.size()-1,size_t(.9*double(finiteEnergy.size()-1)))];if(scale<1e-12)continue;for(size_t i=0;i<grid.size();++i)if(finite(energy[i])){total[i]+=weight*std::min(3.,energy[i]/scale);weights[i]+=weight;}components.push_back(std::move(energy));
    }
    for(size_t i=0;i<grid.size();++i)if(weights[i])total[i]/=weights[i];
    std::vector<CycleEvent> out;for(size_t i=1;i+1<grid.size();++i)if(weights[i]>0&&total[i]>1e-6&&total[i]>=total[i-1]&&total[i]>total[i+1])out.push_back({grid[i],total[i],"{\"method\":\"multiscale_observed_curve_events/v1\",\"score\":"+number(total[i])+",\"probability\":false}"});
    if(out.size()>2048){std::partial_sort(out.begin(),out.begin()+2048,out.end(),[](const auto& a,const auto& b){return a.score>b.score;});out.resize(2048);}std::sort(out.begin(),out.end(),[](auto a,auto b){return a.md<b.md;});return out;
}
InterpretationTemplate extractTemplate(const WellInterpretation& reference,const std::string& id,const std::string& version,double tolerance){
    if(!finite(tolerance)||tolerance<0)throw std::invalid_argument("Source reconciliation tolerance must be nonnegative and explicitly selected");
    auto issues=validateInterpretation(reference);
    if(!issues.empty())throw std::invalid_argument("Reference interpretation is invalid: "+issues.front());
    if(id.empty()||version.empty())throw std::invalid_argument("Template ID and version are required");
    InterpretationTemplate result;result.id=id;result.name=reference.name+" template";result.version=version;
    result.referenceWellId=reference.wellId;result.sourceRevision=reference.input.inputFingerprint;result.status="approved";
    std::string audit="[";
    auto chain=[&](const std::vector<const InterpretationCycle*>& rows,double top,double bottom){
        if(rows.empty())throw std::invalid_argument("Reference sequence is empty");
        double first=boundary(reference,rows.front()->topBoundaryId).md,last=boundary(reference,rows.back()->bottomBoundaryId).md;
        if(std::abs(first-top)>tolerance+1e-6||std::abs(last-bottom)>tolerance+1e-6)throw std::invalid_argument("Reference does not cover the segment within the explicitly selected source reconciliation tolerance");
        for(size_t i=0;i<rows.size();++i){
            const auto& c=*rows[i];double a=boundary(reference,c.topBoundaryId).md,b=boundary(reference,c.bottomBoundaryId).md;
            if(c.source!="manual"&&c.source!="imported_interpretation"&&c.status!="confirmed")throw std::invalid_argument("Standard reference requires manual or explicitly confirmed cycles");
            if(c.direction!=1&&c.direction!=-1)throw std::invalid_argument("Reference cycle direction is not confirmed");
            if(b<=a)throw std::invalid_argument("Inverted manual reference cycle");
            if(i){double previous=boundary(reference,rows[i-1]->bottomBoundaryId).md;if(a<previous-1e-6||a-previous>tolerance+1e-6)throw std::invalid_argument("Reference cycles overlap or exceed the selected source gap tolerance");}
        }
    };
    for(const std::string segment:{"E12","E3","E4"}){
        auto rows=orderedCycles(reference,segment,4);if(rows.empty())continue;
        auto [tn,bn]=endpoints(segment);double top=named(reference,tn).md,bottom=named(reference,bn).md;
        if(bottom<=top)throw std::invalid_argument("Reference segment endpoints are inverted");
        chain(rows,top,bottom);auto parents=orderedCycles(reference,segment,3);if(!parents.empty())chain(parents,top,bottom);
        InterpretationTemplateSegment s;s.segmentId=segment;s.boundaryFractions.push_back(0);
        std::string rowAudit="[";
        for(size_t i=0;i<rows.size();++i){
            const auto& c=*rows[i];double a=boundary(reference,c.topBoundaryId).md,b=boundary(reference,c.bottomBoundaryId).md;
            double templateDepth=i+1<rows.size()?.5*(b+boundary(reference,rows[i+1]->topBoundaryId).md):bottom;
            s.boundaryFractions.push_back((templateDepth-top)/(bottom-top));s.directions.push_back(c.direction);
            s.sequenceIdentities.push_back(c.templateIdentity.empty()?segment+"-L4-"+std::to_string(i+1):c.templateIdentity);
            if(i)rowAudit+=',';rowAudit+="{\"sourceCycleId\":"+quote(c.id)+",\"originalTopMd\":"+number(a)+",\"originalBottomMd\":"+number(b)+",\"templateBottomMd\":"+number(templateDepth)+'}';
        }rowAudit+=']';s.boundaryFractions.back()=1.;
        std::string groupAudit="[";
        if(!parents.empty()){
            if(parents.size()>rows.size())throw std::invalid_argument("More L3 parents than L4 children");
            struct State{double cost;std::vector<size_t> cuts;int alternatives;};std::map<size_t,State> states{{0,{0,{0},1}}};
            for(size_t p=0;p+1<parents.size();++p){
                double pa=boundary(reference,parents[p]->bottomBoundaryId).md,pb=boundary(reference,parents[p+1]->topBoundaryId).md;
                std::map<size_t,State> next;
                for(size_t cut=1;cut<rows.size();++cut){
                    double a=boundary(reference,rows[cut-1]->bottomBoundaryId).md,b=boundary(reference,rows[cut]->topBoundaryId).md;
                    double error=std::max({0.,a-pa,pa-b,a-pb,pb-b});if(error>tolerance+1e-6)continue;
                    if(rows.size()-cut<parents.size()-p-1)continue;
                    double cost=std::pow(.5*(pa+pb-a-b),2);
                    for(const auto& [previous,state]:states){if(previous>=cut)continue;double total=state.cost+cost;auto found=next.find(cut);
                        if(found==next.end()||total<found->second.cost-1e-10){auto path=state.cuts;path.push_back(cut);next[cut]={total,std::move(path),state.alternatives};}
                        else if(std::abs(total-found->second.cost)<1e-10)found->second.alternatives=std::min(2,found->second.alternatives+state.alternatives);
                    }
                }
                if(next.empty())throw std::invalid_argument("No ordered L3-to-L4 ordinal mapping within the explicit source tolerance");states=std::move(next);
            }
            auto best=std::min_element(states.begin(),states.end(),[](const auto& a,const auto& b){return a.second.cost<b.second.cost;});int alternatives=0;
            for(const auto& [cut,state]:states)if(std::abs(state.cost-best->second.cost)<1e-10)alternatives+=state.alternatives;
            if(alternatives!=1)throw std::invalid_argument("Ambiguous L3-to-L4 source ordinal mapping; review standard well");
            auto cuts=best->second.cuts;cuts.push_back(rows.size());s.parentGroups.resize(rows.size());s.parentDirections.resize(rows.size());
            for(size_t p=0;p<parents.size();++p){
                for(size_t i=cuts[p];i<cuts[p+1];++i){s.parentGroups[i]=int(p);s.parentDirections[i]=parents[p]->direction;}
                if(p)groupAudit+=',';groupAudit+="{\"sourceParentId\":"+quote(parents[p]->id)+",\"originalTopMd\":"+number(boundary(reference,parents[p]->topBoundaryId).md)+",\"originalBottomMd\":"+number(boundary(reference,parents[p]->bottomBoundaryId).md)+",\"firstChildOrdinal\":"+std::to_string(cuts[p])+",\"childEndOrdinal\":"+std::to_string(cuts[p+1])+'}';
            }
        }groupAudit+=']';
        if(audit.size()>1)audit+=',';
        audit+="{\"segmentId\":"+quote(segment)+",\"regionalTopMd\":"+number(top)+",\"regionalBottomMd\":"+number(bottom)+",\"sourceL4\":"+rowAudit+",\"parentOrdinalMapping\":"+groupAudit+'}';
        result.segments.push_back(std::move(s));
    }
    if(result.segments.empty())throw std::invalid_argument("Reference has no interpreted regional segment");audit+=']';
    result.sourceReconciliationJson="{\"toleranceM\":"+number(tolerance)+",\"sourceDepthsModified\":false,\"meaning\":\"template ordinal mapping only; target anchors remain separate constraints\",\"segments\":"+audit+'}';
    return result;
}
SegmentationResult divideSegment(const WellInterpretation& well,const InterpretationTemplate& reference,const std::string& segment,const CorrelationAlgorithmProfile& p){
    checkProfile(p);checkAxis(well.input.log);if(reference.status!="approved"&&reference.status!="confirmed")throw std::invalid_argument("Template must be approved before propagation");if(reference.referenceWellId==well.wellId)throw std::invalid_argument("Target well cannot be its own standard reference");auto ti=std::find_if(reference.segments.begin(),reference.segments.end(),[&](const auto& s){return s.segmentId==segment;});if(ti==reference.segments.end())throw std::invalid_argument("Template lacks the selected regional segment");const auto& t=*ti;for(double fraction:t.boundaryFractions)if(!finite(fraction))throw std::invalid_argument("Template fraction must be finite");std::set<std::string> identities;for(const auto& identity:t.sequenceIdentities)if(identity.empty()||!identities.insert(identity).second)throw std::invalid_argument("Template sequence identities must be nonempty and unique");for(int group:t.parentGroups)if(group<0)throw std::invalid_argument("Template parent group must be nonnegative");size_t n=t.directions.size();if(!t.parentDirections.empty()&&t.parentDirections.size()!=n)throw std::invalid_argument("Template parent polarity count must match L4 intervals");if(!n||n>128||t.boundaryFractions.size()!=n+1||t.sequenceIdentities.size()!=n)throw std::invalid_argument("Invalid template sequence structure");if(std::abs(t.boundaryFractions.front())>1e-8||std::abs(t.boundaryFractions.back()-1)>1e-8)throw std::invalid_argument("Template fractions must span zero to one");for(size_t i=0;i<n;++i)if(!finite(t.boundaryFractions[i])||t.boundaryFractions[i+1]<=t.boundaryFractions[i]||(t.directions[i]!=1&&t.directions[i]!=-1))throw std::invalid_argument("Invalid template fractions or polarity");if(!t.parentGroups.empty()&&t.parentGroups.size()!=n)throw std::invalid_argument("L3 parent groups do not match L4 intervals");for(size_t i=1;i<t.parentGroups.size();++i)if(t.parentGroups[i]<t.parentGroups[i-1])throw std::invalid_argument("Template L3 groups must be contiguous and ordered");
    auto [tn,bn]=endpoints(segment);auto top=named(well,tn),bottom=named(well,bn);double span=bottom.md-top.md;if(span<n*p.minimumThickness)throw std::runtime_error("Segment cannot fit the template minimum thickness");if(top.md<well.input.log.axis.front()||bottom.md>well.input.log.axis.back())throw std::runtime_error("Observed curve does not cover the selected segment; fixed-count propagation refused");
    size_t observed=0,queried=0;for(double z=top.md;z<=bottom.md;z+=std::max(p.sampleStep,span/5000.)){++queried;if(finite(sample(well.input.log,z,p.maxGap)))++observed;}if(!queried||double(observed)/queried<.8)throw std::runtime_error("Less than 80 percent observed GR coverage; fixed-count propagation refused");
    auto derived=deriveCurves(well.input.log,p,top.md,bottom.md);std::vector<InterpretationSeries> signals=derived.series;auto raw=well.input.log;raw.kind="GR";signals.insert(signals.begin(),raw);auto events=detectCycleEvents(signals,top.md,bottom.md,p);
    // Template positions are explicit priors, never presented as observed features.
    struct Node{double depth,score;const InterpretationBoundary* pinned=nullptr;std::string evidence;};
    std::vector<Node> nodes{{top.md,0,&top,{}}};
    if(p.allowTemplateGuidedCandidates)for(size_t k=1;k<n;++k){
        const double md=top.md+span*t.boundaryFractions[k];
        if(!finite(sample(well.input.log,md,p.maxGap)))continue;
        nodes.push_back({md,-0.25,nullptr,"{\"method\":\"template_position_prior/v1\",\"observedFeature\":false,\"ordinal\":"+std::to_string(k)+",\"fraction\":"+number(t.boundaryFractions[k])+"}"});
    }
    for(auto e:events)if(e.md>top.md&&e.md<bottom.md)nodes.push_back({e.md,e.score,nullptr,e.evidenceJson});for(const auto& b:well.boundaries)if(b.locked&&b.md>top.md&&b.md<bottom.md)nodes.push_back({b.md,0,&b,b.evidenceJson});nodes.push_back({bottom.md,0,&bottom,{}});std::sort(nodes.begin(),nodes.end(),[](auto a,auto b){if(a.depth!=b.depth)return a.depth<b.depth;return bool(a.pinned)>bool(b.pinned);});std::vector<Node> unique;for(auto row:nodes){if(!unique.empty()&&std::abs(unique.back().depth-row.depth)<1e-6){if(row.pinned&&unique.back().pinned&&row.pinned->id!=unique.back().pinned->id)throw std::invalid_argument("Multiple locked boundary objects at the same depth");if(row.pinned||(!unique.back().pinned&&row.score>unique.back().score))unique.back()=row;}else unique.push_back(row);}nodes=std::move(unique);size_t m=nodes.size();std::vector<int> pins(m+1);for(size_t i=0;i<m;++i)pins[i+1]=pins[i]+int(i>0&&i+1<m&&nodes[i].pinned!=nullptr);if(size_t(pins.back())>n-1)throw std::runtime_error("More protected internal boundaries than the template allows");if(m<n+1)throw std::runtime_error("Insufficient observed curve events; no synthetic equal-thickness boundaries were inserted");
    double neg=-std::numeric_limits<double>::infinity();std::vector<double> previous(m,neg),next(m,neg);previous[0]=0;std::vector<std::vector<int>> back(n+1,std::vector<int>(m,-1));
    for(size_t k=1;k<=n;++k){std::fill(next.begin(),next.end(),neg);for(size_t j=1;j<m;++j){if((k==n)!=(j==m-1))continue;double fraction=(nodes[j].depth-top.md)/span;double penalty=k<n?p.referenceWeight*std::pow((fraction-t.boundaryFractions[k])/.15,2):0.;for(size_t i=0;i<j;++i){if(!finite(previous[i])||nodes[j].depth-nodes[i].depth<p.minimumThickness-1e-8)continue;if(pins[j]-pins[i+1]>0)continue;double candidate=previous[i]+nodes[j].score-penalty;if(candidate>next[j]){next[j]=candidate;back[k][j]=int(i);}}}previous.swap(next);}
    if(!finite(previous.back()))throw std::runtime_error("No ordered event path satisfies manual locks and minimum thickness");std::vector<size_t> path(n+1);size_t index=m-1;for(size_t k=n;k>0;--k){path[k]=index;index=size_t(back[k][index]);}path[0]=0;
    SegmentationResult out;out.events=events;out.warnings=std::move(derived.warnings);out.usedSeries=std::move(derived.series);for(auto& series:out.usedSeries){series.segmentId=segment;series.id+="@"+segment;}std::string prefix="w"+std::to_string(well.wellId)+"-"+key(reference.id+reference.version+segment);
    for(size_t k=0;k<path.size();++k){const auto& node=nodes[path[k]];InterpretationBoundary b;if(node.pinned)b=*node.pinned;else{b.id=prefix+"-b-"+std::to_string(k);b.name=segment+" interface "+std::to_string(k);b.role="cycle";b.md=b.originalMd=node.depth;b.source="algorithm";b.evidenceJson="{\"method\":\"ordered_template_event_dp/v1\",\"referenceWellId\":"+std::to_string(reference.referenceWellId)+",\"templateId\":"+quote(reference.id)+",\"templateVersion\":"+quote(reference.version)+",\"event\":"+(node.evidence.empty()?"{}":node.evidence)+"}";}out.boundaries.push_back(b);}
    std::map<int,std::vector<size_t>> parents;for(size_t i=0;i<n;++i){InterpretationCycle c;c.id=prefix+"-l4-"+std::to_string(i);c.name=t.sequenceIdentities[i];c.segmentId=segment;c.topBoundaryId=out.boundaries[i].id;c.bottomBoundaryId=out.boundaries[i+1].id;c.direction=t.directions[i];c.templateIdentity=t.sequenceIdentities[i];c.templateVersion=reference.version;if(!t.parentGroups.empty()){int g=t.parentGroups[i];c.parentId=prefix+"-l3-"+std::to_string(g);parents[g].push_back(i);}out.cycles.push_back(c);}
    for(const auto& [group,children]:parents){InterpretationCycle c;c.id=prefix+"-l3-"+std::to_string(group);c.name=segment+" L3 "+std::to_string(group+1);c.segmentId=segment;c.level=3;c.topBoundaryId=out.boundaries[children.front()].id;c.bottomBoundaryId=out.boundaries[children.back()+1].id;c.templateIdentity=segment+"-L3-"+std::to_string(group+1);c.templateVersion=reference.version;if(t.parentDirections.size()==n){c.direction=t.parentDirections[children.front()];if(c.direction<-1||c.direction>1)throw std::invalid_argument("Invalid L3 reference polarity");for(size_t child:children)if(t.parentDirections[child]!=c.direction)throw std::invalid_argument("Inconsistent L3 direction within a template parent group");}out.cycles.push_back(c);}
    size_t guided=0;for(auto index:path)if(nodes[index].evidence.find("template_position_prior/v1")!=std::string::npos)++guided;if(guided)out.warnings.push_back("本段有 "+std::to_string(guided)+" 个界面采用统一模板相对位置候选，未识别到足够的曲线特征支持，请结合曲线拖拽校核。");
    if(!parents.empty()&&t.parentDirections.size()!=n)out.warnings.push_back("Template has no explicit L3 directions; parent directions are left unknown");return out;
}
InterpretationSeries fischerCdmt(const WellInterpretation& well,const std::string& segment,int level){auto rows=orderedCycles(well,segment,level);if(rows.empty())throw std::invalid_argument("No cycles available for CDMT");std::vector<double> thickness;double previous=Missing;for(auto c:rows){double a=boundary(well,c->topBoundaryId).md,b=boundary(well,c->bottomBoundaryId).md;if(!finite(a)||!finite(b)||b<=a||finite(previous)&&std::abs(a-previous)>1e-5)throw std::invalid_argument("CDMT requires consecutive, non-overlapping cycle thicknesses");thickness.push_back(b-a);previous=b;}double mean=std::accumulate(thickness.begin(),thickness.end(),0.)/thickness.size();InterpretationSeries s;s.id="FISCHER_CDMT-"+segment+"-L"+std::to_string(level);s.name="Cumulative deviation from mean cycle thickness";s.kind="FISCHER_CDMT";s.axisKind="CYCLE_INDEX";s.axisUnit="1";s.unit="m";s.segmentId=segment;s.axis={0};s.values={0};s.quality={0};double sum=0;for(size_t i=0;i<thickness.size();++i){sum+=thickness[i]-mean;s.axis.push_back(double(i+1));s.values.push_back(sum);s.quality.push_back(0);}s.parametersJson="{\"formula\":\"sum(thickness-meanThickness)\",\"meanThicknessM\":"+number(mean)+",\"level\":"+std::to_string(level)+",\"axis\":\"ordered_cycle_index\"}";s.inputFingerprint=well.input.inputFingerprint;return s;}
std::vector<std::string> validateInterpretation(const WellInterpretation& w){
    std::vector<std::string> errors;std::map<std::string,const InterpretationBoundary*> bounds;std::map<std::string,const InterpretationCycle*> cycles;for(const auto& b:w.boundaries){if(b.id.empty()||!bounds.emplace(b.id,&b).second)errors.push_back("Missing or duplicate boundary ID");if(!finite(b.md))errors.push_back("Boundary MD is not finite: "+b.id);}double prior=-std::numeric_limits<double>::infinity();for(const std::string name:{"T70","T72","T73","T80"}){const InterpretationBoundary* found=nullptr;for(const auto& b:w.boundaries)if(b.name==name||b.role==name){if(found)errors.push_back("Duplicate regional boundary "+name);found=&b;}if(found){if(found->md<=prior)errors.push_back("Regional boundaries must be ordered T70 < T72 < T73 < T80");prior=found->md;}}
    std::map<std::pair<std::string,int>,std::vector<const InterpretationCycle*>> groups;for(const auto& c:w.cycles){if(c.id.empty()||!cycles.emplace(c.id,&c).second)errors.push_back("Missing or duplicate cycle ID");if(!bounds.count(c.topBoundaryId)||!bounds.count(c.bottomBoundaryId)){errors.push_back("Cycle refers to missing boundary: "+c.id);continue;}if(bounds[c.bottomBoundaryId]->md<=bounds[c.topBoundaryId]->md)errors.push_back("Inverted or zero-thickness cycle: "+c.id);if(c.direction<-1||c.direction>1)errors.push_back("Invalid cycle direction: "+c.id);groups[{c.segmentId,c.level}].push_back(&c);try{auto [tn,bn]=endpoints(c.segmentId);double top=named(w,tn).md,bottom=named(w,bn).md;if(bounds[c.topBoundaryId]->md<top-1e-6||bounds[c.bottomBoundaryId]->md>bottom+1e-6)errors.push_back("Cycle extends outside regional segment: "+c.id);}catch(const std::invalid_argument&){errors.push_back("Cycle lacks a valid regional segment framework: "+c.id);}}
    for(auto& [key,rows]:groups){std::sort(rows.begin(),rows.end(),[&](auto a,auto b){return bounds[a->topBoundaryId]->md<bounds[b->topBoundaryId]->md;});for(size_t i=1;i<rows.size();++i)if(bounds[rows[i-1]->bottomBoundaryId]->md>bounds[rows[i]->topBoundaryId]->md+1e-6)errors.push_back("Overlapping cycles at the same level");}
    std::map<std::string,std::vector<const InterpretationCycle*>> children;for(const auto& c:w.cycles)if(!c.parentId.empty()){if(!cycles.count(c.parentId)){errors.push_back("Missing cycle parent: "+c.id);continue;}const auto& parent=*cycles[c.parentId];if(parent.level>=c.level||parent.segmentId!=c.segmentId)errors.push_back("Invalid parent level or regional segment: "+c.id);if(bounds.count(c.topBoundaryId)&&bounds.count(c.bottomBoundaryId)&&bounds.count(parent.topBoundaryId)&&bounds.count(parent.bottomBoundaryId)){if(bounds[c.topBoundaryId]->md<bounds[parent.topBoundaryId]->md-1e-6||bounds[c.bottomBoundaryId]->md>bounds[parent.bottomBoundaryId]->md+1e-6)errors.push_back("Child outside parent: "+c.id);children[c.parentId].push_back(&c);}}
    for(auto& [parentId,rows]:children){std::sort(rows.begin(),rows.end(),[&](auto a,auto b){return bounds[a->topBoundaryId]->md<bounds[b->topBoundaryId]->md;});const auto& parent=*cycles[parentId];if(rows.front()->topBoundaryId!=parent.topBoundaryId||rows.back()->bottomBoundaryId!=parent.bottomBoundaryId)errors.push_back("Parent must share outer boundary IDs with its children");for(size_t i=1;i<rows.size();++i)if(rows[i-1]->bottomBoundaryId!=rows[i]->topBoundaryId)errors.push_back("Parent children must share consecutive boundary IDs");}return errors;
}
void moveBoundary(WellInterpretation& well,const std::string& id,double md){
    if(!finite(md))throw std::invalid_argument("New boundary depth is not finite");
    auto copy=well;auto it=std::find_if(copy.boundaries.begin(),copy.boundaries.end(),[&](auto b){return b.id==id;});
    if(it==copy.boundaries.end())throw std::invalid_argument("Unknown boundary ID");
    if(!finite(it->originalMd))it->originalMd=it->md;
    it->md=md;it->source="manual";it->locked=true;it->status="review";
    std::set<std::string> affected;
    for(auto& c:copy.cycles)if(c.topBoundaryId==id||c.bottomBoundaryId==id){c.status="review";affected.insert(c.segmentId);}
    std::string name=it->name;
    if(name!="T70"&&name!="T72"&&name!="T73"&&name!="T80")name=it->role;
    bool regional=name=="T70"||name=="T72"||name=="T73"||name=="T80";
    if(regional){
        if(name=="T70"||name=="T72")affected.insert("E12");
        if(name=="T72"||name=="T73")affected.insert("E3");
        if(name=="T73"||name=="T80")affected.insert("E4");
        for(auto& c:copy.cycles)if(affected.count(c.segmentId))c.status="review";
    }
    auto issues=validateInterpretation(copy);
    if(!issues.empty())throw std::invalid_argument("Boundary move rejected: "+issues.front());
    for(auto& series:copy.series){
        bool inScope=series.segmentId.empty()||affected.count(series.segmentId)>0;
        if(inScope&&(regional||series.kind=="FISCHER_CDMT"))series.status="stale";
    }
    if(regional)copy.warnings.push_back("Regional boundary changed; affected derived curves are retained as stale until recomputed");
    well=std::move(copy);
}
}