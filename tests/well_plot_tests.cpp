#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
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
      IntervalTrack track;track.trackName="formation";track.trackType="地层";track.depthInfo=well.defaultDepthInfo;
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
    service.closeProject();check(service.openProjectReadOnly(root,error),error);
    WellPlotData data;check(service.readWellPlotData(wells.front(),{},data,error),error);
    check(data.logs.size()==2&&data.intervals.size()==2&&data.markers.size()==1&&data.photos.size()==1,"all sources without raw files");
    check(data.photos.front().value.photos.front().topDepth==100,"photo reference and anchor");
    check(data.logs.front().value.logCurves.front().sampleValues.front()==99,"processed database edit");
    check(data.well.wellheadX==0&&data.well.coordinateSystem=="EPSG:32649"&&data.projectCoordinates.coordinateSystem=="EPSG:4326","distinct stored CRS and valid zero");
    WellPlotOptions raw;raw.processed=false;check(service.readWellPlotData(wells.front(),raw,data,error),error);
    check(data.logs.front().value.logCurves.front().sampleValues.front()==10,"raw selection");
    WellPlotOptions small;small.maxValues=1;
    check(!service.readWellPlotData(wells.front(),small,data,error)&&data.logs.empty()&&data.wellId==0,"budget preflight clears output");
    check(!service.readWellPlotData(wells.front(),{},data,error,[]{return true;})&&data.wellId==0,"cancel clears output");
    check(!service.readWellPlotData(999999,{},data,error)&&data.wellId==0,"missing well");
    check(service.readWellPlotData(wells.front(),{},data,error),"read transaction released after failure");
    DataService writer;check(writer.openProject(root,error),error);int checks=0;
    check(service.readWellPlotData(wells.front(),{},data,error,[&]{
      if(++checks==2){WellInfo value;check(writer.readWellInfo(wells.front(),value,error),error);value.wellheadY=250;check(writer.saveWellInfo(wells.front(),value,error),error);check(writer.updateCurveValue(curves.front().id,0,88,error),error);}return false;
    }),error);
    check(data.well.wellheadY==100&&data.logs.front().value.logCurves.front().sampleValues.front()==99,"coherent WAL snapshot during concurrent update");
    check(service.readWellPlotData(wells.front(),{},data,error)&&data.well.wellheadY==250&&data.logs.front().value.logCurves.front().sampleValues.front()==88,"subsequent read observes committed update");
    std::cout<<"well plot contract passed\n";return 0;
  } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
