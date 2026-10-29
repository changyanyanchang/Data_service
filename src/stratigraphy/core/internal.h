#pragma once
#include "paleo_stratigraphy/core.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace paleo::stratigraphy::detail {
inline bool finite(double x){return std::isfinite(x);}
// Only known aliases are folded. CRS identifiers and datums remain exact.
inline std::string unitToken(std::string v){for(char& c:v)c=char(std::tolower(static_cast<unsigned char>(c)));if(v=="metre"||v=="meter"||v=="metres"||v=="meters")return "m";if(v=="foot"||v=="feet")return "ft";if(v=="msec"||v=="millisecond"||v=="milliseconds")return "ms";if(v=="sec"||v=="second"||v=="seconds")return "s";return v;}
inline std::string axisToken(std::string v){for(char& c:v)c=char(std::toupper(static_cast<unsigned char>(c)));return v;}
inline std::string metadataToken(const std::string& value,const std::string& label){if(label.find("unit")!=std::string::npos)return unitToken(value);if(label.find("basis")!=std::string::npos||label.find("type")!=std::string::npos)return axisToken(value);return value;}
inline std::string number(double x){std::ostringstream s;s<<std::setprecision(17)<<x;return s.str();}
inline std::string quote(const std::string& x){std::string r="\"";for(unsigned char c:x){if(c=='"'||c=='\\'){r+='\\';r+=char(c);}else if(c=='\n')r+="\\n";else if(c>=32)r+=char(c);}return r+'"';}
inline double median(std::vector<double> v){v.erase(std::remove_if(v.begin(),v.end(),[](double x){return !finite(x);}),v.end());if(v.empty())return Missing;std::sort(v.begin(),v.end());return v.size()%2?v[v.size()/2]:.5*(v[v.size()/2-1]+v[v.size()/2]);}
inline void checkAxis(const InterpretationSeries& s){if(axisToken(s.axisKind)!="MD"||unitToken(s.axisUnit)!="m")throw std::invalid_argument("Curve calculation requires an explicitly confirmed MD axis in metres");if(s.axis.size()!=s.values.size()||s.axis.size()<2)throw std::invalid_argument("Curve axis/value size mismatch or fewer than two samples");if(!s.quality.empty()&&s.quality.size()!=s.axis.size())throw std::invalid_argument("Curve quality size mismatch");for(size_t i=0;i<s.axis.size();++i)if(!finite(s.axis[i])||(i&&s.axis[i]<=s.axis[i-1]))throw std::invalid_argument("Curve MD must be finite and strictly increasing; duplicate depths require resolution");}
inline bool valid(const InterpretationSeries& s,size_t i){return finite(s.values[i])&&(s.quality.empty()||(s.quality[i]&(1|4))==0);}
inline double sample(const InterpretationSeries& s,double md,double maxGap){auto it=std::lower_bound(s.axis.begin(),s.axis.end(),md);if(it==s.axis.end())return Missing;size_t i=it-s.axis.begin();if(std::abs(*it-md)<1e-9)return valid(s,i)?s.values[i]:Missing;if(i==0||!valid(s,i-1)||!valid(s,i)||s.axis[i]-s.axis[i-1]>maxGap)return Missing;double t=(md-s.axis[i-1])/(s.axis[i]-s.axis[i-1]);return s.values[i-1]+t*(s.values[i]-s.values[i-1]);}
inline std::pair<std::string,std::string> endpoints(const std::string& segment){if(segment=="E12")return {"T70","T72"};if(segment=="E3")return {"T72","T73"};if(segment=="E4")return {"T73","T80"};throw std::invalid_argument("Unknown Enping segment: "+segment);}
inline const InterpretationBoundary& named(const WellInterpretation& w,const std::string& name){const InterpretationBoundary* b=nullptr;for(const auto& row:w.boundaries)if(row.name==name||row.role==name){if(b&&b!=&row)throw std::invalid_argument("Ambiguous named boundary: "+name);b=&row;}if(!b||!finite(b->md))throw std::invalid_argument("Missing named boundary: "+name);return *b;}
inline const InterpretationBoundary& boundary(const WellInterpretation& w,const std::string& id){auto it=std::find_if(w.boundaries.begin(),w.boundaries.end(),[&](const auto& b){return b.id==id;});if(it==w.boundaries.end())throw std::invalid_argument("Missing boundary object: "+id);return *it;}
inline std::string key(const std::string& value){unsigned long long hash=1469598103934665603ull;for(unsigned char c:value){hash^=c;hash*=1099511628211ull;}std::ostringstream s;s<<std::hex<<hash;return s.str();}
inline void checkProfile(const CorrelationAlgorithmProfile& p){if(!finite(p.sampleStep)||p.sampleStep<=0||!finite(p.maxGap)||p.maxGap<=0||!finite(p.padding)||p.padding<0||!finite(p.minimumThickness)||p.minimumThickness<=0)throw std::invalid_argument("Invalid algorithm spacing, gap, padding or minimum thickness");if(p.arOrders.empty())throw std::invalid_argument("At least one AR order is required");for(int n:p.arOrders)if(n<1||n>4096)throw std::invalid_argument("AR order must be between 1 and 4096 samples");if(p.direction!="forward"&&p.direction!="reverse"&&p.direction!="bidirectional")throw std::invalid_argument("Unknown PACE accumulation direction");for(double weight:{p.paceWeight,p.residualWeight,p.phaseWeight,p.referenceWeight})if(!finite(weight)||weight<0)throw std::invalid_argument("Algorithm weights must be finite and nonnegative");}
}
