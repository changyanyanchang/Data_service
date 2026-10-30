#include "store/project_store.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <algorithm>
#include <set>

namespace paleo::data {
namespace {
using namespace detail;
const QString preferenceKey=QStringLiteral("correlation_time_depth_preferences_v1");
bool normalized(const std::vector<CorrelationTimeDepthPreference>& source,std::vector<CorrelationTimeDepthPreference>& result,std::string& error){
    result.clear();if(source.size()>10000){error="时深优选绑定数量超过限制";return false;}
    std::set<long long> seen;
    for(auto value:source){
        if(value.wellId<=0||value.timeDepthId<=0||!seen.insert(value.wellId).second){error="时深优选须包含唯一正井编号和正时深编号";return false;}
        if(value.evidenceJson.size()>1024*1024){error="单井时深优选依据超过 1 MiB";return false;}
        QJsonParseError parse;const auto evidence=QJsonDocument::fromJson(QByteArray::fromStdString(value.evidenceJson),&parse);
        if(parse.error!=QJsonParseError::NoError||!evidence.isObject()||evidence.object().isEmpty()){error="时深优选必须记录非空 JSON 对象依据";return false;}
        value.evidenceJson=evidence.toJson(QJsonDocument::Compact).toStdString();result.push_back(std::move(value));
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.wellId<b.wellId;});return true;
}
QJsonObject encode(const std::vector<CorrelationTimeDepthPreference>& values){
    QJsonArray rows;for(const auto& v:values)rows.append(QJsonObject{{"wellId",QString::number(v.wellId)},{"timeDepthId",QString::number(v.timeDepthId)},{"evidenceJson",q(v.evidenceJson)}});
    return {{"schemaVersion",1},{"preferences",rows}};
}
bool decode(const QString& stored,std::vector<CorrelationTimeDepthPreference>& result,std::string& error){
    if(stored.toUtf8().size()>16*1024*1024){error="时深优选记录超过 16 MiB";return false;}
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(stored.toUtf8(),&parse);
    if(parse.error!=QJsonParseError::NoError||!doc.isObject()){error="时深优选 JSON 无效";return false;}
    const auto root=doc.object();if(root.size()!=2||root.value("schemaVersion").toDouble()!=1.||!root.value("preferences").isArray()){error="时深优选版本或字段无效";return false;}
    std::vector<CorrelationTimeDepthPreference> values;
    for(const auto& item:root.value("preferences").toArray()){
        if(!item.isObject()){error="时深优选条目须为对象";return false;}const auto row=item.toObject();
        if(row.size()!=3||!row.value("wellId").isString()||!row.value("timeDepthId").isString()||!row.value("evidenceJson").isString()){error="时深优选条目包含未知或无效字段";return false;}
        bool wellOk=false,tdOk=false;const auto well=row.value("wellId").toString().toLongLong(&wellOk),td=row.value("timeDepthId").toString().toLongLong(&tdOk);
        if(!wellOk||!tdOk){error="时深优选编号无效";return false;}values.push_back({well,td,s(row.value("evidenceJson").toString())});
    }
    return normalized(values,result,error);
}
bool validateOwnership(const ProjectStore& store,const std::vector<CorrelationTimeDepthPreference>& values,std::string& error){
    for(const auto& value:values){QSqlQuery row;if(!store.exec("SELECT o.well_id FROM well_objects o JOIN wells w ON w.id=o.well_id WHERE o.id=? AND o.kind='time_depth'",{value.timeDepthId},error,&row))return false;
        if(!row.next()||row.value(0).toLongLong()!=value.wellId){error="时深优选编号不存在或不属于指定井：井 "+std::to_string(value.wellId)+"，时深 "+std::to_string(value.timeDepthId);return false;}}
    return true;
}
}
bool DataService::readCorrelationTimeDepthPreferences(std::vector<CorrelationTimeDepthPreference>& result,std::string& error)const{
    result.clear();error.clear();QSqlQuery row;if(!impl_->store.exec("SELECT value FROM ds_meta WHERE key=?",{preferenceKey},error,&row))return false;
    if(!row.next())return true;const auto stored=row.value(0).toString();row.finish();std::vector<CorrelationTimeDepthPreference> values;
    // Keep historical bindings readable after resource removal. The caller checks
    // each well independently, so one stale relation cannot block the whole field.
    if(!decode(stored,values,error))return false;result=std::move(values);return true;
}
bool DataService::saveCorrelationTimeDepthPreferences(const std::vector<CorrelationTimeDepthPreference>& input,std::string& error){
    error.clear();std::vector<CorrelationTimeDepthPreference> values;if(!normalized(input,values,error))return false;
    const auto after=encode(values);const auto serialized=json(after);if(serialized.toUtf8().size()>16*1024*1024){error="时深优选记录超过 16 MiB";return false;}
    auto& store=impl_->store;Transaction tx(store,error);if(!tx.active||!validateOwnership(store,values,error))return false;
    QSqlQuery row;if(!store.exec("SELECT value FROM ds_meta WHERE key=?",{preferenceKey},error,&row))return false;QJsonValue before(QJsonValue::Null);
    if(row.next()){std::vector<CorrelationTimeDepthPreference> previous;if(!decode(row.value(0).toString(),previous,error))return false;before=encode(previous);}row.finish();
    if(before==QJsonValue(after))return tx.commit(error);
    const auto audit=json(QJsonObject{{"preferenceKey",preferenceKey},{"before",before},{"after",after},{"sourceMetadataChanged",false},{"sourceArraysChanged",false}});
    return store.exec("INSERT INTO ds_meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",{preferenceKey,serialized},error)
        &&store.exec("INSERT INTO changes(kind,entity_id,description) VALUES(?,?,?)",{"correlation_time_depth_preferences",0,audit},error)&&tx.commit(error);
}
}
