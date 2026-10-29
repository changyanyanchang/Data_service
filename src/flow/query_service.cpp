#include "store/project_store.h"
#include <QDir>
#include <QJsonDocument>
#include <algorithm>
#include <limits>

namespace paleo::data {
using namespace detail;
DataService::DataService():impl_(std::make_unique<Impl>()){}
DataService::~DataService()=default;
bool DataService::createProject(const std::string& path,const std::string& name,std::string& error){if(name.empty()){error="工区名称不能为空";return false;}return impl_->store.open(q(path),true,q(name),error);}
bool DataService::openProject(const std::string& path,std::string& error){
    if(!impl_->store.open(q(path),false,{},error))return false;
    if(!impl_->store.updateProjectCoordinateSystem(error)){impl_->store.close();return false;}
    return true;
}
bool DataService::openProjectReadOnly(const std::string& path,std::string& error){return impl_->store.open(q(path),false,{},error,true);}
bool DataService::snapshotDatabase(const std::string& destination,std::string& error)const{
    if(!impl_->store.db.isOpen()){error="请先打开工区";return false;}
    return impl_->store.exec("VACUUM INTO ?",{q(destination)},error);
}
bool DataService::isReadOnly()const{return impl_->store.readOnly;}
void DataService::closeProject(){impl_->store.close();}
std::string DataService::projectPath()const{return s(impl_->store.root);}
std::string DataService::projectName()const{return s(impl_->store.name);}
std::string DataService::projectCrs()const{QSqlQuery r;std::string e;if(!impl_->store.exec("SELECT value FROM ds_meta WHERE key='project_crs'",{},e,&r)||!r.next())return {};return s(r.value(0).toString());}
std::string DataService::projectCrsUnit()const{QSqlQuery r;std::string e;if(!impl_->store.exec("SELECT value FROM ds_meta WHERE key='project_crs_unit'",{},e,&r)||!r.next())return {};return s(r.value(0).toString());}
bool DataService::setProjectCrs(const std::string& epsg,const std::string& unit,std::string& e){
    if(impl_->store.readOnly){e="只读工区不能修改坐标系";return false;}
    const auto code=q(epsg).trimmed().toUpper();bool digits=false;const auto number=code.mid(5).toLongLong(&digits);
    if(!code.startsWith("EPSG:")||!digits||number<=0){e="坐标系须采用 EPSG:<数字> 格式";return false;}
    return impl_->store.exec("INSERT OR REPLACE INTO ds_meta(key,value) VALUES('project_crs',?),('project_crs_unit',?),('project_crs_basis','manual')",{code,q(unit).trimmed()},e);
}
bool DataService::listWells(std::vector<WellSummary>& out,std::string& e)const{
    out.clear();QSqlQuery r;if(!impl_->store.exec("SELECT w.id,w.name,(SELECT count(*) FROM log_sets WHERE well_id=w.id),(SELECT count(*) FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE l.well_id=w.id),(SELECT count(*) FROM tracks WHERE well_id=w.id),(SELECT count(*) FROM samples a JOIN sample_tables t ON t.id=a.table_id WHERE t.well_id=w.id),(SELECT count(*) FROM well_objects WHERE well_id=w.id AND kind='time_depth'),(SELECT count(*) FROM well_objects WHERE well_id=w.id AND kind='trajectories'),(w.x IS NOT NULL AND w.y IS NOT NULL),(SELECT coalesce(sum(json_array_length(payload)),0) FROM well_objects WHERE well_id=w.id AND kind='core_photos'),(SELECT count(*) FROM well_objects WHERE well_id=w.id AND kind='attachments'),(SELECT count(*) FROM well_objects WHERE well_id=w.id AND kind='markers') FROM wells w ORDER BY name",{},e,&r))return false;
    while(r.next())out.push_back({r.value(0).toLongLong(),s(r.value(1).toString()),r.value(2).toInt(),r.value(3).toInt(),r.value(4).toInt(),r.value(5).toInt(),r.value(6).toInt(),r.value(7).toInt(),r.value(8).toBool(),r.value(9).toInt(),r.value(10).toInt(),r.value(11).toInt()});return true;
}
bool DataService::readWellInfo(long long id,WellInfo& w,std::string& e)const{
    QSqlQuery r;if(!impl_->store.exec("SELECT name,field_name,well_type,x,y,crs,unit,total_depth,remark FROM wells WHERE id=?",{id},e,&r))return false;if(!r.next()){e="井不存在";return false;}w.wellName=s(r.value(0).toString());w.fieldName=s(r.value(1).toString());w.wellType=s(r.value(2).toString());w.wellheadX=value(r.value(3));w.wellheadY=value(r.value(4));w.coordinateSystem=s(r.value(5).toString());w.coordinateUnit=s(r.value(6).toString());w.totalDrilledDepth=value(r.value(7));w.remark=s(r.value(8).toString());return true;
}
bool DataService::listWellInfos(std::vector<WellInfoRecord>& out,std::string& e)const{
    out.clear();QSqlQuery r;
    if(!impl_->store.exec("SELECT id,name,field_name,well_type,x,y,crs,unit,total_depth,remark FROM wells ORDER BY id",{},e,&r))return false;
    while(r.next()){
        WellInfoRecord row;row.id=r.value(0).toLongLong();auto& w=row.value;
        w.wellName=s(r.value(1).toString());w.fieldName=s(r.value(2).toString());w.wellType=s(r.value(3).toString());
        w.wellheadX=value(r.value(4));w.wellheadY=value(r.value(5));w.coordinateSystem=s(r.value(6).toString());
        w.coordinateUnit=s(r.value(7).toString());w.totalDrilledDepth=value(r.value(8));w.remark=s(r.value(9).toString());out.push_back(std::move(row));
    }return true;
}
static bool listEntries(const ProjectStore& store,const std::string& kind,long long wellId,long long sourceId,std::vector<DataEntry>& out,std::string& e){
    out.clear();QString sql;QVariantList args;
    if(kind=="curves")sql="SELECT c.id,l.well_id,l.source_id,l.id,c.name,c.unit||' / '||l.name,l.sample_count FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE (?<=0 OR l.well_id=?) ORDER BY l.id,c.id";
    else if(kind=="intervals")sql="SELECT t.id,t.well_id,t.source_id,0,t.name,t.type,(SELECT count(*) FROM intervals WHERE track_id=t.id) FROM tracks t WHERE (?<=0 OR t.well_id=?) ORDER BY t.id";
    else if(kind=="samples")sql="SELECT t.id,t.well_id,t.source_id,0,t.name,t.sheet,(SELECT count(*) FROM samples WHERE table_id=t.id) FROM sample_tables t WHERE (?<=0 OR t.well_id=?) ORDER BY t.id";
    else if(kind=="horizons")sql="SELECT h.id,0,h.source_id,COALESCE(h.volume_id,0),h.name,h.crs||' / '||h.time_unit,(SELECT count(*) FROM horizon_points WHERE horizon_id=h.id) FROM horizons h ORDER BY h.id";
    else if(kind=="maps")sql="SELECT id,0,COALESCE(source_id,0),0,name,type||' / '||area||' / '||strata,1 FROM maps ORDER BY id";
    else if(kind=="volumes")sql="SELECT id,0,source_id,0,name,path,0 FROM volumes ORDER BY id";
    else if(kind=="sections")sql="SELECT s.id,0,0,COALESCE(s.volume_id,0),s.name,s.crs,(SELECT count(*) FROM section_points WHERE section_id=s.id) FROM sections s ORDER BY s.id";
    else if(kind=="tables")sql="SELECT t.id,0,t.source_id,0,t.name,s.original_path,t.rows FROM source_tables t JOIN sources s ON s.id=t.source_id WHERE s.active=1 ORDER BY t.id";
    else if(kind=="markers"||kind=="time_depth"||kind=="trajectories"||kind=="core_photos"||kind=="attachments"){sql="SELECT id,well_id,source_id,0,name,kind,CASE WHEN kind='attachments' THEN 1 ELSE coalesce(json_array_length(payload,'$.points'),json_array_length(payload),0) END FROM well_objects WHERE kind=? AND (?<=0 OR well_id=?)";args<<q(kind);}
    else{e="未知数据类别";return false;}
    if(sql.contains("well_id=?"))args<<wellId<<wellId;
    if(sourceId>0){sql="WITH data(id,well_id,source_id,parent_id,name,detail,n) AS ("+sql+") SELECT * FROM data WHERE source_id=? ORDER BY parent_id,id";args<<sourceId;}
    QSqlQuery r;if(!store.exec(sql,args,e,&r))return false;while(r.next()){DataEntry v;v.id=r.value(0).toLongLong();v.wellId=r.value(1).toLongLong();v.sourceId=r.value(2).toLongLong();v.parentId=r.value(3).toLongLong();v.name=s(r.value(4).toString());v.kind=kind;v.detail=s(r.value(5).toString());v.count=r.value(6).toLongLong();out.push_back(std::move(v));}return true;
}
bool DataService::listData(const std::string& kind,long long wellId,std::vector<DataEntry>& out,std::string& e)const{return listEntries(impl_->store,kind,wellId,0,out,e);}
bool DataService::listSourceData(const std::string& kind,long long sourceId,std::vector<DataEntry>& out,std::string& e)const{
    if(sourceId<=0){out.clear();e="来源编号无效";return false;}
    return listEntries(impl_->store,kind,0,sourceId,out,e);
}
bool DataService::readCurveSlice(long long id,double top,double bottom,bool processed,CurveSlice& out,std::string& e)const{
    out={};if(std::isfinite(top)&&std::isfinite(bottom)&&top>bottom){e="顶深不能大于底深";return false;}
    QSqlQuery r;if(!impl_->store.exec("SELECT c.name,c.unit,l.depth_json,l.depths,c.raw_values,c.processed_values,c.quality FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE c.id=?",{id},e,&r))return false;if(!r.next()){e="曲线不存在";return false;}
    std::vector<double> depths,raw,used;if(!unpack(r.value(3).toByteArray(),depths,e)||!unpack(r.value(4).toByteArray(),raw,e))return false;
    if(processed&&!r.value(5).isNull()){if(!unpack(r.value(5).toByteArray(),used,e))return false;}else used=raw;
    auto quality=r.value(6).toByteArray();if(raw.size()!=depths.size()||used.size()!=depths.size()||size_t(quality.size())!=depths.size()){e="曲线数组长度损坏";return false;}
    out.curveId=id;out.name=s(r.value(0).toString());out.unit=s(r.value(1).toString());out.depthUnit=readDepth(object(r.value(2).toString())).depthUnit;
    for(size_t i=0;i<depths.size();++i){if((std::isfinite(top)&&(!std::isfinite(depths[i])||depths[i]<top))||(std::isfinite(bottom)&&(!std::isfinite(depths[i])||depths[i]>bottom)))continue;out.depths.push_back(depths[i]);out.rawValues.push_back(raw[i]);out.values.push_back(used[i]);out.quality.push_back(static_cast<unsigned char>(quality[qsizetype(i)]));}return true;
}
static IntervalRecord intervalFromRow(const QSqlQuery& r){IntervalRecord v;v.id=r.value(0).toLongLong();v.trackId=r.value(1).toLongLong();auto& a=v.value;a.displayOrder=r.value(2).toInt();a.intervalId=r.value(3).toInt();a.intervalName=s(r.value(4).toString());a.topDepth=value(r.value(5));a.bottomDepth=value(r.value(6));a.topTvd=value(r.value(7));a.bottomTvd=value(r.value(8));a.topTvdss=value(r.value(9));a.bottomTvdss=value(r.value(10));a.faciesType=s(r.value(11).toString());a.topUnconformity=r.value(12).toInt();a.bottomUnconformity=r.value(13).toInt();a.bottomNotPenetrated=r.value(14).toInt();a.properties=readMeasurements(QJsonDocument::fromJson(r.value(15).toString().toUtf8()).array());a.sourceRow=r.value(16).toInt();return v;}
bool DataService::readIntervals(long long track,double top,double bottom,std::vector<IntervalRecord>& out,std::string& e)const{
    out.clear();if(std::isfinite(top)&&std::isfinite(bottom)&&top>bottom){e="深度范围错误";return false;}
    QSqlQuery r;if(!impl_->store.exec("SELECT id,track_id,ordinal,original_id,name,top,bottom,top_tvd,bottom_tvd,top_ss,bottom_ss,facies,top_unc,bottom_unc,not_penetrated,properties,source_row FROM intervals WHERE track_id=? AND (? IS NULL OR bottom IS NULL OR bottom>=?) AND (? IS NULL OR top IS NULL OR top<=?) ORDER BY ordinal,id",{track,number(top),number(top),number(bottom),number(bottom)},e,&r))return false;while(r.next())out.push_back(intervalFromRow(r));return true;
}
static std::string numericText(double v){return std::isfinite(v)?s(QString::number(v,'g',16)):std::string();}
bool DataService::readTable(const std::string& kind,long long id,long long offset,int limit,DataTable& out,std::string& e)const{
    out={};if(offset<0||limit<1||limit>5000){e="分页范围错误（每页1到5000行）";return false;}auto& store=impl_->store;
    if(kind=="curves"){
        CurveSlice curve;if(!readCurveSlice(id,Missing,Missing,true,curve,e))return false;out.columns={"sampleIndex","深度","原始值","处理值","质量标记"};out.totalRows=static_cast<long long>(curve.depths.size());for(long long i=offset;i<std::min(out.totalRows,offset+limit);++i)out.rows.push_back({std::to_string(i),numericText(curve.depths[size_t(i)]),numericText(curve.rawValues[size_t(i)]),numericText(curve.values[size_t(i)]),std::to_string(curve.quality[size_t(i)])});return true;
    }
    if(kind=="tables"){
        QSqlQuery info;if(!store.exec("SELECT rows,columns FROM source_tables WHERE id=?",{id},e,&info)||!info.next()){if(e.empty())e="来源工作表不存在";return false;}
        out.totalRows=info.value(0).toLongLong();int cols=info.value(1).toInt();if(cols<0||cols>16384||1LL*cols*limit>2000000){e="表格窗口过大，请减小每页行数";return false;}out.columns={"row"};for(int c=0;c<cols;++c){QString label;int k=c+1;while(k){label.prepend(QChar('A'+(k-1)%26));k=(k-1)/26;}out.columns.push_back(s(label));}
        auto end=std::min(out.totalRows,offset+limit);for(long long row=offset;row<end;++row){std::vector<std::string> cells(size_t(cols)+1);cells[0]=std::to_string(row+1);out.rows.push_back(std::move(cells));}
        QSqlQuery r;if(!store.exec("SELECT row_num,col_num,text FROM source_cells WHERE table_id=? AND row_num>? AND row_num<=? ORDER BY row_num,col_num",{id,offset,end},e,&r))return false;while(r.next()){auto row=r.value(0).toLongLong()-1-offset;int col=r.value(1).toInt();if(row>=0&&row<static_cast<long long>(out.rows.size())&&col>0&&col<=cols)out.rows[size_t(row)][size_t(col)]=s(r.value(2).toString());}return true;
    }
    if(kind=="time_depth"){
        TimeDepthTable t;if(!readTimeDepth(id,t,e))return false;out.columns={"sampleIndex","深度 ("+t.depthInfo.depthType+" / "+t.depthInfo.depthUnit+")","时间 ("+t.timeType+" / "+t.timeUnit+")"};out.totalRows=static_cast<long long>(t.depthTimePairs.size());for(long long i=offset;i<std::min(out.totalRows,offset+limit);++i){auto p=t.depthTimePairs[size_t(i)];out.rows.push_back({std::to_string(i),numericText(p.depthValue),numericText(p.timeValue)});}return true;
    }
    if(kind=="trajectories"){
        TrajectorySurvey t;if(!readTrajectory(id,t,e))return false;out.columns={"sampleIndex","MD","井斜角 (度)","方位角 (度)"};out.totalRows=static_cast<long long>(t.surveyPoints.size());for(long long i=offset;i<std::min(out.totalRows,offset+limit);++i){auto p=t.surveyPoints[size_t(i)];out.rows.push_back({std::to_string(i),numericText(p.measuredDepth),numericText(p.inclinationDegrees),numericText(p.azimuthDegrees)});}return true;
    }
    if(kind=="core_photos"){
        CorePhotoTrack track;if(!readCorePhotoTrack(id,track,e))return false;
        out.columns={"照片序号","顶深","底深","文件路径","说明"};out.totalRows=static_cast<long long>(track.photos.size());
        for(long long i=offset;i<std::min(out.totalRows,offset+limit);++i){const auto& p=track.photos[size_t(i)];out.rows.push_back({std::to_string(i),numericText(p.topDepth),numericText(p.bottomDepth),p.imagePath,p.description});}return true;
    }
    if(kind=="attachments"){
        Attachment a;if(!readAttachment(id,a,e))return false;out.columns={"名称","类别","文件路径","关联类型","关联编号"};out.totalRows=1;
        if(offset==0)out.rows.push_back({a.attachmentName,a.attachmentType,a.filePath,a.targetType,std::to_string(a.targetId)});return true;
    }
    QString sql,count;QVariantList args{id};
    if(kind=="intervals"){out.columns={"id","层段","顶深","底深","顶TVD","底TVD","顶TVDSS","底TVDSS","相类型","顶不整合","底不整合","底未钻穿","来源行","附加属性"};sql="SELECT id,name,top,bottom,top_tvd,bottom_tvd,top_ss,bottom_ss,facies,top_unc,bottom_unc,not_penetrated,source_row,properties FROM intervals WHERE track_id=? ORDER BY ordinal,id";count="SELECT count(*) FROM intervals WHERE track_id=?";}
    else if(kind=="samples"){out.columns={"id","样品号","类型","顶深","底深","指标","单位","数值","原文","来源单元格"};sql="SELECT a.id,a.number,a.type,a.top,a.bottom,m.name,m.unit,m.value,m.original_text,a.source_range FROM samples a LEFT JOIN measurements m ON m.sample_id=a.id WHERE a.table_id=? ORDER BY a.id,m.id";count="SELECT count(*) FROM samples a LEFT JOIN measurements m ON m.sample_id=a.id WHERE a.table_id=?";}
    else if(kind=="horizons"){out.columns={"id","INLINE","XLINE","X","Y","Z时间"};sql="SELECT id,inline_no,crossline_no,x,y,z FROM horizon_points WHERE horizon_id=? ORDER BY seq,id";count="SELECT count(*) FROM horizon_points WHERE horizon_id=?";}
    else if(kind=="sections"){out.columns={"点顺序","X","Y"};sql="SELECT seq,x,y FROM section_points WHERE section_id=? ORDER BY seq";count="SELECT count(*) FROM section_points WHERE section_id=?";}
    else if(kind=="volumes"){out.columns={"id","名称","路径","格式","振幅单位","空间信息","采样信息"};sql="SELECT id,name,path,format,amplitude_unit,geometry,sampling FROM volumes WHERE id=?";count="SELECT count(*) FROM volumes WHERE id=?";}
    else if(kind=="maps"){out.columns={"id","名称","类型","地区","地层","图片","来源","来源位置","坐标系","配准状态","配准资料","说明"};sql="SELECT id,name,type,area,strata,image_path,source_file,source_location,crs,geo_status,geo_file,description FROM maps WHERE id=?";count="SELECT count(*) FROM maps WHERE id=?";}
    else if(kind=="markers"||kind=="core_photos"||kind=="attachments"){out.columns={"id","名称","类型","数据"};sql="SELECT id,name,kind,payload FROM well_objects WHERE id=? AND kind=?";count="SELECT count(*) FROM well_objects WHERE id=? AND kind=?";args<<q(kind);}
    else{e="未知数据类别";return false;}
    QSqlQuery r;if(!store.exec(count,args,e,&r))return false;r.next();out.totalRows=r.value(0).toLongLong();args<<limit<<offset;if(!store.exec(sql+" LIMIT ? OFFSET ?",args,e,&r))return false;
    while(r.next()){std::vector<std::string> row;for(size_t i=0;i<out.columns.size();++i)row.push_back(s(r.value(int(i)).toString()));out.rows.push_back(std::move(row));}return true;
}
bool DataService::listSources(std::vector<SourceEntry>& out,std::string& e)const{out.clear();QSqlQuery r;if(!impl_->store.exec("SELECT id,original_path,kind,status,message,bytes FROM sources ORDER BY id DESC",{},e,&r))return false;while(r.next())out.push_back({r.value(0).toLongLong(),s(r.value(1).toString()),s(r.value(2).toString()),s(r.value(3).toString()),s(r.value(4).toString()),r.value(5).toLongLong()});return true;}
bool DataService::listSourceIndexEntries(std::vector<SourceIndexEntry>& out,std::string& e)const{
    out.clear();QSqlQuery r;
    if(!impl_->store.exec("SELECT id,original_path,stored_path,kind,status,message,bytes,hash FROM sources ORDER BY id DESC",{},e,&r))return false;
    while(r.next()){
        SourceIndexEntry row;row.id=r.value(0).toLongLong();row.path=s(r.value(1).toString());row.storedPath=s(r.value(2).toString());
        row.kind=s(r.value(3).toString());row.status=s(r.value(4).toString());row.message=s(r.value(5).toString());
        row.bytes=r.value(6).toLongLong();row.sha256=s(r.value(7).toString());out.push_back(std::move(row));
    }return true;
}
bool DataService::listIssues(bool unresolved,std::vector<IssueEntry>& out,std::string& e)const{out.clear();QSqlQuery r;if(!impl_->store.exec("SELECT id,source_id,code,message,original_text,sheet,resolution,row_num,blocking,resolved FROM issues WHERE (?=0 OR resolved=0) ORDER BY blocking DESC,id",{unresolved},e,&r))return false;while(r.next())out.push_back({r.value(0).toLongLong(),r.value(1).toLongLong(),s(r.value(2).toString()),s(r.value(3).toString()),s(r.value(4).toString()),s(r.value(5).toString()),s(r.value(6).toString()),r.value(7).toInt(),r.value(8).toBool(),r.value(9).toBool()});return true;}
bool DataService::sourcePath(long long id,std::string& path,std::string& e)const{QSqlQuery r;if(!impl_->store.exec("SELECT stored_path FROM sources WHERE id=?",{id},e,&r))return false;if(!r.next()){e="来源记录不存在";return false;}path=s(impl_->store.absolutePath(r.value(0).toString()));return true;}
bool DataService::readDocument(long long id,std::string& text,std::string& e)const{QSqlQuery r;if(!impl_->store.exec("SELECT document FROM sources WHERE id=?",{id},e,&r))return false;if(!r.next()){e="来源记录不存在";return false;}text=s(r.value(0).toString());return true;}
}
