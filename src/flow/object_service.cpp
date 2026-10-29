#include "store/project_store.h"
#include <QJsonDocument>
#include <QFileInfo>
#include <limits>
namespace paleo::data {
using namespace detail;
static double jnumber(const QJsonValue& v){return v.isDouble()?v.toDouble():Missing;}
bool DataService::readLogSet(long long id,bool processed,LogSet& result,std::string& e)const{
    result={};QSqlQuery r;
    if(!impl_->store.exec("SELECT l.name,l.depth_json,l.depths,l.tvd,l.tvdss,l.step,l.sheet,s.stored_path FROM log_sets l LEFT JOIN sources s ON s.id=l.source_id WHERE l.id=?",{id},e,&r))return false;
    if(!r.next()){e="曲线组不存在";return false;}
    result.logSetName=s(r.value(0).toString());result.depthInfo=readDepth(object(r.value(1).toString()));
    if(!unpack(r.value(2).toByteArray(),result.samplingAxis.depthValues,e)||!unpack(r.value(3).toByteArray(),result.samplingAxis.tvdValues,e)||!unpack(r.value(4).toByteArray(),result.samplingAxis.tvdssValues,e))return false;
    result.samplingAxis.sampleStep=value(r.value(5));result.sourceSheet=s(r.value(6).toString());result.sourceFile=s(impl_->store.absolutePath(r.value(7).toString()));
    if(!impl_->store.exec("SELECT name,unit,description,raw_values,processed_values FROM curves WHERE set_id=? ORDER BY id",{id},e,&r))return false;
    while(r.next()){LogCurve c;c.curveName=s(r.value(0).toString());c.curveUnit=s(r.value(1).toString());c.description=s(r.value(2).toString());
        if(!unpack(r.value(processed&&!r.value(4).isNull()?4:3).toByteArray(),c.sampleValues,e))return false;
        if(c.sampleValues.size()!=result.samplingAxis.depthValues.size()){e="曲线组数组长度损坏";return false;}result.logCurves.push_back(std::move(c));}
    return true;
}
bool DataService::readCorePhotoTrack(long long id,CorePhotoTrack& result,std::string& e)const{
    result={};QSqlQuery r;if(!impl_->store.exec("SELECT name,payload FROM well_objects WHERE id=? AND kind='core_photos'",{id},e,&r))return false;
    if(!r.next()){e="岩心照片道不存在";return false;}result.trackName=s(r.value(0).toString());
    for(auto value:QJsonDocument::fromJson(r.value(1).toString().toUtf8()).array()){
        const auto o=value.toObject();CorePhoto p;auto path=o["path"].toString();p.imagePath=path.isEmpty()?std::string():s(impl_->store.absolutePath(path));
        p.topDepth=jnumber(o["top"]);p.bottomDepth=jnumber(o["bottom"]);p.description=s(o["description"].toString());result.photos.push_back(std::move(p));}
    return true;
}
bool DataService::readAttachment(long long id,Attachment& result,std::string& e)const{
    result={};QSqlQuery r;if(!impl_->store.exec("SELECT name,payload FROM well_objects WHERE id=? AND kind='attachments'",{id},e,&r))return false;
    if(!r.next()){e="附件不存在";return false;}result.attachmentName=s(r.value(0).toString());auto o=object(r.value(1).toString());
    auto path=o["path"].toString();result.filePath=path.isEmpty()?std::string():s(impl_->store.absolutePath(path));result.attachmentType=s(o["type"].toString());result.targetType=s(o["targetType"].toString());result.targetId=o["targetId"].toInt();return true;
}
bool DataService::readIntervalTrackInfo(long long id,IntervalTrack& t,std::string& e)const{
    t={};QSqlQuery r;if(!impl_->store.exec("SELECT t.name,t.type,t.depth_json,t.sheet,s.stored_path FROM tracks t LEFT JOIN sources s ON s.id=t.source_id WHERE t.id=?",{id},e,&r))return false;if(!r.next()){e="分层道不存在";return false;}t.trackName=s(r.value(0).toString());t.trackType=s(r.value(1).toString());t.depthInfo=readDepth(object(r.value(2).toString()));t.sourceSheet=s(r.value(3).toString());t.sourceFile=s(impl_->store.absolutePath(r.value(4).toString()));return true;
}
bool DataService::readVolume(long long id,SeismicVolume& v,std::string& e)const{
    v={};QSqlQuery r;if(!impl_->store.exec("SELECT name,path,format,amplitude_unit,geometry,sampling FROM volumes WHERE id=?",{id},e,&r))return false;if(!r.next()){e="地震体不存在";return false;}
    v.volumeId=int(id);v.volumeName=s(r.value(0).toString());v.dataFile=s(impl_->store.absolutePath(r.value(1).toString()));v.fileFormat=s(r.value(2).toString());v.amplitudeUnit=s(r.value(3).toString());auto g=object(r.value(4).toString()),a=object(r.value(5).toString());
    v.geometry.traceCount=g["traceCount"].toInteger(-1);v.geometry.inlineMin=g["inlineMin"].toInt();v.geometry.inlineMax=g["inlineMax"].toInt();v.geometry.crosslineMin=g["crosslineMin"].toInt();v.geometry.crosslineMax=g["crosslineMax"].toInt();v.geometry.coordinateSystem=s(g["crs"].toString());v.geometry.coordinateUnit=s(g["unit"].toString());
    v.sampling.sampleCount=a["sampleCount"].toInt();v.sampling.firstTime=jnumber(a["firstTime"]);v.sampling.sampleInterval=jnumber(a["sampleInterval"]);v.sampling.timeUnit=s(a["timeUnit"].toString());v.sampling.timeType=s(a["timeType"].toString());v.sampling.timeReference=s(a["timeReference"].toString());return true;
}
bool DataService::saveVolume(long long id,const SeismicVolume& v,std::string& e){
    SeismicVolume old;if(!readVolume(id,old,e))return false;if(v.volumeName.empty()){e="地震体名称不能为空";return false;}if(!v.traces.empty()){e="此接口只保存元信息；地震振幅保持外部存储";return false;}
    if(!v.dataFile.empty()&&QFileInfo(q(v.dataFile)).absoluteFilePath()!=QFileInfo(q(old.dataFile)).absoluteFilePath()){e="文件路径变更请通过重新定位来源接口校验身份";return false;}
    if(v.sampling.sampleCount<0||(std::isfinite(v.sampling.sampleInterval)&&v.sampling.sampleInterval<=0)||std::isinf(v.sampling.firstTime)){e="采样信息无效";return false;}
    if(!v.sampling.timeType.empty()&&v.sampling.timeType!="TWT"&&v.sampling.timeType!="OWT"){e="时间类型须为 TWT、OWT 或留空";return false;}
    const auto& g=v.geometry;const auto& a=v.sampling;QJsonObject geometry{{"traceCount",g.traceCount},{"inlineMin",g.inlineMin},{"inlineMax",g.inlineMax},{"crosslineMin",g.crosslineMin},{"crosslineMax",g.crosslineMax},{"crs",q(g.coordinateSystem)},{"unit",q(g.coordinateUnit)}};
    QJsonObject sampling{{"sampleCount",a.sampleCount},{"firstTime",QJsonValue::fromVariant(number(a.firstTime))},{"sampleInterval",QJsonValue::fromVariant(number(a.sampleInterval))},{"timeUnit",q(a.timeUnit)},{"timeType",q(a.timeType)},{"timeReference",q(a.timeReference)}};
    auto& st=impl_->store;if(!st.begin(e))return false;bool ok=st.exec("UPDATE volumes SET name=?,amplitude_unit=?,geometry=?,sampling=? WHERE id=?",{q(v.volumeName),q(v.amplitudeUnit),json(geometry),json(sampling),id},e)&&st.exec("INSERT INTO changes(kind,entity_id,description) VALUES('volume',?,'修改地震体元信息')",{id},e)&&st.commit(e);if(!ok)st.rollback();return ok;
}
bool DataService::readHorizon(long long id,long long offset,int limit,SeismicHorizon& h,std::string& e)const{
    h={};if(offset<0||limit<1||limit>100000){e="层位分页范围无效";return false;}QSqlQuery r;if(!impl_->store.exec("SELECT h.name,h.volume_id,h.crs,h.unit,h.time_unit,h.time_type,h.time_reference,s.stored_path FROM horizons h LEFT JOIN sources s ON s.id=h.source_id WHERE h.id=?",{id},e,&r))return false;if(!r.next()){e="地震层位不存在";return false;}
    h.horizonName=s(r.value(0).toString());h.parentVolumeId=r.value(1).toInt();h.coordinateSystem=s(r.value(2).toString());h.coordinateUnit=s(r.value(3).toString());h.timeUnit=s(r.value(4).toString());h.timeType=s(r.value(5).toString());h.timeReference=s(r.value(6).toString());h.sourceFile=s(impl_->store.absolutePath(r.value(7).toString()));
    if(!impl_->store.exec("SELECT inline_no,crossline_no,x,y,z FROM horizon_points WHERE horizon_id=? ORDER BY seq,id LIMIT ? OFFSET ?",{id,limit,offset},e,&r))return false;while(r.next())h.points.push_back({r.value(0).toInt(),r.value(1).toInt(),value(r.value(2)),value(r.value(3)),value(r.value(4))});return true;
}
bool DataService::saveHorizonInfo(long long id,const SeismicHorizon& h,std::string& e){
    if(h.horizonName.empty()){e="层位名称不能为空";return false;}SeismicHorizon old;if(!readHorizon(id,0,1,old,e))return false;
    if(h.parentVolumeId){SeismicVolume v;if(!readVolume(h.parentVolumeId,v,e))return false;for(auto pair:{std::pair{h.coordinateSystem,v.geometry.coordinateSystem},std::pair{h.coordinateUnit,v.geometry.coordinateUnit},std::pair{h.timeUnit,v.sampling.timeUnit},std::pair{h.timeType,v.sampling.timeType},std::pair{h.timeReference,v.sampling.timeReference}})if(!pair.first.empty()&&!pair.second.empty()&&pair.first!=pair.second){e="层位与所属体的坐标或时间约定不一致；请先完成明确转换";return false;}}
    auto& st=impl_->store;if(!st.begin(e))return false;bool ok=st.exec("UPDATE horizons SET name=?,volume_id=?,crs=?,unit=?,time_unit=?,time_type=?,time_reference=? WHERE id=?",{q(h.horizonName),h.parentVolumeId?QVariant(h.parentVolumeId):QVariant(),q(h.coordinateSystem),q(h.coordinateUnit),q(h.timeUnit),q(h.timeType),q(h.timeReference),id},e)&&st.exec("INSERT INTO changes(kind,entity_id,description) VALUES('horizon',?,'修改地震层位约定与所属体')",{id},e)&&st.commit(e);if(!ok)st.rollback();return ok;
}
bool DataService::readSampleTable(long long id,SampleTable& table,std::string& e)const{
    table={};QSqlQuery r;if(!impl_->store.exec("SELECT t.name,t.sheet,s.stored_path FROM sample_tables t LEFT JOIN sources s ON s.id=t.source_id WHERE t.id=?",{id},e,&r))return false;if(!r.next()){e="样品分析表不存在";return false;}table.tableName=s(r.value(0).toString());table.sourceSheet=s(r.value(1).toString());table.sourceFile=s(impl_->store.absolutePath(r.value(2).toString()));
    if(!impl_->store.exec("SELECT a.id,a.number,a.type,a.source_range,a.top,a.bottom,m.id,m.name,m.unit,m.value,m.original_text FROM samples a LEFT JOIN measurements m ON m.sample_id=a.id WHERE a.table_id=? ORDER BY a.id,m.id",{id},e,&r))return false;long long last=-1;
    while(r.next()){if(r.value(0).toLongLong()!=last){last=r.value(0).toLongLong();SampleRecord sample;sample.sampleNumber=s(r.value(1).toString());sample.sampleType=s(r.value(2).toString());sample.sourceRange=s(r.value(3).toString());sample.topDepth=value(r.value(4));sample.bottomDepth=value(r.value(5));table.samples.push_back(std::move(sample));}if(!r.value(6).isNull())table.samples.back().measurements.push_back({s(r.value(7).toString()),s(r.value(8).toString()),value(r.value(9)),s(r.value(10).toString())});}return true;
}
bool DataService::readTimeDepth(long long id,TimeDepthTable& t,std::string& e)const{
    t={};QSqlQuery r;if(!impl_->store.exec("SELECT o.name,o.payload,s.stored_path FROM well_objects o LEFT JOIN sources s ON s.id=o.source_id WHERE o.id=? AND o.kind='time_depth'",{id},e,&r))return false;if(!r.next()){e="时深关系不存在";return false;}
    t.relationName=s(r.value(0).toString());auto o=object(r.value(1).toString());
    t.depthInfo=readDepth(o["depth"].toObject());t.timeUnit=s(o["timeUnit"].toString());
    t.timeType=s(o["timeType"].toString());t.timeReference=s(o["timeReference"].toString());
    t.positionDepthInfo=readDepth(o["positionDepth"].toObject());
    t.coordinateSystem=s(o["coordinateSystem"].toString());t.coordinateUnit=s(o["coordinateUnit"].toString());
    t.sourceSheet=s(o["sourceSheet"].toString());t.sourceFile=s(impl_->store.absolutePath(r.value(2).toString()));
    // Older persisted two-column tables have no XYZ keys; jnumber leaves them Missing.
    for(auto item:o["points"].toArray()){auto p=item.toObject();t.depthTimePairs.push_back({jnumber(p["depth"]),jnumber(p["time"]),jnumber(p["x"]),jnumber(p["y"]),jnumber(p["z"])});}return true;
}
bool DataService::readTrajectory(long long id,TrajectorySurvey& t,std::string& e)const{
    t={};QSqlQuery r;if(!impl_->store.exec("SELECT name,payload FROM well_objects WHERE id=? AND kind='trajectories'",{id},e,&r))return false;if(!r.next()){e="测斜资料不存在";return false;}t.surveyName=s(r.value(0).toString());auto o=object(r.value(1).toString());t.azimuthReference=s(o["azimuthReference"].toString());for(auto item:o["points"].toArray()){auto p=item.toObject();t.surveyPoints.push_back({jnumber(p["md"]),jnumber(p["inclination"]),jnumber(p["azimuth"])});}return true;
}
bool DataService::assignTimeDepthWell(long long id,long long wellId,std::string& e){
    TimeDepthTable table;if(!readTimeDepth(id,table,e))return false;if(wellId<0){e="井编号无效";return false;}if(wellId){WellInfo w;if(!readWellInfo(wellId,w,e))return false;}
    auto& st=impl_->store;Transaction tx(st,e);if(!tx.active)return false;return st.exec("UPDATE well_objects SET well_id=? WHERE id=? AND kind='time_depth'",{wellId?QVariant(wellId):QVariant(),id},e)&&st.exec("INSERT INTO changes(kind,entity_id,description) VALUES('time_depth',?,?)",{id,QString("人工确认归井：%1，数值与来源不变").arg(wellId)},e)&&tx.commit(e);
}
}
