#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace paleo::data;
static void check(bool ok,const std::string& why) { if(!ok)throw std::runtime_error(why); }
static std::string u(const QString& value) { return value.toUtf8().toStdString(); }
static void checkPreparedSegyRelocation(const QTemporaryDir& temp) {
  const auto source=temp.filePath("prepared.sgy");
  const QByteArray bytes="synthetic prepared SEG-Y source; no amplitude decoding required\n";
  QFile file(source); check(file.open(QIODevice::WriteOnly),"write prepared SEG-Y source");
  check(file.write(bytes)==bytes.size(),"write complete prepared SEG-Y source"); file.close();
  const auto hash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().toStdString();
  ParsedFile model; model.kind="seismic"; model.status="registered";
  SeismicVolume volume; volume.volumeName="Synthetic volume"; volume.dataFile=u(source); volume.fileFormat="SEG-Y";
  model.volumes.push_back(volume);
  DataService service; std::string error; long long sourceId=0;
  check(service.createProject(u(temp.filePath("segy-project")),"prepared SEG-Y",error),error);
  check(service.importPreparedFile(u(source),u(source),u(source),hash,model,sourceId,error),error);
  check(service.verifySource(sourceId,error),error);
  std::vector<DataEntry> volumes;
  check(service.listData("volumes",0,volumes,error)&&volumes.size()==1,"prepared volume registered");
  const auto replacement=temp.filePath("relocated.sgy");
  check(QFile::copy(source,replacement),"copy synthetic source to replacement path");
  check(service.relocateSource(sourceId,u(replacement),error),error);
  std::string storedPath;
  check(service.sourcePath(sourceId,storedPath,error)&&storedPath==u(replacement),"source path relocated");
  SeismicVolume relocated;
  check(service.readVolume(volumes.front().id,relocated,error)&&relocated.dataFile==u(replacement),"volume path relocated");
  check(service.verifySource(sourceId,error),error);

  const auto changed=temp.filePath("changed.sgy");
  check(QFile::copy(replacement,changed),"copy synthetic source for drift test");
  QFile changedFile(changed); check(changedFile.open(QIODevice::Append),"open changed source");
  changedFile.write("changed"); changedFile.close();
  check(!service.relocateSource(sourceId,u(changed),error),"changed prepared SEG-Y rejected");
  check(service.sourcePath(sourceId,storedPath,error)&&storedPath==u(replacement),"failed relocation preserves source path");
  check(service.readVolume(volumes.front().id,relocated,error)&&relocated.dataFile==u(replacement),"failed relocation preserves volume path");
  check(service.verifySource(sourceId,error),error);
}
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  try {
    QTemporaryDir temp; const auto source=temp.filePath("source.las");
    const QByteArray original="host normalized source\n";
    QFile file(source); check(file.open(QIODevice::WriteOnly),"write source"); file.write(original); file.close();
    const auto hash=QCryptographicHash::hash(original,QCryptographicHash::Sha256).toHex().toStdString();
    ParsedFile model; model.kind="well_log"; model.status="parsed";
    WellDataset well; well.wellInfo.wellName="SYNTH-1"; well.defaultDepthInfo.depthUnit="m";
    LogSet logs; logs.depthInfo=well.defaultDepthInfo; logs.samplingAxis.depthValues={100,101,102};
    LogCurve curve; curve.curveName="SP"; curve.sampleValues={-999,-99999,Missing};
    logs.logCurves.push_back(curve); well.wellbore.logSets.push_back(logs); model.wells.push_back(well);
    const auto project=temp.filePath("model"); DataService service; std::string error; long long id=0;
    check(service.createProject(u(project),"prepared",error),error);
    check(!service.importPreparedFile(u(source),u(source),"../source.las",std::string(64,'0'),model,id,error),"wrong hash rejected");
    check(!service.importPreparedFile(u(source),u(source),"../source.las",hash,model,id,error,[]{return true;}),"cancel rejected");
    auto broken=model; broken.wells[0].wellbore.logSets[0].logCurves[0].sampleValues.pop_back();
    check(!service.importPreparedFile(u(source),u(source),"../source.las",hash,broken,id,error),"invalid shape rejected");
    std::vector<SourceEntry> sources; check(service.listSources(sources,error)&&sources.empty(),"failed transaction leaves no source");
    check(service.importPreparedFile(u(source),u(source),"../source.las",hash,model,id,error),error);
    const auto publishedId=id;
    check(service.verifySource(id,error),error);
    std::vector<DataEntry> curves; check(service.listData("curves",0,curves,error)&&curves.size()==1,"one curve");
    const auto curveId=curves.front().id;
    CurveSlice slice; check(service.readCurveSlice(curveId,Missing,Missing,true,slice,error),error);
    check(slice.values[0]==-999&&slice.values[1]==-99999&&std::isnan(slice.values[2]),"resolved nulls retain finite negatives");

    // A later conflict must roll back an earlier well in the same prepared batch.
    auto conflict=model;
    conflict.wells.front().defaultDepthInfo.depthUnit="ft";
    conflict.wells.front().wellbore.logSets.front().depthInfo.depthUnit="ft";
    auto newWell=well; newWell.wellInfo.wellName="SYNTH-NEW";
    newWell.wellInfo.wellheadX=300000; newWell.wellInfo.wellheadY=2360000;
    newWell.wellInfo.coordinateSystem="EPSG:32649"; newWell.wellInfo.coordinateUnit="m";
    conflict.wells.insert(conflict.wells.begin(),newWell);
    id=123;
    check(!service.importPreparedFile(u(source),u(source),"../source.las",hash,conflict,id,error),"blocking depth conflict rejected");
    check(id==0&&error.find("well_depth_context_conflict")!=std::string::npos,"blocking error identifies conflict and clears source ID");
    check(service.listSources(sources,error)&&sources.size()==1&&sources.front().id==publishedId,"blocking conflict leaves no source");
    std::vector<WellSummary> wells;
    check(service.listWells(wells,error)&&wells.size()==1&&wells.front().name=="SYNTH-1","blocking conflict rolls back every new well");
    check(service.listData("curves",0,curves,error)&&curves.size()==1&&curves.front().id==curveId,"blocking conflict leaves original curves unchanged");
    std::vector<IssueEntry> issues;
    check(service.listIssues(false,issues,error)&&issues.empty(),"blocking conflict rolls back issues with source");
    ProjectCrsInfo crs;
    check(service.readProjectCrsInfo(crs,error)&&crs.candidates.empty()&&crs.coordinateSystem.empty()&&crs.selectionBasis.empty(),"blocking conflict rolls back coordinate candidates and automatic project CRS");
    check(service.readCurveSlice(curveId,Missing,Missing,true,slice,error)&&slice.values[0]==-999,"original values survive rejected batch");

    // Issues supplied by the host follow the same rollback rule as storage conflicts.
    auto blocked=model; ParseIssue issue; issue.code="HOST_BLOCKED"; issue.message="Host requires a correction"; issue.blocking=true;
    blocked.issues.push_back(issue);
    check(!service.importPreparedFile(u(source),u(source),"../source.las",hash,blocked,id,error),"host blocking issue rejected");
    check(id==0&&error.find("HOST_BLOCKED")!=std::string::npos,"host blocking error is readable");
    check(service.listSources(sources,error)&&sources.size()==1,"host blocking issue leaves no source");
    check(service.listData("curves",0,curves,error)&&curves.size()==1,"host blocking issue leaves no duplicate curves");

    // A warning still publishes its objects, with an explicit review status.
    auto review=model; review.wells.front().wellInfo.wellName="SYNTH-REVIEW";
    issue.code="HOST_REVIEW"; issue.message="Optional metadata needs review"; issue.blocking=false; review.issues.push_back(issue);
    check(service.importPreparedFile(u(source),u(source),"../source.las",hash,review,id,error),error);
    const auto reviewId=id;
    check(service.listSources(sources,error),error);
    const auto reviewSource=std::find_if(sources.begin(),sources.end(),[reviewId](const SourceEntry& entry){return entry.id==reviewId;});
    check(reviewSource!=sources.end()&&reviewSource->status=="review","nonblocking issue marks source for review");
    check(service.listIssues(true,issues,error)&&issues.size()==1&&issues.front().sourceId==reviewId&&!issues.front().blocking,"review issue retained");
    check(service.listData("curves",0,curves,error)&&curves.size()==2,"review curve published");
    service.closeProject();
    check(QDir(temp.path()).rename("model","moved-model"),"relocate project");
    check(service.openProjectReadOnly(u(temp.filePath("moved-model")),error),error);
    check(service.listSources(sources,error)&&sources.size()==2,"reopen retains only published sources");
    const auto reopenedReview=std::find_if(sources.begin(),sources.end(),[reviewId](const SourceEntry& entry){return entry.id==reviewId;});
    check(reopenedReview!=sources.end()&&reopenedReview->status=="review","review status survives reopen");
    check(service.listWells(wells,error)&&wells.size()==2,"rolled-back well stays absent after reopen");
    check(service.readProjectCrsInfo(crs,error)&&crs.candidates.empty()&&crs.coordinateSystem.empty(),"rolled-back coordinate metadata stays absent after reopen");
    check(service.verifySource(id,error),error);
    check(service.readCurveSlice(curveId,Missing,Missing,true,slice,error),error);
    check(!service.importPreparedFile(u(source),u(source),"../source.las",hash,model,id,error),"readonly rejected");
    check(file.open(QIODevice::ReadOnly)&&file.readAll()==original,"source unchanged"); file.close();
    check(file.open(QIODevice::Append),"change source"); file.write("changed"); file.close();
    check(!service.verifySource(publishedId,error),"changed source rejected");
    checkPreparedSegyRelocation(temp);
    std::cout<<"prepared import contract passed\n"; return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
