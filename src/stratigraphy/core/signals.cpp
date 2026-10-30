#include "internal.h"
#include <complex>
#include <numbers>

namespace paleo::stratigraphy {
using namespace detail;
namespace {
using Complex=std::complex<double>;
void radix(std::vector<Complex>& a,bool inverse){
    const size_t n=a.size();for(size_t i=1,j=0;i<n;++i){size_t bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(a[i],a[j]);}
    for(size_t len=2;len<=n;len<<=1){Complex step=std::polar(1.,(inverse?2.:-2.)*std::numbers::pi/double(len));for(size_t i=0;i<n;i+=len){Complex w=1.;for(size_t j=0;j<len/2;++j){Complex u=a[i+j],v=a[i+j+len/2]*w;a[i+j]=u+v;a[i+j+len/2]=u-v;w*=step;}}}
    if(inverse)for(auto& x:a)x/=double(n);
}
// Bluestein preserves the original sample count, including non-power-of-two logs.
std::vector<Complex> transform(const std::vector<Complex>& a,bool inverse=false){
    size_t n=a.size();if(!n)return {};if(inverse){auto c=a;for(auto& z:c)z=std::conj(z);c=transform(c);for(auto& z:c)z=std::conj(z)/double(n);return c;}
    if((n&(n-1))==0){auto c=a;radix(c,false);return c;}
    size_t m=1;while(m<2*n-1)m<<=1;std::vector<Complex> x(m),y(m);
    for(size_t k=0;k<n;++k){double angle=std::numbers::pi*double((static_cast<unsigned long long>(k)*k)%(2*n))/double(n);Complex c=std::polar(1.,angle);x[k]=a[k]*std::conj(c);y[k]=c;if(k)y[m-k]=c;}
    radix(x,false);radix(y,false);for(size_t i=0;i<m;++i)x[i]*=y[i];radix(x,true);x.resize(n);
    for(size_t k=0;k<n;++k){double angle=std::numbers::pi*double((static_cast<unsigned long long>(k)*k)%(2*n))/double(n);x[k]*=std::polar(1.,-angle);}return x;
}
std::vector<double> forwardPace(const std::vector<double>& d,int requested){
    const size_t n=d.size();std::vector<double> result(n,0.);if(n<3)return result;int order=std::min(requested,int(n)-2);
    double mean=std::accumulate(d.begin(),d.end(),0.)/double(n);std::vector<double> x=d;for(auto& v:x)v-=mean;
    std::vector<double> a{1.},f=x,b=x;
    for(int j=0;j<order;++j){double numerator=0,denominator=0;for(size_t i=1;i<f.size();++i){numerator+=f[i]*b[i-1];denominator+=f[i]*f[i]+b[i-1]*b[i-1];}double mu=denominator>0?std::clamp(-2.*numerator/denominator,-.999999,.999999):0.;auto old=a;a.push_back(mu);for(size_t i=1;i<old.size();++i)a[i]=old[i]+mu*old[old.size()-i];std::vector<double> nf(f.size()-1),nb(b.size()-1);for(size_t i=0;i<nf.size();++i){nf[i]=f[i+1]+mu*b[i];nb[i]=b[i]+mu*f[i+1];}f=std::move(nf);b=std::move(nb);}
    double total=0.;for(size_t i=order;i<n;++i){double error=0;for(size_t j=0;j<a.size();++j)error+=a[j]*x[i-j];total+=error;result[i]=total;}return result;
}
std::string profileJson(const CorrelationAlgorithmProfile& p,double top,double bottom){
    std::string o="[";for(int x:p.arOrders){if(o.size()>1)o+=',';o+=std::to_string(x);}o+=']';
    return "{\"algorithmVersion\":"+quote(p.version)+",\"profileId\":"+quote(p.id)+",\"sampleMode\":"+quote(p.resample?"uniform_md":"native_samples")+",\"stepM\":"+number(p.sampleStep)+",\"maxGapM\":"+number(p.maxGap)+",\"paddingM\":"+number(p.padding)+",\"arOrderUnit\":\"samples\",\"arOrders\":"+o+",\"direction\":"+quote(p.direction)+",\"phaseConvention\":"+quote(p.invertPhase?"negative_cosine":"positive_cosine")+",\"normalization\":"+quote(p.normalize?"continuous_compute_segment_minmax_01":"none")+",\"displayTopMd\":"+number(top)+",\"displayBottomMd\":"+number(bottom)+",\"warmupPolicy\":\"mask_initialization_samples\"}";
}
}
std::vector<double> burgPace(const std::vector<double>& values,int order,const std::string& direction){
    if(order<1||order>4096)throw std::invalid_argument("AR order outside [1,4096]");for(double x:values)if(!finite(x))throw std::invalid_argument("Burg input must be one finite continuous segment");
    if(direction=="forward")return forwardPace(values,order);auto r=values;std::reverse(r.begin(),r.end());r=forwardPace(r,order);std::reverse(r.begin(),r.end());if(direction=="reverse")return r;if(direction=="bidirectional"){auto f=forwardPace(values,order);for(size_t i=0;i<r.size();++i)r[i]=.5*(r[i]+f[i]);return r;}throw std::invalid_argument("Unknown PACE direction");
}
std::vector<double> cosinePhase(const std::vector<double>& values,bool invert){
    if(values.empty())return {};double mean=0;for(double x:values){if(!finite(x))throw std::invalid_argument("Hilbert input must be one finite continuous segment");mean+=x;}mean/=double(values.size());std::vector<Complex> signal;double energy=0;for(double x:values){signal.emplace_back(x-mean,0);energy+=(x-mean)*(x-mean);}if(energy<1e-24)return std::vector<double>(values.size(),Missing);
    auto spectrum=transform(signal);const size_t n=spectrum.size();for(size_t k=1;k<n;++k){if(k<(n+1)/2)spectrum[k]*=2.;else if(!(n%2==0&&k==n/2))spectrum[k]=0.;}auto analytic=transform(spectrum,true);std::vector<double> out(n,Missing);for(size_t i=0;i<n;++i){double amp=std::abs(analytic[i]);if(amp>1e-12)out[i]=(invert?-1.:1.)*std::clamp(analytic[i].real()/amp,-1.,1.);}return out;
}
DerivedResult deriveCurves(const InterpretationSeries& input,const CorrelationAlgorithmProfile& p,double top,double bottom){
    checkAxis(input);checkProfile(p);if(!finite(top)||!finite(bottom)||bottom<=top)throw std::invalid_argument("Invalid interpretation interval");
    double first=std::max(input.axis.front(),top-p.padding),last=std::min(input.axis.back(),bottom+p.padding);if(last<=first)throw std::invalid_argument("Interpretation interval is outside the source log");
    std::vector<double> axis,gr;std::vector<unsigned char> inputMask;
    if(p.resample){double origin=std::ceil(first/p.sampleStep)*p.sampleStep;size_t count=size_t(std::floor((last-origin)/p.sampleStep+1e-8))+1;if(count>5000000)throw std::invalid_argument("Requested derived grid exceeds five million samples");for(size_t j=0;j<count;++j){double z=origin+j*p.sampleStep;axis.push_back(z);auto it=std::lower_bound(input.axis.begin(),input.axis.end(),z);size_t k=it-input.axis.begin();if(k<input.axis.size()&&std::abs(input.axis[k]-z)<1e-8){gr.push_back(valid(input,k)?input.values[k]:Missing);inputMask.push_back(valid(input,k)?(input.quality.empty()?0:input.quality[k]):1);}else{double v=sample(input,z,p.maxGap);gr.push_back(v);inputMask.push_back(finite(v)?2:1);}}}
    else for(size_t i=0;i<input.axis.size();++i)if(input.axis[i]>=first&&input.axis[i]<=last){axis.push_back(input.axis[i]);gr.push_back(valid(input,i)?input.values[i]:Missing);inputMask.push_back(valid(input,i)?(input.quality.empty()?0:input.quality[i]):1);}
    if(axis.size()<2)throw std::invalid_argument("Too few observed samples inside interval");
    std::vector<double> deltas;for(size_t i=1;i<axis.size();++i)deltas.push_back(axis[i]-axis[i-1]);double nativeStep=median(deltas);
    std::vector<std::pair<size_t,size_t>> runs;size_t begin=0;while(begin<axis.size()){while(begin<axis.size()&&!finite(gr[begin]))++begin;if(begin==axis.size())break;size_t end=begin+1;while(end<axis.size()&&finite(gr[end])&&axis[end]-axis[end-1]<=p.maxGap&&(p.resample||(axis[end]-axis[end-1]>=.5*nativeStep&&axis[end]-axis[end-1]<=1.5*nativeStep)))++end;runs.emplace_back(begin,end);begin=end;}
    const auto params=profileJson(p,top,bottom);DerivedResult result;
    auto make=[&](const std::string& id,const std::string& kind,const std::string& name,const std::string& unit){InterpretationSeries s;s.id=id;s.kind=kind;s.name=name;s.unit=unit;s.axis=axis;s.values.assign(axis.size(),Missing);s.quality.assign(axis.size(),1);s.parametersJson=params;s.inputFingerprint=input.inputFingerprint;s.runId="derived-"+key(input.id+input.inputFingerprint+params);return s;};
    std::vector<int> orders=p.arOrders;std::sort(orders.begin(),orders.end());orders.erase(std::unique(orders.begin(),orders.end()),orders.end());
    auto residual=make("FISCHER_GR_RESIDUAL","FISCHER_GR_RESIDUAL","GR residual cumulative",p.normalize?"1":"cumulative("+input.unit+")");
    std::vector<InterpretationSeries> paces,phases;for(int o:orders){paces.push_back(make("PACE"+std::to_string(o),"PACE","PACE AR "+std::to_string(o)+" samples",p.normalize?"1":"cumulative("+input.unit+")"));phases.push_back(make("INCOSIN"+std::to_string(o),"COSINE_PHASE","PACE cosine phase AR "+std::to_string(o),"1"));}
    std::string segments="[";size_t usableRuns=0;
    for(auto [a,b]:runs){if(b-a<8){result.warnings.push_back("Continuous segment has fewer than eight valid samples at MD "+number(axis[a]));continue;}++usableRuns;if(segments.size()>1)segments+=',';segments+="{\"topMd\":"+number(axis[a])+",\"bottomMd\":"+number(axis[b-1])+",\"samples\":"+std::to_string(b-a)+",\"actualArOrders\":[";std::vector<double> local(gr.begin()+a,gr.begin()+b);double mean=std::accumulate(local.begin(),local.end(),0.)/local.size(),sum=0;std::vector<double> f(local.size());for(size_t i=local.size();i-->0;){sum+=local[i]-mean;f[i]=sum;}
        auto store=[&](InterpretationSeries& s,const std::vector<double>& values,size_t offset,bool normalize){double lo=std::numeric_limits<double>::infinity(),hi=-lo;for(double v:values)if(finite(v)){lo=std::min(lo,v);hi=std::max(hi,v);}for(size_t j=0;j<values.size();++j)if(finite(values[j])){s.values[offset+j]=normalize?(hi>lo?(values[j]-lo)/(hi-lo):0.):values[j];s.quality[offset+j]=inputMask[offset+j];}};store(residual,f,a,p.normalize);
        for(size_t j=0;j<orders.size();++j){int actual=std::min(orders[j],int(local.size())-2);if(j)segments+=',';segments+=std::to_string(actual);auto v=burgPace(local,actual,p.direction);size_t l=p.direction=="reverse"?0:size_t(actual),r=local.size()-(p.direction=="forward"?0:size_t(actual));if(r<=l){for(size_t i=a;i<b;++i)paces[j].quality[i]=phases[j].quality[i]=4;continue;}std::vector<double> active(v.begin()+l,v.begin()+r);store(paces[j],active,a+l,p.normalize);for(size_t i=0;i<l;++i)paces[j].quality[a+i]=phases[j].quality[a+i]=4;for(size_t i=r;i<local.size();++i)paces[j].quality[a+i]=phases[j].quality[a+i]=4;if(active.size()>=8)store(phases[j],cosinePhase(active,p.invertPhase),a+l,false);}
        segments+="]}";
    }segments+=']';if(!usableRuns)result.warnings.push_back("No continuous interval is long enough for derived curves");
    auto medianPhase=make("INCOSIN","COSINE_PHASE_MEDIAN","Median cosine phase across AR orders","1");for(size_t i=0;i<axis.size();++i){std::vector<double> v;for(const auto& s:phases)if(valid(s,i))v.push_back(s.values[i]);if(!v.empty()){medianPhase.values[i]=median(v);medianPhase.quality[i]=inputMask[i];}}
    result.series=std::move(paces);result.series.push_back(std::move(residual));for(auto& s:phases)result.series.push_back(std::move(s));result.series.push_back(std::move(medianPhase));
    for(auto& s:result.series){s.parametersJson.pop_back();s.parametersJson+=",\"computeTopMd\":"+number(axis.front())+",\"computeBottomMd\":"+number(axis.back())+",\"continuousSegments\":"+segments+'}';std::vector<double> z,v;std::vector<unsigned char> q;for(size_t i=0;i<s.axis.size();++i)if(s.axis[i]>=top-1e-8&&s.axis[i]<=bottom+1e-8){z.push_back(s.axis[i]);v.push_back(s.values[i]);q.push_back(s.quality[i]);}s.axis=std::move(z);s.values=std::move(v);s.quality=std::move(q);}
    if(top<input.axis.front()||bottom>input.axis.back())result.warnings.push_back("Requested interval is only partially covered by source observations");return result;
}
}
