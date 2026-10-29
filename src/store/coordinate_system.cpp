#include "project_store.h"
#include <QRegularExpression>
#include <map>
#include <set>
#include <vector>

namespace paleo::data::detail {
namespace {
QString canonicalCrs(const QString& value){
    const auto text=value.trimmed();
    return text.startsWith("EPSG:",Qt::CaseInsensitive)?text.toUpper():text;
}
bool isEpsg(const QString& value){
    if(!value.startsWith("EPSG:"))return false;
    bool digits=false;const auto number=value.mid(5).toLongLong(&digits);return digits&&number>0;
}
// A WGS84 UTM zone written as a name is an explicit CRS. Normalize it to EPSG
// so it is not replaced by the undeclared-EPSG default.
QString declaredEpsg(const QString& value){
    const auto code=canonicalCrs(value);
    if(isEpsg(code))return code;
    const auto upper=value.toUpper();
    QRegularExpression named(QStringLiteral("UTM84-(\\d{1,2})([NS])\\b"));
    auto match=named.match(upper);
    QString hemisphere;
    if(match.hasMatch())hemisphere=match.captured(2);
    else{
        QRegularExpression spelled(QStringLiteral("WGS\\s*1984\\s*UTM,\\s*ZONE\\s*(\\d{1,2})\\s*(NORTH|SOUTH)\\b"));
        match=spelled.match(upper);
        if(!match.hasMatch())return {};
        hemisphere=match.captured(2);
    }
    bool ok=false;const int zone=match.captured(1).toInt(&ok);
    if(!ok||zone<1||zone>60)return {};
    return QStringLiteral("EPSG:%1").arg((hemisphere.startsWith(QLatin1Char('S'))?32700:32600)+zone);
}
QString defaultProjectCrs(){return QStringLiteral("EPSG:32650");}
bool normalizeStoredCoordinates(ProjectStore& store,std::string& error){
    QSqlQuery rows;
    if(!store.exec("SELECT source_id,object_kind,object_id,crs,unit FROM source_coordinate_systems",{},error,&rows))return false;
    struct Candidate{long long source=0,object=0;QString kind,crs,unit;};
    std::vector<Candidate> candidates;
    while(rows.next()){
        const auto crs=rows.value(3).toString();const auto resolved=declaredEpsg(crs);
        if(!isEpsg(resolved)||canonicalCrs(crs)==resolved)continue;
        auto unit=rows.value(4).toString().trimmed();if(unit.isEmpty())unit=QStringLiteral("m");
        candidates.push_back({rows.value(0).toLongLong(),rows.value(2).toLongLong(),rows.value(1).toString(),resolved,unit});
    }
    rows.finish();
    for(const auto& row:candidates)
        if(!store.exec("UPDATE source_coordinate_systems SET crs=?,unit=? WHERE source_id=? AND object_kind=? AND object_id=?",{row.crs,row.unit,row.source,row.kind,row.object},error))return false;
    if(!store.exec("SELECT id,crs,unit,x,y FROM wells",{},error,&rows))return false;
    struct WellRow{long long id=0;QString crs,unit;};
    std::vector<WellRow> wells;
    while(rows.next()){
        const auto crs=rows.value(1).toString();const auto resolved=declaredEpsg(crs);
        const bool positioned=!rows.value(3).isNull()&&!rows.value(4).isNull();
        QString next;
        if(isEpsg(resolved)&&canonicalCrs(crs)!=resolved)next=resolved;
        else if(crs.trimmed().isEmpty()&&positioned)next=defaultProjectCrs();
        if(next.isEmpty())continue;
        auto unit=rows.value(2).toString().trimmed();if(unit.isEmpty())unit=QStringLiteral("m");
        wells.push_back({rows.value(0).toLongLong(),next,unit});
    }
    rows.finish();
    for(const auto& well:wells)
        if(!store.exec("UPDATE wells SET crs=?,unit=? WHERE id=?",{well.crs,well.unit,well.id},error))return false;
    return true;
}
struct CandidateRows {
    std::vector<ProjectCrsCandidate> items;
    std::map<QString,std::set<QString>> groups;
    bool hasConflict()const{
        if(groups.size()>1)return true;
        return !groups.empty()&&groups.begin()->second.size()>1;
    }
};
bool readCandidates(const ProjectStore& store,CandidateRows& result,std::string& error){
    QSqlQuery rows;
    if(!store.exec("SELECT c.source_id,c.object_kind,c.object_id,c.crs,c.unit,s.original_path FROM source_coordinate_systems c JOIN sources s ON s.id=c.source_id WHERE s.active=1 ORDER BY c.source_id,c.object_kind,c.object_id",{},error,&rows))return false;
    while(rows.next()){
        ProjectCrsCandidate candidate;candidate.sourceId=rows.value(0).toLongLong();candidate.objectKind=s(rows.value(1).toString());
        candidate.objectId=rows.value(2).toLongLong();candidate.coordinateSystem=s(rows.value(3).toString());
        candidate.coordinateUnit=s(rows.value(4).toString());candidate.sourcePath=s(rows.value(5).toString());
        const auto code=canonicalCrs(rows.value(3).toString());const auto unit=rows.value(4).toString().trimmed();
        if(!code.isEmpty()&&!unit.isEmpty())result.groups[code].insert(unit);else if(!code.isEmpty())result.groups[code];
        result.items.push_back(std::move(candidate));
    }
    return true;
}
}
bool ProjectStore::saveCoordinateSystemCandidate(long long sourceId,const std::string& objectKind,long long objectId,
                                                  const std::string& crs,const std::string& unit,std::string& error){
    const auto code=q(crs).trimmed();if(code.isEmpty())return true;
    if(sourceId<=0||objectKind.empty()||objectId<0){error="坐标系统候选来源无效";return false;}
    return exec("INSERT INTO source_coordinate_systems(source_id,object_kind,object_id,crs,unit) VALUES(?,?,?,?,?) ON CONFLICT(source_id,object_kind,object_id) DO UPDATE SET crs=excluded.crs,unit=excluded.unit",
                {sourceId,q(objectKind),objectId,code,q(unit).trimmed()},error);
}
bool ProjectStore::saveParsedCoordinateSystems(long long sourceId,const ParsedFile& file,std::string& error){
    int declaration=0;
    for(const auto& value:file.coordinateSystems)
        if(!saveCoordinateSystemCandidate(sourceId,"source",++declaration,value.coordinateSystem,value.coordinateUnit,error))return false;
    QSqlQuery rows;
    if(!exec("SELECT id,geometry FROM volumes WHERE source_id=? ORDER BY id",{sourceId},error,&rows))return false;
    while(rows.next()){
        const auto geometry=object(rows.value(1).toString());
        if(!saveCoordinateSystemCandidate(sourceId,"volume",rows.value(0).toLongLong(),s(geometry["crs"].toString()),s(geometry["unit"].toString()),error))return false;
    }
    rows.finish();
    if(!exec("SELECT id,crs,unit FROM horizons WHERE source_id=? ORDER BY id",{sourceId},error,&rows))return false;
    while(rows.next())if(!saveCoordinateSystemCandidate(sourceId,"horizon",rows.value(0).toLongLong(),s(rows.value(1).toString()),s(rows.value(2).toString()),error))return false;
    rows.finish();
    if(!exec("SELECT id,crs FROM maps WHERE source_id=? AND geo_status=1 ORDER BY id",{sourceId},error,&rows))return false;
    while(rows.next())if(!saveCoordinateSystemCandidate(sourceId,"map",rows.value(0).toLongLong(),s(rows.value(1).toString()),{},error))return false;
    return true;
}
bool ProjectStore::updateProjectCoordinateSystem(std::string& error){
    if(!normalizeStoredCoordinates(*this,error))return false;
    QSqlQuery meta;QString current,basis;
    if(!exec("SELECT key,value FROM ds_meta WHERE key IN ('project_crs','project_crs_basis')",{},error,&meta))return false;
    while(meta.next()){
        if(meta.value(0).toString()=="project_crs")current=meta.value(1).toString();
        else basis=meta.value(1).toString();
    }
    meta.finish();
    if(basis=="manual"||(!current.isEmpty()&&basis.isEmpty()))return true;
    if(!basis.isEmpty()&&basis!="import"&&basis!="default")return true;
    CandidateRows candidates;if(!readCandidates(*this,candidates,error))return false;
    if(!candidates.hasConflict()&&candidates.groups.size()==1){
        const auto code=declaredEpsg(candidates.groups.begin()->first);
        if(isEpsg(code)){
            const auto& units=candidates.groups.begin()->second;
            const auto unit=units.empty()?QStringLiteral(""):*units.begin();
            return exec("INSERT OR REPLACE INTO ds_meta(key,value) VALUES('project_crs',?),('project_crs_unit',?),('project_crs_basis','import')",{code,unit},error);
        }
    }
    // No explicit EPSG. Keep a named non-EPSG declaration as a candidate, and
    // use EPSG:32650 (WGS 84 / UTM zone 50N) as the project CRS. A conflict
    // among declared systems is not papered over with that default.
    if(!candidates.hasConflict())
        return exec("INSERT OR REPLACE INTO ds_meta(key,value) VALUES('project_crs',?),('project_crs_unit',?),('project_crs_basis','default')",{defaultProjectCrs(),QStringLiteral("m")},error);
    return exec("DELETE FROM ds_meta WHERE key IN ('project_crs','project_crs_unit','project_crs_basis')",{},error);
}
}

namespace paleo::data {
using namespace detail;
bool DataService::readProjectCrsInfo(ProjectCrsInfo& result,std::string& error)const{
    result={};error.clear();auto& store=impl_->store;
    if(!store.exec("BEGIN",{},error))return false;
    struct Rollback { const ProjectStore& store;~Rollback(){std::string ignored;store.exec("ROLLBACK",{},ignored);} } rollback{store};
    ProjectCrsInfo next;QSqlQuery rows;
    if(!store.exec("SELECT key,value FROM ds_meta WHERE key IN ('project_crs','project_crs_unit','project_crs_basis')",{},error,&rows))return false;
    while(rows.next()){
        const auto key=rows.value(0).toString();const auto value=s(rows.value(1).toString());
        if(key=="project_crs")next.coordinateSystem=value;else if(key=="project_crs_unit")next.coordinateUnit=value;else next.selectionBasis=value;
    }
    rows.finish();
    if(!next.coordinateSystem.empty()&&next.selectionBasis.empty())next.selectionBasis="manual";
    bool hasTable=false;
    if(!store.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='source_coordinate_systems'",{},error,&rows))return false;
    hasTable=rows.next();rows.finish();
    if(hasTable){
        CandidateRows candidates;if(!readCandidates(store,candidates,error))return false;
        next.hasConflict=candidates.hasConflict();next.candidates=std::move(candidates.items);
    }
    result=std::move(next);return true;
}
}
