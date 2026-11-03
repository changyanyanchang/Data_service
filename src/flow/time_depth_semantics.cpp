#include "store/project_store.h"
#include <set>

namespace paleo::data {
using namespace detail;
bool DataService::confirmTimeDepthSemantics(long long id,const std::string& unit,
    const std::string& type,const std::string& reference,bool negate,std::string& error){
    return confirmTimeDepthAlignment({id},unit,type,reference,negate,0,error);
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
