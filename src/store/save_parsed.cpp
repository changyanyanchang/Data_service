#include "project_store.h"
#include <QSqlError>
#include <QJsonDocument>

namespace paleo::data::detail {
namespace {
QJsonObject timeDepthJson(const TimeDepthTable& td){
    QJsonArray points;
    for(const auto& p:td.depthTimePairs)points.append(QJsonObject{
        {"depth",QJsonValue::fromVariant(number(p.depthValue))},
        {"time",QJsonValue::fromVariant(number(p.timeValue))},
        {"x",QJsonValue::fromVariant(number(p.x))},
        {"y",QJsonValue::fromVariant(number(p.y))},
        {"z",QJsonValue::fromVariant(number(p.z))}});
    return {{"depth",depthJson(td.depthInfo)},{"timeUnit",q(td.timeUnit)},
        {"timeType",q(td.timeType)},{"timeReference",q(td.timeReference)},
        {"positionDepth",depthJson(td.positionDepthInfo)},
        {"coordinateSystem",q(td.coordinateSystem)},{"coordinateUnit",q(td.coordinateUnit)},
        {"sourceSheet",q(td.sourceSheet)},{"points",points}};
}
}

bool ProjectStore::saveParsed(long long sourceId,const ParsedFile& file,const ParseOptions& options,std::string& e,const CancelCheck& cancelled,bool inputNullsResolved){
    auto insert=[&](const QString& sql,const QVariantList& args,long long& id){QSqlQuery r;if(!exec(sql,args,e,&r))return false;id=r.lastInsertId().toLongLong();return true;};
    auto insertRows=[&](QSqlQuery& query,const QVector<QVariantList>& columns){
        if(columns.isEmpty()||columns.front().isEmpty())return true;
        const auto rows=columns.front().size();
        for(const auto& column:columns)if(column.size()!=rows){e="批量写入列长度不一致";return false;}
        constexpr qsizetype chunkSize=256;
        const auto sql=query.lastQuery();
        for(qsizetype offset=0;offset<rows;offset+=chunkSize){
            if(cancelled&&cancelled()){e="已取消";return false;}
            for(const auto& column:columns)query.addBindValue(column.mid(offset,chunkSize));
            if(!query.execBatch()){e=s(query.lastError().text());return false;}
            query.clear();
            if(offset+chunkSize<rows&&!query.prepare(sql)){e=s(query.lastError().text());return false;}
        }
        return true;
    };
    for(const auto& issue:file.issues)if(!exec("INSERT INTO issues(source_id,code,message,original_text,sheet,row_num,blocking) VALUES(?,?,?,?,?,?,?)",{sourceId,q(issue.code),q(issue.message),q(issue.originalText),q(issue.sheet),issue.row,issue.blocking},e))return false;
    for(const auto& well:file.wells){
        long long wid;if(!ensureWell(well.wellInfo,wid,e))return false;
        const auto& w=well.wellInfo;
        if(std::isfinite(w.wellheadX)&&std::isfinite(w.wellheadY)){
            if(!saveCoordinateSystemCandidate(sourceId,"well",wid,w.coordinateSystem,w.coordinateUnit,e))return false;
            QSqlQuery old;if(!exec("SELECT x,y,crs,unit FROM wells WHERE id=?",{wid},e,&old))return false;old.next();
            if(!old.value(0).isNull()&&(old.value(0).toDouble()!=w.wellheadX||old.value(1).toDouble()!=w.wellheadY||old.value(2).toString()!=q(w.coordinateSystem)||old.value(3).toString()!=q(w.coordinateUnit))){
                if(!exec("INSERT INTO issues(source_id,code,message,original_text,blocking) VALUES(?,?,?,?,1)",{sourceId,"coordinate_conflict",q(w.wellName)+" 的井坐标与已保存记录不同，未覆盖",QString("%1,%2").arg(w.wellheadX,0,'g',16).arg(w.wellheadY,0,'g',16)},e))return false;
            }else if(!exec("UPDATE wells SET x=?,y=?,crs=?,unit=? WHERE id=?",{w.wellheadX,w.wellheadY,q(w.coordinateSystem),q(w.coordinateUnit),wid},e))return false;
        }
        QSqlQuery depthRow;if(!exec("SELECT depth_json FROM wells WHERE id=?",{wid},e,&depthRow)||!depthRow.next())return false;auto merged=object(depthRow.value(0).toString());const auto incoming=depthJson(well.defaultDepthInfo);bool depthConflict=false;
        for(auto it=incoming.begin();it!=incoming.end();++it){if(it.value().toString().isEmpty())continue;const auto previous=merged[it.key()].toString();if(previous.isEmpty())merged[it.key()]=it.value();else if(previous!=it.value().toString())depthConflict=true;}
        if(depthConflict){if(!exec("INSERT INTO issues(source_id,code,message,original_text,blocking) VALUES(?,?,?,?,1)",{sourceId,"well_depth_context_conflict",q(w.wellName)+" 的默认深度约定冲突，本批井数据未发布；请核对映射或明确转换",json(incoming)},e))return false;continue;}
        if(!exec("UPDATE wells SET depth_json=? WHERE id=?",{json(merged),wid},e))return false;
        for(const auto& log:well.wellbore.logSets){
            auto n=log.samplingAxis.depthValues.size();
            if((!log.samplingAxis.tvdValues.empty()&&log.samplingAxis.tvdValues.size()!=n)||(!log.samplingAxis.tvdssValues.empty()&&log.samplingAxis.tvdssValues.size()!=n)){e="附加深度轴长度不一致";return false;}
            long long sid;if(!insert("INSERT INTO log_sets(well_id,source_id,name,depth_json,depths,tvd,tvdss,step,sample_count,sheet) VALUES(?,?,?,?,?,?,?,?,?,?)",{wid,sourceId,q(log.logSetName),json(depthJson(log.depthInfo)),pack(log.samplingAxis.depthValues),pack(log.samplingAxis.tvdValues),pack(log.samplingAxis.tvdssValues),number(log.samplingAxis.sampleStep),qlonglong(n),q(log.sourceSheet)},sid))return false;
            for(const auto& curve:log.logCurves){
                if(curve.sampleValues.size()!=n){e="曲线与深度轴长度不一致";return false;}
                std::vector<double> effective=curve.sampleValues;
                QByteArray quality(qsizetype(n),char(0));bool changed=false;
                const CurveCleaningRule* rule=nullptr;
                for(const auto& candidate:options.cleaningRules)if(!candidate.curveType.empty()&&q(candidate.curveType).compare(q(curve.curveName),Qt::CaseInsensitive)==0){rule=&candidate;break;}
                bool rangeValid=rule&&(!rule->checkMinimum&&!rule->checkMaximum||(!rule->unit.empty()&&rule->unit==curve.curveUnit&&(!rule->checkMinimum||std::isfinite(rule->minimum))&&(!rule->checkMaximum||std::isfinite(rule->maximum))&&(!rule->checkMinimum||!rule->checkMaximum||rule->minimum<=rule->maximum)));
                if(rule&&!rangeValid&&!exec("INSERT INTO issues(source_id,code,message,blocking) VALUES(?,?,?,0)",{sourceId,"cleaning_rule_units",q(curve.curveName)+" 清理范围未应用：单位未确认、不匹配或阈值无效"},e))return false;
                auto nulls=!file.declaredNullValues.empty()?file.declaredNullValues:(rule&&!rule->nullValues.empty()?rule->nullValues:std::vector<double>{-99999,-9999,-999,-999.25});
                for(size_t i=0;i<n;++i){if(i%8192==0&&cancelled&&cancelled()){e="已取消";return false;}bool missing=!std::isfinite(effective[i]);if(!inputNullsResolved)for(double null:nulls)if(effective[i]==null)missing=true;if(missing){quality[qsizetype(i)]=1;if(std::isfinite(effective[i]))changed=true;effective[i]=Missing;}else if(rangeValid&&((rule->checkMinimum&&effective[i]<rule->minimum)||(rule->checkMaximum&&effective[i]>rule->maximum))){quality[qsizetype(i)]=2;if(rule->replaceInvalidWithMissing){effective[i]=Missing;changed=true;}}}
                QJsonArray declared;for(double v:file.declaredNullValues)declared.append(v);
                QJsonObject rules{{"declared_nulls",declared}};if(rule){rules["curve"]=q(rule->curveType);rules["unit"]=q(rule->unit);rules["rangeApplied"]=rangeValid;rules["checkMinimum"]=rule->checkMinimum;rules["minimum"]=rule->minimum;rules["checkMaximum"]=rule->checkMaximum;rules["maximum"]=rule->maximum;rules["replace"]=rule->replaceInvalidWithMissing;QJsonArray configured;for(double v:rule->nullValues)configured.append(v);rules["nulls"]=configured;}
                long long cid;if(!insert("INSERT INTO curves(set_id,name,unit,description,raw_values,processed_values,quality,rule_json) VALUES(?,?,?,?,?,?,?,?)",{sid,q(curve.curveName),q(curve.curveUnit),q(curve.description),pack(curve.sampleValues),changed?QVariant(pack(effective)):QVariant(),quality,json(rules)},cid))return false;
            }
        }
        for(const auto& track:well.wellbore.intervalTracks){
            long long tid;if(!insert("INSERT INTO tracks(well_id,source_id,name,type,depth_json,sheet) VALUES(?,?,?,?,?,?)",{wid,sourceId,q(track.trackName),q(track.trackType),json(depthJson(track.depthInfo)),q(track.sourceSheet)},tid))return false;
            QSqlQuery query(db);if(!query.prepare("INSERT INTO intervals(track_id,ordinal,original_id,name,top,bottom,top_tvd,bottom_tvd,top_ss,bottom_ss,facies,top_unc,bottom_unc,not_penetrated,properties,source_row) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")){e=s(query.lastError().text());return false;}
            QVector<QVariantList> columns(16);int order=0;
            for(const auto& v:track.intervals){int c=0;columns[c++]<<tid;columns[c++]<<order++;columns[c++]<<v.intervalId;columns[c++]<<q(v.intervalName);columns[c++]<<number(v.topDepth);columns[c++]<<number(v.bottomDepth);columns[c++]<<number(v.topTvd);columns[c++]<<number(v.bottomTvd);columns[c++]<<number(v.topTvdss);columns[c++]<<number(v.bottomTvdss);columns[c++]<<q(v.faciesType);columns[c++]<<v.topUnconformity;columns[c++]<<v.bottomUnconformity;columns[c++]<<v.bottomNotPenetrated;columns[c++]<<json(measurementsJson(v.properties));columns[c++]<<v.sourceRow;}
            if(!insertRows(query,columns))return false;
        }
        for(const auto& table:well.wellbore.sampleTables){
            long long tid;if(!insert("INSERT INTO sample_tables(well_id,source_id,name,sheet) VALUES(?,?,?,?)",{wid,sourceId,q(table.tableName),q(table.sourceSheet)},tid))return false;
            for(const auto& sample:table.samples){long long sid;if(!insert("INSERT INTO samples(table_id,number,type,source_range,top,bottom) VALUES(?,?,?,?,?,?)",{tid,q(sample.sampleNumber),q(sample.sampleType),q(sample.sourceRange),number(sample.topDepth),number(sample.bottomDepth)},sid))return false;
                QSqlQuery query(db);if(!query.prepare("INSERT INTO measurements(sample_id,name,unit,value,original_text) VALUES(?,?,?,?,?)")){e=s(query.lastError().text());return false;}
                QVector<QVariantList> columns(5);
                for(const auto& m:sample.measurements){int c=0;columns[c++]<<sid;columns[c++]<<q(m.measurementName);columns[c++]<<q(m.measurementUnit);columns[c++]<<number(m.numericValue);columns[c++]<<q(m.originalText);}
                if(!insertRows(query,columns))return false;
            }
        }
        for(const auto& trajectory:well.wellbore.trajectories){QJsonArray points;for(auto& p:trajectory.surveyPoints)points.append(QJsonObject{{"md",QJsonValue::fromVariant(number(p.measuredDepth))},{"inclination",QJsonValue::fromVariant(number(p.inclinationDegrees))},{"azimuth",QJsonValue::fromVariant(number(p.azimuthDegrees))}});if(!exec("INSERT INTO well_objects(well_id,source_id,kind,name,payload) VALUES(?,?,?,?,?)",{wid,sourceId,"trajectories",q(trajectory.surveyName),json(QJsonObject{{"azimuthReference",q(trajectory.azimuthReference)},{"points",points}})},e))return false;}
        for(const auto& td:well.timeDepthRelations){if(!exec("INSERT INTO well_objects(well_id,source_id,kind,name,payload) VALUES(?,?,?,?,?)",{wid,sourceId,"time_depth",q(td.relationName),json(timeDepthJson(td))},e))return false;}
        for(const auto& mt:well.wellbore.markerTracks){QJsonArray points;for(auto& p:mt.markers)points.append(QJsonObject{{"id",p.markerId},{"name",q(p.markerName)},{"depth",QJsonValue::fromVariant(number(p.depthValue))},{"tvd",QJsonValue::fromVariant(number(p.tvd))},{"tvdss",QJsonValue::fromVariant(number(p.tvdss))},{"row",p.sourceRow}});if(!exec("INSERT INTO well_objects(well_id,source_id,kind,name,payload) VALUES(?,?,?,?,?)",{wid,sourceId,"markers",q(mt.trackName),json(QJsonObject{{"depth",depthJson(mt.depthInfo)},{"points",points}})},e))return false;}
        for(const auto& ct:well.wellbore.corePhotoTracks){QJsonArray images;for(auto& p:ct.photos)images.append(QJsonObject{{"path",q(p.imagePath)},{"top",QJsonValue::fromVariant(number(p.topDepth))},{"bottom",QJsonValue::fromVariant(number(p.bottomDepth))},{"description",q(p.description)}});if(!exec("INSERT INTO well_objects(well_id,source_id,kind,name,payload) VALUES(?,?,?,?,?)",{wid,sourceId,"core_photos",q(ct.trackName),json(images)},e))return false;}
        for(const auto& a:well.attachments)if(!exec("INSERT INTO well_objects(well_id,source_id,kind,name,payload) VALUES(?,?,?,?,?)",{wid,sourceId,"attachments",q(a.attachmentName),json(QJsonObject{{"path",q(a.filePath)},{"type",q(a.attachmentType)},{"targetType",q(a.targetType)},{"targetId",a.targetId}})},e))return false;
    }
    for(const auto& v:file.volumes){QJsonObject g{{"traceCount",double(v.geometry.traceCount)},{"inlineMin",v.geometry.inlineMin},{"inlineMax",v.geometry.inlineMax},{"crosslineMin",v.geometry.crosslineMin},{"crosslineMax",v.geometry.crosslineMax},{"crs",q(v.geometry.coordinateSystem)},{"unit",q(v.geometry.coordinateUnit)}};QJsonObject sampling{{"sampleCount",v.sampling.sampleCount},{"firstTime",QJsonValue::fromVariant(number(v.sampling.firstTime))},{"sampleInterval",QJsonValue::fromVariant(number(v.sampling.sampleInterval))},{"timeUnit",q(v.sampling.timeUnit)},{"timeType",q(v.sampling.timeType)},{"timeReference",q(v.sampling.timeReference)}};
        if(!exec("INSERT INTO volumes(source_id,name,path,format,amplitude_unit,geometry,sampling) VALUES(?,?,?,?,?,?,?)",{sourceId,q(v.volumeName),q(v.dataFile),q(v.fileFormat),q(v.amplitudeUnit),json(g),json(sampling)},e))return false;}
    for(const auto& td:file.unassignedTimeDepth){if(!exec("INSERT INTO well_objects(well_id,source_id,kind,name,payload) VALUES(NULL,?,?,?,?)",{sourceId,"time_depth",q(td.relationName),json(timeDepthJson(td))},e))return false;}
    for(const auto& h:file.horizons){long long hid;if(!insert("INSERT INTO horizons(source_id,name,volume_id,crs,unit,time_unit,time_type,time_reference) VALUES(?,?,?,?,?,?,?,?)",{sourceId,q(h.horizonName),h.parentVolumeId?QVariant(h.parentVolumeId):QVariant(),q(h.coordinateSystem),q(h.coordinateUnit),q(h.timeUnit),q(h.timeType),q(h.timeReference)},hid))return false;
        QSqlQuery query(db);if(!query.prepare("INSERT INTO horizon_points(horizon_id,seq,inline_no,crossline_no,x,y,z) VALUES(?,?,?,?,?,?,?)")){e=s(query.lastError().text());return false;}
        QVector<QVariantList> columns(7);int seq=0;
        for(const auto& p:h.points){int c=0;columns[c++]<<hid;columns[c++]<<seq++;columns[c++]<<p.inlineNumber;columns[c++]<<p.crosslineNumber;columns[c++]<<number(p.x);columns[c++]<<number(p.y);columns[c++]<<number(p.zTime);}
        if(!insertRows(query,columns))return false;
    }
    for(const auto& m:file.maps){QJsonArray strata;for(auto& stratum:m.stratigraphicNames)strata.append(q(stratum));if(!exec("INSERT INTO maps(source_id,name,type,area,strata,image_path,source_file,source_location,crs,geo_status,geo_file,description) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",{sourceId,q(m.mapName),q(m.mapType),q(m.areaName),json(strata),q(m.imagePath),q(m.sourceFile),q(m.sourceLocation),q(m.coordinateSystem),m.georeferenceStatus,q(m.georeferenceFile),q(m.description)},e))return false;}
    for(const auto& t:file.sourceTables){
        long long tid;if(!insert("INSERT INTO source_tables(source_id,name,rows,columns) VALUES(?,?,?,?)",{sourceId,q(t.sheetName),t.rowCount,t.columnCount},tid))return false;
        QSqlQuery query(db);if(!query.prepare("INSERT INTO source_cells(table_id,row_num,col_num,text) VALUES(?,?,?,?)")){e=s(query.lastError().text());return false;}
        QVector<QVariantList> columns(4);
        for(const auto& cell:t.cells){int c=0;columns[c++]<<tid;columns[c++]<<cell.row;columns[c++]<<cell.column;columns[c++]<<q(cell.text);}
        if(!insertRows(query,columns))return false;
    }
    return saveParsedCoordinateSystems(sourceId,file,e);
}
}
