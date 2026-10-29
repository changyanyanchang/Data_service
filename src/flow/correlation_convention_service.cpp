#include "store/project_store.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

namespace paleo::data {
namespace {
using namespace detail;
const QString policyKey=QStringLiteral("correlation_project_conventions_v1");
bool validatePolicy(const CorrelationProjectConventions& v,std::string& error){
    if(!v.depthType.empty()&&v.depthType!="MD"&&v.depthType!="TVD"&&v.depthType!="TVDSS"){
        error="工区深度类型须为 MD、TVD、TVDSS 或留空";return false;
    }
    if(!v.depthUnit.empty()&&v.depthUnit!="m"&&v.depthUnit!="ft"){
        error="工区深度单位须为 m、ft 或留空";return false;
    }
    if(!v.timeUnit.empty()&&v.timeUnit!="ms"&&v.timeUnit!="s"){
        error="工区时间单位须为 ms、s 或留空";return false;
    }
    if(q(v.evidence).trimmed().isEmpty()||v.evidence.size()>16384){
        error="工区约定必须记录非空依据，且依据不超过 16 KiB";return false;
    }
    return true;
}
QJsonObject encodePolicy(const CorrelationProjectConventions& v){
    return {{"schemaVersion",1},{"depthType",q(v.depthType)},{"depthUnit",q(v.depthUnit)},
            {"timeUnit",q(v.timeUnit)},{"evidence",q(v.evidence)},
            {"allowProvisionalAlignment",v.allowProvisionalAlignment}};
}
bool decodePolicy(const QString& stored,CorrelationProjectConventions& out,std::string& error){
    if(stored.toUtf8().size()>32768){error="工区约定内容过大";return false;}
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(stored.toUtf8(),&parse);
    if(parse.error!=QJsonParseError::NoError||!doc.isObject()){error="工区约定 JSON 无效";return false;}
    const auto object=doc.object();const QSet<QString> fields={"schemaVersion","depthType","depthUnit","timeUnit","evidence","allowProvisionalAlignment"};
    for(auto it=object.begin();it!=object.end();++it)if(!fields.contains(it.key())){error="工区约定包含未知字段："+s(it.key());return false;}
    if(object.size()!=fields.size()||!object.value("schemaVersion").isDouble()||object.value("schemaVersion").toDouble()!=1.0){error="工区约定版本或字段不完整";return false;}
    for(const auto* key:{"depthType","depthUnit","timeUnit","evidence"})if(!object.value(key).isString()){error=std::string("工区约定字段须为字符串：")+key;return false;}
    if(!object.value("allowProvisionalAlignment").isBool()){error="工区候选关联开关须为布尔值";return false;}
    CorrelationProjectConventions v;v.depthType=s(object.value("depthType").toString());v.depthUnit=s(object.value("depthUnit").toString());
    v.timeUnit=s(object.value("timeUnit").toString());v.evidence=s(object.value("evidence").toString());v.allowProvisionalAlignment=object.value("allowProvisionalAlignment").toBool();
    if(!validatePolicy(v,error))return false;out=std::move(v);return true;
}
}

bool DataService::readCorrelationProjectConventions(CorrelationProjectConventions& result,std::string& error)const{
    result={};error.clear();QSqlQuery row;
    if(!impl_->store.exec("SELECT value FROM ds_meta WHERE key=?",{policyKey},error,&row))return false;
    if(!row.next())return true;
    return decodePolicy(row.value(0).toString(),result,error);
}

bool DataService::saveCorrelationProjectConventions(const CorrelationProjectConventions& value,std::string& error){
    error.clear();if(!validatePolicy(value,error))return false;
    auto& store=impl_->store;Transaction transaction(store,error);if(!transaction.active)return false;
    QSqlQuery row;if(!store.exec("SELECT value FROM ds_meta WHERE key=?",{policyKey},error,&row))return false;
    QJsonValue before(QJsonValue::Null);
    if(row.next()){
        CorrelationProjectConventions previous;if(!decodePolicy(row.value(0).toString(),previous,error))return false;
        before=encodePolicy(previous);
    }
    row.finish();const auto after=encodePolicy(value);
    if(before==QJsonValue(after))return transaction.commit(error);
    const auto audit=json(QJsonObject{{"policyKey",policyKey},{"before",before},{"after",after},
                                    {"sourceMetadataChanged",false},{"sourceArraysChanged",false}});
    return store.exec("INSERT INTO ds_meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",{policyKey,json(after)},error)
        &&store.exec("INSERT INTO changes(kind,entity_id,description) VALUES(?,?,?)",{"correlation_project_conventions",0,audit},error)
        &&transaction.commit(error);
}
}
