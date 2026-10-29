#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace paleo::data;
static void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static std::string utf8(const QString& value){return value.toUtf8().toStdString();}
static long long importModel(DataService& service,const QTemporaryDir& temp,const QString& name,
                             ParsedFile model,std::string& error){
    const auto path=temp.filePath(name);const QByteArray content=QByteArray("synthetic graph fixture: ")+name.toUtf8();
    QFile file(path);check(file.open(QIODevice::WriteOnly),"write graph source");file.write(content);file.close();
    model.path=utf8(path);if(model.kind.empty())model.kind="registered";if(model.status.empty())model.status="registered";
    long long sourceId=0;
    check(service.importPreparedFile(utf8(path),utf8(path),utf8(path),
                                     QCryptographicHash::hash(content,QCryptographicHash::Sha256).toHex().toStdString(),
                                     model,sourceId,error),error);
    return sourceId;
}
static bool hasNode(const FileGraph& graph,const std::string& id){
    return std::any_of(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return node.id==id;});
}
static const FileGraphEdge* findEdge(const FileGraph& graph,const std::string& relation,
                                     const std::string& from,const std::string& to){
    const auto found=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& edge){
        return edge.relation==relation&&edge.from==from&&edge.to==to;});
    return found==graph.edges.end()?nullptr:&*found;
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temp;check(temp.isValid(),"temporary graph directory");
        const auto project=temp.filePath("工区");DataService service;std::string error;
        check(service.createProject(utf8(project),"file graph",error),error);
        check(service.setProjectCrs("EPSG:4326","degree",error),error);

        ParsedFile logs;logs.kind="well_log";logs.status="parsed";
        WellDataset well;well.wellInfo.wellName="SYNTH-1";
        LogSet log;log.logSetName="Gamma";log.samplingAxis.depthValues={100,101};
        well.wellbore.logSets.push_back(log);logs.wells.push_back(well);
        const auto logSource=importModel(service,temp,"logs.las",logs,error);

        ParsedFile intervals;intervals.kind="well_intervals";intervals.status="parsed";
        IntervalTrack track;track.trackName="Strata";well.wellbore.logSets.clear();well.wellbore.intervalTracks.push_back(track);
        intervals.wells.push_back(well);
        const auto intervalSource=importModel(service,temp,"intervals.txt",intervals,error);

        ParsedFile volume;volume.kind="seismic_volume";SeismicVolume volumeValue;volumeValue.volumeName="Volume A";
        volume.volumes.push_back(volumeValue);
        const auto volumeSource=importModel(service,temp,"volume.sgy",volume,error);
        std::vector<DataEntry> volumes;check(service.listData("volumes",0,volumes,error)&&volumes.size()==1,error);
        const auto volumeId=volumes.front().id;

        ParsedFile horizon;horizon.kind="seismic_horizon";SeismicHorizon horizonValue;horizonValue.horizonName="H1";
        horizonValue.parentVolumeId=static_cast<int>(volumeId);horizon.horizons.push_back(horizonValue);
        const auto horizonSource=importModel(service,temp,"horizon.txt",horizon,error);
        const auto reportSource=importModel(service,temp,"report.pdf",{},error);

        long long relationId=0,secondRelationId=0;
        check(!service.relateSources(reportSource,reportSource,"references","",relationId,error),"self relation rejected");
        check(!service.relateSources(reportSource,999999,"references","",relationId,error),"missing source rejected");
        check(service.relateSources(reportSource,logSource,"derived_from","initial",relationId,error),error);
        check(service.relateSources(reportSource,logSource,"derived_from","confirmed",secondRelationId,error),error);
        check(relationId==secondRelationId&&relationId>0,"relation upsert retains ID");
        std::vector<SourceRelation> relations;
        check(service.listSourceRelations(relations,error)&&relations.size()==1&&relations.front().note=="confirmed",error);

        FileGraph graph;check(service.buildFileGraph({},graph,error),error);
        check(graph.nodes.size()==10&&graph.edges.size()==8,"stored objects and manual relation projected");
        check(graph.projectCrs=="EPSG:4326"&&graph.projectCrsUnit=="degree","graph includes project CRS");
        check(hasNode(graph,"well:1")&&hasNode(graph,"log_set:1")&&hasNode(graph,"interval_track:1"),"well objects present");
        check(hasNode(graph,"volume:"+std::to_string(volumeId))&&hasNode(graph,"file:"+std::to_string(reportSource)),"file and volume nodes present");
        check(findEdge(graph,"contains","file:"+std::to_string(logSource),"log_set:1"),"file contains log set");
        check(findEdge(graph,"belongs_to","interval_track:1","well:1"),"second file joins same well");
        check(findEdge(graph,"references","horizon:1","volume:"+std::to_string(volumeId)),"horizon references volume");
        const auto manual=findEdge(graph,"derived_from","file:"+std::to_string(reportSource),"file:"+std::to_string(logSource));
        check(manual&&manual->basis=="manual"&&manual->relationId==relationId&&manual->note=="confirmed","manual edge metadata");
        std::set<std::string> nodeIds;for(const auto& node:graph.nodes)nodeIds.insert(node.id);
        for(const auto& edge:graph.edges)check(nodeIds.count(edge.from)&&nodeIds.count(edge.to),"no dangling graph edge");

        FileGraphOptions filesOnlyOptions;filesOnlyOptions.includeObjects=false;FileGraph filesOnly;
        check(service.buildFileGraph(filesOnlyOptions,filesOnly,error),error);
        check(filesOnly.nodes.size()==5&&filesOnly.edges.size()==1,"file-only projection");
        for(const auto& node:filesOnly.nodes)check(node.kind=="file"&&!node.path.empty()&&!node.sha256.empty(),"file identity retained");

        service.closeProject();check(service.openProjectReadOnly(utf8(project),error),error);
        FileGraph readonlyGraph;check(service.buildFileGraph({},readonlyGraph,error)&&readonlyGraph.edges.size()==8,error);
        check(!service.relateSources(reportSource,logSource,"related_to","",secondRelationId,error),"readonly relation insert rejected");
        check(!service.removeSourceRelation(relationId,error),"readonly relation removal rejected");
        service.closeProject();check(service.openProject(utf8(project),error),error);
        check(service.removeSourceRelation(relationId,error),error);
        check(service.listSourceRelations(relations,error)&&relations.empty(),error);
        check(!service.removeSourceRelation(relationId,error),"missing relation removal rejected");

        FileGraph limited;FileGraphOptions small;small.maxNodes=1;
        check(!service.buildFileGraph(small,limited,error)&&limited.nodes.empty()&&limited.edges.empty(),"node budget is atomic");
        check(!service.buildFileGraph({},limited,error,[]{return true;})&&limited.nodes.empty(),"cancellation clears output");
        service.closeProject();

        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","graph_fixture_edit");db.setDatabaseName(QDir(project).filePath("data.sqlite"));
            check(db.open(),"open graph fixture");QSqlQuery sql(db);
            check(sql.exec("UPDATE sources SET active=0 WHERE id="+QString::number(reportSource)),"deactivate report");
            check(sql.exec("DROP TABLE source_relations"),"simulate pre-graph schema");
            check(sql.exec("DELETE FROM ds_meta WHERE key='file_graph_schema'"),"remove graph schema marker");
        }
        QSqlDatabase::removeDatabase("graph_fixture_edit");
        check(service.openProjectReadOnly(utf8(project),error),error);
        check(service.listSourceRelations(relations,error)&&relations.empty(),"old schema has no manual relations");
        FileGraph oldSchema;check(service.buildFileGraph({},oldSchema,error),error);
        check(!hasNode(oldSchema,"file:"+std::to_string(reportSource)),"inactive source hidden by default");
        FileGraphOptions withInactive;withInactive.includeInactiveSources=true;
        check(service.buildFileGraph(withInactive,oldSchema,error)&&hasNode(oldSchema,"file:"+std::to_string(reportSource)),error);
        service.closeProject();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","graph_schema_check");db.setDatabaseName(QDir(project).filePath("data.sqlite"));
            db.setConnectOptions("QSQLITE_OPEN_READONLY");check(db.open(),"check old graph schema");QSqlQuery sql(db);
            check(sql.exec("SELECT name FROM sqlite_master WHERE name='source_relations'")&&!sql.next(),"readonly open does not migrate");
        }
        QSqlDatabase::removeDatabase("graph_schema_check");
        check(service.openProject(utf8(project),error),error);
        check(service.listSourceRelations(relations,error)&&relations.empty(),"writable open applies additive graph schema");
        check(service.buildFileGraph({},oldSchema,error),"graph works after additive migration");
        std::cout<<"file graph contract passed\n";return 0;
    }catch(const std::exception& failure){std::cerr<<failure.what()<<'\n';return 1;}
}
