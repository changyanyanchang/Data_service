#include "store/project_store.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <algorithm>

namespace paleo::data {
using namespace detail;
static bool rowExists(ProjectStore& st,const QString& table,long long id,std::string& e){QSqlQuery r;if(!st.exec("SELECT 1 FROM "+table+" WHERE id=?",{id},e,&r))return false;if(!r.next()){e="记录不存在";return false;}return true;}
static bool recordChange(ProjectStore& st,const QString& kind,long long id,const QString& description,std::string& e){return st.exec("INSERT INTO changes(kind,entity_id,description) VALUES(?,?,?)",{kind,id,description},e);}
bool DataService::saveLogSetDepthInfo(long long id,const DepthInfo& value,std::string& e){
    e.clear();if(id<=0){e="曲线组编号无效";return false;}
    const auto supplied=depthJson(value);QJsonObject confirmed;
    for(auto it=supplied.begin();it!=supplied.end();++it){
        const auto text=it.value().toString();if(text.isEmpty())continue;
        const auto trimmed=text.trimmed();if(trimmed.isEmpty()){e="深度约定不能只有空白";return false;}
        confirmed.insert(it.key(),trimmed);
    }
    if(confirmed.isEmpty()){e="请提供至少一项已核对的深度约定";return false;}
    const auto type=confirmed.value("type").toString(),direction=confirmed.value("direction").toString();
    if(!type.isEmpty()&&type!="MD"&&type!="TVD"&&type!="TVDSS"){e="深度类型须为 MD、TVD 或 TVDSS";return false;}
    if(!direction.isEmpty()&&direction!="down"&&direction!="up"){e="TVDSS 正方向须为 down 或 up";return false;}
    auto& st=impl_->store;Transaction tx(st,e);if(!tx.active)return false;QSqlQuery row;
    if(!st.exec("SELECT depth_json FROM log_sets WHERE id=?",{id},e,&row))return false;
    if(!row.next()){e="曲线组不存在";return false;}const auto previous=object(row.value(0).toString());row.finish();auto updated=previous;
    for(auto it=confirmed.begin();it!=confirmed.end();++it){
        const auto old=previous.value(it.key()).toString();
        if(!old.isEmpty()&&old!=it.value().toString()){e="曲线组已有深度约定；单位或基准转换需要明确换算，不能仅改标签";return false;}
        updated.insert(it.key(),it.value());
    }
    if(updated==previous)return tx.commit(e);
    const auto description="依据已核对资料补充曲线组深度约定（原始轴与样点未改动）："+json(previous)+" → "+json(updated);
    return st.exec("UPDATE log_sets SET depth_json=? WHERE id=?",{json(updated),id},e)
        &&recordChange(st,"log_set",id,description,e)&&tx.commit(e);
}
bool DataService::createWell(const WellInfo& w,long long& id,std::string& e){id=0;QSqlQuery r;if(!impl_->store.exec("SELECT id FROM wells WHERE name=?",{q(w.wellName)},e,&r))return false;if(r.next()){e="同名井已存在";return false;}return impl_->store.ensureWell(w,id,e);}
bool DataService::saveWellInfo(long long id,const WellInfo& w,std::string& e){
    auto& st=impl_->store;if(!rowExists(st,"wells",id,e))return false;if(w.wellName.empty()){e="井名不能为空";return false;}
    if(!st.begin(e))return false;bool ok=st.exec("UPDATE wells SET name=?,field_name=?,well_type=?,x=?,y=?,crs=?,unit=?,total_depth=?,remark=? WHERE id=?",{q(w.wellName),q(w.fieldName),q(w.wellType),number(w.wellheadX),number(w.wellheadY),q(w.coordinateSystem),q(w.coordinateUnit),number(w.totalDrilledDepth),q(w.remark),id},e)&&recordChange(st,"well",id,"修改井基础信息",e)&&st.commit(e);if(!ok)st.rollback();return ok;
}
bool DataService::saveInterval(long long track,IntervalRecord& record,std::string& e){
    auto& st=impl_->store;auto& v=record.value;if(!rowExists(st,"tracks",track,e))return false;if(v.intervalName.empty()){e="层段名称不能为空";return false;}if(std::isfinite(v.topDepth)&&std::isfinite(v.bottomDepth)&&v.topDepth>v.bottomDepth){e="顶深不能大于底深";return false;}for(int flag:{v.topUnconformity,v.bottomUnconformity,v.bottomNotPenetrated})if(flag<-1||flag>1){e="状态须为-1、0、1";return false;}
    if(!st.begin(e))return false;QSqlQuery r;QVariantList args{v.displayOrder,v.intervalId,q(v.intervalName),number(v.topDepth),number(v.bottomDepth),number(v.topTvd),number(v.bottomTvd),number(v.topTvdss),number(v.bottomTvdss),q(v.faciesType),v.topUnconformity,v.bottomUnconformity,v.bottomNotPenetrated,json(measurementsJson(v.properties)),v.sourceRow};bool ok;
    if(record.id){args<<record.id<<track;ok=st.exec("UPDATE intervals SET ordinal=?,original_id=?,name=?,top=?,bottom=?,top_tvd=?,bottom_tvd=?,top_ss=?,bottom_ss=?,facies=?,top_unc=?,bottom_unc=?,not_penetrated=?,properties=?,source_row=? WHERE id=? AND track_id=?",args,e,&r);if(ok&&r.numRowsAffected()!=1){e="层段不存在或不属于该道";ok=false;}}
    else{args.prepend(track);ok=st.exec("INSERT INTO intervals(track_id,ordinal,original_id,name,top,bottom,top_tvd,bottom_tvd,top_ss,bottom_ss,facies,top_unc,bottom_unc,not_penetrated,properties,source_row) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",args,e,&r);if(ok)record.id=r.lastInsertId().toLongLong();}
    ok=ok&&recordChange(st,"interval",record.id,"保存层段："+q(v.intervalName),e)&&st.commit(e);if(!ok)st.rollback();record.trackId=track;return ok;
}
bool DataService::cleanCurve(long long id,const CurveCleaningRule& rule,std::string& e){
    auto& st=impl_->store;Transaction transaction(st,e);if(!transaction.active)return false;QSqlQuery r;if(!st.exec("SELECT name,unit,raw_values,rule_json FROM curves WHERE id=?",{id},e,&r))return false;if(!r.next()){e="曲线不存在";return false;}
    if(!rule.curveType.empty()&&q(rule.curveType).compare(r.value(0).toString(),Qt::CaseInsensitive)!=0){e="清理规则的曲线名称与目标不一致";return false;}
    if(rule.checkMinimum||rule.checkMaximum){if(rule.unit.empty()||r.value(1).toString().isEmpty()||q(rule.unit)!=r.value(1).toString()){e="范围清理须确认曲线单位与规则单位一致";return false;}if((rule.checkMinimum&&!std::isfinite(rule.minimum))||(rule.checkMaximum&&!std::isfinite(rule.maximum))||(rule.checkMinimum&&rule.checkMaximum&&rule.minimum>rule.maximum)){e="清理阈值无效";return false;}}
    std::vector<double> raw;if(!unpack(r.value(2).toByteArray(),raw,e))return false;auto stored=object(r.value(3).toString());auto declared=stored["declared_nulls"].toArray();std::vector<double> nulls;
    if(!declared.isEmpty())for(auto v:declared)nulls.push_back(v.toDouble());else nulls=rule.nullValues.empty()?std::vector<double>{-99999,-9999,-999,-999.25}:rule.nullValues;
    auto clean=raw;QByteArray quality(qsizetype(raw.size()),char(0));long long flagged=0;
    for(size_t i=0;i<raw.size();++i){double v=raw[i];bool missing=!std::isfinite(v)||std::find(nulls.begin(),nulls.end(),v)!=nulls.end();if(missing){clean[i]=Missing;quality[qsizetype(i)]=1;continue;}if((rule.checkMinimum&&v<rule.minimum)||(rule.checkMaximum&&v>rule.maximum)){++flagged;quality[qsizetype(i)]=2;if(rule.replaceInvalidWithMissing)clean[i]=Missing;}}
    QJsonArray configured;for(double v:rule.nullValues)configured.append(v);stored["curve"]=q(rule.curveType);stored["unit"]=q(rule.unit);stored["nulls"]=configured;stored["checkMinimum"]=rule.checkMinimum;stored["minimum"]=rule.minimum;stored["checkMaximum"]=rule.checkMaximum;stored["maximum"]=rule.maximum;stored["replace"]=rule.replaceInvalidWithMissing;
    return st.exec("UPDATE curves SET processed_values=?,quality=?,rule_json=? WHERE id=?",{pack(clean),quality,json(stored),id},e)&&recordChange(st,"curve",id,QString("重新清理（基于原始值），标记 %1 个样点；规则 %2").arg(flagged).arg(json(stored)),e)&&transaction.commit(e);
}
bool DataService::updateCurveValue(long long id,long long index,double v,std::string& e){
    if(std::isinf(v)){e="不允许写入无穷值";return false;}auto& st=impl_->store;Transaction transaction(st,e);if(!transaction.active)return false;QSqlQuery r;if(!st.exec("SELECT raw_values,processed_values,quality FROM curves WHERE id=?",{id},e,&r))return false;if(!r.next()){e="曲线不存在";return false;}
    std::vector<double> current;if(!unpack((r.value(1).isNull()?r.value(0):r.value(1)).toByteArray(),current,e))return false;if(index<0||index>=static_cast<long long>(current.size())){e="样点索引越界";return false;}auto quality=r.value(2).toByteArray();if(size_t(quality.size())!=current.size()){e="质量数组长度损坏";return false;}double old=current[size_t(index)];current[size_t(index)]=v;quality[qsizetype(index)]=4;
    return st.exec("UPDATE curves SET processed_values=?,quality=? WHERE id=?",{pack(current),quality,id},e)&&recordChange(st,"curve",id,QString("样点 %1：%2 → %3").arg(index).arg(std::isfinite(old)?QString::number(old,'g',17):"Missing").arg(std::isfinite(v)?QString::number(v,'g',17):"Missing"),e)&&transaction.commit(e);
}
bool DataService::resetCurve(long long id,std::string& e){CurveCleaningRule rule;return cleanCurve(id,rule,e);}
bool DataService::saveCurveUnit(long long id,const std::string& unit,std::string& e){
    const auto confirmed=q(unit).trimmed();if(confirmed.isEmpty()){e="确认的单位不能为空";return false;}auto& st=impl_->store;Transaction tx(st,e);if(!tx.active)return false;QSqlQuery r;if(!st.exec("SELECT unit FROM curves WHERE id=?",{id},e,&r))return false;if(!r.next()){e="曲线不存在";return false;}const auto previous=r.value(0).toString();if(!previous.isEmpty()&&previous!=confirmed){e="该曲线已有单位；单位转换需要明确换算，不能仅改标签";return false;}return st.exec("UPDATE curves SET unit=? WHERE id=?",{confirmed,id},e)&&recordChange(st,"curve",id,"依据人工核对确认单位："+confirmed,e)&&tx.commit(e);
}
bool DataService::resolveIssue(long long id,const std::string& resolution,std::string& e){if(resolution.empty()){e="请填写核对依据或处理说明";return false;}auto& st=impl_->store;if(!rowExists(st,"issues",id,e))return false;return st.exec("UPDATE issues SET resolved=1,resolution=? WHERE id=?",{q(resolution),id},e);}
bool DataService::deleteData(const std::string& kind,long long id,std::string& e){
    if(kind=="core_photos"||kind=="attachments"||kind=="markers"||kind=="trajectories"||kind=="time_depth"){
        auto& st=impl_->store;Transaction tx(st,e);if(!tx.active)return false;QSqlQuery r;
        if(!st.exec("SELECT 1 FROM well_objects WHERE id=? AND kind=?",{id,q(kind)},e,&r))return false;
        if(!r.next()){e="该类别中不存在这条记录";return false;}r.finish();
        return st.exec("DELETE FROM well_objects WHERE id=? AND kind=?",{id,q(kind)},e)&&recordChange(st,q(kind),id,"删除业务记录，保留原始文件",e)&&tx.commit(e);
    }
    QString table;if(kind=="curves")table="curves";else if(kind=="interval")table="intervals";else if(kind=="intervals")table="tracks";else if(kind=="samples")table="sample_tables";else if(kind=="horizons")table="horizons";else if(kind=="maps")table="maps";else if(kind=="volumes")table="volumes";else if(kind=="sections")table="sections";else if(kind=="tables")table="source_tables";else if(kind=="well"||kind=="wells")table="wells";else if(kind=="source")table="sources";else{e="不支持删除该类别";return false;}
    auto& st=impl_->store;if(!rowExists(st,table,id,e))return false;if(!st.begin(e))return false;
    bool ok=true;if(kind=="source")ok=st.removeSourceObjects(id,e)&&st.exec("UPDATE sources SET status='removed',message='业务数据已移除，原始资料仍保留' WHERE id=?",{id},e);else ok=st.exec("DELETE FROM "+table+" WHERE id=?",{id},e);
    if(kind=="source")ok=ok&&st.updateProjectCoordinateSystem(e);
    ok=ok&&recordChange(st,q(kind),id,"删除业务记录，保留原始文件",e)&&st.commit(e);if(!ok){st.rollback();if(e.find("FOREIGN KEY")!=std::string::npos)e="该对象仍被其他数据引用，请先处理关联";}return ok;
}
bool DataService::readMap(long long id,GeologicalMap& m,std::string& e)const{
    QSqlQuery r;if(!impl_->store.exec("SELECT name,type,area,strata,image_path,source_file,source_location,crs,geo_status,geo_file,description FROM maps WHERE id=?",{id},e,&r))return false;if(!r.next()){e="图件不存在";return false;}m={};m.mapId=int(id);m.mapName=s(r.value(0).toString());m.mapType=s(r.value(1).toString());m.areaName=s(r.value(2).toString());for(auto n:QJsonDocument::fromJson(r.value(3).toString().toUtf8()).array())m.stratigraphicNames.push_back(s(n.toString()));m.imagePath=s(r.value(4).toString());m.sourceFile=s(r.value(5).toString());m.sourceLocation=s(r.value(6).toString());m.coordinateSystem=s(r.value(7).toString());m.georeferenceStatus=r.value(8).toInt();m.georeferenceFile=s(r.value(9).toString());m.description=s(r.value(10).toString());return true;
}
bool DataService::saveMap(long long& id,const GeologicalMap& m,std::string& e){
    if(m.mapName.empty()||m.imagePath.empty()||m.sourceFile.empty()){e="请填写图名、图片路径与来源";return false;}if(m.georeferenceStatus<-1||m.georeferenceStatus>1||(m.georeferenceStatus==1&&(m.coordinateSystem.empty()||m.georeferenceFile.empty()))){e="配准状态或配准依据不完整";return false;}QJsonArray names;for(auto& n:m.stratigraphicNames)names.append(q(n));
    auto& st=impl_->store;QVariantList args{q(m.mapName),q(m.mapType),q(m.areaName),json(names),q(m.imagePath),q(m.sourceFile),q(m.sourceLocation),q(m.coordinateSystem),m.georeferenceStatus,q(m.georeferenceFile),q(m.description)};QSqlQuery r;
    if(id){if(!rowExists(st,"maps",id,e))return false;args<<id;return st.exec("UPDATE maps SET name=?,type=?,area=?,strata=?,image_path=?,source_file=?,source_location=?,crs=?,geo_status=?,geo_file=?,description=? WHERE id=?",args,e);}
    if(!st.exec("INSERT INTO maps(name,type,area,strata,image_path,source_file,source_location,crs,geo_status,geo_file,description) VALUES(?,?,?,?,?,?,?,?,?,?,?)",args,e,&r))return false;id=r.lastInsertId().toLongLong();return true;
}
bool DataService::saveSection(long long& id,const SeismicSection& a,std::string& e){
    if(a.sectionName.empty()||a.pathPoints.size()<2){e="剖面需要名称和至少两个路径点";return false;}for(size_t i=0;i<a.pathPoints.size();++i){auto p=a.pathPoints[i];if(!std::isfinite(p.x)||!std::isfinite(p.y)){e="路径坐标必须为有限数值";return false;}if(i&&p.x==a.pathPoints[i-1].x&&p.y==a.pathPoints[i-1].y){e="连续路径点不能重合";return false;}}
    auto& st=impl_->store;if(a.parentVolumeId&&!rowExists(st,"volumes",a.parentVolumeId,e))return false;if(id&&!rowExists(st,"sections",id,e))return false;if(!st.begin(e))return false;QSqlQuery r;QVariant volume=a.parentVolumeId?QVariant(a.parentVolumeId):QVariant();bool ok;
    if(id)ok=st.exec("UPDATE sections SET name=?,volume_id=?,crs=?,unit=? WHERE id=?",{q(a.sectionName),volume,q(a.coordinateSystem),q(a.coordinateUnit),id},e)&&st.exec("DELETE FROM section_points WHERE section_id=?",{id},e);
    else{ok=st.exec("INSERT INTO sections(name,volume_id,crs,unit) VALUES(?,?,?,?)",{q(a.sectionName),volume,q(a.coordinateSystem),q(a.coordinateUnit)},e,&r);if(ok)id=r.lastInsertId().toLongLong();}
    for(size_t i=0;ok&&i<a.pathPoints.size();++i)ok=st.exec("INSERT INTO section_points VALUES(?,?,?,?)",{id,qlonglong(i),a.pathPoints[i].x,a.pathPoints[i].y},e);
    ok=ok&&st.commit(e);if(!ok)st.rollback();return ok;
}
bool DataService::readSection(long long id,SeismicSection& a,std::string& e)const{a={};QSqlQuery r;if(!impl_->store.exec("SELECT name,volume_id,crs,unit FROM sections WHERE id=?",{id},e,&r))return false;if(!r.next()){e="剖面不存在";return false;}a.sectionName=s(r.value(0).toString());a.parentVolumeId=r.value(1).toInt();a.coordinateSystem=s(r.value(2).toString());a.coordinateUnit=s(r.value(3).toString());if(!impl_->store.exec("SELECT x,y FROM section_points WHERE section_id=? ORDER BY seq",{id},e,&r))return false;while(r.next())a.pathPoints.push_back({value(r.value(0)),value(r.value(1))});return true;}
bool DataService::backupDatabase(const std::string& dest,std::string& e)const{if(QFileInfo::exists(q(dest))){e="备份目标已存在，请选择新文件";return false;}return impl_->store.exec("VACUUM INTO ?",{q(dest)},e);}
}
