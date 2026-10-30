#include "interpretation_store.h"
#include <QJsonArray>

namespace paleo::data::detail::interpretation {
namespace {
bool readPayload(ProjectStore& st,const QString& hash,const QString& kind,QJsonObject& value,std::string& error){
    value={};if(hash.size()!=64){error="解释资源引用格式无效";return false;}
    QSqlQuery r;if(!st.exec("SELECT kind,payload FROM interpretation_payloads WHERE hash=?",{hash},error,&r))return false;
    if(!r.next()){error="解释资源缺失";return false;}QJsonObject wrapper;
    if(r.value(0).toString()!=kind||!parse(r.value(1).toByteArray(),hash,wrapper,error)){if(error.empty())error="解释资源类型不一致";return false;}
    if(wrapper["schema"].toInt()!=1||wrapper["kind"].toString()!=kind||!wrapper["data"].isObject()){error="解释资源内容格式无效";return false;}
    value=wrapper["data"].toObject();return true;
}
bool savePayload(ProjectStore& st,const QString& kind,const QJsonObject& value,QString& hash,std::string& error){
    const auto payload=bytes(QJsonObject{{"schema",1},{"kind",kind},{"data",value}});hash=digest(payload);
    QSqlQuery r;if(!st.exec("SELECT hash FROM interpretation_payloads WHERE hash=?",{hash},error,&r))return false;
    if(r.next()){r.finish();QJsonObject existing;if(!readPayload(st,hash,kind,existing,error))return false;if(existing!=value){error="内容寻址资源不可覆盖";return false;}return true;}r.finish();
    return st.exec("INSERT INTO interpretation_payloads(hash,kind,payload,created) VALUES(?,?,?,?)",{hash,kind,payload,now()},error);
}
// Object deltas retain the immutable snapshot's unchanged arrays, even when a
// caller supplies explicit unit/reference metadata on a calculation copy.
QJsonObject difference(const QJsonObject& base,const QJsonObject& current){
    QJsonObject set,objects;QJsonArray removed;
    for(auto it=base.begin();it!=base.end();++it)if(!current.contains(it.key()))removed.append(it.key());
    for(auto it=current.begin();it!=current.end();++it){if(base.contains(it.key())&&base[it.key()]==it.value())continue;
        if(base[it.key()].isObject()&&it.value().isObject())objects[it.key()]=difference(base[it.key()].toObject(),it.value().toObject());else set[it.key()]=it.value();}
    QJsonObject delta;if(!set.isEmpty())delta["set"]=set;if(!objects.isEmpty())delta["objects"]=objects;if(!removed.isEmpty())delta["remove"]=removed;return delta;
}
bool applyDifference(QJsonObject& base,const QJsonObject& delta,std::string& error){
    for(auto it=delta.begin();it!=delta.end();++it)if(it.key()!="set"&&it.key()!="objects"&&it.key()!="remove"){error="输入差量字段不受支持";return false;}
    if((delta.contains("set")&&!delta["set"].isObject())||(delta.contains("objects")&&!delta["objects"].isObject())||(delta.contains("remove")&&!delta["remove"].isArray())){error="输入差量格式错误";return false;}
    for(const auto& key:delta["remove"].toArray()){if(!key.isString()||!base.contains(key.toString())){error="输入差量删除目标缺失";return false;}base.remove(key.toString());}
    const auto set=delta["set"].toObject();for(auto it=set.begin();it!=set.end();++it)base[it.key()]=it.value();
    const auto objects=delta["objects"].toObject();for(auto it=objects.begin();it!=objects.end();++it){if(!base[it.key()].isObject()||!it.value().isObject()){error="输入差量对象目标缺失";return false;}auto child=base[it.key()].toObject();if(!applyDifference(child,it.value().toObject(),error))return false;base[it.key()]=child;}
    return true;
}
bool snapshotObject(ProjectStore& st,const QJsonObject& ref,QJsonObject& canonical,std::string& error){
    QJsonObject stored;
    if(ref.contains("id")){
        QSqlQuery r;if(!st.exec("SELECT payload,digest FROM correlation_input_snapshots WHERE id=?",{ref["id"].toString()},error,&r))return false;
        if(!r.next()){error="修订引用的输入快照不存在";return false;}
        if(r.value(1).toString()!=ref["digest"].toString()){error="修订引用的输入快照摘要不一致";return false;}
        if(!parse(r.value(0).toByteArray(),r.value(1).toString(),stored,error))return false;
        if(stored["id"].toString()!=ref["id"].toString()){error="修订输入快照身份不一致";return false;}
    }else if(ref.contains("hash")){if(!readPayload(st,ref["hash"].toString(),"snapshot",stored,error))return false;}
    else{error="修订缺少输入快照引用";return false;}
    CorrelationInputSnapshot typed;if(stored["schema"].toInt()!=1||!decode(stored,typed,error))return false;canonical=encode(typed);return true;
}
QJsonObject findWell(const QJsonObject& snapshot,const QString& wellId){for(const auto& item:snapshot["wells"].toArray()){const auto well=item.toObject();if(well["wellId"].toString()==wellId)return well;}return {};}
}

bool encodeStored(ProjectStore& st,const InterpretationDocument& value,QJsonObject& result,std::string& error){
    result={};auto manifest=encode(value);QJsonObject snapshotRef;
    if(value.snapshot.id.empty()){QString hash;if(!savePayload(st,"snapshot",manifest["snapshot"].toObject(),hash,error))return false;snapshotRef["hash"]=hash;}
    else{QSqlQuery r;if(!st.exec("SELECT digest FROM correlation_input_snapshots WHERE id=?",{q(value.snapshot.id)},error,&r))return false;if(!r.next()){error="保存修订前必须发布输入快照";return false;}snapshotRef={{"id",q(value.snapshot.id)},{"digest",r.value(0).toString()}};}
    QJsonObject snapshot;if(!snapshotObject(st,snapshotRef,snapshot,error))return false;
    manifest.remove("snapshot");manifest["snapshotRef"]=snapshotRef;manifest["schema"]=2;QJsonArray wells;
    for(const auto& item:manifest["wells"].toArray()){
        auto well=item.toObject();const auto input=well.take("input").toObject();const auto base=findWell(snapshot,well["wellId"].toString());QJsonObject inputRef;
        if(base.isEmpty()){QString hash;if(!savePayload(st,"well_input",input,hash,error))return false;inputRef["objectRef"]=hash;}
        else{inputRef["snapshotWellId"]=well["wellId"];const auto delta=difference(base,input);if(!delta.isEmpty()){QString hash;if(!savePayload(st,"input_patch",delta,hash,error))return false;inputRef["patchRef"]=hash;}}
        well["inputRef"]=inputRef;QJsonArray series;
        for(const auto& entry:well["series"].toArray()){auto s=entry.toObject();QJsonObject arrays;for(const auto* key:{"axis","values","quality"})arrays[key]=s.take(key);QString hash;if(!savePayload(st,"series_arrays",arrays,hash,error))return false;s["dataRef"]=hash;series.append(s);}
        well["series"]=series;wells.append(well);
    }
    manifest["wells"]=wells;result=std::move(manifest);return true;
}

bool decodeStored(ProjectStore& st,const QJsonObject& value,InterpretationDocument& result,std::string& error){
    result={};if(value["schema"].toInt()==1)return decode(value,result,error);
    if(value["schema"].toInt()!=2||!value["snapshotRef"].isObject()||!value["wells"].isArray()){error="解释修订引用格式不受支持";return false;}
    auto expanded=value;QJsonObject snapshot;if(!snapshotObject(st,value["snapshotRef"].toObject(),snapshot,error))return false;expanded["snapshot"]=snapshot;expanded.remove("snapshotRef");expanded["schema"]=1;QJsonArray wells;
    for(const auto& item:value["wells"].toArray()){
        auto well=item.toObject();const auto ref=well.take("inputRef").toObject();QJsonObject input;
        if(ref.contains("objectRef")){if(!readPayload(st,ref["objectRef"].toString(),"well_input",input,error))return false;}
        else if(ref.contains("snapshotWellId")&&ref["snapshotWellId"].toString()==well["wellId"].toString()){
            input=findWell(snapshot,ref["snapshotWellId"].toString());if(input.isEmpty()){error="修订引用的井输入不存在";return false;}
            if(ref.contains("patchRef")){QJsonObject patch;if(!readPayload(st,ref["patchRef"].toString(),"input_patch",patch,error)||!applyDifference(input,patch,error))return false;}
        }else{error="修订井输入引用无效";return false;}
        well["input"]=input;if(!well["series"].isArray()){error="修订派生曲线集合缺失";return false;}QJsonArray series;
        for(const auto& entry:well["series"].toArray()){auto s=entry.toObject();QJsonObject arrays;if(!readPayload(st,s.take("dataRef").toString(),"series_arrays",arrays,error))return false;for(const auto* key:{"axis","values","quality"}){if(!arrays[key].isArray()){error="派生曲线数组资源无效";return false;}s[key]=arrays[key];}series.append(s);}
        well["series"]=series;wells.append(well);
    }
    expanded["wells"]=wells;InterpretationDocument document;if(!decode(expanded,document,error))return false;result=std::move(document);return true;
}
}
