#include "paleo_data/catalog.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <limits>
#include <utility>
namespace paleo::data {
namespace {
QString q(const std::string& v) { return QString::fromUtf8(v.data(),qsizetype(v.size())); }
std::string s(const QString& v) { return v.toUtf8().toStdString(); }
bool execute(const std::string& name,const QString& sql,const QVariantList& args,
             std::string& error,QSqlQuery* result=nullptr) {
    error.clear();
    auto db=QSqlDatabase::database(q(name),false);
    if(!db.isValid()||!db.isOpen()||db.driverName()!=QStringLiteral("QSQLITE")) {
        error="catalog connection is not an open QSQLITE connection on this thread"; return false;
    }
    QSqlQuery query(db);
    if(!query.prepare(sql)) { error=s(query.lastError().text()); return false; }
    for(const auto& value:args) query.addBindValue(value);
    if(!query.exec()) { error=s(query.lastError().text()); return false; }
    if(result) *result=std::move(query);
    return true;
}
}
CatalogStorage::CatalogStorage(std::string connection):connection_(std::move(connection)) {}
const char *CatalogStorage::schemaSql() { return
    "CREATE TABLE IF NOT EXISTS entities ("
    " id TEXT PRIMARY KEY NOT NULL,"
    " entity_type TEXT NOT NULL DEFAULT '',"
    " name TEXT NOT NULL DEFAULT '',"
    " surface_x REAL NOT NULL DEFAULT 0,"
    " surface_y REAL NOT NULL DEFAULT 0,"
    " has_surface INTEGER NOT NULL DEFAULT 0,"
    " kb REAL NOT NULL DEFAULT 0,"
    " td REAL NOT NULL DEFAULT 0,"
    " coordinate_status TEXT NOT NULL DEFAULT '',"
    " inline_min REAL NOT NULL DEFAULT 0,"
    " inline_max REAL NOT NULL DEFAULT 0,"
    " xline_min REAL NOT NULL DEFAULT 0,"
    " xline_max REAL NOT NULL DEFAULT 0,"
    " sample_interval_us REAL NOT NULL DEFAULT 0,"
    " start_time_ms REAL NOT NULL DEFAULT 0,"
    " corners_json TEXT NOT NULL DEFAULT '[]',"
    " extra_json TEXT NOT NULL DEFAULT '{}'"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_entities_type_name ON entities(entity_type, name);"
    "CREATE TABLE IF NOT EXISTS assets ("
    " id TEXT PRIMARY KEY NOT NULL,"
    " type TEXT NOT NULL DEFAULT '',"
    " format TEXT NOT NULL DEFAULT '',"
    " display_name TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE TABLE IF NOT EXISTS versions ("
    " id TEXT PRIMARY KEY NOT NULL,"
    " asset_id TEXT NOT NULL,"
    " stage TEXT NOT NULL DEFAULT '',"
    " version_number INTEGER NOT NULL DEFAULT 1,"
    " managed INTEGER NOT NULL DEFAULT 1,"
    " path TEXT NOT NULL DEFAULT '',"
    " source_uri TEXT NOT NULL DEFAULT '',"
    " sha256 TEXT NOT NULL DEFAULT '',"
    " file_name TEXT NOT NULL DEFAULT '',"
    " parent_version_ids_json TEXT NOT NULL DEFAULT '[]',"
    " extra_json TEXT NOT NULL DEFAULT '{}'"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_versions_asset ON versions(asset_id);"
    "CREATE TABLE IF NOT EXISTS entity_asset_links ("
    " ord INTEGER PRIMARY KEY NOT NULL,"
    " entity_type TEXT NOT NULL DEFAULT '',"
    " entity_id TEXT NOT NULL DEFAULT '',"
    " asset_id TEXT NOT NULL DEFAULT '',"
    " role TEXT NOT NULL DEFAULT '',"
    " is_primary INTEGER NOT NULL DEFAULT 1,"
    " unresolved INTEGER NOT NULL DEFAULT 0,"
    " ordinal INTEGER NOT NULL DEFAULT 0,"
    " note TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_links_entity_role "
    " ON entity_asset_links(entity_type, entity_id, role);"
    "CREATE INDEX IF NOT EXISTS idx_links_asset ON entity_asset_links(asset_id);"
    "CREATE TABLE IF NOT EXISTS catalog_meta ("
    " key TEXT PRIMARY KEY NOT NULL,"
    " value TEXT NOT NULL"
    ");";
}

bool CatalogStorage::upsert(const CatalogEntity& v,std::string& error) const {
    return execute(connection_,QStringLiteral("INSERT INTO entities (id,entity_type,name,surface_x,surface_y,has_surface,kb,td,coordinate_status,inline_min,inline_max,xline_min,xline_max,sample_interval_us,start_time_ms,corners_json,extra_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET entity_type=excluded.entity_type,name=excluded.name,surface_x=excluded.surface_x,surface_y=excluded.surface_y,has_surface=excluded.has_surface,kb=excluded.kb,td=excluded.td,coordinate_status=excluded.coordinate_status,inline_min=excluded.inline_min,inline_max=excluded.inline_max,xline_min=excluded.xline_min,xline_max=excluded.xline_max,sample_interval_us=excluded.sample_interval_us,start_time_ms=excluded.start_time_ms,corners_json=excluded.corners_json,extra_json=excluded.extra_json"),{q(v.id), q(v.entityType), q(v.name), v.surfaceX, v.surfaceY, int(v.hasSurface), v.kb, v.td, q(v.coordinateStatus), v.inlineMin, v.inlineMax, v.xlineMin, v.xlineMax, v.sampleIntervalUs, v.startTimeMs, q(v.cornersJson), q(v.extraJson)},error);
}

bool CatalogStorage::upsert(const CatalogAsset& v,std::string& error) const {
    return execute(connection_,QStringLiteral("INSERT INTO assets (id,type,format,display_name) VALUES (?,?,?,?) ON CONFLICT(id) DO UPDATE SET type=excluded.type,format=excluded.format,display_name=excluded.display_name"),{q(v.id), q(v.type), q(v.format), q(v.displayName)},error);
}

bool CatalogStorage::upsert(const CatalogVersion& v,std::string& error) const {
    return execute(connection_,QStringLiteral("INSERT INTO versions (id,asset_id,stage,version_number,managed,path,source_uri,sha256,file_name,parent_version_ids_json,extra_json) VALUES (?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET asset_id=excluded.asset_id,stage=excluded.stage,version_number=excluded.version_number,managed=excluded.managed,path=excluded.path,source_uri=excluded.source_uri,sha256=excluded.sha256,file_name=excluded.file_name,parent_version_ids_json=excluded.parent_version_ids_json,extra_json=excluded.extra_json"),{q(v.id), q(v.assetId), q(v.stage), v.versionNumber, int(v.managed), q(v.path), q(v.sourceUri), q(v.sha256), q(v.fileName), q(v.parentVersionIdsJson), q(v.extraJson)},error);
}

bool CatalogStorage::upsert(const CatalogLink& v,std::string& error) const {
    return execute(connection_,QStringLiteral("INSERT INTO entity_asset_links (ord,entity_type,entity_id,asset_id,role,is_primary,unresolved,ordinal,note) VALUES (?,?,?,?,?,?,?,?,?) ON CONFLICT(ord) DO UPDATE SET entity_type=excluded.entity_type,entity_id=excluded.entity_id,asset_id=excluded.asset_id,role=excluded.role,is_primary=excluded.is_primary,unresolved=excluded.unresolved,ordinal=excluded.ordinal,note=excluded.note"),{v.ord, q(v.entityType), q(v.entityId), q(v.assetId), q(v.role), int(v.isPrimary), int(v.unresolved), v.ordinal, q(v.note)},error);
}

bool CatalogStorage::load(CatalogSnapshot& result,std::string& error) const {
    result={}; CatalogSnapshot next; QSqlQuery rows;
    if(!execute(connection_,QStringLiteral("SELECT key,value FROM catalog_meta"),{},error,&rows)) return false;
    while(rows.next()) next.meta.emplace_back(s(rows.value(0).toString()),s(rows.value(1).toString()));
    if(rows.lastError().isValid()) { error=s(rows.lastError().text()); return false; }
    if(!execute(connection_,QStringLiteral("SELECT id,entity_type,name,surface_x,surface_y,has_surface,kb,td,coordinate_status,inline_min,inline_max,xline_min,xline_max,sample_interval_us,start_time_ms,corners_json,extra_json FROM entities ORDER BY rowid"),{},error,&rows)) return false;
    while(rows.next()) {
        CatalogEntity v;
        v.id=s(rows.value(0).toString());
        v.entityType=s(rows.value(1).toString());
        v.name=s(rows.value(2).toString());
        v.surfaceX=rows.value(3).toDouble();
        v.surfaceY=rows.value(4).toDouble();
        v.hasSurface=(rows.value(5).toInt()!=0);
        v.kb=(rows.value(6).isNull()?std::numeric_limits<double>::quiet_NaN():rows.value(6).toDouble());
        v.td=(rows.value(7).isNull()?std::numeric_limits<double>::quiet_NaN():rows.value(7).toDouble());
        v.coordinateStatus=s(rows.value(8).toString());
        v.inlineMin=rows.value(9).toDouble();
        v.inlineMax=rows.value(10).toDouble();
        v.xlineMin=rows.value(11).toDouble();
        v.xlineMax=rows.value(12).toDouble();
        v.sampleIntervalUs=rows.value(13).toDouble();
        v.startTimeMs=rows.value(14).toDouble();
        v.cornersJson=s(rows.value(15).toString());
        v.extraJson=s(rows.value(16).toString());
        next.entities.push_back(std::move(v));
    }
    if(rows.lastError().isValid()) { error=s(rows.lastError().text()); return false; }
    if(!execute(connection_,QStringLiteral("SELECT id,type,format,display_name FROM assets ORDER BY rowid"),{},error,&rows)) return false;
    while(rows.next()) {
        CatalogAsset v;
        v.id=s(rows.value(0).toString());
        v.type=s(rows.value(1).toString());
        v.format=s(rows.value(2).toString());
        v.displayName=s(rows.value(3).toString());
        next.assets.push_back(std::move(v));
    }
    if(rows.lastError().isValid()) { error=s(rows.lastError().text()); return false; }
    if(!execute(connection_,QStringLiteral("SELECT id,asset_id,stage,version_number,managed,path,source_uri,sha256,file_name,parent_version_ids_json,extra_json FROM versions ORDER BY rowid"),{},error,&rows)) return false;
    while(rows.next()) {
        CatalogVersion v;
        v.id=s(rows.value(0).toString());
        v.assetId=s(rows.value(1).toString());
        v.stage=s(rows.value(2).toString());
        v.versionNumber=rows.value(3).toInt();
        v.managed=(rows.value(4).toInt()!=0);
        v.path=s(rows.value(5).toString());
        v.sourceUri=s(rows.value(6).toString());
        v.sha256=s(rows.value(7).toString());
        v.fileName=s(rows.value(8).toString());
        v.parentVersionIdsJson=s(rows.value(9).toString());
        v.extraJson=s(rows.value(10).toString());
        next.versions.push_back(std::move(v));
    }
    if(rows.lastError().isValid()) { error=s(rows.lastError().text()); return false; }
    if(!execute(connection_,QStringLiteral("SELECT ord,entity_type,entity_id,asset_id,role,is_primary,unresolved,ordinal,note FROM entity_asset_links ORDER BY ord"),{},error,&rows)) return false;
    while(rows.next()) {
        CatalogLink v;
        v.ord=rows.value(0).toInt();
        v.entityType=s(rows.value(1).toString());
        v.entityId=s(rows.value(2).toString());
        v.assetId=s(rows.value(3).toString());
        v.role=s(rows.value(4).toString());
        v.isPrimary=(rows.value(5).toInt()!=0);
        v.unresolved=(rows.value(6).toInt()!=0);
        v.ordinal=rows.value(7).toInt();
        v.note=s(rows.value(8).toString());
        next.links.push_back(std::move(v));
    }
    if(rows.lastError().isValid()) { error=s(rows.lastError().text()); return false; }
    result=std::move(next); return true;
}
bool CatalogStorage::eraseAsset(const std::string& id,std::string& error) const {
    return execute(connection_,QStringLiteral("DELETE FROM assets WHERE id=?"),{q(id)},error);
}
bool CatalogStorage::eraseVersion(const std::string& id,std::string& error) const {
    return execute(connection_,QStringLiteral("DELETE FROM versions WHERE id=?"),{q(id)},error);
}
bool CatalogStorage::clearLinks(std::string& error) const {
    return execute(connection_,QStringLiteral("DELETE FROM entity_asset_links"),{},error);
}
bool CatalogStorage::clearRecords(std::string& error) const {
    // Caller holds a transaction; no commit or recovery policy is hidden here.
    for(const auto* table:{"entity_asset_links","versions","assets","entities"})
        if(!execute(connection_,QStringLiteral("DELETE FROM ")+QString::fromLatin1(table),{},error)) return false;
    return true;
}
bool CatalogStorage::writeMeta(const std::vector<std::pair<std::string,std::string>>& rows,std::string& error) const {
    for(const auto& [key,value]:rows)
        if(!execute(connection_,QStringLiteral("INSERT INTO catalog_meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"),{q(key),q(value)},error)) return false;
    return true;
}
}
