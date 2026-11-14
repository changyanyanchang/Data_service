#include "model_change_journal.h"
#include "well_query_index.h"
#include "well_data_query_index.h"
#include "project_store.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlError>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <QDir>
#include <atomic>

namespace paleo::data::detail {
namespace {
bool raw(const ProjectStore& store,const QString& sql,const QVariantList& args,std::string& error,QSqlQuery* result=nullptr) {
    if(!store.db.isOpen()){error="请先打开工区";return false;}QSqlQuery query(store.db);
    if(!query.prepare(sql)){error=s(query.lastError().text());return false;}for(const auto& value:args)query.addBindValue(value);
    if(!query.exec()){error=s(query.lastError().text());return false;}if(result)*result=std::move(query);return true;
}
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
bool ownIdentity(const ProjectStore& store,QString& id,QString& generation,std::string& error) {
    QSqlQuery query;if(!raw(store,"SELECT store_id,generation FROM pds_model_state WHERE singleton=1",{},error,&query)||!query.next())return false;
    id=query.value(0).toString();generation=query.value(1).toString();return true;
}
bool writerFence(const ProjectStore& store,std::string& error) {
    const auto file=QDir(store.root).filePath("data.sqlite");
    const auto identity=modelDatabaseFileIdentity(file,error);
    if(identity.isEmpty()||identity!=store.journalFileIdentity){error="StaleModelGeneration: model database was replaced; reopen before writing";return false;}
    static std::atomic<unsigned long long> serial{0};const auto name="paleo_model_fence_"+QString::number(++serial);
    QString id,generation;bool ok=false;
    {
        auto probe=QSqlDatabase::addDatabase("QSQLITE",name);probe.setDatabaseName(file);probe.setConnectOptions("QSQLITE_OPEN_READONLY");
        if(probe.open()){
            {QSqlQuery query(probe);if(query.exec("SELECT store_id,generation FROM pds_model_state WHERE singleton=1")&&query.next()){id=query.value(0).toString();generation=query.value(1).toString();ok=true;}}
            probe.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    if(!ok||id!=store.journalStoreId||generation!=store.journalGeneration){error="StaleModelGeneration: active model identity/generation changed; reopen before writing";return false;}
    return true;
}
struct Bypass {const ProjectStore& store;bool before;explicit Bypass(const ProjectStore& value):store(value),before(value.journalBypass){store.journalBypass=true;}~Bypass(){store.journalBypass=before;}};
QString dirty(const QString& kind,const QString& id,const QString& source,const QString& deleted="0") {
    return "INSERT INTO pds_model_dirty(kind,object_id,source_id,deleted) SELECT "+kind+","+id+",coalesce("+source+",0),"+deleted+
        " WHERE (SELECT operation_id FROM pds_model_context WHERE singleton=1) IS NOT NULL ON CONFLICT(kind,object_id) DO UPDATE SET source_id=excluded.source_id,deleted=excluded.deleted;";
}
QString source(const QString& id){const auto key="coalesce("+id+",0)";return dirty("'source'",key,key);}
QString touch(const QString& kind,const QString& id,const QString& sourceId,const QString& wellId="0",const QString& deleted="0") {
    return dirty("'"+kind+"'",id,sourceId,deleted)+source("coalesce("+sourceId+",0)")+
        dirty("'well'","coalesce("+wellId+",0)","0");
}
QString relationSource(const QString& table,const QString& id,const QString& key="id") {
    return "(SELECT source_id FROM "+table+" WHERE "+key+"="+id+")";
}
QString relationWell(const QString& table,const QString& id,const QString& key="id") {
    return "(SELECT well_id FROM "+table+" WHERE "+key+"="+id+")";
}
bool installTriggers(ProjectStore& store,std::string& error) {
    QSqlQuery tables;if(!raw(store,"SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",{},error,&tables))return false;
    QStringList names;while(tables.next())names.push_back(tables.value(0).toString());tables.finish();
    for(const auto& table:names){
        if(table.startsWith("pds_model_")||table.startsWith("pds_well_query_")||table.startsWith("curve_window_")||table=="object_revisions"||table=="source_scan_cache"||table=="changes")continue;
        for(const auto& event:QStringList{"INSERT","UPDATE","DELETE"}){
            const auto image=[&](const QString& row){
                const auto id=row+".id",sid=row+".source_id",wid=row+".well_id";const bool removed=event=="DELETE";
                if(table=="sources")return dirty("'source'",id,id,removed?"1":"0");
                if(table=="wells"){
                    QString body=dirty("'well'",id,"0",removed?"1":"0");
                    for(const auto& child:QStringList{"log_sets","tracks","sample_tables","well_objects"})body+=
                        "INSERT INTO pds_model_dirty SELECT 'source',source_id,source_id,0 FROM "+child+" WHERE well_id="+id+" AND source_id IS NOT NULL AND (SELECT operation_id FROM pds_model_context WHERE singleton=1) IS NOT NULL ON CONFLICT(kind,object_id) DO NOTHING;";
                    return body;
                }
                if(table=="curves")return touch("curves",id,relationSource("log_sets",row+".set_id"),relationWell("log_sets",row+".set_id"),removed?"1":"0");
                if(table=="intervals")return touch("tracks",row+".track_id",relationSource("tracks",row+".track_id"),relationWell("tracks",row+".track_id"));
                if(table=="horizon_points")return touch("horizons",row+".horizon_id",relationSource("horizons",row+".horizon_id"));
                if(table=="source_cells")return touch("source_tables",row+".table_id",relationSource("source_tables",row+".table_id"));
                if(table=="samples")return touch("sample_tables",row+".table_id",relationSource("sample_tables",row+".table_id"),relationWell("sample_tables",row+".table_id"));
                if(table=="measurements")return dirty("'store'","0","0");
                if(table=="source_relations")return source(sid)+source(row+".target_source_id");
                if(table=="issues"||table=="source_coordinate_systems")return source(sid);
                if(table=="section_points")return dirty("'sections'",row+".section_id","0");
                if(table=="sections")return dirty("'sections'",id,"0",removed?"1":"0");
                if(QStringList{"log_sets","tracks","sample_tables","well_objects"}.contains(table))return touch(table,id,sid,wid,removed?"1":"0");
                if(QStringList{"volumes","horizons","maps","source_tables"}.contains(table))return touch(table,id,sid,"0",removed?"1":"0");
                // Remaining configuration/interpretation tables have no numeric image in this journal.
                // The explicit conservative item prevents an incremental consumer claiming coverage.
                return dirty("'store'","0","0");
            };
            const auto trigger="pds_model_"+table+"_"+event.left(1).toLower();
            QString body="UPDATE pds_model_state SET unmanaged=1 WHERE singleton=1 AND (SELECT operation_id FROM pds_model_context WHERE singleton=1) IS NULL;";
            if(event=="UPDATE")body+=image("OLD");body+=image(event=="DELETE"?"OLD":"NEW");
            QString condition;QString timing="AFTER";
            if(table=="wells" && event=="UPDATE") {
                QStringList changed;
                for(const auto& column:QStringList{"id","name","field_name","well_type","x","y","crs","unit","total_depth","remark","depth_json"})
                    changed.append("OLD."+column+" IS NOT NEW."+column);
                condition=" WHEN "+changed.join(" OR ");
            }
            if(table=="ds_meta"){
                condition=" WHEN "+QString(event=="DELETE"?"OLD":"NEW")+".key NOT LIKE 'model_change_%'";
                if(event=="INSERT"){
                    // BEFORE alone misreports INSERT OR IGNORE as a real write.
                    // Cache the old scalar and emit only after an actual change.
                    if(!raw(store,"CREATE TRIGGER IF NOT EXISTS pds_model_ds_meta_before_i BEFORE INSERT ON ds_meta WHEN NEW.key NOT LIKE 'model_change_%' BEGIN INSERT INTO pds_model_meta_before(key,old_value,had_value) VALUES(NEW.key,(SELECT value FROM ds_meta WHERE key=NEW.key),EXISTS(SELECT 1 FROM ds_meta WHERE key=NEW.key)) ON CONFLICT(key) DO UPDATE SET old_value=excluded.old_value,had_value=excluded.had_value; END",{},error))return false;
                    condition+=" AND EXISTS(SELECT 1 FROM pds_model_meta_before WHERE key=NEW.key AND (had_value=0 OR old_value IS NOT NEW.value))";
                }else if(event=="UPDATE")condition+=" AND (OLD.key IS NOT NEW.key OR OLD.value IS NOT NEW.value)";
            }
            if(table=="ds_meta"&&event=="INSERT") {
                QSqlQuery existing;
                if(!raw(store,"SELECT sql FROM sqlite_master WHERE type='trigger' AND name=?",{trigger},error,&existing))return false;
                const bool legacy=existing.next()&&existing.value(0).toString().contains("BEFORE INSERT",Qt::CaseInsensitive);existing.finish();
                if(legacy&&!raw(store,"DROP TRIGGER \""+trigger+"\"",{},error))return false;
            }
            if(!raw(store,"CREATE TRIGGER IF NOT EXISTS \""+trigger+"\" "+timing+" "+event+" ON \""+table+"\""+condition+" BEGIN "+body+" END",{},error))return false;
        }
    }return true;
}
QJsonValue idValue(const QVariant& value){return value.isNull()?QJsonValue():QJsonValue(value.toString());}
bool rows(ProjectStore& store,const QString& sql,const QVariantList& args,const QStringList& keys,QJsonArray& out,std::string& error) {
    QSqlQuery query;if(!raw(store,sql,args,error,&query))return false;
    while(query.next()){
        if(out.size()>=100000){error="ModelChangeBudgetExceeded: metadata membership limit";return false;}
        QJsonObject item;for(int i=0;i<keys.size();++i){const auto key=keys[i];const auto value=query.value(i);
            if(key.endsWith("Id")||key=="id")item[key]=idValue(value);else item[key]=QJsonValue::fromVariant(value);
        }out.append(item);
    }return true;
}
bool sourceImage(ProjectStore& store,long long id,QJsonObject& out,bool& deleted,std::string& error) {
    QJsonArray card;if(!rows(store,"SELECT id,original_path,stored_path,kind,status,message,bytes,hash,active,options FROM sources WHERE id=?",{id},
        {"id","path","storedPath","kind","status","message","bytes","sha256","active","options"},card,error))return false;
    deleted=card.empty();out={{"schema",1}};if(deleted){out["deleted"]=true;return true;}auto sourceCard=card.first().toObject();sourceCard["active"]=sourceCard["active"].toInt()!=0;out["source"]=sourceCard;
    QJsonArray wells,objects,volumes,horizons;
    if(!rows(store,"SELECT DISTINCT w.id,w.name FROM wells w JOIN (SELECT well_id FROM log_sets WHERE source_id=? UNION SELECT well_id FROM tracks WHERE source_id=? UNION SELECT well_id FROM sample_tables WHERE source_id=? UNION SELECT well_id FROM well_objects WHERE source_id=?) a ON a.well_id=w.id ORDER BY w.id",{id,id,id,id},{"id","name"},wells,error))return false;
    const QString entries="SELECT c.id,l.well_id,l.source_id,l.id,'curves',c.name,c.unit,l.sample_count FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE l.source_id=? UNION ALL SELECT id,well_id,source_id,0,'intervals',name,type,0 FROM tracks WHERE source_id=? UNION ALL SELECT id,well_id,source_id,0,kind,name,'',0 FROM well_objects WHERE source_id=? UNION ALL SELECT id,well_id,source_id,0,'samples',name,'',0 FROM sample_tables WHERE source_id=? UNION ALL SELECT id,0,source_id,0,'tables',name,'',rows FROM source_tables WHERE source_id=? UNION ALL SELECT id,0,source_id,0,'maps',name,type,0 FROM maps WHERE source_id=? ORDER BY 5,1";
    if(!rows(store,entries,{id,id,id,id,id,id},{"id","wellId","sourceId","parentId","kind","name","detail","count"},objects,error)||
       !rows(store,"SELECT id,source_id,name,path,format,amplitude_unit,geometry,sampling FROM volumes WHERE source_id=? ORDER BY id",{id},{"id","sourceId","name","path","format","amplitudeUnit","geometry","sampling"},volumes,error)||
       !rows(store,"SELECT id,source_id,name,volume_id,crs,unit,time_unit,time_type,time_reference FROM horizons WHERE source_id=? ORDER BY id",{id},{"id","sourceId","name","volumeId","crs","unit","timeUnit","timeType","timeReference"},horizons,error))return false;
    out["wells"]=wells;out["objects"]=objects;out["volumes"]=volumes;out["horizons"]=horizons;return true;
}
bool objectImage(ProjectStore& store,const QString& kind,long long id,QJsonObject& out,bool& deleted,std::string& error) {
    out={{"schema",1},{"kind",kind},{"id",QString::number(id)}};
    if(kind=="source")return sourceImage(store,id,out,deleted,error);
    if(kind=="store"){out["requiresRebuild"]=true;return true;}
    QString sql;QStringList keys;
    if(kind=="well"){sql="SELECT id,name,field_name,well_type,x,y,crs,unit,total_depth,remark,depth_json FROM wells WHERE id=?";keys={"id","name","fieldName","wellType","x","y","crs","unit","totalDepth","remark","depthJson"};}
    else if(kind=="curves"){sql="SELECT c.id,c.set_id,l.source_id,l.well_id,c.name,c.unit,l.sample_count,l.depth_json FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE c.id=?";keys={"id","parentId","sourceId","wellId","name","unit","count","depthJson"};}
    else if(kind=="log_sets"){sql="SELECT id,source_id,well_id,name,sample_count,depth_json FROM log_sets WHERE id=?";keys={"id","sourceId","wellId","name","count","depthJson"};}
    else if(kind=="tracks"){sql="SELECT id,source_id,well_id,name,type,depth_json FROM tracks WHERE id=?";keys={"id","sourceId","wellId","name","type","depthJson"};}
    else if(kind=="well_objects"){sql="SELECT id,source_id,well_id,name,kind FROM well_objects WHERE id=?";keys={"id","sourceId","wellId","name","objectKind"};}
    else if(kind=="volumes"){sql="SELECT id,source_id,name,path,format,amplitude_unit,geometry,sampling FROM volumes WHERE id=?";keys={"id","sourceId","name","path","format","amplitudeUnit","geometry","sampling"};}
    else if(kind=="horizons"){sql="SELECT id,source_id,name,volume_id,crs,unit,time_unit,time_type,time_reference FROM horizons WHERE id=?";keys={"id","sourceId","name","volumeId","crs","unit","timeUnit","timeType","timeReference"};}
    else if(kind=="sample_tables"){sql="SELECT id,source_id,well_id,name FROM sample_tables WHERE id=?";keys={"id","sourceId","wellId","name"};}
    else if(kind=="source_tables"){sql="SELECT id,source_id,name,rows,columns FROM source_tables WHERE id=?";keys={"id","sourceId","name","rows","columns"};}
    else if(kind=="maps"){sql="SELECT id,source_id,name,type,strata,crs,geo_status,geo_file FROM maps WHERE id=?";keys={"id","sourceId","name","type","strata","crs","geoStatus","geoFile"};}
    else if(kind=="sections"){sql="SELECT id,name,volume_id,crs,unit FROM sections WHERE id=?";keys={"id","name","volumeId","crs","unit"};}
    else {out["requiresRebuild"]=true;return true;}
    QJsonArray values;if(!rows(store,sql,{id},keys,values,error))return false;deleted=values.empty();if(!deleted)out["value"]=values.first();else out["deleted"]=true;return true;
}
bool activate(const ProjectStore& store,std::string& error) {
    QSqlQuery version;if(!raw(store,"SELECT value FROM ds_meta WHERE key='model_change_schema'",{},error,&version)||!version.next()||version.value(0).toString()!="1"){error="UnsupportedModelChangeVersion: writes refused";return false;}version.finish();
    if(!store.journalWriteActivated){QString id,generation;if(!ownIdentity(store,id,generation,error))return false;
        if(id!=store.journalStoreId||generation!=store.journalGeneration){error="StaleModelGeneration: connection generation changed before the write transaction";return false;}}
    QSqlQuery context;if(!raw(store,"SELECT operation_id FROM pds_model_context WHERE singleton=1",{},error,&context)||!context.next())return false;
    const bool active=!context.value(0).isNull();context.finish();
    if(!active&&!raw(store,"UPDATE pds_model_context SET operation_id=? WHERE singleton=1",{uuid()},error))return false;
    store.journalWriteActivated=true;return true;
}
bool finalize(const ProjectStore& constant,std::string& error) {
    auto& store=const_cast<ProjectStore&>(constant);QSqlQuery context;
    if(!raw(store,"SELECT operation_id FROM pds_model_context WHERE singleton=1",{},error,&context)||!context.next())return false;
    const auto operation=context.value(0).toString();context.finish();if(operation.isEmpty())return true;
    QSqlQuery dirtyRows;if(!raw(store,"SELECT kind,object_id,source_id,deleted FROM pds_model_dirty WHERE object_id>0 OR kind='store' ORDER BY kind,object_id",{},error,&dirtyRows))return false;
    struct Item{QString kind;long long id,source;bool deleted;};std::vector<Item> items;
    while(dirtyRows.next()){if(items.size()>=100000){error="ModelChangeBudgetExceeded: dirty object limit";return false;}items.push_back({dirtyRows.value(0).toString(),dirtyRows.value(1).toLongLong(),dirtyRows.value(2).toLongLong(),dirtyRows.value(3).toBool()});}dirtyRows.finish();
    if(!items.empty()){
        QSqlQuery head;if(!raw(store,"SELECT sequence FROM pds_model_state WHERE singleton=1",{},error,&head)||!head.next())return false;
        const auto previous=head.value(0).toLongLong();if(previous<0||previous==std::numeric_limits<long long>::max()){error="Model change sequence exhausted";return false;}const auto seq=previous+1;head.finish();
        if(!raw(store,"INSERT INTO pds_model_batches(sequence,operation_id) VALUES(?,?)",{seq,operation},error))return false;
        long long ordinal=0,totalBytes=0;
        for(auto& item:items){
            QJsonObject image;if(!objectImage(store,item.kind,item.id,image,item.deleted,error))return false;
            if(item.kind=="well"&&!updateWellQueryIndex(store,item.id,item.deleted,image.value("value").toObject().value("name").toString(),error))return false;
            const auto bytes=QJsonDocument(image).toJson(QJsonDocument::Compact);totalBytes+=bytes.size();
            if(bytes.size()>8*1024*1024||totalBytes>64*1024*1024){error="ModelChangeBudgetExceeded: metadata image limit";return false;}
            if(!raw(store,"INSERT INTO pds_model_items VALUES(?,?,?,?,?,?,?)",{seq,++ordinal,item.kind,item.id,item.source,item.deleted,QString::fromUtf8(bytes)},error))return false;
        }
        if(!raw(store,"UPDATE pds_model_state SET sequence=? WHERE singleton=1",{seq},error))return false;
    }
    return raw(store,"DELETE FROM pds_model_dirty",{},error)&&raw(store,"UPDATE pds_model_context SET operation_id=NULL WHERE singleton=1",{},error);
}
void resetTransaction(const ProjectStore& store){store.journalTransaction=false;store.journalOuterSavepoint.clear();store.journalWriteActivated=false;store.journalWriteLease={};}
bool gate(const ProjectStore& store,std::string& error){
    if(store.journalWriteLease)return true;
    auto lease=tryAcquireModelWriteLease(s(store.root),5000,error);if(!lease||!writerFence(store,error))return false;
    store.journalWriteLease=std::move(lease);return true;
}
bool finishTransaction(const ProjectStore& store,const QString& sql,const QVariantList& args,std::string& error,QSqlQuery* result) {
    if(!finalize(store,error))return false;
    QString id,generation;const bool ownWrite=store.journalWriteActivated;
    if(ownWrite&&!ownIdentity(store,id,generation,error))return false;
    if(!raw(store,sql,args,error,result))return false;
    // Only a transaction fenced by this instance can advance its cached identity.
    // This permits its own generation fork without refreshing idle old writers.
    if(ownWrite){store.journalStoreId=id;store.journalGeneration=generation;}
    resetTransaction(store);return true;
}
QString cleanStatement(QString sql) {
    for(;;){sql=sql.trimmed();if(sql.startsWith("--")){const auto end=sql.indexOf('\n');if(end<0)return {};sql=sql.mid(end+1);continue;}if(sql.startsWith("/*")){const auto end=sql.indexOf("*/");if(end<0)return {};sql=sql.mid(end+2);continue;}return sql;}
}
}

bool ensureModelChangeJournal(ProjectStore& store,std::string& error) {
    auto lease=tryAcquireModelWriteLease(s(store.root),5000,error);if(!lease)return false;
    if(!store.journalFileIdentity.isEmpty()&&!writerFence(store,error))return false;
    Bypass bypass(store);
    if(!raw(store,"BEGIN IMMEDIATE",{},error))return false;
    const auto fail=[&]{std::string ignored;raw(store,"ROLLBACK",{},ignored);return false;};
    // A legacy/external writer need not honor the filesystem gate. Check the
    // format only after SQLite has granted the write lock, never before waiting.
    QSqlQuery version;
    if(!raw(store,"SELECT value FROM ds_meta WHERE key='model_change_schema'",{},error,&version))return fail();
    const bool exists=version.next();if(exists&&version.value(0).toString()!="1"){error="UnsupportedModelChangeVersion: writes refused";version.finish();return fail();}version.finish();
    const QStringList statements{
        "CREATE TABLE IF NOT EXISTS pds_model_state(singleton INTEGER PRIMARY KEY,store_id TEXT NOT NULL,generation TEXT NOT NULL,sequence INTEGER NOT NULL,unmanaged INTEGER NOT NULL) WITHOUT ROWID",
        "CREATE TABLE IF NOT EXISTS pds_model_context(singleton INTEGER PRIMARY KEY,operation_id TEXT) WITHOUT ROWID",
        "CREATE TABLE IF NOT EXISTS pds_model_meta_before(key TEXT PRIMARY KEY,old_value TEXT,had_value INTEGER NOT NULL) WITHOUT ROWID",
        "CREATE TABLE IF NOT EXISTS pds_model_dirty(kind TEXT NOT NULL,object_id INTEGER NOT NULL,source_id INTEGER NOT NULL,deleted INTEGER NOT NULL,PRIMARY KEY(kind,object_id)) WITHOUT ROWID",
        "CREATE TABLE IF NOT EXISTS pds_model_batches(sequence INTEGER PRIMARY KEY,operation_id TEXT NOT NULL UNIQUE) WITHOUT ROWID",
        "CREATE TABLE IF NOT EXISTS pds_model_items(sequence INTEGER NOT NULL,ordinal INTEGER NOT NULL,kind TEXT NOT NULL,object_id INTEGER NOT NULL,source_id INTEGER NOT NULL,deleted INTEGER NOT NULL,metadata TEXT NOT NULL,PRIMARY KEY(sequence,ordinal)) WITHOUT ROWID"
    };
    for(const auto& statement:statements)if(!raw(store,statement,{},error))return fail();
    if(!raw(store,"INSERT OR IGNORE INTO pds_model_state VALUES(1,?,?,0,0)",{uuid(),uuid()},error)||!raw(store,"INSERT OR IGNORE INTO pds_model_context VALUES(1,NULL)",{},error))return fail();
    QSqlQuery state;if(!raw(store,"SELECT unmanaged FROM pds_model_state WHERE singleton=1",{},error,&state)||!state.next())return fail();const bool rebuild=state.value(0).toBool();state.finish();
    if(rebuild&&(!raw(store,"DELETE FROM pds_model_items",{},error)||!raw(store,"DELETE FROM pds_model_batches",{},error)||!raw(store,"DELETE FROM pds_model_dirty",{},error)||!raw(store,"UPDATE pds_model_state SET generation=?,sequence=0,unmanaged=0 WHERE singleton=1",{uuid()},error)))return fail();
    if(!ensureWellQueryIndex(store,rebuild,error)||!ensureWellDataQueryIndex(store,rebuild,error)||!raw(store,"UPDATE pds_model_context SET operation_id=NULL WHERE singleton=1",{},error)||!installTriggers(store,error)||!raw(store,"INSERT OR REPLACE INTO ds_meta VALUES('model_change_schema','1')",{},error)||!raw(store,"COMMIT",{},error))return fail();
    QSqlQuery cookie;if(!raw(store,"PRAGMA schema_version",{},error,&cookie)||!cookie.next())return false;store.journalSchemaCookie=cookie.value(0).toLongLong();cookie.finish();
    if(!ownIdentity(store,store.journalStoreId,store.journalGeneration,error))return false;
    store.journalFileIdentity=modelDatabaseFileIdentity(QDir(store.root).filePath("data.sqlite"),error);if(store.journalFileIdentity.isEmpty())return false;
    store.journalReady=true;return true;
}
bool executeJournalled(const ProjectStore& store,const QString& sql,const QVariantList& args,std::string& error,QSqlQuery* result) {
    if(store.journalBypass||!store.journalReady||store.readOnly)return raw(store,sql,args,error,result);
    const auto statement=cleanStatement(sql);const auto upper=statement.toUpper();
    const bool begin=upper.startsWith("BEGIN"),savepoint=upper.startsWith("SAVEPOINT "),release=upper.startsWith("RELEASE ");
    const bool rollback=upper.startsWith("ROLLBACK"),rollbackTo=rollback&&upper.contains(" TO ");
    const bool commit=upper=="COMMIT"||upper=="COMMIT;"||upper=="END"||upper.startsWith("COMMIT TRANSACTION");
    const bool write=QRegularExpression("^(INSERT|UPDATE|DELETE|REPLACE|CREATE|ALTER|DROP|REINDEX)\\b").match(upper).hasMatch();
    const auto savepointName=[&]{auto text=statement.mid(statement.indexOf(' ')+1).trimmed();if(text.startsWith("SAVEPOINT ",Qt::CaseInsensitive))text=text.mid(10).trimmed();if(text.endsWith(';'))text.chop(1);return text;};
    if(begin||savepoint){
        const bool outer=!store.journalTransaction;
        if(outer&&(savepoint||upper.contains("IMMEDIATE")||upper.contains("EXCLUSIVE"))&&!gate(store,error))return false;
        if(!raw(store,sql,args,error,result)){if(outer)store.journalWriteLease={};return false;}
        if(outer){store.journalTransaction=true;store.journalOuterSavepoint=savepoint?savepointName():QString();}
        if(begin&&(upper.contains("IMMEDIATE")||upper.contains("EXCLUSIVE"))&&!activate(store,error)){std::string ignored;raw(store,"ROLLBACK",{},ignored);resetTransaction(store);return false;}
        return true;
    }
    if(rollback){const bool ok=raw(store,sql,args,error,result);if(ok&&!rollbackTo)resetTransaction(store);else if(ok)store.journalWriteActivated=false;return ok;}
    const bool closes=commit||(release&&!store.journalOuterSavepoint.isEmpty()&&savepointName().compare(store.journalOuterSavepoint,Qt::CaseInsensitive)==0);
    if(closes){
        return finishTransaction(store,sql,args,error,result);
    }
    if(write){
        const bool automatic=!store.journalTransaction;
        const auto failWrite=[&]{if(automatic){std::string ignored;raw(store,"ROLLBACK",{},ignored);resetTransaction(store);}return false;};
        if(!gate(store,error))return false;
        if(automatic){if(!raw(store,"BEGIN IMMEDIATE",{},error)){store.journalWriteLease={};return false;}store.journalTransaction=true;}
        if(!activate(store,error)||!raw(store,sql,args,error,result)){
            if(automatic){std::string ignored;raw(store,"ROLLBACK",{},ignored);resetTransaction(store);}return false;
        }
        if(upper.startsWith("CREATE TABLE")){
            QSqlQuery cookie;if(!raw(store,"PRAGMA schema_version",{},error,&cookie)||!cookie.next())return failWrite();
            if(cookie.value(0).toLongLong()!=store.journalSchemaCookie){cookie.finish();Bypass bypass(store);if(!installTriggers(const_cast<ProjectStore&>(store),error)||!raw(store,"PRAGMA schema_version",{},error,&cookie)||!cookie.next())return failWrite();store.journalSchemaCookie=cookie.value(0).toLongLong();}
        }
        if(automatic){
            if(!finishTransaction(store,"COMMIT",{},error,nullptr)){std::string ignored;raw(store,"ROLLBACK",{},ignored);resetTransaction(store);return false;}
        }return true;
    }
    if(!raw(store,sql,args,error,result))return false;
    return true;
}
bool readJournalHead(const ProjectStore& store,ModelChangeHead& result,std::string& error) {
    result={};error.clear();QSqlQuery version;if(!raw(store,"SELECT value FROM ds_meta WHERE key='model_change_schema'",{},error,&version))return false;
    if(!version.next())return true;if(version.value(0).toString()!="1"){error="UnsupportedModelChangeVersion";return false;}version.finish();
    QSqlQuery state;if(!raw(store,"SELECT store_id,generation,sequence,unmanaged FROM pds_model_state WHERE singleton=1",{},error,&state)||!state.next())return false;
    if(state.value(3).toBool()){error="RebuildRequired: unmanaged model writes invalidate incremental history";return false;}
    result.storeId=s(state.value(0).toString());result.generation=s(state.value(1).toString());result.sequence=state.value(2).toLongLong();result.available=true;return true;
}
}
