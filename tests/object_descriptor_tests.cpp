#include "paleo_data/service.h"
#include "paleo_data/object_descriptor.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
std::string u(const QString& value){return value.toUtf8().toStdString();}
void require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
QVariant sql(const QString& project,const QString& command,const QVariantList& values={}) {
    const auto name="object-test-"+QUuid::createUuid().toString();QVariant out;std::string error;
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(project+"/data.sqlite");
        if(!db.open())error=u(db.lastError().text());else {
            QSqlQuery query(db);if(!query.prepare(command))error=u(query.lastError().text());else {
                for(const auto& value:values)query.addBindValue(value);
                if(!query.exec())error=u(query.lastError().text());else if(query.next())out=query.value(0);
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);require(error.empty(),error);return out;
}
void removeRevisionSchema(const QString& project) {
    for(const auto& table:QStringList{"time_depth","tracks","intervals","volumes"})
        for(const auto& operation:QStringList{"ai","au","ad"})sql(project,"DROP TRIGGER pds_revision_"+table+"_"+operation);
    sql(project,"DROP TABLE object_revisions");sql(project,"DELETE FROM ds_meta WHERE key IN ('object_revision_schema','object_revision_epoch')");
}
DataObjectDescriptor describe(DataService& service,DataObjectKind kind,long long id) {
    DataObjectDescriptor out;std::string error;require(service.describeDataObject(kind,id,out,error),error);return out;
}
void exercise(const QString& project,const QString& source) {
    DataService service;std::string error;require(service.createProject(u(project),"Descriptor synthetic",error),error);
    {QFile file(source);require(file.open(QIODevice::WriteOnly)&&file.write("synthetic")==9,"Write private fixture");}
    ParsedFile parsed;parsed.kind="test";parsed.status="parsed";WellDataset well;well.wellInfo.wellName="SYNTH-A";
    for(int i=0;i<2;++i) {
        TimeDepthTable table;table.relationName="same-name";table.depthInfo.depthType="MD";table.depthInfo.depthUnit="m";
        table.timeUnit="ms";table.timeType="TWT";table.timeReference="fixture";table.depthTimePairs={{100,double(200+i*100)},{200,double(400+i*100)}};
        well.timeDepthRelations.push_back(table);
        IntervalTrack track;track.trackName="same-track";track.trackType="stratigraphy";track.depthInfo=table.depthInfo;GeologicalInterval row;row.intervalName="unit";row.topDepth=100;row.bottomDepth=200;
        track.intervals.push_back(row);well.wellbore.intervalTracks.push_back(track);
    }
    parsed.wells.push_back(well);long long sourceId=0;
    const auto sha=QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex().toStdString();
    require(service.importPreparedFile(u(source),u(source),u(source),sha,parsed,sourceId,error),error);
    std::vector<DataEntry> relations,tracks;require(service.listSourceData("time_depth",sourceId,relations,error)&&relations.size()==2,error);
    require(service.listSourceData("intervals",sourceId,tracks,error)&&tracks.size()==2,error);
    auto first=describe(service,DataObjectKind::TimeDepth,relations[0].id),second=describe(service,DataObjectKind::TimeDepth,relations[1].id);
    require(first.revisionKnown&&second.revisionKnown&&first.id!=second.id&&first.name==second.name&&second.sourceId==sourceId&&second.wellId==relations[1].wellId,"Exact same-name identity/ownership");
    TimeDepthTable value;require(service.readTimeDepthAtRevision(second.id,second.revision,value,error),error);
    require(value.depthTimePairs.front().timeValue==300,"Read the selected second relation, not the first same-name relation");
    require(!service.readTimeDepthAtRevision(second.id,second.revision,value,error,{},1)&&value.depthTimePairs.empty(),"Point budget must reject and clear output");
    require(!service.readTimeDepthAtRevision(second.id,second.revision,value,error,[]{return true;})&&value.depthTimePairs.empty(),"Cancellation must clear output");
    int checks=0;require(!service.readTimeDepthAtRevision(second.id,second.revision,value,error,[&]{return ++checks==4;})&&value.depthTimePairs.empty(),"Cancellation after numeric read must clear output");
    sql(project,"CREATE TRIGGER reject_semantic_audit BEFORE INSERT ON changes WHEN NEW.kind='time_depth' BEGIN SELECT RAISE(ABORT,'injected semantic audit failure'); END");
    require(!service.confirmTimeDepthSemantics(second.id,"ms","TWT","rejected datum",false,error),"Fail after objects and revision counters have changed");
    require(describe(service,DataObjectKind::TimeDepth,second.id).revision==second.revision,"Failed semantic transaction must roll back revision counters");
    require(service.readTimeDepthAtRevision(second.id,second.revision,value,error)&&value.timeReference=="fixture",error);
    sql(project,"DROP TRIGGER reject_semantic_audit");
    require(service.confirmTimeDepthSemantics(second.id,"ms","TWT","updated datum",false,error),error);
    require(!service.readTimeDepthAtRevision(second.id,second.revision,value,error)&&value.depthTimePairs.empty(),"Changed semantics must reject old revision");
    second=describe(service,DataObjectKind::TimeDepth,second.id);require(second.revisionKnown,"Changed object still has revision");
    sql(project,"UPDATE well_objects SET name='display only' WHERE id=?",{second.id});
    require(describe(service,DataObjectKind::TimeDepth,second.id).revision==second.revision,"Relation display names are not semantic identity");
    const auto trackBefore=describe(service,DataObjectKind::IntervalTrack,tracks[0].id);
    sql(project,"UPDATE tracks SET name='display only' WHERE id=?",{tracks[0].id});
    require(describe(service,DataObjectKind::IntervalTrack,tracks[0].id).revision==trackBefore.revision,"Track display names are not semantic identity");
    sql(project,"UPDATE intervals SET bottom=201 WHERE track_id=?",{tracks[0].id});
    require(describe(service,DataObjectKind::IntervalTrack,tracks[0].id).revision!=trackBefore.revision,"Interval edits must invalidate the track token");
    const auto untouched=describe(service,DataObjectKind::IntervalTrack,tracks[1].id);
    sql(project,"UPDATE intervals SET track_id=? WHERE track_id=?",{tracks[1].id,tracks[0].id});
    require(describe(service,DataObjectKind::IntervalTrack,tracks[1].id).revision!=untouched.revision,"Moving an interval invalidates both owners");
    sql(project,"INSERT INTO volumes(source_id,name,path,geometry,sampling) VALUES(?,'metadata-only','offline.sgy','not geometry','not sampling')",{sourceId});
    const auto volumeId=sql(project,"SELECT max(id) FROM volumes").toLongLong();const auto volume=describe(service,DataObjectKind::SeismicVolume,volumeId);
    sql(project,"UPDATE volumes SET sampling='changed' WHERE id=?",{volumeId});
    require(describe(service,DataObjectKind::SeismicVolume,volumeId).revision!=volume.revision,"Volume metadata changes invalidate its token");
    const auto located=describe(service,DataObjectKind::SeismicVolume,volumeId);const auto moved=source+".moved";
    require(QFile::copy(source,moved),"Copy a same-content relocation candidate");
    std::shared_ptr<const PreparedSourceRelocation> prepared;
    require(service.prepareSourceRelocation(sourceId,u(moved),prepared,error),error);require(service.commitSourceRelocation(*prepared,error),error);
    require(describe(service,DataObjectKind::SeismicVolume,volumeId).revision==located.revision,"Source relocation must not change volume semantics");
    SourceIndexEntry identity;require(service.readSourceIndexEntry(sourceId,identity,error)&&identity.id==sourceId&&identity.sha256==sha,"Source identity survives relocation");
    sql(project,"UPDATE volumes SET name='renamed' WHERE id=?",{volumeId});
    require(describe(service,DataObjectKind::SeismicVolume,volumeId).revision==located.revision,"Display name is not object semantics");
    sql(project,"UPDATE volumes SET geometry='changed geometry' WHERE id=?",{volumeId});
    require(describe(service,DataObjectKind::SeismicVolume,volumeId).revision!=located.revision,"Geometry changes invalidate volume semantics");
    for(const auto& table:QStringList{"tracks","volumes"}) {
        const auto target=table=="tracks"?tracks[0].id:volumeId;const QString kind=table=="tracks"?"intervals":"volumes";
        sql(project,"INSERT INTO "+table+"(source_id,name) VALUES(?,'replacement identity')",{sourceId});
        const auto replacement=sql(project,"SELECT max(id) FROM "+table).toLongLong();
        sql(project,"DELETE FROM "+table+" WHERE id=?",{target});
        const auto tombstone=sql(project,"SELECT revision FROM object_revisions WHERE kind=? AND object_id=?",{kind,target}).toLongLong();
        sql(project,"UPDATE "+table+" SET id=? WHERE id=?",{target,replacement});
        const auto movedObject=describe(service,table=="tracks"?DataObjectKind::IntervalTrack:DataObjectKind::SeismicVolume,target);
        require(movedObject.revisionKnown&&sql(project,"SELECT revision FROM object_revisions WHERE kind=? AND object_id=?",{kind,target}).toLongLong()>tombstone,
            "Moving a primary ID onto a tombstone must mint a new known revision");
    }
    sql(project,"UPDATE well_objects SET payload='not-json' WHERE id=?",{first.id});
    require(describe(service,DataObjectKind::TimeDepth,first.id).revisionKnown,"Descriptor must not deserialize payload");
    sql(project,"UPDATE well_objects SET payload=zeroblob(67108865) WHERE id=?",{first.id});first=describe(service,DataObjectKind::TimeDepth,first.id);
    require(!service.readTimeDepthAtRevision(first.id,first.revision,value,error)&&value.depthTimePairs.empty(),"64 MiB hard payload budget must apply before JSON loading");
    sql(project,"DELETE FROM well_objects WHERE id=?",{first.id});
    DataObjectDescriptor missing;require(!service.describeDataObject(DataObjectKind::TimeDepth,first.id,missing,error)&&missing.id==0,"Deleted object cannot resolve");
    sql(project,"INSERT INTO well_objects(id,well_id,source_id,kind,name,payload) VALUES(?,?,?,'time_depth','reused','{}')",{first.id,first.wellId,first.sourceId});
    require(describe(service,DataObjectKind::TimeDepth,first.id).revision!=first.revision,"Reused ID must not revive old token");
    const auto stable=describe(service,DataObjectKind::TimeDepth,second.id);service.closeProject();require(service.openProject(u(project),error),error);
    require(describe(service,DataObjectKind::TimeDepth,second.id).revision==stable.revision,"RW reopen must preserve tokens");
    service.closeProject();removeRevisionSchema(project);
    DataService reader;require(reader.openProjectReadOnly(u(project),error),error);
    require(!describe(reader,DataObjectKind::TimeDepth,second.id).revisionKnown,"Legacy RO descriptor reports unknown revision");
    require(sql(project,"SELECT count(*) FROM sqlite_master WHERE name='object_revisions'").toInt()==0,"RO describe must not upgrade");
    checks=0;require(!upgradeObjectRevisionSchema(u(project),error,[&]{return ++checks==3;}),"Cancel immediately before upgrade commit");
    require(sql(project,"SELECT count(*) FROM sqlite_master WHERE name='object_revisions'").toInt()==0,"Cancelled upgrade must roll back schema and triggers");
    require(upgradeObjectRevisionSchema(u(project),error),error);
    require(reader.isReadOnly()&&describe(reader,DataObjectKind::TimeDepth,second.id).revisionKnown,"Explicit free upgrade leaves reader read-only");
    require(sql(project,"SELECT hash FROM sources WHERE id=?",{sourceId}).toString()==QString::fromStdString(sha),"Semantic revisions do not change source hash");
    {QFile file(source);require(file.open(QIODevice::ReadOnly)&&file.readAll()=="synthetic","Original source bytes unchanged");}
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);try {QTemporaryDir temp;require(temp.isValid(),"Temporary fixture");exercise(temp.filePath("project"),temp.filePath("source.txt"));
        std::cout<<"PASS object point metadata, revisions, exact numeric read, budgets, cancellation, upgrade and source preservation\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
