#include "paleo_data/object_descriptor.h"
#include "store/project_store.h"
#include "store/object_revision_schema.h"
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QUuid>

namespace paleo::data {
using namespace detail;
bool DataService::describeDataObject(DataObjectKind kind,long long id,DataObjectDescriptor& result,std::string& error) const {
    result={};error.clear();if(id<=0){error="对象编号无效";return false;}
    QString sql,key;
    switch(kind) {
    case DataObjectKind::TimeDepth:key="time_depth";sql="SELECT id,source_id,well_id,name FROM well_objects WHERE id=? AND kind='time_depth'";break;
    case DataObjectKind::IntervalTrack:key="intervals";sql="SELECT id,source_id,well_id,name FROM tracks WHERE id=?";break;
    case DataObjectKind::SeismicVolume:key="volumes";sql="SELECT id,source_id,0,name FROM volumes WHERE id=?";break;
    default:error="对象类别不受支持";return false;
    }
    bool ready=false;QString epoch;if(!objectRevisionSchema(impl_->store,epoch,ready,error))return false;
    if(ready)sql="SELECT o.*,r.revision FROM ("+sql+") o LEFT JOIN object_revisions r ON r.kind=? AND r.object_id=o.id";
    QVariantList args{id};if(ready)args.append(key);QSqlQuery row;
    if(!impl_->store.exec(sql,args,error,&row))return false;
    if(!row.next()){error="指定对象不存在或类别不匹配";return false;}
    result.kind=kind;result.id=row.value(0).toLongLong();result.sourceId=row.value(1).toLongLong();result.wellId=row.value(2).toLongLong();result.name=s(row.value(3).toString());
    result.revisionKnown=ready && !row.value(4).isNull() && row.value(4).toLongLong()>0;
    if(result.revisionKnown)result.revision=s(epoch+":"+row.value(4).toString());return true;
}
bool DataService::readTimeDepthAtRevision(long long id,const std::string& revision,TimeDepthTable& result,
    std::string& error,const CancelCheck& cancelled,long long maxPoints) const {
    result={};error.clear();
    const auto stop=[&]{if(cancelled&&cancelled()){error="时深读取已取消";return true;}return false;};
    if(revision.empty() || maxPoints<1 || maxPoints>1000000){error="时深修订或点数预算无效";return false;}
    if(stop())return false;
    auto& store=impl_->store;
    if(!store.exec("BEGIN",{},error))return false;
    struct Rollback {ProjectStore& store;bool active=true;~Rollback(){if(active)store.rollback();}} rollback{store};
    DataObjectDescriptor object;
    if(!describeDataObject(DataObjectKind::TimeDepth,id,object,error))return false;
    if(!object.revisionKnown || object.revision!=revision){error="时深对象修订已变化或尚未升级";return false;}
    SourceIndexEntry source;if(!readSourceIndexEntry(object.sourceId,source,error))return false;
    if(!source.active){error="时深来源不再有效";return false;}
    QSqlQuery budget;
    if(!store.exec("SELECT length(CAST(payload AS BLOB)) FROM well_objects WHERE id=? AND kind='time_depth'",{id},error,&budget)||!budget.next())return false;
    const auto bytes=budget.value(0).toLongLong();budget.finish();
    if(bytes<1 || bytes>64LL*1024*1024){error="时深文本超过64 MiB预算";return false;}
    if(stop())return false;
    if(!store.exec("SELECT json_type(payload,'$.points'),json_array_length(payload,'$.points') FROM well_objects WHERE id=? AND kind='time_depth'",{id},error,&budget)||!budget.next())return false;
    const auto count=budget.value(1).toLongLong();const auto type=budget.value(0).toString();budget.finish();
    if(type!="array" || count<0 || count>maxPoints){error="时深点数组无效或超过点数预算";return false;}
    TimeDepthTable value;
    if(stop() || !readTimeDepth(id,value,error) || stop())return false;
    if(static_cast<long long>(value.depthTimePairs.size())!=count){error="时深点数不一致";return false;}
    if(!store.commit(error))return false;rollback.active=false;
    result=std::move(value);return true;
}
bool upgradeObjectRevisionSchema(const std::string& projectDirectory,std::string& error,const std::function<bool()>& cancelled) {
    error.clear();if(cancelled&&cancelled()){error="对象修订元数据升级已取消";return false;}
    if(q(projectDirectory).trimmed().isEmpty()){error="工区目录不能为空";return false;}
    const auto file=QDir(q(projectDirectory)).filePath("data.sqlite");
    if(!QFileInfo(file).isFile()){error="目录中没有 data.sqlite";return false;}
    ProjectStore store;store.root=QFileInfo(file).absolutePath();store.connection="paleo_object_upgrade_"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    store.db=QSqlDatabase::addDatabase("QSQLITE",store.connection);store.db.setDatabaseName(file);
    if(!store.db.open()){error=s(store.db.lastError().text());return false;}
    if(!store.exec("PRAGMA busy_timeout=5000",{},error))return false;QSqlQuery schema;
    if(!store.exec("SELECT value FROM ds_meta WHERE key='schema_version'",{},error,&schema)||!schema.next()||schema.value(0).toString()!="1") {
        error="工区格式不受支持";return false;
    }
    schema.finish();return ensureObjectRevisionSchema(store,error,cancelled);
}
}
