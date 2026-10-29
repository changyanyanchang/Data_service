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
#include <stdexcept>

using namespace paleo::data;
static void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
static std::string utf8(const QString& value){return value.toUtf8().toStdString();}
static void write(const QString& path,const QByteArray& content){
    QFile file(path);check(file.open(QIODevice::WriteOnly),"write CRS fixture");check(file.write(content)==content.size(),"write CRS content");
}
static long long importModel(DataService& service,const QTemporaryDir& temp,const QString& name,
                             ParsedFile model,std::string& error){
    const auto path=temp.filePath(name);const QByteArray content=QByteArray("synthetic CRS fixture: ")+name.toUtf8();write(path,content);
    model.path=utf8(path);if(model.kind.empty())model.kind="registered";if(model.status.empty())model.status="registered";
    long long sourceId=0;
    check(service.importPreparedFile(utf8(path),utf8(path),utf8(path),
                                     QCryptographicHash::hash(content,QCryptographicHash::Sha256).toHex().toStdString(),
                                     model,sourceId,error),error);
    return sourceId;
}
static bool hasKind(const ProjectCrsInfo& info,const std::string& kind){
    return std::any_of(info.candidates.begin(),info.candidates.end(),[&](const auto& candidate){return candidate.objectKind==kind;});
}
static QByteArray petrel(const QByteArray& crs,const QByteArray& unit,const QByteArray& well="SYNTH-1"){
    return "# Petrel well heads\n# Coordinate reference system X, Y: "+crs+
           "\n# Unit in X and Y direction: "+unit+"\n# Unit in depth: m\nVERSION 1\nBEGIN HEADER\nName\nSurface X\nSurface Y\nEND HEADER\n"+
           well+" 110.5 22.25\n";
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temp;check(temp.isValid(),"temporary CRS directory");
        const auto input=temp.filePath("input");check(QDir().mkpath(input),"create CRS input");
        write(input+"/wells.txt",petrel("EPSG:4326","degree"));
        write(input+"/spatial.crsmeta.xml","<SpatialCompanion><AuthorityCode>EPSG:4326</AuthorityCode><Description>synthetic</Description></SpatialCompanion>");
        const auto project=temp.filePath("project");DataService service;std::string error;
        check(service.createProject(utf8(project),"CRS auto",error),error);
        ImportOptions options;options.copySources=false;options.parserThreads=2;ImportReport report;
        check(service.importDirectory(utf8(input),options,report,error),error);
        check(report.seen==2,"folder CRS sources");
        check(service.projectCrs()=="EPSG:4326"&&service.projectCrsUnit()=="degree","folder import selects declared CRS");
        ProjectCrsInfo info;check(service.readProjectCrsInfo(info,error),error);
        check(info.selectionBasis=="import"&&!info.hasConflict&&info.candidates.size()==2,"well and spatial candidates");
        check(hasKind(info,"well")&&hasKind(info,"source"),"coordinate-only well and XML are retained");

        ParsedFile volume;SeismicVolume volumeValue;volumeValue.volumeName="Volume";volumeValue.geometry.coordinateSystem="EPSG:4326";volumeValue.geometry.coordinateUnit="degree";
        volume.volumes.push_back(volumeValue);importModel(service,temp,"volume.sgy",volume,error);
        ParsedFile horizon;SeismicHorizon horizonValue;horizonValue.horizonName="H1";horizonValue.coordinateSystem="EPSG:4326";horizonValue.coordinateUnit="degree";
        horizon.horizons.push_back(horizonValue);importModel(service,temp,"horizon.txt",horizon,error);
        ParsedFile map;GeologicalMap mapValue;mapValue.mapName="Map";mapValue.coordinateSystem="EPSG:4326";mapValue.georeferenceStatus=1;mapValue.georeferenceFile="synthetic.wld";
        map.maps.push_back(mapValue);importModel(service,temp,"map.png",map,error);
        check(service.readProjectCrsInfo(info,error)&&info.candidates.size()==5&&!info.hasConflict,error);
        check(hasKind(info,"volume")&&hasKind(info,"horizon")&&hasKind(info,"map"),"other typed CRS candidates");

        ParsedFile unconfirmed=map;unconfirmed.maps[0].coordinateSystem="EPSG:3857";unconfirmed.maps[0].georeferenceStatus=-1;
        importModel(service,temp,"unconfirmed.png",unconfirmed,error);
        check(service.readProjectCrsInfo(info,error)&&info.candidates.size()==5,"unregistered georeference cannot select CRS");

        ParsedFile conflict=volume;conflict.volumes[0].geometry.coordinateUnit="m";
        const auto conflictSource=importModel(service,temp,"conflict.sgy",conflict,error);
        check(service.readProjectCrsInfo(info,error),error);
        check(info.hasConflict&&info.coordinateSystem.empty()&&info.selectionBasis.empty(),"unit conflict revokes automatic selection");
        check(service.deleteData("source",conflictSource,error),error);
        check(service.readProjectCrsInfo(info,error)&&!info.hasConflict&&info.coordinateSystem=="EPSG:4326"&&info.selectionBasis=="import",error);

        check(service.setProjectCrs("EPSG:3857","m",error),error);
        ParsedFile otherWell;WellDataset well;well.wellInfo.wellName="OTHER-1";well.wellInfo.wellheadX=500000;well.wellInfo.wellheadY=4100000;
        well.wellInfo.coordinateSystem="EPSG:32649";well.wellInfo.coordinateUnit="m";otherWell.wells.push_back(well);
        importModel(service,temp,"other-well.txt",otherWell,error);
        check(service.readProjectCrsInfo(info,error),error);
        check(info.hasConflict&&info.coordinateSystem=="EPSG:3857"&&info.selectionBasis=="manual","manual CRS survives conflicting imports");
        service.closeProject();check(service.openProjectReadOnly(utf8(project),error),error);
        check(service.readProjectCrsInfo(info,error)&&info.coordinateSystem=="EPSG:3857"&&info.candidates.size()==6,error);
        service.closeProject();

        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","crs_old_schema");db.setDatabaseName(QDir(project).filePath("data.sqlite"));
            check(db.open(),"open old CRS schema");QSqlQuery sql(db);
            check(sql.exec("DROP TABLE source_coordinate_systems"),"remove CRS candidate table");
            check(sql.exec("DELETE FROM ds_meta WHERE key='coordinate_system_schema'"),"remove CRS schema marker");
        }
        QSqlDatabase::removeDatabase("crs_old_schema");
        check(service.openProjectReadOnly(utf8(project),error),error);
        check(service.readProjectCrsInfo(info,error)&&info.coordinateSystem=="EPSG:3857"&&info.candidates.empty()&&!info.hasConflict,"legacy readonly CRS query");
        service.closeProject();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","crs_schema_check");db.setDatabaseName(QDir(project).filePath("data.sqlite"));
            db.setConnectOptions("QSQLITE_OPEN_READONLY");check(db.open(),"check CRS schema");QSqlQuery sql(db);
            check(sql.exec("SELECT name FROM sqlite_master WHERE name='source_coordinate_systems'")&&!sql.next(),"readonly open does not migrate CRS schema");
        }
        QSqlDatabase::removeDatabase("crs_schema_check");
        check(service.openProject(utf8(project),error),error);
        check(service.readProjectCrsInfo(info,error)&&info.coordinateSystem=="EPSG:3857"&&info.candidates.empty(),"writable open restores additive schema without inventing candidates");
        service.closeProject();

        const auto localProject=temp.filePath("local-project");
        check(service.createProject(utf8(localProject),"CRS local",error),error);
        ParsedFile local=otherWell;local.wells[0].wellInfo.coordinateSystem="LOCAL-GRID";
        importModel(service,temp,"local-well.txt",local,error);
        check(service.readProjectCrsInfo(info,error),error);
        check(info.coordinateSystem=="EPSG:32650"&&info.coordinateUnit=="m"&&info.selectionBasis=="default"&&!info.hasConflict&&info.candidates.size()==1,"named non-EPSG stays a candidate and the project CRS defaults to EPSG:32650");
        ParsedFile noCoordinates=otherWell;noCoordinates.wells[0].wellInfo.wellheadX=Missing;noCoordinates.wells[0].wellInfo.wellheadY=Missing;
        importModel(service,temp,"no-coordinates.txt",noCoordinates,error);
        check(service.readProjectCrsInfo(info,error)&&info.coordinateSystem=="EPSG:32650"&&info.candidates.size()==1,"CRS without well coordinates is not a well-position candidate");
        const auto blankProject=temp.filePath("blank-project");
        check(service.createProject(utf8(blankProject),"CRS blank",error),error);
        ParsedFile blank;WellDataset blankWell;blankWell.wellInfo.wellName="BLANK-1";blankWell.wellInfo.wellheadX=500000;blankWell.wellInfo.wellheadY=2400000;
        blank.wells.push_back(blankWell);importModel(service,temp,"blank-well.txt",blank,error);
        check(service.readProjectCrsInfo(info,error)&&info.coordinateSystem=="EPSG:32650"&&info.coordinateUnit=="m"&&info.selectionBasis=="default"&&info.candidates.empty(),"missing EPSG defaults to EPSG:32650");
        std::vector<WellSummary> blankListed;WellInfo blankStored;
        check(service.listWells(blankListed,error)&&blankListed.size()==1&&service.readWellInfo(blankListed[0].id,blankStored,error),error);
        check(blankStored.coordinateSystem=="EPSG:32650"&&blankStored.coordinateUnit=="m","well without a CRS is stored as EPSG:32650");
        const auto utmProject=temp.filePath("utm-project");
        check(service.createProject(utf8(utmProject),"CRS utm",error),error);
        ParsedFile utm;WellDataset utmWell;utmWell.wellInfo.wellName="UTM-1";utmWell.wellInfo.wellheadX=333868;utmWell.wellInfo.wellheadY=2361747;
        utmWell.wellInfo.coordinateSystem="UTM84-50N (\"MENTOR:UTM84-50N:WGS 1984 UTM, Zone 50 North, Meter\")";utmWell.wellInfo.coordinateUnit="m";
        utm.wells.push_back(utmWell);importModel(service,temp,"utm-well.txt",utm,error);
        check(service.readProjectCrsInfo(info,error)&&info.coordinateSystem=="EPSG:32650"&&info.coordinateUnit=="m"&&info.selectionBasis=="import"&&info.candidates.size()==1,"explicit WGS84 UTM zone is EPSG:32650");
        std::vector<WellSummary> utmListed;WellInfo utmStored;
        check(service.listWells(utmListed,error)&&utmListed.size()==1&&service.readWellInfo(utmListed[0].id,utmStored,error),error);
        check(utmStored.coordinateSystem=="EPSG:32650"&&utmStored.coordinateUnit=="m","UTM84-50N well CRS is stored as EPSG:32650");
        std::cout<<"coordinate system contract passed\n";return 0;
    }catch(const std::exception& failure){std::cerr<<failure.what()<<'\n';return 1;}
}
