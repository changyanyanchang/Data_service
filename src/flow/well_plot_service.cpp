#include "store/project_store.h"
#include <QJsonDocument>
#include <set>

namespace paleo::data {
using namespace detail;
bool DataService::readProjectCoordinates(ProjectCoordinates& result, std::string& error) const {
    result={}; error.clear(); QSqlQuery rows;
    if(!impl_->store.exec("SELECT key,value FROM ds_meta WHERE key IN ('project_crs','project_crs_unit')",{},error,&rows))return false;
    while(rows.next()) {
        if(rows.value(0).toString()=="project_crs")result.coordinateSystem=s(rows.value(1).toString());
        else result.coordinateUnit=s(rows.value(1).toString());
    }
    return true;
}
bool DataService::listSourceWells(long long source, std::vector<long long>& result, std::string& error) const {
    result.clear(); error.clear(); QSqlQuery rows;
    if(source<=0){error="Invalid source ID";return false;}
    if(!impl_->store.exec("SELECT well_id FROM log_sets WHERE source_id=? UNION SELECT well_id FROM tracks WHERE source_id=? UNION SELECT well_id FROM sample_tables WHERE source_id=? UNION SELECT well_id FROM well_objects WHERE source_id=? ORDER BY well_id",{source,source,source,source},error,&rows))return false;
    while(rows.next())if(rows.value(0).toLongLong()>0)result.push_back(rows.value(0).toLongLong());
    return true;
}
bool DataService::readWellPlotData(long long well, const WellPlotOptions& options, WellPlotData& result,
                                  std::string& error, const CancelCheck& cancelled) const {
    result={}; error.clear();
    if(well<=0||options.maxValues<=0||options.maxValues>64000000){error="Invalid well ID or plot budget";return false;}
    auto& store=impl_->store;
    // BEGIN (deferred) is a read snapshot, including WAL; never BEGIN IMMEDIATE.
    if(!store.exec("BEGIN",{},error))return false;
    struct Rollback { ProjectStore& store; ~Rollback(){std::string ignored;store.exec("ROLLBACK",{},ignored);} } rollback{store};
    const auto stopped=[&]{if(cancelled&&cancelled()){error="Cancelled";return true;}return false;};
    if(stopped())return false;
    WellPlotData next;next.wellId=well;
    if(!readWellInfo(well,next.well,error)||!readProjectCoordinates(next.projectCoordinates,error))return false;
    QSqlQuery row;
    if(!store.exec("SELECT depth_json FROM wells WHERE id=?",{well},error,&row)||!row.next())return false;
    next.defaultDepthInfo=readDepth(object(row.value(0).toString()));row.finish();
    std::vector<DataEntry> logs,tracks,markers,photos;
    if(!listData("curves",well,logs,error)||!listData("intervals",well,tracks,error)||
       !listData("markers",well,markers,error)||!listData("core_photos",well,photos,error))return false;
    long long remaining=options.maxValues;
    const auto reserve=[&](long long count){if(count<0||count>remaining){error="Well plot exceeds value budget";return false;}remaining-=count;return true;};
    for(const auto& entries:{&logs,&tracks,&markers,&photos})for(const auto& entry:*entries)if(!reserve(entry.count))return false;
    std::set<long long> seen;
    for(const auto& entry:logs) {
        if(stopped())return false;
        if(!seen.insert(entry.parentId).second)continue;
        WellPlotLog item;item.entry=entry;
        if(!readLogSet(entry.parentId,options.processed,item.value,error))return false;
        next.logs.push_back(std::move(item));
    }
    for(const auto& entry:tracks) {
        if(stopped())return false;
        WellPlotIntervals item;item.entry=entry;std::vector<IntervalRecord> values;
        if(!readIntervalTrackInfo(entry.id,item.value,error)||!readIntervals(entry.id,Missing,Missing,values,error))return false;
        for(auto& value:values)item.value.intervals.push_back(std::move(value.value));
        next.intervals.push_back(std::move(item));
    }
    for(const auto& entry:markers) {
        if(stopped())return false;
        if(!store.exec("SELECT payload FROM well_objects WHERE id=? AND kind='markers'",{entry.id},error,&row)||!row.next())return false;
        WellPlotMarkers item;item.entry=entry;item.value.trackName=entry.name;
        const auto payload=object(row.value(0).toString());row.finish();
        item.value.depthInfo=readDepth(payload["depth"].toObject());
        for(const auto& v:payload["points"].toArray()) {
            const auto p=v.toObject();Marker marker;marker.markerId=p["id"].toInt();marker.markerName=s(p["name"].toString());
            const auto number=[](const QJsonValue& value){return value.isDouble()?value.toDouble():Missing;};
            marker.depthValue=number(p["depth"]);marker.tvd=number(p["tvd"]);marker.tvdss=number(p["tvdss"]);marker.sourceRow=p["row"].toInt();
            item.value.markers.push_back(std::move(marker));
        }
        next.markers.push_back(std::move(item));
    }
    for(const auto& entry:photos) {
        if(stopped())return false;
        WellPlotPhotos item;item.entry=entry;
        if(!readCorePhotoTrack(entry.id,item.value,error))return false;
        next.photos.push_back(std::move(item));
    }
    if(stopped())return false;
    result=std::move(next);return true;
}
}
