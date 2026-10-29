// Repository fixtures are generated synthetic examples; no private source data is bundled.
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <array>
#include <barrier>
#include <thread>
using namespace paleo::data;
static std::string u(const QString& v){return v.toUtf8().toStdString();}
static void check(bool ok,const std::string& what){if(!ok)throw std::runtime_error(what);}
static void writeFile(const QString& path,const QByteArray& data){QFile f(path);check(f.open(QIODevice::WriteOnly),"fixture open");check(f.write(data)==data.size(),"fixture write");}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir temp;check(temp.isValid(),"temporary directory");auto root=temp.path();DataService svc;std::string e;ImportOptions opts;ImportReport report;
    check(svc.createProject(u(root+"/project"),"验证工区",e),e);check(!svc.createProject(u(root+"/project"),"重复",e),"existing project must not overwrite");
    // Reopening after a failed create is explicit; no ambiguous previous-project writes.
    check(svc.openProject(u(root+"/project"),e),e);
    check(!svc.setProjectCrs("invalid","m",e),"invalid CRS rejected");
    check(svc.setProjectCrs("epsg:32649","m",e),e);check(svc.projectCrs()=="EPSG:32649"&&svc.projectCrsUnit()=="m","project CRS metadata");
    const auto las=root+"/HZ1-1-1.las";
    writeFile(las,"~Version\nVERS. 2.0 : version\nWRAP. NO : wrap\n~Well\nWELL. HZ1-1-1 : well\nNULL. -999.25 : null\n~Curve\nDEPT.M : depth\nGR.API : gamma\nSP.MV : potential\n~Ascii\n100 40 -20\n101 -3 -999\n102 -999.25 -999.25\n103 80 4\n");
    CurveCleaningRule rule;rule.curveType="GR";rule.unit="API";rule.checkMinimum=true;rule.minimum=0;rule.replaceInvalidWithMissing=true;opts.parsing.cleaningRules={rule};
    check(svc.importFile(u(las),opts,report,e),e);check(report.seen==1&&report.imported+report.review==1,"LAS import accounting");
    std::vector<WellSummary> wells;check(svc.listWells(wells,e)&&wells.size()==1,"one well");auto wellId=wells[0].id;
    std::vector<DataEntry> curves;check(svc.listData("curves",wellId,curves,e)&&curves.size()==2,"two curves");long long gr=0,sp=0;for(auto& c:curves){if(c.name=="GR")gr=c.id;if(c.name=="SP")sp=c.id;}check(gr&&sp,"curve identity");
    CurveSlice slice;check(svc.readCurveSlice(gr,100,103,true,slice,e),e);check(slice.values.size()==4&&slice.rawValues[1]==-3&&std::isnan(slice.values[1])&&slice.quality[1]==2,"configured cleaning preserves original/alignment");check(slice.rawValues[2]==-999.25&&std::isnan(slice.values[2]),"source null preserved");
    check(svc.readCurveSlice(sp,Missing,Missing,true,slice,e),e);check(slice.values[0]==-20&&slice.values[1]==-999,"negative SP valid and declared null overrides defaults");
    WellFeatureTable features;check(svc.prepareWellFeatures(wellId,{gr,sp},100,103,true,features,e)&&features.depths.size()==4&&features.columns.size()==2&&features.columns[1].values[0]==-20,"business feature composition");check(!svc.prepareWellFeatures(wellId,{gr,gr},100,103,true,features,e),"duplicate feature columns rejected");
    auto wrongRule=rule;wrongRule.unit="mV";check(!svc.cleanCurve(gr,wrongRule,e),"unit mismatch rejected");
    check(svc.updateCurveValue(gr,1,33,e),e);check(svc.readCurveSlice(gr,101,101,true,slice,e)&&slice.values.size()==1&&slice.values[0]==33&&slice.rawValues[0]==-3&&slice.quality[0]==4,"point edit stable raw");
    DataTable table;check(svc.readTable("curves",gr,1,1,table,e)&&table.rows.size()==1&&table.rows[0][0]=="1","sample index stable through pagination");
    check(svc.importFile(u(las),opts,report,e)&&report.skipped==1,"idempotent repeat");
    const auto strata=root+"/layers.csv";writeFile(strata,QString::fromUtf8("井号,层位名称,顶深,底深,相类型,未钻穿\nHZ1-1-1,文昌组,100,110,湖相,True\n").toUtf8());
    // Use the supported header alias explicitly rather than relying on folder names.
    writeFile(strata,QString::fromUtf8("井号,地层,顶深,底深,相类型,未钻穿\nHZ1-1-1,文昌组,100,110,湖相,True\n").toUtf8());
    opts.parsing.depthUnit="m";check(svc.importFile(u(strata),opts,report,e),e);std::vector<DataEntry> tracks;check(svc.listData("intervals",wellId,tracks,e)&&!tracks.empty(),"formation track");
    std::vector<IntervalRecord> intervals;check(svc.readIntervals(tracks[0].id,Missing,Missing,intervals,e)&&intervals.size()==1,"formation record");auto record=intervals[0];record.value.intervalName="文昌组四段";record.value.bottomDepth=115;record.value.bottomNotPenetrated=1;check(svc.saveInterval(tracks[0].id,record,e),e);auto changedId=record.id;
    record.id=0;record.value.intervalName="新增测试层";record.value.topDepth=115;record.value.bottomDepth=120;check(svc.saveInterval(tracks[0].id,record,e),e);check(svc.deleteData("interval",record.id,e),e);check(!svc.deleteData("well",wellId,e),"referenced well deletion rejected");
    const auto conflict=root+"/HZ2-2-2.csv";writeFile(conflict,QString::fromUtf8("井号,深度,GR\nHZ3-3-3,1,2\n").toUtf8());check(svc.importFile(u(conflict),{},report,e)&&report.review==1,"conflict review");std::vector<SourceEntry> sources;check(svc.listSources(sources,e),e);long long conflictId=0;for(auto& src:sources)if(src.path==u(conflict))conflictId=src.id;check(conflictId>0,"conflict source");
    check(svc.listWells(wells,e)&&wells.size()==1,"conflict cannot silently create wrong well");ParseOptions correction;correction.wellNameOverride="HZ3-3-3";correction.depthUnit="m";check(svc.reimportSource(conflictId,correction,report,e),e);check(svc.listWells(wells,e)&&wells.size()==2,"corrected mapping publishes well");
    std::vector<DataEntry> correctedCurves;check(svc.listData("curves",0,correctedCurves,e)&&correctedCurves.size()==3,"corrected curves available");check(svc.listSources(sources,e),e);auto correctedSource=sources.front().id;check(svc.reimportSource(correctedSource,{},report,e)&&report.review==1,"reimport unresolved again is review");check(svc.listData("curves",0,correctedCurves,e)&&correctedCurves.size()==3,"blocked reimport cannot erase old valid batch");
    std::vector<IssueEntry> issues;check(svc.listIssues(false,issues,e),e);bool oldResolved=false;for(auto& i:issues)if(i.sourceId==conflictId&&i.resolved)oldResolved=true;check(oldResolved,"reimport resolves original issue");
    // Cancellation during database insertion must leave no source or partial horizon.
    const auto horizon=root+"/cancel.dat";QByteArray dat;for(int n=0;n<10000;++n)dat+="INLINE : 2800 XLINE : "+QByteArray::number(n)+" 310000 2350000 100.25\n";writeFile(horizon,dat);
    int parseChecks=0;auto parsed=parseFile(u(horizon),{},[&]{++parseChecks;return false;});check(parsed.horizons.size()==1,"horizon fixture");check(svc.listSources(sources,e),e);auto before=sources.size();int checks=0;
    check(!svc.importFile(u(horizon),{},report,e,[&]{return ++checks>parseChecks+30;})&&report.cancelled,"mid-import cancellation");check(svc.listSources(sources,e)&&sources.size()==before,"cancel source rollback");std::vector<DataEntry> horizons;check(svc.listData("horizons",0,horizons,e)&&horizons.empty(),"cancel points rollback");
    const auto empty=root+"/empty.sgy";writeFile(empty,{});check(!svc.importFile(u(empty),{},report,e)&&report.failed==1,"empty seismic failure visible");check(svc.listSources(sources,e)&&sources.size()==before+1,"failed file recorded");
    const auto backup=root+"/backup/data.sqlite";QDir().mkpath(root+"/backup");check(svc.backupDatabase(u(backup),e),e);check(!svc.backupDatabase(u(backup),e),"backup overwrite prevented");
    svc.closeProject();check(svc.openProject(u(root+"/project"),e),e);check(svc.projectCrs()=="EPSG:32649"&&svc.projectCrsUnit()=="m","CRS persists on reopen");check(svc.readCurveSlice(gr,101,101,true,slice,e)&&slice.values[0]==33,"edit persisted on reopen");check(svc.readIntervals(tracks[0].id,Missing,Missing,intervals,e)&&intervals.size()==1&&intervals[0].id==changedId&&intervals[0].value.bottomDepth==115&&intervals[0].value.bottomNotPenetrated==1,"interval persisted on reopen");
    check(svc.resetCurve(gr,e),e);check(svc.readCurveSlice(gr,101,102,true,slice,e)&&slice.values[0]==-3&&std::isnan(slice.values[1]),"reset raw with declared null");
    const auto tdr=root+"/checkshots";writeFile(tdr,"# Petrel checkshots format\n# Unit in depth: m\nVERSION 1\nBEGIN HEADER\nX\nY\nZ\nTWT picked\nMD\nWell\nAverage velocity\nInterval velocity\nEND HEADER\n1 2 -100 -50 110 \"HZ1-1-1\" 2000 2000\n1 2 -200 -100 210 \"HZ1-1-1\" 2000 2000\n");check(svc.importFile(u(tdr),{},report,e),e);std::vector<DataEntry> relations;check(svc.listData("time_depth",wellId,relations,e)&&relations.size()==1,"Petrel times structured");TimeDepthTable relation;check(svc.readTimeDepth(relations[0].id,relation,e)&&relation.depthTimePairs.size()==2&&relation.depthTimePairs[0].depthValue==110&&relation.depthTimePairs[0].timeValue==-50&&relation.timeUnit.empty()&&relation.depthInfo.depthType=="MD","time sign preserved and unknown unit not guessed");check(svc.readTable("time_depth",relations[0].id,1,1,table,e)&&table.rows[0][1]=="210","typed time pagination");
    DataService recovered;check(recovered.openProject(u(root+"/backup"),e),e);check(recovered.readCurveSlice(gr,101,101,true,slice,e)&&slice.values[0]==33,"consistent backup");

    // A rejected mapping must roll back changes to shared wells as well as source-owned rows.
    WellInfo coordinateA;coordinateA.wellName="HZ90-1-1";coordinateA.wellheadX=1;coordinateA.wellheadY=2;coordinateA.coordinateSystem="TEST";coordinateA.coordinateUnit="m";long long coordinateAId=0;
    check(svc.createWell(coordinateA,coordinateAId,e),e);
    const auto coordinateFile=root+"/wells2.txt";writeFile(coordinateFile,"HZ90-1-1 10 20\nHZ90-1-2 30 40\n");
    check(svc.importFile(u(coordinateFile),{},report,e)&&report.review==1,"unconfirmed coordinate source");
    check(svc.listSources(sources,e),e);const auto coordinateSource=sources.front().id;check(svc.listWells(wells,e),e);long long coordinateBId=0;for(const auto& w:wells)if(w.name=="HZ90-1-2")coordinateBId=w.id;check(coordinateBId>0,"second coordinate well registered");
    WellInfo coordinateB;check(svc.readWellInfo(coordinateBId,coordinateB,e)&&std::isnan(coordinateB.wellheadX),"unknown axis leaves XY missing");
    std::vector<DataEntry> originalTables;check(svc.listData("tables",0,originalTables,e),e);const auto tableCount=originalTables.size();
    ParseOptions confirmedXY;confirmedXY.coordinateOrder="xy";confirmedXY.coordinateSystem="TEST";confirmedXY.coordinateUnit="m";
    check(svc.reimportSource(coordinateSource,confirmedXY,report,e)&&report.review==1,"conflicting coordinate reimport is review");
    check(svc.readWellInfo(coordinateBId,coordinateB,e)&&std::isnan(coordinateB.wellheadX),"blocked reimport cannot leak B coordinates into shared wells");
    check(svc.readWellInfo(coordinateAId,coordinateA,e)&&coordinateA.wellheadX==1,"blocked reimport preserves existing A coordinates");
    check(svc.listData("tables",0,originalTables,e)&&originalTables.size()==tableCount,"blocked reimport preserves old tables without publishing new ones");
    check(svc.listIssues(true,issues,e),e);bool coordinateConflict=false;for(const auto& issue:issues)if(issue.code=="coordinate_conflict"&&issue.blocking)coordinateConflict=true;check(coordinateConflict,"rolled-back coordinate conflict remains reviewable");

    // Separate service instances own separate connections on their calling threads.
    // Repeated simultaneous edits of different samples must not overwrite one another's BLOB.
    const auto projectPath=svc.projectPath();constexpr int editRounds=24;
    std::barrier editStart(3),editFinished(3);std::array<bool,2> editOk{true,true};std::array<std::string,2> editErrors;
    auto editWorker=[&](int worker){
        DataService local;editOk[worker]=local.openProject(projectPath,editErrors[worker]);
        for(int round=0;round<editRounds;++round){editStart.arrive_and_wait();if(editOk[worker])editOk[worker]=local.updateCurveValue(gr,worker==0?0:3,(worker+1)*1000.0+round,editErrors[worker]);editFinished.arrive_and_wait();}
    };
    std::thread firstEditor(editWorker,0),secondEditor(editWorker,1);bool allEditsPreserved=true;
    for(int round=0;round<editRounds;++round){editStart.arrive_and_wait();editFinished.arrive_and_wait();CurveSlice current;if(!svc.readCurveSlice(gr,Missing,Missing,true,current,e)||current.values.size()!=4||current.values[0]!=1000.0+round||current.values[3]!=2000.0+round)allEditsPreserved=false;}
    firstEditor.join();secondEditor.join();check(editOk[0]&&editOk[1],"concurrent point edit failed: "+editErrors[0]+" / "+editErrors[1]);check(allEditsPreserved,"concurrent point updates lost another sample edit");
    check(svc.readCurveSlice(gr,Missing,Missing,true,slice,e)&&slice.rawValues[0]==40&&slice.rawValues[3]==80,"concurrent edits still preserve raw values");

    // Two importers can parse concurrently; publication remains idempotent inside the write transaction.
    const auto parallelFile=root+"/HZ8-8-8.csv";QByteArray parallelCsv=QString::fromUtf8("井号,深度,GR\n").toUtf8();for(int row=0;row<20000;++row)parallelCsv+="HZ8-8-8,"+QByteArray::number(row)+",42\n";writeFile(parallelFile,parallelCsv);
    std::barrier importStart(3);std::array<bool,2> importOk{false,false};std::array<ImportReport,2> importReports;std::array<std::string,2> importErrors;
    auto importWorker=[&](int worker){DataService local;const bool opened=local.openProject(projectPath,importErrors[worker]);importStart.arrive_and_wait();if(opened){ImportOptions parallelOptions;parallelOptions.copySources=false;importOk[worker]=local.importFile(u(parallelFile),parallelOptions,importReports[worker],importErrors[worker]);}};
    std::thread firstImporter(importWorker,0),secondImporter(importWorker,1);importStart.arrive_and_wait();firstImporter.join();secondImporter.join();
    check(importOk[0]&&importOk[1],"parallel import failed: "+importErrors[0]+" / "+importErrors[1]);
    check(importReports[0].skipped+importReports[1].skipped==1&&importReports[0].imported+importReports[0].review+importReports[1].imported+importReports[1].review==1,"parallel import must publish exactly one batch and skip the other");
    check(svc.listSources(sources,e),e);int matchingSources=0;for(const auto& source:sources)if(source.path==u(parallelFile))++matchingSources;check(matchingSources==1,"parallel import created duplicate source rows");
    check(svc.listWells(wells,e),e);long long parallelWell=0;for(const auto& w:wells)if(w.name=="HZ8-8-8")parallelWell=w.id;check(parallelWell>0&&svc.listData("curves",parallelWell,curves,e)&&curves.size()==1&&curves[0].count==20000,"parallel import created duplicate or partial curves");
    check(svc.saveCurveUnit(curves[0].id,"API",e),e);check(svc.cleanCurve(curves[0].id,rule,e),e);check(!svc.saveCurveUnit(curves[0].id,"mV",e),"existing unit cannot be silently relabeled");
    QFile tdrInput(tdr);check(tdrInput.open(QIODevice::ReadOnly),"time fixture read");auto unknownData=tdrInput.readAll();unknownData.replace("HZ1-1-1","Well 1");const auto unknownTdr=root+"/unassigned-checkshots";writeFile(unknownTdr,unknownData);check(svc.importFile(u(unknownTdr),{},report,e)&&report.review==1,"placeholder time series retained for review");
    check(svc.listWells(wells,e),e);for(const auto& w:wells)check(w.name!="WELL 1","placeholder cannot create a fake well");check(svc.listData("time_depth",0,relations,e),e);long long unassigned=0;for(auto& r:relations)if(r.wellId==0)unassigned=r.id;check(unassigned>0&&svc.readTimeDepth(unassigned,relation,e)&&relation.depthTimePairs.size()==2&&relation.depthTimePairs[0].timeValue==-50,"unassigned time values remain structured");check(svc.assignTimeDepthWell(unassigned,wellId,e),e);check(svc.listData("time_depth",wellId,relations,e)&&relations.size()==2,"explicit time series association");
    // A well discovered from time/coordinates is valid even when no logs were supplied.
    WellInfo timeOnly;timeOnly.wellName="TDR-ONLY-1";long long timeOnlyId=0;check(svc.createWell(timeOnly,timeOnlyId,e),e);check(svc.assignTimeDepthWell(unassigned,timeOnlyId,e),e);
    svc.closeProject();check(svc.openProject(projectPath,e),e);check(svc.listWells(wells,e),e);
    bool sawTimeOnly=false,sawCoordinatesOnly=false,sawLogs=false;
    for(const auto& well:wells){
        if(well.id==timeOnlyId){sawTimeOnly=true;check(well.curves==0&&well.timeDepthTables==1&&!well.hasCoordinates,"time-only well coverage survives reopen");}
        if(well.id==coordinateAId){sawCoordinatesOnly=true;check(well.curves==0&&well.timeDepthTables==0&&well.hasCoordinates,"coordinate-only well must not look like a failed log import");}
        if(well.id==wellId){sawLogs=true;check(well.curves==2&&well.logSets==1&&well.tracks==1&&well.timeDepthTables==1,"mixed well coverage matches actual business records");}
    }
    check(sawTimeOnly&&sawCoordinatesOnly&&sawLogs,"well coverage test records present");
    std::cout<<"PASS service: import, cleanup, idempotency, conflict mapping, shared-well rollback, concurrent writes/imports, CRUD, cancellation rollback, reopen, backup, pending time data, well data coverage\n";return 0;
}catch(const std::exception& ex){std::cerr<<"FAIL "<<ex.what()<<"\n";return 1;}}
