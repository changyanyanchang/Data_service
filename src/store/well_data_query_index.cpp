#include "well_data_query_index.h"
#include "project_store.h"
#include <QSqlError>

namespace paleo::data::detail {
namespace {
bool sql(const ProjectStore& store,const QString& statement,std::string& error,QSqlQuery* result=nullptr) {
    QSqlQuery query(store.db);if(!query.exec(statement)){error=s(query.lastError().text());return false;}
    if(result)*result=std::move(query);return true;
}
const QStringList definitions{
    "CREATE TABLE IF NOT EXISTS pds_well_query_data_schema(singleton INTEGER PRIMARY KEY,version INTEGER NOT NULL)",
    "CREATE TABLE IF NOT EXISTS pds_well_query_curves(curve_id INTEGER PRIMARY KEY,well_id INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS pds_well_query_curve_well ON pds_well_query_curves(well_id,curve_id)",
    "CREATE TABLE IF NOT EXISTS pds_well_query_tracks(track_id INTEGER PRIMARY KEY,well_id INTEGER NOT NULL,record_count INTEGER NOT NULL CHECK(record_count>=0))",
    "CREATE INDEX IF NOT EXISTS pds_well_query_track_well ON pds_well_query_tracks(well_id,track_id)",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_curve_i AFTER INSERT ON curves BEGIN INSERT INTO pds_well_query_curves VALUES(NEW.id,coalesce((SELECT well_id FROM log_sets WHERE id=NEW.set_id),0)); END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_curve_u AFTER UPDATE OF id,set_id ON curves BEGIN DELETE FROM pds_well_query_curves WHERE curve_id=OLD.id; INSERT INTO pds_well_query_curves VALUES(NEW.id,coalesce((SELECT well_id FROM log_sets WHERE id=NEW.set_id),0)); END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_curve_d AFTER DELETE ON curves BEGIN DELETE FROM pds_well_query_curves WHERE curve_id=OLD.id; END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_log_well_u AFTER UPDATE OF well_id ON log_sets BEGIN UPDATE pds_well_query_curves SET well_id=coalesce(NEW.well_id,0) WHERE curve_id IN (SELECT id FROM curves WHERE set_id=NEW.id); END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_log_d BEFORE DELETE ON log_sets BEGIN DELETE FROM pds_well_query_curves WHERE curve_id IN (SELECT id FROM curves WHERE set_id=OLD.id); END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_track_i AFTER INSERT ON tracks BEGIN INSERT INTO pds_well_query_tracks VALUES(NEW.id,coalesce(NEW.well_id,0),0); END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_track_u AFTER UPDATE OF id,well_id ON tracks BEGIN UPDATE pds_well_query_tracks SET track_id=NEW.id,well_id=coalesce(NEW.well_id,0) WHERE track_id=OLD.id; END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_track_d AFTER DELETE ON tracks BEGIN DELETE FROM pds_well_query_tracks WHERE track_id=OLD.id; END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_interval_i AFTER INSERT ON intervals BEGIN UPDATE pds_well_query_tracks SET record_count=record_count+1 WHERE track_id=NEW.track_id; END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_interval_d AFTER DELETE ON intervals BEGIN UPDATE pds_well_query_tracks SET record_count=record_count-1 WHERE track_id=OLD.track_id; END",
    "CREATE TRIGGER IF NOT EXISTS pds_well_query_interval_u AFTER UPDATE OF track_id ON intervals WHEN OLD.track_id IS NOT NEW.track_id BEGIN UPDATE pds_well_query_tracks SET record_count=record_count-1 WHERE track_id=OLD.track_id; UPDATE pds_well_query_tracks SET record_count=record_count+1 WHERE track_id=NEW.track_id; END"};
}
QString wellDataPageSql(const std::string& kind,bool filterWell) {
    const auto filter=filterWell?QStringLiteral("i.well_id=? AND "):QString();QString inner;
    if(kind=="curves")inner="SELECT c.id,l.well_id,l.source_id,l.id,c.name,c.unit||' / '||l.name,l.sample_count FROM pds_well_query_curves i "+QString(filterWell?"INDEXED BY pds_well_query_curve_well ":"")+"CROSS JOIN curves c ON c.id=i.curve_id CROSS JOIN log_sets l ON l.id=c.set_id WHERE "+filter+"i.curve_id>? ORDER BY i.curve_id LIMIT ?";
    else if(kind=="intervals")inner="SELECT t.id,t.well_id,t.source_id,0,t.name,t.type,i.record_count FROM pds_well_query_tracks i "+QString(filterWell?"INDEXED BY pds_well_query_track_well ":"")+"CROSS JOIN tracks t ON t.id=i.track_id WHERE "+filter+"i.track_id>? ORDER BY i.track_id LIMIT ?";
    else return {};
    return "WITH page(id,well_id,source_id,parent_id,name,detail,n) AS ("+inner+") SELECT id,well_id,source_id,parent_id,CASE WHEN length(CAST(name AS BLOB))<=4096 THEN name END,CASE WHEN length(CAST(detail AS BLOB))<=4096 THEN detail END,n,length(CAST(name AS BLOB))>4096 OR length(CAST(detail AS BLOB))>4096 FROM page ORDER BY id";
}
bool wellDataQueryIndexReady(const ProjectStore& store,std::string& error) {
    QSqlQuery query;
    if(!sql(store,"SELECT count(*) FROM sqlite_master WHERE name IN ('pds_well_query_data_schema','pds_well_query_curves','pds_well_query_curve_well','pds_well_query_tracks','pds_well_query_track_well','pds_well_query_curve_i','pds_well_query_curve_u','pds_well_query_curve_d','pds_well_query_log_well_u','pds_well_query_log_d','pds_well_query_track_i','pds_well_query_track_u','pds_well_query_track_d','pds_well_query_interval_i','pds_well_query_interval_d','pds_well_query_interval_u')",error,&query)||!query.next())return false;
    const bool complete=query.value(0).toInt()==definitions.size();query.finish();
    if(!complete){error="DataPageIndexUnavailable: reopen writable to build metadata index";return false;}
    if(!sql(store,"SELECT version FROM pds_well_query_data_schema WHERE singleton=1",error,&query)||!query.next()||query.value(0).toInt()!=1){error="DataPageIndexUnavailable: unsupported metadata index version";return false;}
    return true;
}
bool ensureWellDataQueryIndex(ProjectStore& store,bool rebuild,std::string& error) {
    std::string ignored;const bool ready=wellDataQueryIndexReady(store,ignored);
    if(store.db.tables().contains("pds_well_query_data_schema")) {
        QSqlQuery version;if(!sql(store,"SELECT version FROM pds_well_query_data_schema WHERE singleton=1",error,&version))return false;
        if(version.next()&&version.value(0).toInt()!=1){error="UnsupportedDataPageIndexVersion";return false;}
    }
    for(const auto& definition:definitions)if(!sql(store,definition,error))return false;
    if(!ready||rebuild) {
        for(const auto& statement:QStringList{
          "DELETE FROM pds_well_query_curves","DELETE FROM pds_well_query_tracks",
          "INSERT INTO pds_well_query_curves SELECT c.id,coalesce(l.well_id,0) FROM curves c JOIN log_sets l ON l.id=c.set_id",
          "INSERT INTO pds_well_query_tracks SELECT t.id,coalesce(t.well_id,0),(SELECT count(*) FROM intervals WHERE track_id=t.id) FROM tracks t",
          "INSERT OR REPLACE INTO pds_well_query_data_schema VALUES(1,1)"})if(!sql(store,statement,error))return false;
    }
    return true;
}
}
