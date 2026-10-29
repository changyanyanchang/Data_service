#include "internal.h"
#include <delaunator.hpp>
#include <array>
#include <numbers>
#include <set>

namespace paleo::stratigraphy {
using namespace detail;
namespace {
double metres(double value,const std::string& unit){if(unitToken(unit)=="m")return value;if(unitToken(unit)=="ft")return value*.3048;throw std::invalid_argument("Depth units must be explicitly m or ft");}
double timeScale(const std::string& unit,const std::string& type,bool owt){double scale=unitToken(unit)=="ms"?1.:unitToken(unit)=="s"?1000.:0.;if(!scale)throw std::invalid_argument("Time unit must be explicitly ms or s");if(axisToken(type)=="TWT")return scale;if(axisToken(type)=="OWT"&&owt)return 2.*scale;throw std::invalid_argument("Time type must be TWT, or explicit OWT conversion must be enabled");}
void confirmedMatch(const std::string& a,const std::string& b,const std::string& confirmation,const std::string& label){auto equivalent=[&](const auto& x,const auto& y){return metadataToken(x,label)==metadataToken(y,label);};if(!a.empty()&&!b.empty()&&!equivalent(a,b))throw std::invalid_argument(label+" differ; core does not silently transform them");if((a.empty()||b.empty())&&confirmation.empty())throw std::invalid_argument(label+" are missing; explicit confirmation is required");if(!confirmation.empty()&&(!a.empty()&&!equivalent(a,confirmation)||!b.empty()&&!equivalent(b,confirmation)))throw std::invalid_argument(label+" confirmation conflicts with source metadata");}
void fillConfirmed(std::string& source,const std::string& confirmation,const std::string& label){if(confirmation.empty()){source=metadataToken(source,label);return;}if(!source.empty()&&metadataToken(source,label)!=metadataToken(confirmation,label))throw std::invalid_argument(label+" confirmation conflicts with declared source value");if(source.empty())source=confirmation;source=metadataToken(source,label);}
std::string confirmations(const GeometryOptions& o){return "{\"wellTimeUnit\":"+quote(o.confirmedWellTimeUnit)+",\"horizonTimeUnit\":"+quote(o.confirmedHorizonTimeUnit)+",\"wellTimeType\":"+quote(o.confirmedWellTimeType)+",\"horizonTimeType\":"+quote(o.confirmedHorizonTimeType)+",\"depthUnit\":"+quote(o.confirmedDepthUnit)+",\"timeDepthBasis\":"+quote(o.confirmedDepthType)+",\"logDepthBasis\":"+quote(o.confirmedLogDepthType)+",\"mdReference\":"+quote(o.confirmedMdReference)+",\"verticalDepthReference\":"+quote(o.confirmedVerticalDepthReference)+",\"coordinateSystem\":"+quote(o.confirmedCoordinateSystem)+",\"coordinateUnit\":"+quote(o.confirmedCoordinateUnit)+",\"timeReference\":"+quote(o.confirmedTimeReference)+'}';}
void checkPositions(std::vector<TrajectoryPosition>& positions){std::sort(positions.begin(),positions.end(),[](auto a,auto b){return a.md<b.md;});if(positions.size()<2)throw std::invalid_argument("At least two trajectory positions are needed");for(size_t i=0;i<positions.size();++i)if(!finite(positions[i].md)||!finite(positions[i].x)||!finite(positions[i].y)||(i&&positions[i].md<=positions[i-1].md))throw std::invalid_argument("Trajectory positions require finite unique MD/X/Y values");}
TrajectoryPosition positionAt(const std::vector<TrajectoryPosition>& p,double md){auto it=std::lower_bound(p.begin(),p.end(),md,[](const auto& a,double b){return a.md<b;});if(it==p.end())return {};if(std::abs(it->md-md)<1e-8)return *it;if(it==p.begin())return {};auto a=*(it-1),b=*it;double f=(md-a.md)/(b.md-a.md);auto lerp=[&](double x,double y){return finite(x)&&finite(y)?x+f*(y-x):Missing;};return {md,lerp(a.x,b.x),lerp(a.y,b.y),lerp(a.tvd,b.tvd),lerp(a.tvdss,b.tvdss)};}
double interpolate(const std::vector<std::pair<double,double>>& rows,double x){auto it=std::lower_bound(rows.begin(),rows.end(),x,[](auto a,double b){return a.first<b;});if(it==rows.end())return Missing;if(std::abs(it->first-x)<1e-8)return it->second;if(it==rows.begin())return Missing;auto a=*(it-1),b=*it;return a.second+(x-a.first)*(b.second-a.second)/(b.first-a.first);}
struct EndpointFit {
    double top=Missing,bottom=Missing,slope=Missing,rms=Missing,window=Missing,dx=0,dy=0;
    size_t samples=0;
    bool lowSupport=false;
};
struct TimeDepthExtension {
    bool enabled=false;
    double timeTop=Missing,timeBottom=Missing,xyTop=Missing,xyBottom=Missing;
    double logTop=Missing,logBottom=Missing,calculationTop=Missing,calculationBottom=Missing;
    double maximumTop=0,maximumBottom=0,maximumLog=0;
    EndpointFit topFit,bottomFit;
    static double distance(double md,double top,double bottom){return std::max({0.,top-md,md-bottom});}
    static std::string sideOf(double md,double top,double bottom){return md<top-1e-8?"top":md>bottom+1e-8?"bottom":"none";}
    std::string side(double md)const{
        const bool top=md<timeTop-1e-8||md<xyTop-1e-8||md<logTop-1e-8;
        const bool bottom=md>timeBottom+1e-8||md>xyBottom+1e-8||md>logBottom+1e-8;
        return top?(bottom?"mixed":"top"):(bottom?"bottom":"none");
    }
    bool applied(double md)const{return side(md)!="none";}
    std::string json(double md)const{
        const double timeDistance=distance(md,timeTop,timeBottom),xyDistance=distance(md,xyTop,xyBottom),outsideLog=distance(md,logTop,logBottom);
        const bool top=md<timeTop-1e-8||(timeDistance<1e-8&&(md<xyTop-1e-8||md<logTop-1e-8));
        const auto& fit=top?topFit:bottomFit;
        const auto& xyFit=md<xyTop-1e-8?topFit:bottomFit;
        auto n=[](double v){return finite(v)?number(v):"null";};
        return "{\"enabled\":"+std::string(enabled?"true":"false")+",\"applied\":"+std::string(applied(md)?"true":"false")
            +",\"method\":\"endpoint_anchored_theil_sen/v2\",\"side\":"+quote(side(md))
            +",\"observedTimeDepthTopMd\":"+n(timeTop)+",\"observedTimeDepthBottomMd\":"+n(timeBottom)
            +",\"observedTrajectoryTopMd\":"+n(xyTop)+",\"observedTrajectoryBottomMd\":"+n(xyBottom)
            +",\"effectiveLogTopMd\":"+n(logTop)+",\"effectiveLogBottomMd\":"+n(logBottom)
            +",\"calculationTopMd\":"+n(calculationTop)+",\"calculationBottomMd\":"+n(calculationBottom)
            +",\"timeDepthExtrapolationM\":"+n(timeDistance)+",\"trajectoryExtrapolationM\":"+n(xyDistance)
            +",\"outsideLogDistanceM\":"+n(outsideLog)+",\"timeDepthSide\":"+quote(sideOf(md,timeTop,timeBottom))
            +",\"trajectorySide\":"+quote(sideOf(md,xyTop,xyBottom))+",\"outsideLogSide\":"+quote(sideOf(md,logTop,logBottom))
            +",\"maximumTopExtrapolationM\":"+n(maximumTop)+",\"maximumBottomExtrapolationM\":"+n(maximumBottom)
            +",\"maximumLogExtrapolationM\":"+n(maximumLog)+",\"zeroDistanceCapMeans\":\"effective_log_extent_plus_margin\""
            +",\"fitWindowM\":"+n(fit.window)+",\"fitTopMd\":"+n(fit.top)+",\"fitBottomMd\":"+n(fit.bottom)
            +",\"fitSamples\":"+std::to_string(fit.samples)+",\"slopeMsPerM\":"+n(fit.slope)+",\"fitRmsMs\":"+n(fit.rms)
            +",\"lowSupport\":"+std::string(timeDistance>1e-8&&fit.lowSupport?"true":"false")
            +",\"trajectoryMethod\":"+quote(xyDistance>1e-8?(md<xyTop?"first_observed_XY_tangent/v1":"last_observed_XY_tangent/v1"):"observed_positions")
            +",\"trajectoryDxPerM\":"+n(xyFit.dx)+",\"trajectoryDyPerM\":"+n(xyFit.dy)
            +",\"sourceValuesModified\":false,\"grValuesExtrapolated\":false,\"horizonSpaceExtrapolated\":false}";
    }
};
bool fitEndpointTimeSlope(const std::vector<std::pair<double,double>>& times,bool top,double window,EndpointFit& fit){
    const auto anchor=top?times.front():times.back();
    size_t first=0,last=times.size()-1;
    if(top){auto end=std::upper_bound(times.begin(),times.end(),anchor.first+window,[](double md,const auto& p){return md<p.first;});last=size_t(end-times.begin())-1;if(last==0)last=1;}
    else{auto begin=std::lower_bound(times.begin(),times.end(),anchor.first-window,[](const auto& p,double md){return p.first<md;});first=size_t(begin-times.begin());if(first==last)--first;}
    const size_t count=last-first+1;
    // If only two observed ties support the endpoint, they still define a
    // line, but cannot establish curvature or residual reliability. Keep this
    // low-support condition explicit in the resulting review candidate.
    const size_t samples=std::min<size_t>(count,32);std::vector<std::pair<double,double>> selected;selected.reserve(samples);
    for(size_t i=0;i<samples;++i)selected.push_back(times[first+i*(count-1)/(samples-1)]);
    std::vector<double> slopes;for(size_t i=0;i<samples;++i)for(size_t j=i+1;j<samples;++j)slopes.push_back((selected[j].second-selected[i].second)/(selected[j].first-selected[i].first));
    const double slope=median(slopes);if(!finite(slope)||slope<=0)return false;
    double squares=0,maximum=0;for(size_t i=first;i<=last;++i){const auto residual=anchor.second+slope*(times[i].first-anchor.first)-times[i].second;squares+=residual*residual;maximum=std::max(maximum,std::abs(residual));}
    const double rms=std::sqrt(squares/double(count)),span=times[last].second-times[first].second;
    if(!finite(rms)||rms>std::max(2.,span*.10)||maximum>std::max(5.,span*.25))return false;
    fit.top=times[first].first;fit.bottom=times[last].first;fit.samples=count;fit.slope=slope;fit.rms=rms;fit.window=window;
    fit.lowSupport=count==2||fit.bottom-fit.top<20.;return true;
}
TimeDepthExtension extendTimeDepth(std::vector<std::pair<double,double>>& times,std::vector<TrajectoryPosition>& positions,const InterpretationSeries& log,const std::string& tdBasis,const std::string& xyUnit,const GeometryOptions& o,std::vector<std::string>& warnings){
    TimeDepthExtension receipt;receipt.enabled=o.allowBottomExtrapolation||o.allowTopExtrapolation;
    receipt.maximumTop=o.maximumTopExtrapolationM;receipt.maximumBottom=o.maximumBottomExtrapolationM;receipt.maximumLog=o.maximumLogExtrapolationM;
    receipt.timeTop=times.front().first;receipt.timeBottom=times.back().first;receipt.xyTop=positions.front().md;receipt.xyBottom=positions.back().md;
    receipt.logTop=log.axis.front();receipt.logBottom=log.axis.back();
    if(receipt.enabled){
        if(!finite(o.maximumLogExtrapolationM)||o.maximumLogExtrapolationM<0)throw std::invalid_argument("Log extrapolation margin must be finite and nonnegative");
        size_t first=0,last=log.axis.size()-1;while(first<log.axis.size()&&!valid(log,first))++first;while(last>first&&!valid(log,last))--last;
        if(first>=last)throw std::invalid_argument("Endpoint extrapolation needs at least two valid GR samples to establish its finite extent");
        receipt.logTop=log.axis[first];receipt.logBottom=log.axis[last];
    }
    double logTop=receipt.logTop-(o.allowTopExtrapolation?o.maximumLogExtrapolationM:0.);
    double logBottom=receipt.logBottom+(o.allowBottomExtrapolation?o.maximumLogExtrapolationM:0.);
    if(!finite(logTop)||!finite(logBottom))throw std::invalid_argument("Calculation log extent is not finite");
    receipt.calculationTop=std::max({logTop,receipt.timeTop,receipt.xyTop});receipt.calculationBottom=std::min({logBottom,receipt.timeBottom,receipt.xyBottom});
    if(!receipt.enabled)return receipt;
    const auto observedTimes=times;const auto observedPositions=positions;
    for(bool top:{true,false}){
        if(!(top?o.allowTopExtrapolation:o.allowBottomExtrapolation))continue;
        const double cap=top?o.maximumTopExtrapolationM:o.maximumBottomExtrapolationM;
        const double window=top?o.topExtrapolationFitWindowM:o.bottomExtrapolationFitWindowM;
        if(!finite(cap)||cap<0||!finite(window)||window<20.)throw std::invalid_argument("Endpoint extrapolation requires a nonnegative distance cap and a fit window of at least 20 m");
        if(tdBasis!="MD"){warnings.push_back("两端时深趋势外推只支持已明确的 MD 关系；本次保留原始时深与轨迹覆盖范围。");continue;}
        auto& fit=top?receipt.topFit:receipt.bottomFit;fit.window=window;
        const auto anchor=top?observedTimes.front():observedTimes.back();
        const auto position=top?observedPositions.front():observedPositions.back();
        double target=top?logTop:logBottom;
        if(cap>0)target=top?std::max({target,anchor.first-cap,position.md-cap}):std::min({target,anchor.first+cap,position.md+cap});
        auto extends=[&](double end){return top?target<end-1e-8:target>end+1e-8;};
        auto restrictTo=[&](double end){target=top?std::max(target,end):std::min(target,end);};
        if(extends(anchor.first)&&!fitEndpointTimeSlope(observedTimes,top,window,fit)){
            warnings.push_back(std::string(top?"上端":"下端")+"时深局部趋势不稳定；该端未外推时深关系。");restrictTo(anchor.first);
        }
        if(extends(position.md)){
            const auto& a=top?observedPositions.front():observedPositions[observedPositions.size()-2];
            const auto& b=top?observedPositions[1]:observedPositions.back();const double step=b.md-a.md;
            fit.dx=(b.x-a.x)/step;fit.dy=(b.y-a.y)/step;
            if(unitToken(xyUnit)!="m"||!finite(fit.dx)||!finite(fit.dy)||std::hypot(fit.dx,fit.dy)>1.001){
                warnings.push_back(std::string(top?"上端":"下端")+" XY 轨迹单位或切向斜率不可靠；该端未延伸井轨迹。");restrictTo(position.md);
            }
        }
        if(extends(anchor.first)){
            const double time=anchor.second+fit.slope*(target-anchor.first);if(!finite(time))throw std::invalid_argument("Extrapolated time is not finite");
            if(top)times.insert(times.begin(),{target,time});else times.emplace_back(target,time);
            if(fit.lowSupport)warnings.push_back(std::string(top?"上端":"下端")+"时深趋势仅由两个样点或不足 20 m 的样段支持，已生成低支持度外推候选，需复核。");
        }
        if(extends(position.md)){
            auto p=position;const double distance=target-position.md;p.md=target;p.x+=fit.dx*distance;p.y+=fit.dy*distance;p.tvd=Missing;p.tvdss=Missing;
            if(!finite(p.x)||!finite(p.y))throw std::invalid_argument("Extrapolated XY position is not finite");
            if(top)positions.insert(positions.begin(),p);else positions.push_back(p);
        }
    }
    receipt.calculationTop=std::max({logTop,times.front().first,positions.front().md});
    receipt.calculationBottom=std::min({logBottom,times.back().first,positions.back().md});return receipt;
}
struct Triangle{std::array<SeismicHorizonPoint,3> p;double xmin,xmax,ymin,ymax;};
std::array<double,3> triangleWeights(const Triangle& t,double x,double y){
    const auto& a=t.p[0];const auto& b=t.p[1];const auto& c=t.p[2];
    const double det=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
    const double u=((b.y-c.y)*(x-c.x)+(c.x-b.x)*(y-c.y))/det,v=((c.y-a.y)*(x-c.x)+(a.x-c.x)*(y-c.y))/det;
    return {u,v,1-u-v};
}
bool clipTriangleWeights(const std::array<double,3>& first,const std::array<double,3>& last,double& low,double& high){
    low=0;high=1;bool inside=true;
    for(size_t i=0;i<3;++i){const double change=last[i]-first[i];if(std::abs(change)<1e-14){if(first[i]<-1e-10)inside=false;}else if(change>0)low=std::max(low,(-1e-10-first[i])/change);else high=std::min(high,(-1e-10-first[i])/change);}
    if(!inside||low>high+1e-12)return false;low=std::clamp(low,0.,1.);high=std::clamp(high,0.,1.);return true;
}
class SurfaceSampler {
    std::vector<Triangle> triangles;
    // Rejected faces are retained only for missing-support diagnostics. They
    // are never indexed as supported faces, sampled, or used to solve a pick.
    std::vector<std::pair<Triangle,double>> rejectedLongEdges;
    std::map<std::pair<int,int>,std::vector<size_t>> buckets;
    double xmin=0,ymin=0,cellX=1,cellY=1;
    int bins=1;
    int bx(double x)const{return int(std::floor((x-xmin)/cellX));}int by(double y)const{return int(std::floor((y-ymin)/cellY));}
public:
    struct SegmentRoots {std::vector<double> fractions;bool coincident=false,fullySupported=false,anySupported=false;};
    explicit SurfaceSampler(const SeismicHorizon& h,double maxEdge,bool irregular){
        if(irregular&&(!finite(maxEdge)||maxEdge<=0))throw std::invalid_argument("Irregular surface interpolation requires an explicit finite positive maximum edge");
        std::map<std::pair<int,int>,SeismicHorizonPoint> points;std::set<int> ins,xls;for(const auto& p:h.points){if(irregular&&(std::isinf(p.x)||std::isinf(p.y)||std::isinf(p.zTime)))throw std::invalid_argument("Infinite horizon coordinate or time");if(finite(p.x)&&finite(p.y)&&finite(p.zTime)){auto [it,inserted]=points.emplace(std::make_pair(p.inlineNumber,p.crosslineNumber),p);if(!inserted&&(std::abs(it->second.x-p.x)>1e-8||std::abs(it->second.y-p.y)>1e-8||std::abs(it->second.zTime-p.zTime)>1e-8))throw std::invalid_argument("Conflicting duplicate horizon grid node");ins.insert(p.inlineNumber);xls.insert(p.crosslineNumber);}}
        if(!irregular&&(ins.size()<2||xls.size()<2))throw std::invalid_argument("局部层位点不能围合成受支持的面；规则网格至少需要两条有效主测线与联络线。");
        auto increment=[](const std::set<int>& values){int step=0,prev=*values.begin();for(int x:values){if(x!=prev)step=std::gcd(step,x-prev);prev=x;}return step;};int di=irregular||ins.empty()?0:increment(ins),dj=irregular||xls.empty()?0:increment(xls);
        auto add=[&](const SeismicHorizonPoint& a,const SeismicHorizonPoint& b,const SeismicHorizonPoint& c){double area=(b.x-a.x)*(c.y-a.y)-(c.x-a.x)*(b.y-a.y);if(std::abs(area)<1e-12)return;double edge=std::max({std::hypot(a.x-b.x,a.y-b.y),std::hypot(a.x-c.x,a.y-c.y),std::hypot(b.x-c.x,b.y-c.y)});Triangle t{{a,b,c},std::min({a.x,b.x,c.x}),std::max({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),std::max({a.y,b.y,c.y})};if(finite(maxEdge)&&edge>maxEdge){rejectedLongEdges.emplace_back(std::move(t),edge);return;}triangles.push_back(std::move(t));};
        if(irregular){
            // Only this explicitly requested path fills spaces between sampled
            // interpretation lines. Long triangles and the exterior are never
            // supported; the default grid path keeps its original hole rules.
            std::map<std::pair<double,double>,SeismicHorizonPoint> unique;
            for(const auto& [key,p]:points){auto [it,inserted]=unique.emplace(std::make_pair(p.x,p.y),p);if(!inserted&&std::abs(it->second.zTime-p.zTime)>1e-8)throw std::invalid_argument("Conflicting horizon times at one XY position");}
            if(unique.size()<3)throw std::invalid_argument("局部层位点不能围合成受支持的面；至少需要三个不同且不共线的有效位置。");
            double ox=unique.begin()->first.first,oy=INFINITY,span=0;for(const auto& [xy,p]:unique)oy=std::min(oy,p.y);
            for(const auto& [xy,p]:unique)span=std::max({span,p.x-ox,p.y-oy});if(!finite(span)||span<=0)throw std::invalid_argument("Invalid irregular horizon coordinate extent");
            std::vector<SeismicHorizonPoint> vertices;std::vector<double> coordinates;vertices.reserve(unique.size());coordinates.reserve(2*unique.size());
            for(const auto& [xy,p]:unique){vertices.push_back(p);coordinates.push_back((p.x-ox)/span);coordinates.push_back((p.y-oy)/span);}
            auto mesh=[&](){try{return delaunator::Delaunator(coordinates);}catch(const std::runtime_error& e){if(std::string(e.what())=="not triangulation")throw std::invalid_argument("局部层位点不能围合成受支持的面（有效位置共线或退化）；未生成界面。");throw;}}();
            for(size_t i=0;i+2<mesh.triangles.size();i+=3)add(vertices.at(mesh.triangles[i]),vertices.at(mesh.triangles[i+1]),vertices.at(mesh.triangles[i+2]));
        }else for(const auto& [key,a]:points){auto b=points.find({key.first+di,key.second}),c=points.find({key.first,key.second+dj}),d=points.find({key.first+di,key.second+dj});if(b!=points.end()&&c!=points.end())add(a,b->second,c->second);if(b!=points.end()&&c!=points.end()&&d!=points.end())add(b->second,d->second,c->second);}
        if(triangles.empty()){
            if(!rejectedLongEdges.empty()){double least=INFINITY;for(const auto& rejected:rejectedLongEdges)least=std::min(least,rejected.second);throw std::invalid_argument("局部层位三角边均超过上限 "+number(maxEdge)+"（坐标单位），最小所需边长 "+number(least)+"；未跨越源层位空白生成界面。");}
            throw std::invalid_argument("局部层位点不能围合成受支持的面；保留源层位空白，不生成界面。");
        }xmin=triangles[0].xmin;ymin=triangles[0].ymin;double xmax=triangles[0].xmax,ymax=triangles[0].ymax;for(const auto& t:triangles){xmin=std::min(xmin,t.xmin);ymin=std::min(ymin,t.ymin);xmax=std::max(xmax,t.xmax);ymax=std::max(ymax,t.ymax);}bins=std::clamp(int(std::sqrt(triangles.size())),1,512);cellX=std::max(1e-9,(xmax-xmin)/bins);cellY=std::max(1e-9,(ymax-ymin)/bins);for(size_t i=0;i<triangles.size();++i){const auto& t=triangles[i];for(int x=bx(t.xmin);x<=bx(t.xmax);++x)for(int y=by(t.ymin);y<=by(t.ymax);++y)buckets[{x,y}].push_back(i);}
    }
    double sampleAt(double x,double y)const{if(!finite(x)||!finite(y))return Missing;auto it=buckets.find({bx(x),by(y)});if(it==buckets.end())return Missing;double answer=Missing;for(size_t index:it->second){const auto& t=triangles[index];const auto& a=t.p[0];const auto& b=t.p[1];const auto& c=t.p[2];double det=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);double u=((b.y-c.y)*(x-c.x)+(c.x-b.x)*(y-c.y))/det,v=((c.y-a.y)*(x-c.x)+(a.x-c.x)*(y-c.y))/det,w=1-u-v;if(u>=-1e-8&&v>=-1e-8&&w>=-1e-8){double z=u*a.zTime+v*b.zTime+w*c.zTime;if(finite(answer)&&std::abs(answer-z)>1e-4)return Missing;answer=z;}}return answer;}
    SegmentRoots intersectSegment(const TrajectoryPosition& first,const TrajectoryPosition& last,double timeFirst,double timeLast,double scale,double offset)const{
        SegmentRoots out;std::set<size_t> candidates;
        int x0=std::max(0,bx(std::min(first.x,last.x))),x1=std::min(bins,bx(std::max(first.x,last.x)));
        int y0=std::max(0,by(std::min(first.y,last.y))),y1=std::min(bins,by(std::max(first.y,last.y)));
        for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y){auto it=buckets.find({x,y});if(it!=buckets.end())candidates.insert(it->second.begin(),it->second.end());}
        std::vector<std::pair<double,double>> coverage;
        for(size_t index:candidates){
            const auto& t=triangles[index];auto wa=triangleWeights(t,first.x,first.y),wb=triangleWeights(t,last.x,last.y);
            double low,high;if(!clipTriangleWeights(wa,wb,low,high))continue;coverage.push_back({low,high});out.anySupported=true;
            double sa=offset,sb=offset;for(size_t i=0;i<3;++i){sa+=wa[i]*t.p[i].zTime*scale;sb+=wb[i]*t.p[i].zTime*scale;}
            double fa=timeFirst-sa,fb=timeLast-sb,denominator=fa-fb;
            if(std::abs(denominator)<1e-10){if(std::abs(fa)<1e-8){if(high-low>1e-9)out.coincident=true;else out.fractions.push_back(.5*(low+high));}continue;}
            double root=fa/denominator;if(root>=low-1e-9&&root<=high+1e-9)out.fractions.push_back(std::clamp(root,0.,1.));
        }
        std::sort(coverage.begin(),coverage.end());double covered=0;for(auto [a,b]:coverage){if(a>covered+1e-8)break;covered=std::max(covered,b);}out.fullySupported=covered>=1-1e-8;
        return out;
    }
    double leastRejectedEdgeAlong(const std::vector<TrajectoryPosition>& positions,double top,double bottom)const{
        double result=INFINITY;
        for(const auto& [triangle,edge]:rejectedLongEdges){
            if(edge>=result)continue;
            for(size_t i=1;i<positions.size();++i){const double a=std::max(top,positions[i-1].md),b=std::min(bottom,positions[i].md);if(b<=a)continue;
                const auto first=positionAt(positions,a),last=positionAt(positions,b);
                if(std::max(first.x,last.x)<triangle.xmin||std::min(first.x,last.x)>triangle.xmax||std::max(first.y,last.y)<triangle.ymin||std::min(first.y,last.y)>triangle.ymax)continue;
                double low,high;if(clipTriangleWeights(triangleWeights(triangle,first.x,first.y),triangleWeights(triangle,last.x,last.y),low,high)){result=edge;break;}
            }
        }
        return finite(result)?result:Missing;
    }
};
}
std::vector<TrajectoryPosition> buildTrajectory(const CorrelationWellInput& well){
    if(!well.positions.empty()){auto p=well.positions;checkPositions(p);return p;}
    std::vector<TrajectoryPosition> positions;const auto& td=well.timeDepth;
    if(td.depthInfo.depthType=="MD")for(const auto& point:td.depthTimePairs)if(finite(point.depthValue)&&finite(point.x)&&finite(point.y)){TrajectoryPosition p;p.md=metres(point.depthValue,td.depthInfo.depthUnit);p.x=point.x;p.y=point.y;if(finite(point.z)){const auto& info=td.positionDepthInfo;if(info.depthType=="TVD")p.tvd=metres(point.z,info.depthUnit);if(info.depthType=="TVDSS"&&(info.tvdssDirection=="up"||info.tvdssDirection=="down"))p.tvdss=metres(point.z,info.depthUnit)*(info.tvdssDirection=="up"?-1.:1.);}positions.push_back(p);}if(positions.size()>=2){checkPositions(positions);return positions;}
    if(!well.survey.surveyPoints.empty()){
        if(unitToken(well.well.coordinateUnit)!="m"||!finite(well.well.wellheadX)||!finite(well.well.wellheadY))throw std::invalid_argument("Minimum-curvature trajectory requires a confirmed wellhead in metres");if(well.survey.azimuthReference!="grid_north"&&well.survey.azimuthReference!="grid"&&well.survey.azimuthReference!="网格北")throw std::invalid_argument("Survey azimuth must be confirmed relative to grid north before XY integration");auto survey=well.survey.surveyPoints;std::sort(survey.begin(),survey.end(),[](auto a,auto b){return a.measuredDepth<b.measuredDepth;});if(survey.size()<2||std::abs(metres(survey.front().measuredDepth,well.surveyDepthInfo.depthUnit))>1e-6)throw std::invalid_argument("Survey needs an explicit MD=0 position; no unmeasured shallow trajectory is assumed");for(size_t i=0;i<survey.size();++i)if(!finite(survey[i].inclinationDegrees)||!finite(survey[i].azimuthDegrees)||survey[i].inclinationDegrees<0||survey[i].inclinationDegrees>180||!finite(survey[i].measuredDepth)||i&&survey[i].measuredDepth<=survey[i-1].measuredDepth)throw std::invalid_argument("Invalid survey sample");TrajectoryPosition last;last.md=0;last.x=well.well.wellheadX;last.y=well.well.wellheadY;last.tvd=0;positions.push_back(last);
        for(size_t i=1;i<survey.size();++i){double i1=survey[i-1].inclinationDegrees*std::numbers::pi/180.,i2=survey[i].inclinationDegrees*std::numbers::pi/180.,a1=survey[i-1].azimuthDegrees*std::numbers::pi/180.,a2=survey[i].azimuthDegrees*std::numbers::pi/180.;double md=metres(survey[i].measuredDepth,well.surveyDepthInfo.depthUnit),ds=md-last.md;double dogleg=std::acos(std::clamp(std::cos(i1)*std::cos(i2)+std::sin(i1)*std::sin(i2)*std::cos(a2-a1),-1.,1.));if(dogleg>=std::numbers::pi-1e-6)throw std::invalid_argument("Survey dogleg approaches 180 degrees");double rf=dogleg<1e-8?1.:2.*std::tan(dogleg/2)/dogleg;last.md=md;last.x+=.5*ds*rf*(std::sin(i1)*std::sin(a1)+std::sin(i2)*std::sin(a2));last.y+=.5*ds*rf*(std::sin(i1)*std::cos(a1)+std::sin(i2)*std::cos(a2));last.tvd+=.5*ds*rf*(std::cos(i1)+std::cos(i2));positions.push_back(last);}return positions;
    }
    if(well.confirmedVertical&&finite(well.well.wellheadX)&&finite(well.well.wellheadY)){checkAxis(well.log);for(double z:{well.log.axis.front(),well.log.axis.back()})positions.push_back({z,well.well.wellheadX,well.well.wellheadY,Missing,Missing});return positions;}
    throw std::invalid_argument("No confirmed trajectory: supply XYZ-MD checkshots, positions, survey, or explicitly confirm a vertical well");
}
HorizonIntersectionResult intersectHorizons(const CorrelationWellInput& original,const std::vector<CorrelationHorizon>& horizons,const GeometryOptions& o){
    // Work only on a calculation copy. Explicit confirmations never rewrite the
    // captured source metadata, source arrays, or caller's input snapshot.
    CorrelationWellInput well=original;
    fillConfirmed(well.log.axisUnit,o.confirmedDepthUnit,"Log depth unit");
    fillConfirmed(well.depthInfo.depthUnit,o.confirmedDepthUnit,"Well depth unit");
    fillConfirmed(well.log.axisKind,o.confirmedLogDepthType,"Log depth basis");
    if(well.log.axisKind.empty()&&o.confirmedDepthType=="MD")well.log.axisKind="MD";
    fillConfirmed(well.timeDepth.depthInfo.depthUnit,o.confirmedDepthUnit,"Time-depth depth unit");
    fillConfirmed(well.timeDepth.depthInfo.depthType,o.confirmedDepthType,"Time-depth depth basis");
    fillConfirmed(well.timeDepth.timeUnit,o.confirmedWellTimeUnit,"Well time unit");
    fillConfirmed(well.timeDepth.timeType,o.confirmedWellTimeType,"Well time type");
    fillConfirmed(well.surveyDepthInfo.depthUnit,o.confirmedDepthUnit,"Survey depth unit");
    if(well.log.axisUnit=="ft"){for(auto& d:well.log.axis)d*=.3048;well.log.axisUnit="m";}
    if(std::abs(o.horizonTimeSign)!=1||std::abs(o.wellTimeSign)!=1||!finite(o.horizonTimeOffsetMs)||!finite(o.wellTimeOffsetMs)||!finite(o.searchStepMd)||o.searchStepMd<=0||!finite(o.intersectionToleranceMd)||o.intersectionToleranceMd<=0)throw std::invalid_argument("Invalid explicit geometry sign, offset or search precision");checkAxis(well.log);const auto& td=well.timeDepth;double tdScale=timeScale(td.timeUnit,td.timeType,o.allowOneWayConversion)*o.wellTimeSign;auto positions=buildTrajectory(well);std::string xyCrs=well.well.coordinateSystem,xyUnit=well.well.coordinateUnit;bool checkshotPositions=well.positions.empty()&&td.depthInfo.depthType=="MD"&&std::count_if(td.depthTimePairs.begin(),td.depthTimePairs.end(),[](auto p){return finite(p.x)&&finite(p.y)&&finite(p.depthValue);})>=2;if(checkshotPositions){xyCrs=td.coordinateSystem;xyUnit=td.coordinateUnit;}
    if(td.depthInfo.depthType=="MD")confirmedMatch(well.depthInfo.mdReference,td.depthInfo.mdReference,o.confirmedMdReference,"MD reference");else if(td.depthInfo.depthType=="TVD")confirmedMatch(well.depthInfo.tvdReference,td.depthInfo.tvdReference,o.confirmedVerticalDepthReference,"TVD reference");else if(td.depthInfo.depthType=="TVDSS")confirmedMatch(well.depthInfo.tvdssReference,td.depthInfo.tvdssReference,o.confirmedVerticalDepthReference,"TVDSS reference");std::vector<std::pair<double,double>> times;for(const auto& p:td.depthTimePairs){if(!finite(p.depthValue)||!finite(p.timeValue))continue;double md=Missing;if(td.depthInfo.depthType=="MD")md=metres(p.depthValue,td.depthInfo.depthUnit);else if(td.depthInfo.depthType=="TVD"||td.depthInfo.depthType=="TVDSS"){std::vector<std::pair<double,double>> inverse;for(const auto& position:positions){double z=td.depthInfo.depthType=="TVD"?position.tvd:position.tvdss;if(finite(z))inverse.push_back({z,position.md});}if(inverse.size()<2)throw std::invalid_argument("Time-depth vertical coordinate requires a matching trajectory transform");for(size_t i=1;i<inverse.size();++i)if(inverse[i].first<=inverse[i-1].first)throw std::invalid_argument("TVD-to-MD is ambiguous or nonmonotonic");double z=metres(p.depthValue,td.depthInfo.depthUnit);if(td.depthInfo.depthType=="TVDSS"){if(td.depthInfo.tvdssDirection!="up"&&td.depthInfo.tvdssDirection!="down")throw std::invalid_argument("TVDSS direction must be explicit");if(td.depthInfo.tvdssDirection=="up")z=-z;}md=interpolate(inverse,z);}else throw std::invalid_argument("Unknown time-depth depth basis");if(finite(md))times.push_back({md,p.timeValue*tdScale+o.wellTimeOffsetMs});}// For a TVD/TVDSS table, TWT is linear in its declared vertical coordinate,
    // not generally in MD. Include trajectory knots in the composed transform.
    if(td.depthInfo.depthType=="TVD"||td.depthInfo.depthType=="TVDSS"){
        std::vector<std::pair<double,double>> verticalTimes;
        for(const auto& point:td.depthTimePairs)if(finite(point.depthValue)&&finite(point.timeValue)){
            double z=metres(point.depthValue,td.depthInfo.depthUnit);
            if(td.depthInfo.depthType=="TVDSS"&&td.depthInfo.tvdssDirection=="up")z=-z;
            verticalTimes.push_back({z,point.timeValue*tdScale+o.wellTimeOffsetMs});
        }
        std::sort(verticalTimes.begin(),verticalTimes.end());
        for(size_t i=1;i<verticalTimes.size();++i)if(verticalTimes[i].first<=verticalTimes[i-1].first)throw std::invalid_argument("Duplicate time-depth vertical coordinates require resolution");
        for(const auto& position:positions){double z=td.depthInfo.depthType=="TVD"?position.tvd:position.tvdss;if(finite(z)){double time=interpolate(verticalTimes,z);if(finite(time))times.push_back({position.md,time});}}
    }
    std::sort(times.begin(),times.end());
    std::vector<std::pair<double,double>> uniqueTimes;
    for(auto pair:times){if(!uniqueTimes.empty()&&std::abs(pair.first-uniqueTimes.back().first)<1e-7){if(std::abs(pair.second-uniqueTimes.back().second)>1e-6)throw std::invalid_argument("Conflicting time values at one MD");}else uniqueTimes.push_back(pair);}
    times=std::move(uniqueTimes);
    if(times.size()<2)throw std::invalid_argument("At least two usable time-depth samples required");for(size_t i=1;i<times.size();++i)if(times[i].first<=times[i-1].first||times[i].second<=times[i-1].second)throw std::invalid_argument("Time-depth relationship must increase in MD and canonical positive TWT; confirm negative-TWT sign explicitly");
    HorizonIntersectionResult out;auto extension=extendTimeDepth(times,positions,well.log,td.depthInfo.depthType,xyUnit.empty()?o.confirmedCoordinateUnit:xyUnit,o,out.warnings);
    double lo=extension.calculationTop,hi=extension.calculationBottom;if(hi<=lo)throw std::invalid_argument("No common supported trajectory, time-depth and log interval");
    for(const auto& h:horizons){auto surface=h.surface;
        const bool anyPoint=std::any_of(surface.points.begin(),surface.points.end(),[](const auto& p){return finite(p.x)&&finite(p.y)&&finite(p.zTime);});
        const bool infinitePoint=std::any_of(surface.points.begin(),surface.points.end(),[](const auto& p){return std::isinf(p.x)||std::isinf(p.y)||std::isinf(p.zTime);});
        if(!anyPoint&&!infinitePoint){out.warnings.push_back(h.role+"：局部无有效层位点（X/Y/时间），无法取得该界面；其他有支持的层位继续计算。");continue;}
        fillConfirmed(surface.timeUnit,o.confirmedHorizonTimeUnit,"Horizon time unit");fillConfirmed(surface.timeType,o.confirmedHorizonTimeType,"Horizon time type");confirmedMatch(xyCrs,surface.coordinateSystem,o.confirmedCoordinateSystem,"Coordinate system");confirmedMatch(xyUnit,surface.coordinateUnit,o.confirmedCoordinateUnit,"Coordinate unit");confirmedMatch(td.timeReference,surface.timeReference,o.confirmedTimeReference,"Time reference");double scale=timeScale(surface.timeUnit,surface.timeType,o.allowOneWayConversion)*o.horizonTimeSign;
        auto sampler=[&](){try{return SurfaceSampler(surface,o.maximumSurfaceEdge,o.allowIrregularSurface);}catch(const std::invalid_argument& e){throw std::invalid_argument(h.role+"："+e.what());}}();
        // Each well/TD interval and every supported horizon triangle is linear.
        // Intersect those pieces exactly, so a narrow double crossing cannot be
        // missed by an arbitrary fixed MD scan step and holes are never bridged.
        std::vector<double> knots{lo,hi};
        for(const auto& p:positions)if(p.md>lo&&p.md<hi)knots.push_back(p.md);
        for(const auto& p:times)if(p.first>lo&&p.first<hi)knots.push_back(p.first);
        std::sort(knots.begin(),knots.end());knots.erase(std::unique(knots.begin(),knots.end()),knots.end());
        std::vector<double> roots;size_t unsupported=0;bool coincident=false,anySupported=false;
        for(size_t i=1;i<knots.size();++i){
            double a=knots[i-1],b=knots[i];auto first=positionAt(positions,a),last=positionAt(positions,b);
            auto piece=sampler.intersectSegment(first,last,interpolate(times,a),interpolate(times,b),scale,o.horizonTimeOffsetMs);
            if(!piece.fullySupported)++unsupported;coincident=coincident||piece.coincident;anySupported=anySupported||piece.anySupported;
            for(double fraction:piece.fractions)roots.push_back(a+(b-a)*fraction);
        }
        if(coincident){roots.clear();out.warnings.push_back(h.role+": the trajectory lies on a horizon segment; no unique intersection can be adopted");}
        std::sort(roots.begin(),roots.end());roots.erase(std::unique(roots.begin(),roots.end(),[&](double a,double b){return std::abs(a-b)<=std::max(1e-8,1e-10*(hi-lo));}),roots.end());
        if(roots.empty()&&!coincident){
            const double rejectedEdge=sampler.leastRejectedEdgeAlong(positions,lo,hi);
            const std::string unit=xyUnit.empty()?o.confirmedCoordinateUnit:xyUnit;
            if(!anySupported){
                if(finite(rejectedEdge))out.warnings.push_back(h.role+"：井轨迹处为层位支持空白，围合需跨越长边（沿路径的围合三角至少需要边长 "+number(rejectedEdge)+" "+unit+"，当前上限 "+number(o.maximumSurfaceEdge)+" "+unit+"）；未跨空白补出界面。");
                else out.warnings.push_back(h.role+"：局部有层位点，但井轨迹处不能围合成受支持的面（位于覆盖外或源层位空白）；未进行空间外推。");
            }else{
                std::string message=h.role+(unsupported?"：部分井轨迹有层位空间支持，但空间覆盖不完整；受支持部分":"：已有层位空间支持，但");
                message+="在当前时深与曲线允许范围内没有交点（MD "+number(lo)+"–"+number(hi)+" m，有效 GR "+number(extension.logTop)+"–"+number(extension.logBottom)+" m，上/下端余量 "+number(o.allowTopExtrapolation?o.maximumLogExtrapolationM:0.)+"/"+number(o.allowBottomExtrapolation?o.maximumLogExtrapolationM:0.)+" m）。";
                if(finite(rejectedEdge))message+="其余路径包含超长三角被拒的源层位空白（围合至少需要边长 "+number(rejectedEdge)+" "+unit+"，上限 "+number(o.maximumSurfaceEdge)+" "+unit+"）。";
                out.warnings.push_back(std::move(message));
            }
        }
        if(roots.size()>1)out.warnings.push_back(h.role+": multiple intersections; manual selection is required");
        if(unsupported&&!roots.empty())out.warnings.push_back(h.role+"：部分井轨迹位于层位支持范围外；仅保留有空间支持的交点，未跨空白补齐。");
        for(size_t i=0;i<roots.size();++i){InterpretationBoundary b;b.id="horizon-"+std::to_string(h.resourceId)+"-w"+std::to_string(well.wellId)+"-"+std::to_string(i);b.name=h.role;b.role=h.role;b.md=b.originalMd=roots[i];b.time=interpolate(times,b.md);b.source="seismic_horizon";b.status="review";auto xy=positionAt(positions,b.md);b.evidenceJson="{\"method\":\"piecewise_linear_trajectory_triangle_intersection/v2\",\"horizonResourceId\":"+std::to_string(h.resourceId)+",\"horizonFingerprint\":"+quote(h.inputFingerprint)+",\"inputFingerprint\":"+quote(well.inputFingerprint)+",\"originalMd\":"+number(b.md)+",\"x\":"+number(xy.x)+",\"y\":"+number(xy.y)+",\"canonicalTwtMs\":"+number(b.time)+",\"horizonTimeSign\":"+number(o.horizonTimeSign)+",\"wellTimeSign\":"+number(o.wellTimeSign)+",\"horizonTimeOffsetMs\":"+number(o.horizonTimeOffsetMs)+",\"wellTimeOffsetMs\":"+number(o.wellTimeOffsetMs)+",\"coordinateSystem\":"+quote(xyCrs.empty()?o.confirmedCoordinateSystem:xyCrs)+",\"coordinateUnit\":"+quote(xyUnit.empty()?o.confirmedCoordinateUnit:xyUnit)+",\"timeReference\":"+quote(td.timeReference.empty()?o.confirmedTimeReference:td.timeReference)+",\"multipleIntersections\":"+(roots.size()>1?"true":"false")+'}';b.evidenceJson.pop_back();b.evidenceJson+=",\"surfaceInterpolation\":"+quote(o.allowIrregularSurface?"delaunay_max_edge/v1":"adjacent_grid/v2")+",\"maximumSurfaceEdge\":"+(finite(o.maximumSurfaceEdge)?number(o.maximumSurfaceEdge):"null");b.evidenceJson+=",\"timeDepthExtrapolation\":"+extension.json(b.md);if(extension.applied(b.md))out.warnings.push_back(h.role+"：界面采用 "+extension.side(b.md)+" 端外推/曲线外候选（时深外推 "+number(TimeDepthExtension::distance(b.md,extension.timeTop,extension.timeBottom))+" m，轨迹外推 "+number(TimeDepthExtension::distance(b.md,extension.xyTop,extension.xyBottom))+" m，距有效 GR 范围 "+number(TimeDepthExtension::distance(b.md,extension.logTop,extension.logBottom))+" m），结果待复核。");b.evidenceJson+=",\"explicitConfirmations\":"+confirmations(o)+",\"sourceWellTimeUnit\":"+quote(original.timeDepth.timeUnit)+",\"sourceHorizonTimeUnit\":"+quote(h.surface.timeUnit)+"}";out.boundaries.push_back(b);}
    }return out;
}
}
