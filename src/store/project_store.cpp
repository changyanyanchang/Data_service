#include "project_store.h"
#include "interpretation_store.h"
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QJsonDocument>
#include <QtEndian>
#include <atomic>
#include <cstring>
#include <limits>

namespace paleo::data::detail {
QVariant number(double v){return std::isfinite(v)?QVariant(v):QVariant();}
double value(const QVariant& v){return v.isNull()?Missing:v.toDouble();}
QByteArray pack(const std::vector<double>& values){
    QByteArray bytes;bytes.resize(qsizetype(values.size()*8));
    for(size_t i=0;i<values.size();++i){quint64 bits;std::memcpy(&bits,&values[i],8);qToLittleEndian(bits,bytes.data()+i*8);}return bytes;
}
bool unpack(const QByteArray& bytes,std::vector<double>& values,std::string& error){
    if(bytes.size()%8){error="数值数组字节长度损坏";return false;}values.resize(size_t(bytes.size()/8));
    for(size_t i=0;i<values.size();++i){quint64 bits=qFromLittleEndian<quint64>(bytes.constData()+i*8);std::memcpy(&values[i],&bits,8);}return true;
}
QString json(const QJsonObject& v){return QString::fromUtf8(QJsonDocument(v).toJson(QJsonDocument::Compact));}
QString json(const QJsonArray& v){return QString::fromUtf8(QJsonDocument(v).toJson(QJsonDocument::Compact));}
QJsonObject object(const QString& text){return QJsonDocument::fromJson(text.toUtf8()).object();}
QJsonObject depthJson(const DepthInfo& d){return {{"type",q(d.depthType)},{"unit",q(d.depthUnit)},{"md",q(d.mdReference)},{"tvd",q(d.tvdReference)},{"ss",q(d.tvdssReference)},{"direction",q(d.tvdssDirection)}};}
DepthInfo readDepth(const QJsonObject& o){DepthInfo d;d.depthType=s(o["type"].toString());d.depthUnit=s(o["unit"].toString());d.mdReference=s(o["md"].toString());d.tvdReference=s(o["tvd"].toString());d.tvdssReference=s(o["ss"].toString());d.tvdssDirection=s(o["direction"].toString());return d;}
QJsonArray measurementsJson(const std::vector<Measurement>& a){QJsonArray out;for(auto& m:a)out.append(QJsonObject{{"name",q(m.measurementName)},{"unit",q(m.measurementUnit)},{"value",std::isfinite(m.numericValue)?QJsonValue(m.numericValue):QJsonValue()},{"text",q(m.originalText)}});return out;}
std::vector<Measurement> readMeasurements(const QJsonArray& a){std::vector<Measurement> out;for(auto v:a){auto o=v.toObject();Measurement m;m.measurementName=s(o["name"].toString());m.measurementUnit=s(o["unit"].toString());m.numericValue=o["value"].isDouble()?o["value"].toDouble():Missing;m.originalText=s(o["text"].toString());out.push_back(m);}return out;}
bool ProjectStore::exec(const QString& sql,const QVariantList& args,std::string& error,QSqlQuery* result)const{
    if(!db.isOpen()){error="请先打开工区";return false;}QSqlQuery query(db);if(!query.prepare(sql)){error=s(query.lastError().text());return false;}for(const auto& a:args)query.addBindValue(a);
    if(!query.exec()){error=s(query.lastError().text());return false;}if(result)*result=std::move(query);return true;
}
bool ProjectStore::begin(std::string& e){return exec("BEGIN IMMEDIATE",{},e);}
bool ProjectStore::commit(std::string& e){return exec("COMMIT",{},e);}
void ProjectStore::rollback(){std::string ignore;exec("ROLLBACK",{},ignore);}
void ProjectStore::close(){if(db.isValid()){db.close();db=QSqlDatabase();}if(!connection.isEmpty())QSqlDatabase::removeDatabase(connection);connection.clear();root.clear();name.clear();readOnly=false;}
QString ProjectStore::absolutePath(const QString& p)const{return QFileInfo(p).isAbsolute()?p:QDir(root).absoluteFilePath(p);}
// Additive and once per database: preserve IDs, manual edits and the original schema version.
static bool ensureImportPerformanceSchema(ProjectStore& st,std::string& error){
    QSqlQuery existing;
    if(!st.exec("SELECT value FROM ds_meta WHERE key='import_performance_schema'",{},error,&existing))return false;
    if(existing.next()&&existing.value(0).toString()=="2")return true;
    existing.finish();
    if(!st.begin(error))return false;
    const QStringList statements={
        "CREATE TABLE IF NOT EXISTS source_scan_cache(source_id INTEGER PRIMARY KEY REFERENCES sources(id) ON DELETE CASCADE,signature TEXT NOT NULL)",
        "CREATE INDEX IF NOT EXISTS issues_source_resolved ON issues(source_id,resolved)",
        "CREATE INDEX IF NOT EXISTS log_sets_well ON log_sets(well_id)",
        "CREATE INDEX IF NOT EXISTS log_sets_source ON log_sets(source_id)",
        "CREATE INDEX IF NOT EXISTS curves_set ON curves(set_id)",
        "CREATE INDEX IF NOT EXISTS tracks_well ON tracks(well_id)",
        "CREATE INDEX IF NOT EXISTS tracks_source ON tracks(source_id)",
        "CREATE INDEX IF NOT EXISTS sample_tables_well ON sample_tables(well_id)",
        "CREATE INDEX IF NOT EXISTS sample_tables_source ON sample_tables(source_id)",
        "CREATE INDEX IF NOT EXISTS source_tables_source ON source_tables(source_id)",
        "CREATE INDEX IF NOT EXISTS samples_table ON samples(table_id)",
        "CREATE INDEX IF NOT EXISTS measurements_sample ON measurements(sample_id)",
        "CREATE INDEX IF NOT EXISTS well_objects_well_kind ON well_objects(well_id,kind)",
        "CREATE INDEX IF NOT EXISTS well_objects_source_kind ON well_objects(source_id,kind)",
        "INSERT OR REPLACE INTO ds_meta(key,value) VALUES('import_performance_schema','2')"
    };
    for(const auto& sql:statements)if(!st.exec(sql,{},error)){st.rollback();return false;}
    if(!st.commit(error)){st.rollback();return false;}return true;
}
static bool ensureFileGraphSchema(ProjectStore& st,std::string& error){
    QSqlQuery existing;
    if(!st.exec("SELECT value FROM ds_meta WHERE key='file_graph_schema'",{},error,&existing))return false;
    if(existing.next()&&existing.value(0).toString()=="1")return true;
    existing.finish();
    if(!st.begin(error))return false;
    const QStringList statements={
        "CREATE TABLE IF NOT EXISTS source_relations(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,target_source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,relation TEXT NOT NULL,note TEXT NOT NULL DEFAULT '',created TEXT DEFAULT CURRENT_TIMESTAMP,UNIQUE(source_id,target_source_id,relation),CHECK(source_id<>target_source_id))",
        "CREATE INDEX IF NOT EXISTS source_relations_target ON source_relations(target_source_id)",
        "INSERT OR REPLACE INTO ds_meta(key,value) VALUES('file_graph_schema','1')"
    };
    for(const auto& sql:statements)if(!st.exec(sql,{},error)){st.rollback();return false;}
    if(!st.commit(error)){st.rollback();return false;}return true;
}
static bool ensureCoordinateSystemSchema(ProjectStore& st,std::string& error){
    QSqlQuery existing;
    if(!st.exec("SELECT value FROM ds_meta WHERE key='coordinate_system_schema'",{},error,&existing))return false;
    if(existing.next()&&existing.value(0).toString()=="1")return true;
    existing.finish();
    if(!st.begin(error))return false;
    const QStringList statements={
        "CREATE TABLE IF NOT EXISTS source_coordinate_systems(source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,object_kind TEXT NOT NULL,object_id INTEGER NOT NULL DEFAULT 0,crs TEXT NOT NULL,unit TEXT NOT NULL DEFAULT '',PRIMARY KEY(source_id,object_kind,object_id))",
        "CREATE INDEX IF NOT EXISTS source_coordinate_systems_code ON source_coordinate_systems(crs)",
        "INSERT OR REPLACE INTO ds_meta(key,value) VALUES('coordinate_system_schema','1')"
    };
    for(const auto& sql:statements)if(!st.exec(sql,{},error)){st.rollback();return false;}
    if(!st.commit(error)){st.rollback();return false;}return true;
}
bool ProjectStore::open(const QString& directory,bool create,const QString& projectName,std::string& error,bool readonly){
    close();error.clear();QDir dir(directory);if(create){if(QFileInfo::exists(dir.filePath("data.sqlite"))){error="该目录已有工区，请使用打开工区";return false;}if(!dir.mkpath(".")){error="无法创建工区目录";return false;}}
    if(!create&&!QFileInfo::exists(dir.filePath("data.sqlite"))){error="目录中没有 data.sqlite";return false;}
    readOnly=readonly;root=dir.absolutePath();static std::atomic<long long> serial{0};connection=QString("paleo_data_%1").arg(++serial);
    db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(dir.filePath("data.sqlite"));
    if(readOnly)db.setConnectOptions("QSQLITE_OPEN_READONLY");
    if(!db.open()){error=s(db.lastError().text());close();return false;}
    if(!exec("PRAGMA foreign_keys=ON",{},error)||!exec("PRAGMA busy_timeout=5000",{},error)||(!readOnly&&!exec("PRAGMA journal_mode=WAL",{},error))){close();return false;}
    if(!create){QSqlQuery r;if(!exec("SELECT value FROM ds_meta WHERE key='schema_version'",{},error,&r)||!r.next()||r.value(0).toInt()!=1){error="工区格式不受支持";close();return false;}if(!exec("SELECT value FROM ds_meta WHERE key='project_name'",{},error,&r))return false;if(r.next())name=r.value(0).toString();
        if(readOnly)return true;
        // Additive upgrade: keep existing rows/IDs and handwritten edits intact.
        r.finish();bool hasSheet=false;
        if(!exec("PRAGMA table_info(log_sets)",{},error,&r))return false;
        while(r.next())if(r.value(1).toString()=="sheet")hasSheet=true;r.finish();
        if(!hasSheet){
            if(!begin(error))return false;
            // Another connection might have upgraded while this connection waited for the lock.
            if(!exec("PRAGMA table_info(log_sets)",{},error,&r)){rollback();return false;}
            while(r.next())if(r.value(1).toString()=="sheet")hasSheet=true;r.finish();
            if((!hasSheet&&!exec("ALTER TABLE log_sets ADD COLUMN sheet TEXT DEFAULT ''",{},error))||!commit(error)){rollback();return false;}
        }
        if(!ensureImportPerformanceSchema(*this,error)||!ensureFileGraphSchema(*this,error)||!ensureCoordinateSystemSchema(*this,error)||!interpretation::ensureSchema(*this,error)){close();return false;}return true;
    }
    const QString schema=QString::fromUtf8(R"SQL(
CREATE TABLE ds_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
CREATE TABLE sources(id INTEGER PRIMARY KEY AUTOINCREMENT,original_path TEXT NOT NULL,stored_path TEXT NOT NULL,hash TEXT NOT NULL,options TEXT NOT NULL,kind TEXT,status TEXT,message TEXT,bytes INTEGER,document TEXT,active INTEGER NOT NULL DEFAULT 1,created TEXT DEFAULT CURRENT_TIMESTAMP);
CREATE INDEX sources_identity ON sources(original_path,hash,options);
CREATE TABLE issues(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER REFERENCES sources(id),code TEXT,message TEXT,original_text TEXT,sheet TEXT,row_num INTEGER,blocking INTEGER,resolved INTEGER DEFAULT 0,resolution TEXT DEFAULT '');
CREATE TABLE wells(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL UNIQUE,field_name TEXT,well_type TEXT,x REAL,y REAL,crs TEXT,unit TEXT,total_depth REAL,remark TEXT,depth_json TEXT DEFAULT '{}');
CREATE TABLE log_sets(id INTEGER PRIMARY KEY AUTOINCREMENT,well_id INTEGER REFERENCES wells(id),source_id INTEGER REFERENCES sources(id),name TEXT,depth_json TEXT,depths BLOB,tvd BLOB,tvdss BLOB,step REAL,sample_count INTEGER,encoding TEXT DEFAULT 'float64-le',sheet TEXT DEFAULT '');
CREATE TABLE curves(id INTEGER PRIMARY KEY AUTOINCREMENT,set_id INTEGER REFERENCES log_sets(id) ON DELETE CASCADE,name TEXT,unit TEXT,description TEXT,raw_values BLOB NOT NULL,processed_values BLOB,quality BLOB,rule_json TEXT DEFAULT '{}');
CREATE TABLE tracks(id INTEGER PRIMARY KEY AUTOINCREMENT,well_id INTEGER REFERENCES wells(id),source_id INTEGER REFERENCES sources(id),name TEXT,type TEXT,depth_json TEXT,sheet TEXT);
CREATE TABLE intervals(id INTEGER PRIMARY KEY AUTOINCREMENT,track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,ordinal INTEGER,original_id INTEGER,name TEXT,top REAL,bottom REAL,top_tvd REAL,bottom_tvd REAL,top_ss REAL,bottom_ss REAL,facies TEXT,top_unc INTEGER,bottom_unc INTEGER,not_penetrated INTEGER,properties TEXT,source_row INTEGER);
CREATE INDEX intervals_depth ON intervals(track_id,top,bottom);
CREATE TABLE sample_tables(id INTEGER PRIMARY KEY AUTOINCREMENT,well_id INTEGER REFERENCES wells(id),source_id INTEGER REFERENCES sources(id),name TEXT,sheet TEXT);
CREATE TABLE samples(id INTEGER PRIMARY KEY AUTOINCREMENT,table_id INTEGER REFERENCES sample_tables(id) ON DELETE CASCADE,number TEXT,type TEXT,source_range TEXT,top REAL,bottom REAL);
CREATE TABLE measurements(id INTEGER PRIMARY KEY AUTOINCREMENT,sample_id INTEGER REFERENCES samples(id) ON DELETE CASCADE,name TEXT,unit TEXT,value REAL,original_text TEXT);
CREATE TABLE well_objects(id INTEGER PRIMARY KEY AUTOINCREMENT,well_id INTEGER REFERENCES wells(id),source_id INTEGER REFERENCES sources(id),kind TEXT,name TEXT,payload TEXT);
CREATE TABLE volumes(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER REFERENCES sources(id),name TEXT,path TEXT,format TEXT,amplitude_unit TEXT,geometry TEXT,sampling TEXT);
CREATE TABLE horizons(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER REFERENCES sources(id),name TEXT,volume_id INTEGER REFERENCES volumes(id),crs TEXT,unit TEXT,time_unit TEXT,time_type TEXT,time_reference TEXT);
CREATE TABLE horizon_points(id INTEGER PRIMARY KEY AUTOINCREMENT,horizon_id INTEGER REFERENCES horizons(id) ON DELETE CASCADE,seq INTEGER,inline_no INTEGER,crossline_no INTEGER,x REAL,y REAL,z REAL);
CREATE INDEX horizon_line ON horizon_points(horizon_id,inline_no,crossline_no);
CREATE INDEX horizon_order ON horizon_points(horizon_id,seq);
CREATE TABLE sections(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT,volume_id INTEGER REFERENCES volumes(id),crs TEXT,unit TEXT);
CREATE TABLE section_points(section_id INTEGER REFERENCES sections(id) ON DELETE CASCADE,seq INTEGER,x REAL,y REAL,PRIMARY KEY(section_id,seq));
CREATE TABLE maps(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER REFERENCES sources(id),name TEXT,type TEXT,area TEXT,strata TEXT,image_path TEXT,source_file TEXT,source_location TEXT,crs TEXT,geo_status INTEGER,geo_file TEXT,description TEXT);
CREATE TABLE source_tables(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER REFERENCES sources(id),name TEXT,rows INTEGER,columns INTEGER);
CREATE TABLE source_cells(table_id INTEGER REFERENCES source_tables(id) ON DELETE CASCADE,row_num INTEGER,col_num INTEGER,text TEXT,PRIMARY KEY(table_id,row_num,col_num));
CREATE TABLE changes(id INTEGER PRIMARY KEY AUTOINCREMENT,kind TEXT,entity_id INTEGER,description TEXT,created TEXT DEFAULT CURRENT_TIMESTAMP);
)SQL");
    if(!begin(error))return false;for(auto statement:schema.split(';',Qt::SkipEmptyParts)){statement=statement.trimmed();if(statement.isEmpty())continue;if(!exec(statement,{},error)){rollback();return false;}}
    if(!exec("INSERT INTO ds_meta VALUES('schema_version','1'),('project_name',?)",{projectName},error)||!commit(error)){rollback();return false;}name=projectName;dir.mkpath("sources");dir.mkpath("images");dir.mkpath("exports");if(!ensureImportPerformanceSchema(*this,error)||!ensureFileGraphSchema(*this,error)||!ensureCoordinateSystemSchema(*this,error)||!interpretation::ensureSchema(*this,error)){close();return false;}return true;
}
bool ProjectStore::ensureWell(const WellInfo& w,long long& id,std::string& e){
    if(w.wellName.empty()){e="井名为空，不能保存";return false;}QSqlQuery r;
    if(!exec("INSERT INTO wells(name,field_name,well_type,x,y,crs,unit,total_depth,remark) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(name) DO NOTHING",{q(w.wellName),q(w.fieldName),q(w.wellType),number(w.wellheadX),number(w.wellheadY),q(w.coordinateSystem),q(w.coordinateUnit),number(w.totalDrilledDepth),q(w.remark)},e))return false;
    if(!exec("SELECT id FROM wells WHERE name=?",{q(w.wellName)},e,&r)||!r.next()){if(e.empty())e="井保存失败";return false;}id=r.value(0).toLongLong();return true;
}
bool ProjectStore::removeSourceObjects(long long id,std::string& e){
    for(const QString table:{"source_coordinate_systems","log_sets","tracks","sample_tables","well_objects","horizons","maps","source_tables","volumes"})if(!exec("DELETE FROM "+table+" WHERE source_id=?",{id},e))return false;
    return exec("UPDATE sources SET active=0 WHERE id=?",{id},e);
}
}
