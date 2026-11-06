// Business layer. Compose public data-flow queries; no SQLite or parser access here.
#include "paleo_data/service.h"
#include <QString>
#include <algorithm>
#include <set>
namespace paleo::data {
bool DataService::prepareWellFeatures(long long wellId,const std::vector<long long>& ids,double top,double bottom,bool processed,WellFeatureTable& result,std::string& error)const{
    return prepareWellFeatures(wellId,ids,top,bottom,processed,result,error,CurveReadOptions{});
}
bool DataService::prepareWellFeatures(long long wellId,const std::vector<long long>& ids,double top,double bottom,bool processed,
    WellFeatureTable& result,std::string& error,const CurveReadOptions& options)const{
    result={};if(wellId<=0||ids.empty()||ids.size()>64){error="特征表需要一口井与1到64条曲线";return false;}
    std::vector<DataEntry> entries;if(!listData("curves",wellId,entries,error))return false;
    WellFeatureTable table;table.wellId=wellId;long long parent=0;std::set<long long> seen;
    long long consumed=0;
    for(auto id:ids){auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& v){return v.id==id;});if(entry==entries.end()||!seen.insert(id).second){error="曲线重复或不属于指定井";return false;}
        auto budget=options;budget.maxReadBytes-=consumed;budget.maxSamples=std::min(budget.maxSamples,8000000LL/static_cast<long long>(ids.size()));
        CurveReadStats usage;CurveSlice curve;if(!readCurveSlice(id,top,bottom,processed,curve,error,budget,&usage))return false;
        consumed+=usage.payloadBytesRead;
        for(double d:curve.depths)if(!std::isfinite(d)){error="特征表中存在未知深度，先核对深度轴";return false;}
        if(table.columns.empty()){table.depths=std::move(curve.depths);table.depthUnit=curve.depthUnit;parent=entry->parentId;}
        else if(curve.depths!=table.depths||curve.depthUnit!=table.depthUnit||entry->parentId!=parent){error="一期特征表仅组合同一曲线组内共享采样轴的曲线；不同组的深度基准与采样需要显式处理";return false;}
        if(table.depths.size()*ids.size()>8000000){error="特征表超过800万数值预算，请缩小深度范围或曲线数量";return false;}
        table.columns.push_back({id,curve.name,curve.unit,std::move(curve.values),std::move(curve.quality)});
    }result=std::move(table);return true;
}
bool DataService::compareIntervals(const std::vector<long long>& ids,double top,double bottom,std::vector<IntervalTrackResult>& result,std::string& error)const{
    result.clear();if(ids.empty()||ids.size()>64){error="请选择1到64个分层道";return false;}std::vector<DataEntry> entries;if(!listData("intervals",0,entries,error))return false;std::vector<IntervalTrackResult> groups;std::set<long long> seen;
    for(auto id:ids){auto e=std::find_if(entries.begin(),entries.end(),[&](const auto& v){return v.id==id;});if(e==entries.end()||!seen.insert(id).second){error="分层道不存在或重复";return false;}IntervalTrackResult group;group.trackId=id;group.wellId=e->wellId;group.name=e->name;IntervalTrack info;if(!readIntervalTrackInfo(id,info,error))return false;group.depthInfo=info.depthInfo;if(!readIntervals(id,top,bottom,group.intervals,error))return false;groups.push_back(std::move(group));}result=std::move(groups);return true;
}
bool DataService::findMaps(const std::string& area,const std::string& type,const std::string& stratum,std::vector<GeologicalMap>& result,std::string& error)const{
    result.clear();std::vector<DataEntry> entries;if(!listData("maps",0,entries,error))return false;auto matches=[](const std::string& value,const std::string& requested){return requested.empty()||QString::fromUtf8(value).contains(QString::fromUtf8(requested),Qt::CaseInsensitive);};
    std::vector<GeologicalMap> maps;for(const auto& entry:entries){GeologicalMap map;if(!readMap(entry.id,map,error))return false;if(!matches(map.areaName,area)||!matches(map.mapType,type))continue;if(!stratum.empty()&&std::none_of(map.stratigraphicNames.begin(),map.stratigraphicNames.end(),[&](const auto& n){return matches(n,stratum);}))continue;maps.push_back(std::move(map));}result=std::move(maps);return true;
}
}
