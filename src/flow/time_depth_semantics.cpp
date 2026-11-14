#include "store/project_store.h"
#include "paleo_data/object_descriptor.h"
#include <utility>
#include <set>

namespace paleo::data {
using namespace detail;
bool DataService::confirmTimeDepthSemantics(long long id,const std::string& unit,
    const std::string& type,const std::string& reference,bool negate,std::string& error){
    return confirmTimeDepthAlignment({id},unit,type,reference,negate,0,error);
}
bool DataService::confirmTimeDepthSemanticsAtRevision(long long id,const std::string& expectedRevision,
    const std::string& mdReference,const std::string& unit,const std::string& type,
    const std::string& timeReference,bool negate,DataObjectDescriptor& updated,std::string& error)
{
    updated={};
    error.clear();
    const auto md=q(mdReference).trimmed();
    const auto time=q(timeReference).trimmed();
    if(expectedRevision.empty() || md.isEmpty() || md.size()>256 || time.isEmpty() || time.size()>256) {
        error="时深修订、MD 起算面和时间基准须明确";
        return false;
    }
    if((unit!="ms" && unit!="s") || (type!="TWT" && type!="OWT")) {
        error="时深须有明确的 ms/s 和 TWT/OWT 声明";
        return false;
    }

    auto& store=impl_->store;
    Transaction transaction(store,error);
    if(!transaction.active)
        return false;
    DataObjectDescriptor object;
    if(!describeDataObject(DataObjectKind::TimeDepth,id,object,error))
        return false;
    if(!object.revisionKnown || object.revision!=expectedRevision) {
        error="时深对象修订已变化或尚未升级";
        return false;
    }
    SourceIndexEntry source;
    if(!readSourceIndexEntry(object.sourceId,source,error))
        return false;
    if(!source.active || object.wellId<=0) {
        error="时深来源不再有效或尚未明确归井";
        return false;
    }

    QSqlQuery row;
    if(!store.exec("SELECT length(CAST(payload AS BLOB)) FROM well_objects WHERE id=? AND kind='time_depth'",{id},error,&row) || !row.next())
        return false;
    const auto bytes=row.value(0).toLongLong();
    row.finish();
    if(bytes<1 || bytes>64LL*1024*1024) {
        error="时深文本超过64 MiB预算";
        return false;
    }
    if(!store.exec("SELECT json_extract(payload,'$.depth.type'),json_extract(payload,'$.depth.unit'),"
                   "json_extract(payload,'$.depth.md'),json_extract(payload,'$.timeUnit'),"
                   "json_extract(payload,'$.timeType'),json_extract(payload,'$.timeReference'),"
                   "json_extract(payload,'$.timeSign') FROM well_objects WHERE id=? AND kind='time_depth'",{id},error,&row) || !row.next())
        return false;
    const auto depthUnit=row.value(1).toString();
    if(row.value(0).toString()!=QLatin1String("MD") || (depthUnit!=QLatin1String("m") && depthUnit!=QLatin1String("ft"))) {
        error="时深关系缺少明确的 MD/m 或 MD/ft 轴";
        return false;
    }
    const auto sign=negate?QStringLiteral("negated"):QStringLiteral("");
    const bool changed=row.value(2).toString()!=md || row.value(3).toString()!=q(unit) ||
        row.value(4).toString()!=q(type) || row.value(5).toString()!=time || row.value(6).toString()!=sign;
    row.finish();
    if(changed) {
        // Update interpretation fields directly. The points array is neither decoded by C++ nor transformed.
        const auto note=QStringLiteral("人工确认单条时深关系口径：MD 起算面 %1，%2/%3/%4，%5；原始点值保留")
            .arg(md,q(type),q(unit),time,negate?QStringLiteral("符号按导出口径取反"):QStringLiteral("符号按原值"));
        if(!store.exec("UPDATE well_objects SET payload=json_set(payload,'$.depth.md',?,'$.timeUnit',?,"
                       "'$.timeType',?,'$.timeReference',?,'$.timeSign',?) WHERE id=? AND kind='time_depth'",
                       {md,q(unit),q(type),time,sign,id},error) ||
           !store.exec("INSERT INTO changes(kind,entity_id,description) VALUES('time_depth',?,?)",{id,note},error) ||
           !describeDataObject(DataObjectKind::TimeDepth,id,object,error))
            return false;
    }
    if(!transaction.commit(error))
        return false;
    updated=std::move(object);
    return true;
}
bool DataService::confirmTimeDepthAlignment(const std::vector<long long>& ids,
    const std::string& unit,const std::string& type,const std::string& reference,
    bool negate,long long volumeId,std::string& error){
    error.clear();
    if(unit!="ms"&&unit!="s"){error="时间单位须确认为 ms 或 s";return false;}
    if(type!="TWT"&&type!="OWT"){error="时间类型须确认为 TWT 或 OWT";return false;}
    if(q(reference).trimmed().isEmpty()){error="时间基准不能为空（如 MSL/海平面）";return false;}
    if(ids.empty()||volumeId<0){error="请选择时深来源与有效地震体";return false;}
    auto& store=impl_->store;Transaction tx(store,error);if(!tx.active)return false;
    std::set<long long> sources;QSqlQuery row;
    for(auto id:ids){
        if(!store.exec("SELECT source_id FROM well_objects WHERE id=? AND kind='time_depth'",{id},error,&row))return false;
        if(!row.next()||row.value(0).isNull()){error="时深关系不存在";return false;}
        sources.insert(row.value(0).toLongLong());
    }
    QJsonObject sampling;
    if(volumeId){
        if(!store.exec("SELECT sampling FROM volumes WHERE id=?",{volumeId},error,&row))return false;
        if(!row.next()){error="地震体不存在";return false;}
        sampling=object(row.value(0).toString());
        const auto declaredUnit=sampling["timeUnit"].toString();
        const auto declaredType=sampling["timeType"].toString();
        if((declaredUnit!="ms"&&declaredUnit!="s")||(!declaredType.isEmpty()&&declaredType!="TWT")){
            error="地震体须有明确的 ms/s 采样轴，且为 TWT；不自动缩放或改写单程采样";return false;
        }
        sampling["timeType"]="TWT";sampling["timeReference"]=q(reference).trimmed();
    }
    const QString note=QString("人工确认时深口径：%1/%2/%3，%4；原始点值保留")
        .arg(q(type),q(unit),q(reference).trimmed(),negate?QStringLiteral("符号按导出口径取反"):QStringLiteral("符号按原值"));
    for(auto source:sources){
        if(!store.exec("SELECT id,payload FROM well_objects WHERE source_id=? AND kind='time_depth'",{source},error,&row))return false;
        std::vector<std::pair<long long,QString>> updates;
        while(row.next()){
            auto payload=object(row.value(1).toString());payload["timeUnit"]=q(unit);payload["timeType"]=q(type);
            payload["timeReference"]=q(reference).trimmed();payload["timeSign"]=negate?QStringLiteral("negated"):QString();
            updates.push_back({row.value(0).toLongLong(),json(payload)});
        }
        for(const auto& update:updates)
            if(!store.exec("UPDATE well_objects SET payload=? WHERE id=?",{update.second,update.first},error))return false;
        if(!store.exec("INSERT INTO changes(kind,entity_id,description) VALUES('time_depth',?,?)",{updates.front().first,note},error)||
           !store.exec("UPDATE issues SET resolved=1,resolution=? WHERE source_id=? AND resolved=0 AND code IN ('TIME_UNIT_UNKNOWN','TIME_REFERENCE_UNKNOWN','TIME_SIGN_REVIEW')",{note,source},error))return false;
    }
    if(volumeId&&(!store.exec("UPDATE volumes SET sampling=? WHERE id=?",{json(sampling),volumeId},error)||
       !store.exec("INSERT INTO changes(kind,entity_id,description) VALUES('volume',?,?)",{volumeId,QString("与时深来源共用时间基准：%1；采样轴不变").arg(q(reference).trimmed())},error)))return false;
    return tx.commit(error);
}
}
