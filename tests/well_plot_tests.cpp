#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <iostream>
#include <stdexcept>
using namespace paleo::data;
static void check(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  try {
    QTemporaryDir temp;DataService service;std::string error;
    const auto root=temp.filePath("project").toStdString();
    check(service.createProject(root,"plot test",error),error);
    check(service.setProjectCrs("EPSG:4326","degree",error),error);
    long long firstSource=0;
    for(int n=0;n<2;++n) {
      const auto path=temp.filePath(QString("source%1.txt").arg(n));
      QFile file(path);check(file.open(QIODevice::WriteOnly),"fixture");file.write("fixture");file.close();
      ParsedFile parsed;parsed.path=path.toStdString();parsed.kind="test";parsed.status="parsed";
      WellDataset well;well.wellInfo.wellName="SYNTH-1";well.wellInfo.wellheadX=0;well.wellInfo.wellheadY=100;
      well.wellInfo.coordinateSystem="EPSG:32649";well.wellInfo.coordinateUnit="m";
      well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit="m";
      LogSet logs;logs.logSetName="logs";logs.depthInfo=well.defaultDepthInfo;
      logs.samplingAxis.depthValues={100,101};LogCurve curve;curve.curveName=n?"RT":"GR";curve.sampleValues={10,20};logs.logCurves.push_back(curve);
      well.wellbore.logSets.push_back(logs);
      IntervalTrack track;track.trackName="formation";track.trackType="地层";
      if(n){track.depthInfo.depthType="TVD";track.depthInfo.depthUnit="ft";}
      GeologicalInterval interval;interval.intervalName="F1";interval.topDepth=100;interval.bottomDepth=101;track.intervals.push_back(interval);
      well.wellbore.intervalTracks.push_back(track);
      if(n==0){
        MarkerTrack markers;markers.trackName="markers";markers.depthInfo=well.defaultDepthInfo;Marker m;m.markerName="M1";m.depthValue=100;markers.markers.push_back(m);well.wellbore.markerTracks.push_back(markers);
        CorePhotoTrack photos;photos.trackName="core";CorePhoto photo;photo.imagePath="core.png";photo.topDepth=100;photo.bottomDepth=101;photos.photos.push_back(photo);well.wellbore.corePhotoTracks.push_back(photos);
      }
      parsed.wells.push_back(well);long long source=0;
      const auto sha=QCryptographicHash::hash("fixture",QCryptographicHash::Sha256).toHex().toStdString();
      check(service.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),sha,parsed,source,error),error);
      if(!n)firstSource=source;
      check(QFile::remove(path),"remove external source");
    }
    std::vector<long long> wells;check(service.listSourceWells(firstSource,wells,error)&&wells.size()==1,"explicit well association");
    std::vector<DataEntry> curves;check(service.listData("curves",wells.front(),curves,error),error);
    check(service.updateCurveValue(curves.front().id,0,99,error),error);
    std::vector<DataEntry> intervals;check(service.listData("intervals",wells.front(),intervals,error),error);
    IntervalTrack unknown;check(service.readIntervalTrackInfo(intervals.front().id,unknown,error)&&unknown.depthInfo.depthType.empty(),"generic source remains undeclared");
    {
      auto fixtureDb=QSqlDatabase::addDatabase("QSQLITE","legacy-confirmed-plot");fixtureDb.setDatabaseName(QString::fromStdString(root)+"/data.sqlite");check(fixtureDb.open(),"fixture metadata connection");
      QSqlQuery edit(fixtureDb);check(edit.exec("UPDATE sources SET kind='workbook',options='{\"comprehensiveWellMetreDepth\":true}'"),"simulate older confirmed workbook metadata");
      check(edit.exec("UPDATE log_sets SET depth_json=CASE WHEN source_id=(SELECT MIN(id) FROM sources) THEN '{}' ELSE '{\"type\":\"TVD\",\"unit\":\"ft\"}' END"),"simulate missing and explicit log conventions");
      check(edit.exec("SELECT depth_json FROM tracks ORDER BY id LIMIT 1")&&edit.next()&&!edit.value(0).toString().contains("MD"),"stored track depth declaration remains untouched");
    }
    QSqlDatabase::removeDatabase("legacy-confirmed-plot");
    service.closeProject();check(service.openProjectReadOnly(root,error),error);
    WellPlotData data;check(service.readWellPlotData(wells.front(),{},data,error),error);
    check(data.logs.size()==2&&data.intervals.size()==2&&data.markers.size()==1&&data.photos.size()==1,"all sources without raw files");
    check(data.intervals[0].value.depthInfo.depthType=="MD"&&data.intervals[0].value.depthInfo.depthUnit=="m","offline confirmed workbook fills missing track conventions");
    check(data.intervals[1].value.depthInfo.depthType=="TVD"&&data.intervals[1].value.depthInfo.depthUnit=="ft","explicit track convention wins over source default");
    check(data.logs[0].value.depthInfo.depthType=="MD"&&data.logs[0].value.depthInfo.depthUnit=="m","offline confirmed workbook fills missing log conventions");
    check(data.logs[1].value.depthInfo.depthType=="TVD"&&data.logs[1].value.depthInfo.depthUnit=="ft","explicit log convention wins over source default");
    check(data.photos.front().value.photos.front().topDepth==100,"photo reference and anchor");
    check(data.logs.front().value.logCurves.front().sampleValues.front()==99,"processed database edit");
    check(data.well.wellheadX==0&&data.well.coordinateSystem=="EPSG:32649"&&data.projectCoordinates.coordinateSystem=="EPSG:4326","distinct stored CRS and valid zero");
    WellPlotOptions raw;raw.processed=false;check(service.readWellPlotData(wells.front(),raw,data,error),error);
    check(data.logs.front().value.logCurves.front().sampleValues.front()==10,"raw selection");
    WellPlotOptions small;small.maxValues=1;
    check(!service.readWellPlotData(wells.front(),small,data,error)&&data.logs.empty()&&data.wellId==0,"budget preflight clears output");
    WellPlotOptions intervalsOnly;intervalsOnly.includeLogs=false;intervalsOnly.includeMarkers=false;
    intervalsOnly.includePhotos=false;intervalsOnly.maxValues=2;
    check(service.readWellPlotData(wells.front(),intervalsOnly,data,error),error);
    check(data.logs.empty()&&data.markers.empty()&&data.photos.empty()&&data.intervals.size()==2,"interval-only groups and budget");
    check(data.defaultDepthInfo.depthType=="MD"&&data.defaultDepthInfo.depthUnit=="m"&&
          data.intervals.front().value.intervals.front().topDepth==100,"interval-only conventions and values after source removal");
    check(!service.readWellPlotData(wells.front(),{},data,error,[]{return true;})&&data.wellId==0,"cancel clears output");
    check(!service.readWellPlotData(999999,{},data,error)&&data.wellId==0,"missing well");
    check(service.readWellPlotData(wells.front(),{},data,error),"read transaction released after failure");
    DataService writer;check(writer.openProject(root,error),error);int checks=0;
    check(service.readWellPlotData(wells.front(),{},data,error,[&]{
      if(++checks==2){WellInfo value;check(writer.readWellInfo(wells.front(),value,error),error);value.wellheadY=250;check(writer.saveWellInfo(wells.front(),value,error),error);check(writer.updateCurveValue(curves.front().id,0,88,error),error);}return false;
    }),error);
    check(data.well.wellheadY==100&&data.logs.front().value.logCurves.front().sampleValues.front()==99,"coherent WAL snapshot during concurrent update");
    check(service.readWellPlotData(wells.front(),{},data,error)&&data.well.wellheadY==250&&data.logs.front().value.logCurves.front().sampleValues.front()==88,"subsequent read observes committed update");
    DataService lasService;const auto lasRoot=temp.filePath("las-project").toStdString();check(lasService.createProject(lasRoot,"las",error),error);
    const auto lasPath=temp.filePath("HZ1-1-1.las");{QFile las(lasPath);check(las.open(QIODevice::WriteOnly),"LAS fixture");las.write("~Well\nWELL. HZ1-1-1 : well\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~ASCII\n100 20\n101 80\n");}
    auto las=parseFile(lasPath.toStdString());check(las.wells.size()==1,"LAS parse");
    check(las.wells.front().wellbore.logSets.front().depthInfo.depthType=="MD","LAS DEPT parser keeps measured-depth index");
    las.wells.front().wellbore.logSets.front().depthInfo.depthType.clear(); // legacy import, original values remain.
    QFile lasOriginal(lasPath);check(lasOriginal.open(QIODevice::ReadOnly),"LAS hash input");
    const auto lasSha=QCryptographicHash::hash(lasOriginal.readAll(),QCryptographicHash::Sha256).toHex().toStdString();lasOriginal.close();
    long long lasSource=0;check(lasService.importPreparedFile(lasPath.toStdString(),lasPath.toStdString(),lasPath.toStdString(),lasSha,las,lasSource,error),error);
    std::vector<DataEntry> lasCurves;check(lasService.listData("curves",0,lasCurves,error)&&lasCurves.size()==1,"LAS group identity");
    check(QFile::remove(lasPath),"remove LAS original");lasService.closeProject();check(lasService.openProjectReadOnly(lasRoot,error),error);
    LogSet legacy;check(lasService.readLogSet(lasCurves.front().parentId,true,legacy,error),error);
    check(legacy.depthInfo.depthType=="MD"&&legacy.depthInfo.depthUnit=="M"&&legacy.samplingAxis.depthValues.front()==100,"old LAS read is MD, preserves declared unit and raw axis offline");
    std::cout<<"well plot contract passed\n";return 0;
  } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
