#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace paleo::data;
static void check(bool ok,const std::string& why) { if(!ok)throw std::runtime_error(why); }
static std::string u(const QString& value) { return value.toUtf8().toStdString(); }
static void executeAt(const QString& project,const QString& sql) {
  const auto name="relocation-contract-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
  {
    auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(project+"/data.sqlite");check(db.open(),"open test metadata");
    QSqlQuery query(db);check(query.exec(sql),"execute test metadata change");query.finish();db.close();
  }
  QSqlDatabase::removeDatabase(name);
}
static void checkBoundedRowBatches(const QTemporaryDir& temp) {
  const auto source=temp.filePath("row-batches.txt"),project=temp.filePath("row-batches");
  const QByteArray bytes="synthetic multi-row persistence fixture";
  {QFile file(source);check(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"write row fixture");}
  const auto hash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().toStdString();
  ParsedFile parsed;parsed.kind="synthetic";parsed.status="parsed";
  WellDataset well;well.wellInfo.wellName="BATCH-ROWS";
  IntervalTrack track;track.trackName="layers";
  for(int i=0;i<129;++i){GeologicalInterval row;row.intervalName="layer-"+std::to_string(i);row.topDepth=i-10.;row.bottomDepth=i-9.;row.sourceRow=i+2;track.intervals.push_back(row);}
  well.wellbore.intervalTracks.push_back(track);
  SampleTable samples;samples.tableName="analysis";SampleRecord sample;sample.sampleNumber="sample";
  for(int i=0;i<190;++i)sample.measurements.push_back({"m-"+std::to_string(i),"",i%3?double(i):Missing,"raw-"+std::to_string(i)});
  samples.samples.push_back(sample);well.wellbore.sampleTables.push_back(samples);parsed.wells.push_back(well);
  SeismicHorizon horizon;horizon.horizonName="surface";
  for(int i=0;i<275;++i)horizon.points.push_back({i,2*i,100.+i,200.+i,i%3?double(-i):Missing});
  parsed.horizons.push_back(horizon);
  SourceTable table;table.sheetName="raw";table.rowCount=679;table.columnCount=1;
  for(int i=0;i<679;++i)table.cells.push_back({i+1,1,"cell-"+std::to_string(i)});
  parsed.sourceTables.push_back(table);
  DataService service;std::string error;long long sourceId=0;
  check(service.createProject(u(project),"row batches",error),error);
  ModelChangeHead before;check(service.readModelChangeHead(before,error),error);
  executeAt(project,"CREATE TRIGGER reject_middle_cell BEFORE INSERT ON source_cells WHEN NEW.row_num=300 BEGIN SELECT RAISE(ABORT,'injected middle-row failure'); END");
  check(!service.importPreparedFile(u(source),u(source),u(source),hash,parsed,sourceId,error)&&sourceId==0,"middle statement constraint failure rejects file");
  check(error.find("injected middle-row failure")!=std::string::npos,"injected row failure reached");
  std::vector<SourceEntry> sources;std::vector<WellSummary> wells;
  check(service.listSources(sources,error)&&sources.empty()&&service.listWells(wells,error)&&wells.empty(),"constraint failure rolls back earlier batches and objects");
  ModelChangeHead after;check(service.readModelChangeHead(after,error)&&after.sequence==before.sequence,"failed batches publish no journal operation");
  executeAt(project,"DROP TRIGGER reject_middle_cell");
  bool sawStoredChunk=false;
  const auto cancelAfterChunk=[&]{
    for(const auto& name:QSqlDatabase::connectionNames()){
      auto db=QSqlDatabase::database(name,false);if(db.databaseName()!=project+"/data.sqlite")continue;
      QSqlQuery query(db);if(query.exec("SELECT count(*) FROM source_cells")&&query.next()&&query.value(0).toInt()>=225){sawStoredChunk=true;return true;}
    }return false;
  };
  check(!service.importPreparedFile(u(source),u(source),u(source),hash,parsed,sourceId,error,cancelAfterChunk)&&sourceId==0&&sawStoredChunk,"cancel after a full stored chunk rejects file");
  check(service.listSources(sources,error)&&sources.empty()&&service.listWells(wells,error)&&wells.empty(),"cancel rolls back already-stored chunks");
  check(service.readModelChangeHead(after,error)&&after.sequence==before.sequence,"cancelled batches publish no journal operation");
  check(service.importPreparedFile(u(source),u(source),u(source),hash,parsed,sourceId,error),error);
  std::vector<DataEntry> entries;check(service.listData("intervals",0,entries,error)&&entries.size()==1,error);
  std::vector<IntervalRecord> intervals;check(service.readIntervals(entries.front().id,Missing,Missing,intervals,error)&&intervals.size()==129,"all interval chunks and tail persist");
  for(size_t i=0;i<intervals.size();++i)check(intervals[i].value.intervalName==track.intervals[i].intervalName&&intervals[i].value.topDepth==double(i)-10.,"interval ordering and negative values persist");
  check(service.listData("samples",0,entries,error)&&entries.size()==1,error);SampleTable read;
  check(service.readSampleTable(entries.front().id,read,error)&&read.samples.size()==1&&read.samples.front().measurements.size()==190,"measurement chunk and tail persist");
  for(int i=0;i<190;++i){const auto& m=read.samples.front().measurements[i];check(m.originalText=="raw-"+std::to_string(i)&&(i%3?m.numericValue==i:std::isnan(m.numericValue)),"measurement nulls and original strings persist");}
  check(service.listData("horizons",0,entries,error)&&entries.size()==1,error);SeismicHorizon readHorizon;
  check(service.readHorizon(entries.front().id,0,1000,readHorizon,error)&&readHorizon.points.size()==275,"horizon chunks and tail persist");
  for(int i=0;i<275;++i)check(readHorizon.points[i].inlineNumber==i&&(i%3?readHorizon.points[i].zTime==-i:std::isnan(readHorizon.points[i].zTime)),"horizon indices and nulls persist");
  service.closeProject();check(service.openProjectReadOnly(u(project),error),error);
  const auto inspect="batch-inspect-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
  {auto db=QSqlDatabase::addDatabase("QSQLITE",inspect);db.setConnectOptions("QSQLITE_OPEN_READONLY");db.setDatabaseName(project+"/data.sqlite");check(db.open(),"open persisted rows");
   QSqlQuery q(db);check(q.exec("SELECT row_num,col_num,text FROM source_cells ORDER BY row_num,col_num"),"read original cells");int i=0;
   while(q.next()){check(q.value(0).toInt()==i+1&&q.value(1).toInt()==1&&q.value(2).toString()==QString("cell-%1").arg(i),"raw positions and text persist");++i;}check(i==679,"all raw-cell chunks and tail survive reopen");}
  QSqlDatabase::removeDatabase(inspect);
}
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
  // Preparation is read-only; publication rejects stale files and other projects.
  DataService reader;
  check(reader.openProjectReadOnly(u(temp.filePath("segy-project")),error),error);
  SourceIndexEntry entry;
  check(reader.readSourceIndexEntry(sourceId,entry,error)&&entry.id==sourceId,"indexed source lookup");
  check(!reader.readSourceIndexEntry(-1,entry,error),"missing source rejected");
  const auto candidate=temp.filePath("prepared-candidate.bin");
  check(QFile::copy(source,candidate),"copy prepared candidate");
  std::shared_ptr<const PreparedSourceRelocation> prepared;
  check(reader.prepareSourceRelocation(sourceId,u(candidate),prepared,error),error);
  check(prepared->verifiedBytes()==bytes.size(),"SHA source reads exactly its bytes");
  check(!reader.commitSourceRelocation(*prepared,error),"read-only publication rejected");
  check(!reader.openProjectForSourceRelocation(*prepared,error),"minimal opener cannot upgrade read-only connection");
  DataService other;check(other.createProject(u(temp.filePath("other-project")),"other",error),error);
  check(!other.commitSourceRelocation(*prepared,error),"cross-project plan rejected");
  check(!other.openProjectForSourceRelocation(*prepared,error),"minimal opener cannot rebind another project");
  QFile changedCandidate(candidate);check(changedCandidate.open(QIODevice::Append),"change candidate");
  changedCandidate.write("changed");changedCandidate.close();
  check(!service.commitSourceRelocation(*prepared,error),"candidate drift rejected before publication");
  check(!reader.prepareSourceRelocation(sourceId,u(candidate),prepared,error,[]{return true;})&&!prepared,"cancel clears prepared output");
  check(QFile::remove(candidate)&&QFile::copy(source,candidate),"restore private candidate");
  check(reader.prepareSourceRelocation(sourceId,u(candidate),prepared,error),error);
  const auto project=temp.filePath("segy-project");
  WellInfo sentinel;sentinel.wellName="NORMALIZATION-GUARD";sentinel.wellheadX=500000;sentinel.wellheadY=4000000;
  sentinel.coordinateSystem="EPSG:32650";sentinel.coordinateUnit="m";long long sentinelId=0;
  check(service.createWell(sentinel,sentinelId,error),error);
  executeAt(project,"UPDATE wells SET crs='UTM84-50N' WHERE id="+QString::number(sentinelId));
  executeAt(project,"CREATE TRIGGER reject_unrelated_crs_update BEFORE UPDATE OF crs ON wells BEGIN SELECT RAISE(ABORT,'source publication must not normalize coordinates'); END");
  executeAt(project,"UPDATE ds_meta SET value='2' WHERE key='schema_version'");
  DataService publisher;
  check(!publisher.openProjectForSourceRelocation(*prepared,error),"minimal opener rejects future schema");
  check(!service.commitSourceRelocation(*prepared,error),"already-open publisher rechecks future schema inside transaction");
  executeAt(project,"UPDATE ds_meta SET value='1' WHERE key='schema_version'");
  check(publisher.openProjectForSourceRelocation(*prepared,error),error);
  reader.closeProject();
  check(reader.openProjectForSourceRelocation(*prepared,error),"closed read-only preparer can reopen for minimal publication");
  check(reader.commitSourceRelocation(*prepared,error),error);
  check(!publisher.commitSourceRelocation(*prepared,error),"another minimal writer rejects stale prepared row");
  // The trigger would abort a field-wide CRS normalization. Querying afterward
  // also proves the sentinel's original representation was not silently changed.
  {
    const auto connection="relocation-sentinel-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(project+"/data.sqlite");check(db.open(),"open sentinel");
     QSqlQuery query(db);check(query.exec("SELECT crs FROM wells WHERE id="+QString::number(sentinelId))&&query.next()&&query.value(0).toString()=="UTM84-50N","minimal publication preserves unrelated coordinates");}
    QSqlDatabase::removeDatabase(connection);
  }
  check(!service.commitSourceRelocation(*prepared,error),"source-row CAS rejects stale plan");
  check(error.find("StaleModelGeneration")!=std::string::npos,"A writable reopen after unmanaged SQL fences the original writer");
  service.closeProject();
  check(service.openProjectForSourceRelocation(*prepared,error),"Explicitly reopen the stale writer without unrelated coordinate normalization");
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
    checkBoundedRowBatches(temp);
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
    ProjectCrsInfo initialCrs;check(service.readProjectCrsInfo(initialCrs,error),error);
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
    check(service.readProjectCrsInfo(crs,error)&&crs.candidates.empty()&&crs.coordinateSystem==initialCrs.coordinateSystem&&crs.selectionBasis==initialCrs.selectionBasis,"blocking conflict preserves preexisting project CRS and rolls back coordinate candidates");
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
    check(service.readProjectCrsInfo(crs,error)&&crs.candidates.empty()&&crs.coordinateSystem==initialCrs.coordinateSystem,"rolled-back coordinate candidates stay absent after reopen");
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
