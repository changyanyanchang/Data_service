#include "store/project_store.h"
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>

namespace paleo::data {
using namespace detail;
bool DataService::commitSourceRelocation(const PreparedSourceRelocation& prepared,std::string& error){
    error.clear();auto& store=impl_->store;
    if(store.readOnly){error="只读工区不能重定位来源";return false;}
    if(s(QDir::cleanPath(store.root))!=prepared.root_){error="来源准备结果属于其他工区";return false;}
    QFileInfo file(q(prepared.candidate_));
    if(!file.isFile()||file.size()!=prepared.size_||file.lastModified().toMSecsSinceEpoch()!=prepared.mtime_||
       file.metadataChangeTime().toMSecsSinceEpoch()!=prepared.change_||file.birthTime().toMSecsSinceEpoch()!=prepared.birth_){
        error="新来源在核验后发生变化，请重新核验";return false;
    }
    Transaction transaction(store,error);if(!transaction.active)return false;
    SourceIndexEntry current;if(!readSourceIndexEntry(prepared.sourceId_,current,error))return false;
    if(!current.active||current.sha256!=prepared.fingerprint_||current.storedPath!=prepared.stored_){
        error="来源登记在核验后发生变化，请重新准备";return false;
    }
    const auto previous=q(prepared.stored_),replacement=q(prepared.candidate_);
    if(previous==replacement)return transaction.commit(error);
    QSqlQuery objects;
    if(!store.exec("SELECT id,payload FROM well_objects WHERE source_id=? AND kind IN ('core_photos','attachments')",
                   {prepared.sourceId_},error,&objects))return false;
    std::vector<std::pair<qlonglong,QString>> updates;
    while(objects.next()){
        const auto doc=QJsonDocument::fromJson(objects.value(1).toString().toUtf8());
        const auto replace=[&](QJsonObject value){
            const auto path=value["path"].toString();
            if(!path.isEmpty()&&(path==previous||store.absolutePath(path)==store.absolutePath(previous)))value["path"]=replacement;
            return value;
        };
        if(doc.isArray()){QJsonArray values;for(auto value:doc.array())values.append(replace(value.toObject()));updates.emplace_back(objects.value(0).toLongLong(),json(values));}
        else updates.emplace_back(objects.value(0).toLongLong(),json(replace(doc.object())));
    }
    objects.finish();
    for(const auto& [id,payload]:updates)
        if(!store.exec("UPDATE well_objects SET payload=? WHERE id=?",{payload,id},error))return false;
    if(!store.exec("UPDATE sources SET stored_path=? WHERE id=?",{replacement,prepared.sourceId_},error)||
       !store.exec("UPDATE volumes SET path=? WHERE source_id=?",{replacement,prepared.sourceId_},error)||
       !store.exec("UPDATE maps SET image_path=CASE WHEN image_path=? THEN ? ELSE image_path END,source_file=CASE WHEN source_file=? THEN ? ELSE source_file END WHERE source_id=?",
                   {previous,replacement,previous,replacement,prepared.sourceId_},error)||
       !store.exec("INSERT INTO changes(kind,entity_id,description) VALUES('source',?,'核验文件身份后重新定位来源')",{prepared.sourceId_},error))return false;
    return transaction.commit(error);
}
}
