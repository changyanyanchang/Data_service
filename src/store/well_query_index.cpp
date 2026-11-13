#include "well_query_index.h"
#include "project_store.h"
#include <QJsonObject>
#include <QSqlError>

namespace paleo::data::detail {
namespace {
bool sql(const ProjectStore& store,const QString& statement,const QVariantList& args,std::string& error,QSqlQuery* result=nullptr) {
    QSqlQuery query(store.db);query.prepare(statement);for(const auto& arg:args)query.addBindValue(arg);
    if(!query.exec()){error=s(query.lastError().text());return false;}if(result)*result=std::move(query);return true;
}
}
QString normalizedWellQueryName(const QString& name) {
    QString result;for(const auto c:name)if(!c.isSpace()&&c!='-'&&c!='_')result.append(c.toLower());return result;
}
bool updateWellQueryIndex(const ProjectStore& store,long long id,bool deleted,const QJsonObject& image,std::string& error) {
    if(deleted){if(!sql(store,"DELETE FROM pds_well_query_names WHERE well_id=?",{id},error))return false;}
    else if(!sql(store,"INSERT OR REPLACE INTO pds_well_query_names(well_id,name_key) VALUES(?,?)",{id,normalizedWellQueryName(image.value("name").toString())},error))return false;
    return sql(store,"DELETE FROM pds_well_query_dirty WHERE well_id=?",{id},error);
}
bool ensureWellQueryIndex(ProjectStore& store,bool rebuild,std::string& error) {
    const bool exists=store.db.tables().contains("pds_well_query_names");
    const QStringList statements{
      "CREATE TABLE IF NOT EXISTS pds_well_query_names(well_id INTEGER PRIMARY KEY,name_key TEXT NOT NULL)",
      "CREATE INDEX IF NOT EXISTS pds_well_query_name_key ON pds_well_query_names(name_key,well_id)",
      "CREATE TABLE IF NOT EXISTS pds_well_query_dirty(well_id INTEGER PRIMARY KEY)",
      "CREATE TRIGGER IF NOT EXISTS pds_well_query_i AFTER INSERT ON wells BEGIN INSERT OR IGNORE INTO pds_well_query_dirty VALUES(NEW.id); END",
      "CREATE TRIGGER IF NOT EXISTS pds_well_query_u AFTER UPDATE OF name ON wells BEGIN INSERT OR IGNORE INTO pds_well_query_dirty VALUES(NEW.id); END",
      "CREATE TRIGGER IF NOT EXISTS pds_well_query_d AFTER DELETE ON wells BEGIN INSERT OR IGNORE INTO pds_well_query_dirty VALUES(OLD.id); END"};
    for(const auto& statement:statements)if(!sql(store,statement,{},error))return false;
    if(!exists||rebuild){
        if(!sql(store,"DELETE FROM pds_well_query_names",{},error))return false;
        QSqlQuery wells;if(!sql(store,"SELECT id,name FROM wells ORDER BY id",{},error,&wells))return false;
        while(wells.next())if(!updateWellQueryIndex(store,wells.value(0).toLongLong(),false,QJsonObject{{"name",wells.value(1).toString()}},error))return false;
        return sql(store,"DELETE FROM pds_well_query_dirty",{},error);
    }
    QSqlQuery dirty;if(!sql(store,"SELECT d.well_id,w.name FROM pds_well_query_dirty d LEFT JOIN wells w ON w.id=d.well_id ORDER BY d.well_id",{},error,&dirty))return false;
    struct Entry{long long id;QString name;bool deleted;};QVector<Entry> entries;
    while(dirty.next())entries.append({dirty.value(0).toLongLong(),dirty.value(1).toString(),dirty.value(1).isNull()});
    dirty.finish();
    for(const auto& entry:entries)if(!updateWellQueryIndex(store,entry.id,entry.deleted,QJsonObject{{"name",entry.name}},error))return false;
    return true;
}
bool wellQueryIndexReady(const ProjectStore& store,std::string& error) {
    if(!store.db.tables().contains("pds_well_query_names")){error="WellQueryIndexUnavailable: reopen writable to build metadata index";return false;}
    QSqlQuery dirty;if(!sql(store,"SELECT well_id FROM pds_well_query_dirty LIMIT 1",{},error,&dirty))return false;
    if(dirty.next()){error="WellQueryIndexUnavailable: unprojected well metadata changes";return false;}return true;
}
}
