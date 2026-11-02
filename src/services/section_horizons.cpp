#include "store/project_store.h"
#include "store/horizon_grid_sampler.h"
#include <QSqlError>
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>

namespace paleo::data {
using namespace detail;
namespace {
bool finiteCurve(const SectionHorizonCurve& curve){
    return std::any_of(curve.timesMs.begin(),curve.timesMs.end(),[](double t){return std::isfinite(t);});
}

}
bool DataService::readSectionHorizons(const SectionHorizonQuery& query,SectionHorizonResult& result,
                                     std::string& error,const CancelCheck& cancelled)const{
    result={};error.clear();
    const auto stopped=[&]{if(cancelled&&cancelled()){error="层位叠加读取已取消";return true;}return false;};
    if(stopped())return false;
    if(query.columns.empty()||query.columns.size()>65536||query.interpolationRadius<0||query.interpolationRadius>128){error="层位叠加列数或插值邻域无效";return false;}
    for(const auto& col:query.columns)if(query.interpolationRadius&&
      (col.inlineNumber<=std::numeric_limits<int>::min()+query.interpolationRadius||col.inlineNumber>=std::numeric_limits<int>::max()-query.interpolationRadius||
       col.crosslineNumber<=std::numeric_limits<int>::min()+query.interpolationRadius||col.crosslineNumber>=std::numeric_limits<int>::max()-query.interpolationRadius)){
        error="层位插值邻域超出测线编号范围";return false;
    }
    auto& store=impl_->store;
    if(!store.exec("BEGIN",{},error))return false;
    struct Rollback{const ProjectStore& store;~Rollback(){std::string ignored;store.exec("ROLLBACK",{},ignored);}}rollback{store};
    SeismicVolume volume;if(!readVolume(query.volumeId,volume,error))return false;
    const auto volumeType=volume.sampling.timeType.empty()?query.undeclaredTimeType:volume.sampling.timeType;
    if(volumeType!="TWT"){error="层位叠加需要 TWT 时间剖面";return false;}
    // Do not call listData("horizons"): its inventory counts scan every point.
    // The overlay needs identities only, then indexed lookups at displayed columns.
    std::vector<DataEntry> entries;QSqlQuery rows;
    if(!store.exec("SELECT h.id,h.name FROM horizons h LEFT JOIN sources s ON s.id=h.source_id WHERE h.source_id IS NULL OR s.active=1 ORDER BY h.id",{},error,&rows))return false;
    while(rows.next()){DataEntry entry;entry.id=rows.value(0).toLongLong();entry.name=s(rows.value(1).toString());entries.push_back(std::move(entry));}
    rows.finish();
    SectionHorizonResult next;std::map<long long,size_t> byResource;
    for(const auto& entry:entries){
        if(stopped())return false;
        if((next.curves.size()+1)*query.columns.size()>8000000){error="层位叠加超出点数预算";return false;}
        SeismicHorizon h;if(!readHorizon(entry.id,0,1,h,error))return false;
        if(h.parentVolumeId&&h.parentVolumeId!=query.volumeId)continue;
        const auto skip=[&](const std::string& reason){next.warnings.push_back(h.horizonName+": "+reason);};
        if(h.coordinateSystem!=volume.geometry.coordinateSystem||h.coordinateUnit!=volume.geometry.coordinateUnit){skip("坐标约定与地震体不同");continue;}
        if(!h.timeReference.empty()&&!volume.sampling.timeReference.empty()&&h.timeReference!=volume.sampling.timeReference){skip("时间基准与地震体不同");continue;}
        const auto unit=h.timeUnit.empty()?(volume.sampling.timeUnit.empty()?query.undeclaredTimeUnit:volume.sampling.timeUnit):h.timeUnit;
        const auto type=h.timeType.empty()?volumeType:h.timeType;
        if((unit!="ms"&&unit!="s")||(type!="TWT"&&type!="OWT")){skip("没有可用时间单位或单双程约定");continue;}
        const double scale=(unit=="s"?1000.:1.)*(type=="OWT"?2.:1.);
        GridSampler sampler(store,entry.id,query.interpolationRadius);if(!sampler.prepare(error))return false;
        SectionHorizonCurve curve;curve.id="horizon:"+std::to_string(entry.id);curve.name=h.horizonName;curve.horizonId=entry.id;
        curve.timesMs.reserve(query.columns.size());
        for(const auto& column:query.columns){
            if(stopped())return false;
            curve.timesMs.push_back(column.present?sampler.sample(column.inlineNumber,column.crosslineNumber,error)*scale:Missing);
            if(!error.empty())return false;
        }
        if(finiteCurve(curve)){byResource[entry.id]=next.curves.size();next.curves.push_back(std::move(curve));}
    }
    std::vector<StratigraphicSchemeSummary> schemes;if(!listStratigraphicSchemes(schemes,error))return false;
    const auto selected=std::find_if(schemes.begin(),schemes.end(),[&](const auto& s){return s.id==query.schemeId;});
    if(selected==schemes.end())next.warnings.push_back("没有关联的地层方案，仅显示可用大层界面。");
    if(selected!=schemes.end()){
        StratigraphicScheme scheme;if(!readStratigraphicScheme(query.schemeId,query.revision,scheme,error))return false;
        next.schemeId=scheme.id;next.revision=scheme.revision.value;
        std::map<std::string,long long> boundary;
        for(const auto& b:scheme.boundaries)boundary[b.id.value]=b.seismicHorizonResourceId;
        for(const auto& interval:scheme.intervals){
            const auto top=byResource.find(boundary[interval.top.value]),bottom=byResource.find(boundary[interval.bottom.value]);
            if(top==byResource.end()||bottom==byResource.end()||interval.status!="valid")continue;
            const auto topId=next.curves[top->second].id,bottomId=next.curves[bottom->second].id;
            const auto parentName=next.curves[top->second].name+"–"+next.curves[bottom->second].name;
            next.bands.push_back({interval.id.value,parentName,topId,bottomId,0});
            const auto division=std::find_if(scheme.divisions.begin(),scheme.divisions.end(),[&](const auto& d){return d.interval==interval.id;});
            if(division==scheme.divisions.end())continue;
            const auto fractions=division->fractions;
            if(!(fractions[0]>0&&fractions[0]<fractions[1]&&fractions[1]<fractions[2]&&fractions[2]<1))continue;
            const auto topTimes=next.curves[top->second].timesMs,bottomTimes=next.curves[bottom->second].timesMs;
            std::vector<std::string> edges{topId};
            for(int edge=0;edge<3;++edge){
                if(stopped())return false;
                if((next.curves.size()+1)*query.columns.size()>8000000){error="层位叠加超出点数预算";return false;}
                SectionHorizonCurve curve;curve.id=interval.id.value+":edge:"+std::to_string(edge+1);
                curve.name=parentName+"/S"+std::to_string(edge+1)+"/S"+std::to_string(edge+2);
                curve.intervalId=interval.id.value;curve.internalEdge=edge+1;curve.provisional=division->edgeStatus[edge]!="confirmed";
                for(size_t col=0;col<query.columns.size();++col){
                    const double t=topTimes[col],b=bottomTimes[col];
                    curve.timesMs.push_back(std::isfinite(t)&&std::isfinite(b)&&b>t?t+(b-t)*fractions[edge]:Missing);
                }
                if(!finiteCurve(curve))break;
                edges.push_back(curve.id);next.curves.push_back(std::move(curve));
            }
            if(edges.size()!=4)continue;
            edges.push_back(bottomId);
            for(int sub=1;sub<=4;++sub)next.bands.push_back({interval.id.value+":S"+std::to_string(sub),
                parentName+"/S"+std::to_string(sub),edges[sub-1],edges[sub],sub});
        }
    }
    if(stopped())return false;
    result=std::move(next);return true;
}
}
