#include "paleo_data/service.h"
#include "segy_reader.h"
#include <algorithm>
#include <climits>
#include <cmath>

namespace paleo::data {
bool DataService::readSeismicGridSection(long long id,const std::vector<std::pair<int,int>>& path,
    int columns,bool interpolate,bool outside,SeismicSectionResult& result,std::string& error,const CancelCheck& cancel) {
  result={};error.clear();SeismicVolume volume;
  if(!readVolume(id,volume,error))return false;
  std::vector<DataEntry> entries;if(!listData("volumes",0,entries,error))return false;
  for(const auto& entry:entries)if(entry.id==id) {
    if(!verifySource(entry.sourceId,error))return false;
    return readSeismicFileGridSection(volume.dataFile,path,columns,interpolate,outside,result,error,cancel);
  }
  error="地震体来源不存在";return false;
}
bool DataService::readSeismicFileGridSection(const std::string& file,const std::vector<std::pair<int,int>>& path,
    int columns,bool interpolate,bool outside,SeismicSectionResult& result,std::string& error,const CancelCheck& cancel) {
  result={};error.clear();
  try {
    const auto stopped=[&]{if(cancel&&cancel()){error="地震读取已取消";return true;}return false;};
    if(stopped())return false;
    if(path.size()<2||path.size()>10000||columns<2||columns>8192){error="剖面路径或列预算无效";return false;}
    segy_detail::Reader reader;if(!reader.open(file,error,cancel))return false;
    const auto& info=reader.info();const auto& ils=reader.inlines();const auto& xls=reader.crosslines();
    if(ils.empty()||xls.empty()){error="地震测网不可用";return false;}
    std::vector<double> lengths;double length=0;
    for(size_t n=1;n<path.size();++n){const double step=std::hypot(double(path[n].first)-path[n-1].first,double(path[n].second)-path[n-1].second);lengths.push_back(step);length+=step;}
    if(!std::isfinite(length)||length<=1e-4){error="剖面路径太短";return false;}
    const int natural=int(std::min(double(INT_MAX-1),std::round(length)))+1;
    const int count=interpolate?columns:std::clamp(natural,2,columns);
    if(static_cast<long long>(count)*info.sampleCount>64000000){error="剖面振幅超过64000000预算";return false;}
    const auto nearest=[](const std::vector<int>& axis,double value){
      auto at=std::lower_bound(axis.begin(),axis.end(),value);
      if(at==axis.begin())return *at;if(at==axis.end())return axis.back();
      return value-*(at-1)<=*at-value?*(at-1):*at;
    };
    SeismicSectionResult output;output.columns=count;output.samples=info.sampleCount;output.nativeColumns=natural;output.decimated=count<natural;
    output.firstTime=info.firstTime;output.sampleInterval=info.sampleInterval;output.timeUnit="ms";
    size_t segment=0;double start=0;
    for(int column=0;column<count;++column){
      if(stopped())return false;
      const double distance=length*column/(count-1);
      while(segment+1<lengths.size()&&distance>start+lengths[segment]){start+=lengths[segment];++segment;}
      const double fraction=lengths[segment]>0?std::clamp((distance-start)/lengths[segment],0.,1.):0.;
      const double il=path[segment].first+fraction*(double(path[segment+1].first)-path[segment].first);
      const double xl=path[segment].second+fraction*(double(path[segment+1].second)-path[segment].second);
      const bool missing=outside&&(il<ils.front()||il>ils.back()||xl<xls.front()||xl>xls.back());
      const int actualIl=nearest(ils,il),actualXl=nearest(xls,xl);
      output.traceIndexes.push_back(missing?-1:reader.traceIndex(actualIl,actualXl));
      output.inlineNumbers.push_back(actualIl);output.crosslineNumbers.push_back(actualXl);output.distances.push_back(distance);
      double x=Missing,y=Missing;reader.mapLine(il,xl,x,y);output.pathX.push_back(x);output.pathY.push_back(y);
    }
    std::vector<long long> unique;for(auto ordinal:output.traceIndexes)if(ordinal>=0)unique.push_back(ordinal);
    std::sort(unique.begin(),unique.end());unique.erase(std::unique(unique.begin(),unique.end()),unique.end());
    segy_detail::TraceWindowBatch batch;SeismicReadStatistics stats;segy_detail::BatchReadOptions options;
    if(!unique.empty()&&!reader.readTraceBatch(unique,0,info.sampleCount,options,batch,stats,error,cancel))return false;
    output.amplitudes.assign(size_t(count)*info.sampleCount,std::numeric_limits<float>::quiet_NaN());
    std::vector<std::pair<long long,double>> order;
    for(int column=0;column<count;++column){
      const auto ordinal=output.traceIndexes[column];double x=Missing,y=Missing;
      if(ordinal>=0){const auto at=size_t(std::lower_bound(unique.begin(),unique.end(),ordinal)-unique.begin());
        std::copy_n(batch.amplitudes.begin()+at*info.sampleCount,info.sampleCount,output.amplitudes.begin()+size_t(column)*info.sampleCount);
        x=batch.x[at];y=batch.y[at];
        if(order.empty()||order.back().first!=ordinal)order.push_back({ordinal,output.distances[column]});
      }
      output.x.push_back(x);output.y.push_back(y);
    }
    // Same along-path interpolation convention as the preview library, with data-service trace reads.
    if(interpolate&&order.size()>1){size_t at=0;
      for(int column=0;column<count;++column){if(stopped())return false;const auto ordinal=output.traceIndexes[column];if(ordinal<0)continue;
        while(at+1<order.size()&&order[at+1].second<=output.distances[column])++at;
        if(at+1>=order.size())continue;
        const double span=order[at+1].second-order[at].second;if(span<=1e-6)continue;
        const float weight=float(std::clamp((output.distances[column]-order[at].second)/span,0.,1.));
        const auto a=size_t(std::lower_bound(unique.begin(),unique.end(),ordinal)-unique.begin());
        const auto b=size_t(std::lower_bound(unique.begin(),unique.end(),order[at+1].first)-unique.begin());
        for(int sample=0;sample<info.sampleCount;++sample)output.amplitudes[size_t(column)*info.sampleCount+sample]=
          (1-weight)*batch.amplitudes[a*info.sampleCount+sample]+weight*batch.amplitudes[b*info.sampleCount+sample];
      }
    }
    if(stopped())return false;result=std::move(output);return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
