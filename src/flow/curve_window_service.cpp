#include "store/curve_windows.h"
#include <algorithm>
#include <limits>

namespace paleo::data {
bool DataService::readCurveSlice(long long id,double top,double bottom,bool processed,CurveSlice& result,std::string& error)const {
    return readCurveSlice(id,top,bottom,processed,result,error,CurveReadOptions{});
}
bool DataService::readCurveSlice(long long id,double top,double bottom,bool processed,CurveSlice& result,std::string& error,
                                 const CurveReadOptions& options,CurveReadStats* stats)const {
    result={};CurveReadStats readStats;DepthInfo info;std::vector<double> tvd,ss;long long total=0;
    const bool ok=detail::readCurveWindows(impl_->store,id,0,std::numeric_limits<long long>::max(),top,bottom,processed,false,
                                         result,info,tvd,ss,total,options,readStats,error);
    if(stats)*stats=readStats;return ok;
}
bool DataService::readCurveWindow(const CurveWindowQuery& query,CurveWindowResult& result,std::string& error,
                                  const CurveReadOptions& options,CurveReadStats* stats)const {
    result={};CurveReadStats readStats;CurveWindowResult next;next.firstSample=query.firstSample;
    const bool ok=detail::readCurveWindows(impl_->store,query.curveId,query.firstSample,query.sampleCount,Missing,Missing,
        query.processed,query.includeAuxiliaryAxes,next.curve,next.depthInfo,next.tvdValues,next.tvdssValues,next.totalSamples,
        options,readStats,error,&next.depthOrigins);
    if(stats)*stats=readStats;if(ok)result=std::move(next);return ok;
}
bool DataService::readCurveDepthWindow(long long id,double top,double bottom,bool processed,
    CurveWindowResult& result,std::string& error,const CurveReadOptions& options,CurveReadStats* stats) const {
    result={};CurveReadStats usage;CurveWindowResult next;next.firstSample=-1;
    const bool ok=detail::readCurveWindows(impl_->store,id,0,std::numeric_limits<long long>::max(),top,bottom,
        processed,true,next.curve,next.depthInfo,next.tvdValues,next.tvdssValues,next.totalSamples,options,usage,error,&next.depthOrigins);
    if(stats)*stats=usage;if(ok)result=std::move(next);return ok;
}
bool DataService::readCurveTableWindow(long long id,long long offset,int limit,DataTable& result,std::string& error,
                                       const CurveReadOptions& options,CurveReadStats* stats)const {
    result={};if(stats)*stats={};if(offset<0||limit<1||limit>5000){error="分页范围错误（每页1到5000行）";return false;}
    CurveWindowQuery query;query.curveId=id;query.firstSample=offset;query.sampleCount=limit;query.includeAuxiliaryAxes=false;
    CurveWindowResult window;if(!readCurveWindow(query,window,error,options,stats))return false;
    DataTable table;table.columns={"sampleIndex","深度","原始值","处理值","质量标记"};table.totalRows=window.totalSamples;
    const auto text=[](double value){return std::isfinite(value)?detail::s(QString::number(value,'g',17)):std::string();};
    for(size_t i=0;i<window.curve.depths.size();++i){
        if(options.cancelled&&options.cancelled()){error="CurveWindowCancelled: 已取消曲线读取";return false;}
        table.rows.push_back({std::to_string(offset+static_cast<long long>(i)),text(window.curve.depths[i]),text(window.curve.rawValues[i]),text(window.curve.values[i]),std::to_string(window.curve.quality[i])});
    }
    result=std::move(table);return true;
}
bool DataService::upgradeCurveWindowStorage(std::string& error,const CancelCheck& cancelled,long long maxLegacyBytes) {
    return detail::upgradeCurveWindows(impl_->store,error,cancelled,maxLegacyBytes);
}
}
