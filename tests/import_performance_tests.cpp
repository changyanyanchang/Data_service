// Repository fixtures are generated synthetic examples; no private source data is bundled.
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QVariant>
#include <algorithm>
#include <atomic>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace paleo::data;
namespace {
std::string utf8(const QString& s){return s.toUtf8().toStdString();}
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void writeFile(const QString& path,const QByteArray& bytes){
    QFile f(path);check(f.open(QIODevice::WriteOnly),"open synthetic fixture");
    check(f.write(bytes)==bytes.size(),"write synthetic fixture");
}
QByteArray las(const QByteArray& well="HZ91-1-1"){
    return "~Version\nVERS. 2.0 : version\nWRAP. NO : wrap\n~Well\nWELL. "+well+
        " : well\nNULL. -999.25 : null\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~Ascii\n100 42\n101 43\n102 44\n";
}
// SQL only inspects results. The migration fixture below is the sole schema mutation,
// against a disposable project whose DataService has first been closed.
struct Database {
    QString name;QSqlDatabase db;
    explicit Database(const QString& project,bool writable=false){
        static int serial=0;name=QStringLiteral("import_perf_test_%1").arg(++serial);
        db=QSqlDatabase::addDatabase("QSQLITE",name);
        if(!writable)db.setConnectOptions("QSQLITE_OPEN_READONLY");
        db.setDatabaseName(project+"/data.sqlite");
        check(db.open(),"open fixture database: "+utf8(db.lastError().text()));
    }
    ~Database(){db.close();db=QSqlDatabase();QSqlDatabase::removeDatabase(name);}
    QVariant scalar(const QString& sql){
        QSqlQuery q(db);check(q.exec(sql),"inspect SQL: "+utf8(q.lastError().text()));
        check(q.next(),"inspection returned no row");return q.value(0);
    }
    std::vector<QVariantList> rows(const QString& sql){
        QSqlQuery q(db);check(q.exec(sql),"inspect rows: "+utf8(q.lastError().text()));
        std::vector<QVariantList> result;
        while(q.next()){QVariantList row;for(int i=0;i<q.record().count();++i)row.push_back(q.value(i));result.push_back(std::move(row));}
        return result;
    }
    void execute(const QString& sql){QSqlQuery q(db);check(q.exec(sql),"migration setup: "+utf8(q.lastError().text()));}
};
long long sourceCount(DataService& service){std::vector<SourceEntry> rows;std::string error;check(service.listSources(rows,error),error);return static_cast<long long>(rows.size());}
long long onlyCurve(DataService& service){std::vector<DataEntry> rows;std::string error;check(service.listData("curves",0,rows,error),error);check(rows.size()==1,"expected one stable curve");return rows.front().id;}
void checkEditedCurve(DataService& service,long long id){
    CurveSlice result;std::string error;check(service.readCurveSlice(id,Missing,Missing,true,result,error),error);
    check(result.values.size()==3&&result.values[0]==84&&result.rawValues[0]==42&&result.quality[0]==4,"manual edit/raw source must survive fast and strict imports");
}
void checkTimes(const ImportReport& r){
    check(r.elapsedMs>=0&&r.hashMs>=0&&r.parseMs>=0&&r.copyMs>=0&&r.storeMs>=0&&r.hashedBytes>=0,"nonnegative import accounting");
}
void editBytePreserveMtime(const QString& path,qint64 offset,char value){
    const auto modified=QFileInfo(path).lastModified();QFile f(path);
    check(f.open(QIODevice::ReadWrite)&&f.seek(offset),"open changed-byte fixture");
    check(f.write(&value,1)==1&&f.flush(),"replace fixture byte");
    check(f.setFileTime(modified,QFileDevice::FileModificationTime),"restore fixture modification time");
}

void cacheAndEdits(const QString& root){
    const QString project=root+"/cache-project",path=root+"/HZ91-1-1.las";
    const QByteArray bytes=las();writeFile(path,bytes);
    DataService service;std::string error;check(service.createProject(utf8(project),"cache and edits",error),error);
    ImportOptions options;options.copySources=false;ImportReport report;
    check(service.importFile(utf8(path),options,report,error),error);
    check(report.seen==1&&report.skipped==0&&report.cached==0&&report.hashedBytes==bytes.size(),"first import fully hashes one file");checkTimes(report);
    const auto curve=onlyCurve(service);check(service.updateCurveValue(curve,0,84,error),error);
    {
        Database db(project);
        check(db.scalar("SELECT count(*) FROM source_scan_cache").toLongLong()==1,"successful import publishes scan cache");
        const QString expected=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
        check(db.scalar("SELECT hash FROM sources LIMIT 1").toString()==expected,"full SHA identity remains persisted");
    }
    check(service.importFile(utf8(path),options,report,error)&&report.skipped==1&&report.cached==1&&report.hashedBytes==0,"unchanged import uses bounded cache without full hash");
    checkEditedCurve(service,curve);check(sourceCount(service)==1,"cached import must not create a batch");checkTimes(report);
    service.closeProject();check(service.openProject(utf8(project),error),error);
    check(service.importFile(utf8(path),options,report,error)&&report.cached==1&&report.hashedBytes==0,"cache survives reopen");checkEditedCurve(service,curve);
    options.verifyUnchangedFiles=true;
    check(service.importFile(utf8(path),options,report,error)&&report.skipped==1&&report.cached==0&&report.hashedBytes==bytes.size(),"strict mode rehashes unchanged source and preserves batch");
    checkEditedCurve(service,curve);check(sourceCount(service)==1,"strict repeat keeps IDs");
    options.verifyUnchangedFiles=false;
    options.parsing.depthUnit="m";
    check(service.importFile(utf8(path),options,report,error)&&report.cached==0&&report.skipped==0&&report.hashedBytes==bytes.size(),"different parse options cannot reuse cached interpretation");
    check(sourceCount(service)==2,"new interpretation has explicit source batch");checkEditedCurve(service,curve);
    options.forceNewBatch=true;
    check(service.importFile(utf8(path),options,report,error)&&report.cached==0&&report.skipped==0&&report.hashedBytes==bytes.size(),"explicit force batch bypasses cache and duplicate shortcut");
    check(sourceCount(service)==3,"forced batch is published");
}

void comprehensiveDepthOptionInvalidatesCache(const QString& root){
    // Exact exporter headers make this fixture sensitive to the host-confirmed
    // MD/metre option. The source bytes never change between the three imports.
    const QByteArray bytes=R"XML(<?xml version="1.0"?><Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet"><Worksheet ss:Name="测井曲线-HZ97-1-1井"><Table>
<Row><Cell><Data>井号</Data></Cell><Cell><Data>深度</Data></Cell><Cell><Data>TVD</Data></Cell><Cell><Data>TVDSS</Data></Cell><Cell><Data>GR</Data></Cell></Row>
<Row><Cell><Data>HZ97-1-1</Data></Cell><Cell><Data>100</Data></Cell><Cell><Data>99</Data></Cell><Cell><Data>-20</Data></Cell><Cell><Data>42</Data></Cell></Row>
<Row><Cell><Data>HZ97-1-1</Data></Cell><Cell><Data>101</Data></Cell><Cell><Data>100</Data></Cell><Cell><Data>-19</Data></Cell><Cell><Data>43</Data></Cell></Row>
<Row><Cell><Data>HZ97-1-1</Data></Cell><Cell><Data>102</Data></Cell><Cell><Data>101</Data></Cell><Cell><Data>-18</Data></Cell><Cell><Data>44</Data></Cell></Row>
</Table></Worksheet></Workbook>)XML";
    // Exercise both the direct importer and the directory worker preparation,
    // with both bounded scan-cache reuse and full SHA duplicate detection.
    for(bool parallel:{false,true})for(bool strict:{false,true}){
        const QString name=QStringLiteral("depth-option-%1-%2").arg(parallel).arg(strict);
        const QString project=root+"/"+name+"-project",input=root+"/"+name+"-input";
        check(QDir().mkpath(input),"create depth-option fixture directory");
        const QString path=input+"/HZ97-1-1.xml";writeFile(path,bytes);
        DataService service;std::string error;check(service.createProject(utf8(project),utf8(name),error),error);
        ImportOptions options;options.copySources=false;options.verifyUnchangedFiles=strict;options.parserThreads=parallel?2:1;
        ImportReport report;
        const auto runImport=[&]{return parallel?service.importDirectory(utf8(input),options,report,error):service.importFile(utf8(path),options,report,error);};
        check(runImport(),error);
        check(report.seen==1&&report.skipped==0&&report.cached==0,"first unconfirmed import is parsed");
        std::vector<DataEntry> curves;check(service.listData("curves",0,curves,error)&&curves.size()==1,"unconfirmed export contains one curve");
        const auto oldCurve=curves.front().id;LogSet log;
        check(service.readLogSet(curves.front().parentId,true,log,error),error);
        check(log.depthInfo.depthType.empty()&&log.depthInfo.depthUnit.empty(),"unconfirmed depth stays unknown");
        check(service.updateCurveValue(oldCurve,0,84,error),error);

        options.parsing.comprehensiveWellMetreDepth=true;
        check(runImport(),error);
        check(report.skipped==0&&report.cached==0&&report.hashedBytes==bytes.size(),"changed exporter contract bypasses scan cache and SHA duplicate shortcut");
        check(service.listData("curves",0,curves,error)&&curves.size()==2&&sourceCount(service)==2,"changed depth contract publishes a separate interpretation");
        const auto confirmedCurve=curves.back().id;
        check(service.readLogSet(curves.back().parentId,true,log,error),error);
        check(log.depthInfo.depthType=="MD"&&log.depthInfo.depthUnit=="m","confirmed depth convention is persisted and returned");
        check(log.samplingAxis.depthValues==std::vector<double>{100,101,102}&&log.logCurves.front().sampleValues==std::vector<double>{42,43,44},"changing convention preserves original depth and curve numbers");
        checkEditedCurve(service,oldCurve);
        check(service.updateCurveValue(confirmedCurve,0,84,error),error);

        check(runImport(),error);
        check(report.skipped==1&&report.cached==(strict?0:1)&&report.hashedBytes==(strict?bytes.size():0),"same confirmed contract reuses the existing interpretation");
        check(report.workerCount==(parallel?2:1)&&sourceCount(service)==2,"repeat keeps worker route and source IDs");
        checkEditedCurve(service,confirmedCurve);checkEditedCurve(service,oldCurve);
    }
}

void changedAndMissing(const QString& root){
    const QString project=root+"/changed-project",path=root+"/HZ92-1-1.las";
    const QByteArray bytes=las("HZ92-1-1");writeFile(path,bytes);
    DataService service;std::string error;check(service.createProject(utf8(project),"changed and missing",error),error);
    ImportOptions options;options.copySources=false;ImportReport report;
    check(service.importFile(utf8(path),options,report,error),error);
    const auto oldCurve=onlyCurve(service);
    const auto offset=bytes.indexOf("100 42")+5;check(offset>=5,"find amplitude fixture");
    editBytePreserveMtime(path,offset,'7');
    check(service.importFile(utf8(path),options,report,error)&&report.cached==0&&report.skipped==0&&report.hashedBytes==bytes.size(),"same size/mtime content change is detected");
    std::vector<DataEntry> curves;check(service.listData("curves",0,curves,error)&&curves.size()==2,"changed source gets separate curve batch");
    CurveSlice values;check(service.readCurveSlice(curves.back().id,Missing,Missing,true,values,error)&&values.rawValues[0]==47,"changed amplitude was parsed rather than stale cache");
    check(service.readCurveSlice(oldCurve,Missing,Missing,true,values,error)&&values.rawValues[0]==42,"previous source batch remains intact");
    check(QFile::remove(path),"remove synthetic source");
    check(!service.importFile(utf8(path),options,report,error)&&report.failed==1&&report.cached==0&&report.skipped==0,"missing source cannot be accepted through cache");
    check(sourceCount(service)==2,"missing source does not publish partial data");
}

void sampledChangedBytes(const QString& root){
    const QString project=root+"/samples-project",path=root+"/large-unknown.bin";
    QByteArray bytes(512*1024,'A');writeFile(path,bytes);
    DataService service;std::string error;check(service.createProject(utf8(project),"sampled file checks",error),error);
    ImportOptions options;options.copySources=false;ImportReport report;
    check(service.importFile(utf8(path),options,report,error),error);
    const std::vector<qint64> positions={17,bytes.size()/2,bytes.size()-17};
    for(size_t i=0;i<positions.size();++i){
        check(service.importFile(utf8(path),options,report,error)&&report.cached==1&&report.hashedBytes==0,"large ordinary unchanged file is fast-skipped");
        editBytePreserveMtime(path,positions[i],char('B'+i));
        check(service.importFile(utf8(path),options,report,error)&&report.cached==0&&report.skipped==0&&report.hashedBytes==bytes.size(),"first/middle/tail changed bytes trigger full identity verification");
    }
    check(sourceCount(service)==4,"three modified files have distinct identities");
}

void failedFileRetry(const QString& root){
    const QString project=root+"/failure-project",path=root+"/empty.sgy";writeFile(path,{});
    DataService service;std::string error;check(service.createProject(utf8(project),"failure retries",error),error);
    ImportOptions options;options.copySources=false;ImportReport report;
    for(int retry=0;retry<2;++retry){
        check(!service.importFile(utf8(path),options,report,error)&&report.failed==1&&report.cached==0&&report.skipped==0,"empty SEG-Y failure must be retried, never cached as success");
    }
    check(sourceCount(service)==2,"each failed retry remains visible");
    Database db(project);check(db.scalar("SELECT count(*) FROM source_scan_cache").toLongLong()==0,"failed sources have no reusable success cache");
}

bool hasStage(const std::vector<ImportProgress>& progress,const QString& stage){
    return std::any_of(progress.begin(),progress.end(),[&](const ImportProgress& p){return QString::fromUtf8(p.status).contains(stage);});
}
void directoryProgressAndCancel(const QString& root){
    const QString project=root+"/directory-project",input=root+"/directory-input";
    check(QDir().mkpath(input),"create directory fixture");writeFile(input+"/HZ93-1-1.las",las("HZ93-1-1"));
    DataService service;std::string error;check(service.createProject(utf8(project),"directory progress",error),error);
    ImportOptions options;options.copySources=true;options.parserThreads=1;ImportReport report;std::vector<ImportProgress> progress;
    check(service.importDirectory(utf8(input),options,report,error,[&](const ImportProgress& p){progress.push_back(p);}),error);
    check(report.seen==1&&report.failed==0&&report.cached==0,"directory full import accounting");
    check(!progress.empty()&&progress.front().total==0,"directory scanning is visible before total is known");
    for(const auto& stage:{QStringLiteral("校验"),QStringLiteral("解析"),QStringLiteral("复制"),QStringLiteral("入库")})check(hasStage(progress,stage),"missing progress stage "+utf8(stage));
    check(progress.back().total==1&&progress.back().completed==1,"terminal progress reaches known total");
    std::vector<SourceEntry> sources;check(service.listSources(sources,error)&&sources.size()==1,"one copied source");
    std::string stored;check(service.sourcePath(sources.front().id,stored,error),error);
    check(QFileInfo(QString::fromUtf8(stored)).absoluteFilePath()!=QFileInfo(input+"/HZ93-1-1.las").absoluteFilePath(),"explicit copy option still stores ordinary source copy");
    check(QFileInfo::exists(QString::fromUtf8(stored)),"copied source exists");
    progress.clear();
    check(service.importDirectory(utf8(input),options,report,error,[&](const ImportProgress& p){progress.push_back(p);}),error);
    check(report.cached==1&&report.skipped==1&&report.hashedBytes==0,"directory repeat uses cache");
    bool cancel=false,sawScanning=false;
    check(!service.importDirectory(utf8(input),options,report,error,[&](const ImportProgress& p){if(p.total==0){sawScanning=true;cancel=true;}},[&]{return cancel;}),"scan cancellation must stop import");
    check(sawScanning&&report.cancelled&&report.seen==0&&report.hashedBytes==0,"scan cancellation happens before processing any file");
    check(sourceCount(service)==1,"scan cancellation preserves completed sources");
}

void changedDuringParse(const QString& root){
    const QString project=root+"/unstable-project",input=root+"/unstable-input";
    check(QDir().mkpath(input),"create unstable fixture directory");
    const QString path=input+"/HZ94-1-1.las";const QByteArray bytes=las("HZ94-1-1");writeFile(path,bytes);
    DataService service;std::string error;check(service.createProject(utf8(project),"unstable source",error),error);
    ImportOptions options;options.copySources=false;options.parserThreads=1;ImportReport report;bool changed=false;
    const bool ok=service.importDirectory(utf8(input),options,report,error,[&](const ImportProgress& p){
        if(!changed&&QString::fromUtf8(p.status).contains(QStringLiteral("解析"))){changed=true;writeFile(path,bytes+"\n# source changed during this import\n");}
    });
    check(changed&&!ok&&report.failed==1&&report.cached==0,"source changed after hashing must not publish mismatched provenance");
    check(sourceCount(service)==0,"unstable source publishes neither successful source nor cache");
    std::vector<DataEntry> curves;check(service.listData("curves",0,curves,error)&&curves.empty(),"unstable import has no leaked curves");
    Database db(project);check(db.scalar("SELECT count(*) FROM source_scan_cache").toLongLong()==0,"unstable import cannot seed cache");
}


QString parallelFixtures(const QString& root){
    const QString input=root+"/parallel-input";check(QDir().mkpath(input),"parallel input directory");
    for(int file=1;file<=8;++file){
        const QByteArray well="HZ96-1-"+QByteArray::number(file);
        QByteArray bytes=las(well);
        for(int row=103;row<603;++row)bytes+=QByteArray::number(row)+" "+QByteArray::number(file*1000+row)+"\n";
        writeFile(input+"/"+QString::fromLatin1(well)+".las",bytes);
    }
    writeFile(input+"/layers.csv",QString::fromUtf8("井号,地层,顶深,底深\nHZ96-1-1,文昌组,100,200\nHZ96-1-2,恩平组,150,250\n").toUtf8());
    QByteArray horizon;
    for(int row=0;row<100;++row)horizon+="INLINE : 2800 XLINE : "+QByteArray::number(1500+row)+" 310000 2350000 "+QByteArray::number(2300+row)+"\n";
    writeFile(input+"/surface.dat",horizon);
    writeFile(input+"/well_coordinates.csv",QString::fromUtf8("井号,X,Y\nHZ96-1-1,310000,2350000\nHZ96-1-2,310100,2350100\n").toUtf8());
    writeFile(input+"/unknown.bin",QByteArray(64*1024,'A'));
    return input;
}
void compareProjectData(const QString& first,const QString& second){
    Database a(first),b(second);
    const QStringList tables={"wells","log_sets","curves","tracks","intervals","sample_tables","samples",
        "measurements","well_objects","volumes","horizons","horizon_points","sections","section_points","maps","source_tables","source_cells","issues"};
    for(const auto& table:tables){
        const QString count="SELECT count(*) FROM "+table;
        check(a.scalar(count)==b.scalar(count),"parallel business count differs: "+utf8(table));
        check(a.rows("SELECT * FROM "+table+" ORDER BY rowid")==b.rows("SELECT * FROM "+table+" ORDER BY rowid"),"parallel persisted rows/arrays/IDs differ: "+utf8(table));
    }
    // Timestamps may differ between independent runs. Source identities, hashes and
    // publication order must not depend on which parsing task happened to finish first.
    const QString sources="SELECT id,original_path,stored_path,hash,options,kind,status,message,bytes,document,active FROM sources ORDER BY id";
    check(a.rows(sources)==b.rows(sources),"parallel source identity/hash/order differs from serial");
    check(a.scalar("SELECT count(*) FROM curves").toInt()==8&&a.scalar("SELECT count(*) FROM horizon_points").toInt()==100&&a.scalar("SELECT count(*) FROM intervals").toInt()==2,"serial comparison has nontrivial curves, horizon points and intervals");
    check(a.scalar("PRAGMA integrity_check").toString()=="ok"&&b.scalar("PRAGMA integrity_check").toString()=="ok","serial/parallel databases are valid");
    check(a.rows("PRAGMA foreign_key_check").empty()&&b.rows("PRAGMA foreign_key_check").empty(),"parallel foreign keys remain valid");
    for(const auto& row:b.rows("SELECT original_path,hash FROM sources ORDER BY id")){
        QFile file(row[0].toString());check(file.open(QIODevice::ReadOnly),"read source for independent SHA");
        const QString expected=QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex());
        check(row[1].toString()==expected,"parallel persisted full source SHA differs from independent digest");
    }
}
void parallelEquivalent(const QString& root){
    const QString input=parallelFixtures(root),serialProject=root+"/serial-project",parallelProject=root+"/parallel-project";
    DataService serial,parallel;std::string error;
    check(serial.createProject(utf8(serialProject),"serial",error),error);check(parallel.createProject(utf8(parallelProject),"parallel",error),error);
    ImportOptions options;options.copySources=false;options.parsing.coordinateOrder="xy";options.parsing.coordinateSystem="TEST";options.parsing.coordinateUnit="m";
    options.parserThreads=1;ImportReport serialReport,parallelReport;
    check(serial.importDirectory(utf8(input),options,serialReport,error),error);
    const auto callingThread=std::this_thread::get_id();bool progressOnCaller=true;std::vector<ImportProgress> progress;
    options.parserThreads=4;
    check(parallel.importDirectory(utf8(input),options,parallelReport,error,[&](const ImportProgress& p){progressOnCaller=progressOnCaller&&std::this_thread::get_id()==callingThread;progress.push_back(p);}),error);
    check(serialReport.workerCount==1&&parallelReport.workerCount==4,"serial/parallel worker setting reported");
    check(serialReport.seen==12&&parallelReport.seen==serialReport.seen&&parallelReport.failed==0,"all parallel input files accounted for");
    check(serialReport.imported==parallelReport.imported&&serialReport.review==parallelReport.review&&serialReport.registered==parallelReport.registered&&serialReport.ignored==parallelReport.ignored&&serialReport.hashedBytes==parallelReport.hashedBytes,"serial and parallel classifications/hashed bytes agree");
    check(progressOnCaller&&!progress.empty()&&progress.back().completed==12,"parallel progress callbacks remain on calling thread and reach completion");
    compareProjectData(serialProject,parallelProject);
    // Public readers also agree, not only the raw persistence layout.
    std::vector<DataEntry> a,b;check(serial.listData("curves",0,a,error)&&parallel.listData("curves",0,b,error)&&a.size()==b.size(),error);
    for(size_t i=0;i<a.size();++i){
        CurveSlice x,y;check(serial.readCurveSlice(a[i].id,Missing,Missing,true,x,error)&&parallel.readCurveSlice(b[i].id,Missing,Missing,true,y,error),error);
        check(x.depths==y.depths&&x.rawValues==y.rawValues&&x.values==y.values&&x.quality==y.quality,"serial/parallel returned curve arrays agree");
    }
    check(parallel.importDirectory(utf8(input),options,parallelReport,error)&&parallelReport.skipped==12&&parallelReport.cached==12&&parallelReport.hashedBytes==0,"parallel repeat reuses all cached files without hashing");
    check(sourceCount(parallel)==12,"parallel cached repeat does not duplicate sources");
    // A deliberately tiny staging budget exercises serial draining without dropping work.
    const QString budgetProject=root+"/budget-project";DataService budget;
    check(budget.createProject(utf8(budgetProject),"budget",error),error);options.maxPendingBytes=1;
    check(budget.importDirectory(utf8(input),options,parallelReport,error)&&parallelReport.failed==0&&parallelReport.seen==12,"small staging budget still processes every file");
    compareProjectData(serialProject,budgetProject);
}
void cancelPreparedWorkers(const QString& root){
    const QString input=root+"/parallel-input",project=root+"/worker-cancel-project";
    DataService service;std::string error;check(service.createProject(utf8(project),"worker cancellation",error),error);
    ImportOptions options;options.copySources=false;options.parserThreads=4;ImportReport report;
    const auto callingThread=std::this_thread::get_id();std::atomic_bool cancelled{false},callbacksOnCaller{true};std::atomic_int cancelChecks{0};
    bool preparationObserved=false;
    auto cancel=[&]{
        cancelChecks.fetch_add(1);
        if(std::this_thread::get_id()!=callingThread)callbacksOnCaller=false;
        return cancelled.load();
    };
    const bool ok=service.importDirectory(utf8(input),options,report,error,[&](const ImportProgress& p){
        if(std::this_thread::get_id()!=callingThread)callbacksOnCaller=false;
        // Depending on fixture speed this is either waiting for the first worker,
        // or validating its prepared result. Both are before publishing the first file.
        if(p.total>0&&p.completed==0&&QString::fromUtf8(p.status).contains(QStringLiteral("校验"))){
            preparationObserved=true;cancelled=true;
        }
    },cancel);
    check(!ok&&preparationObserved&&report.cancelled,"cancellation during parallel preparation stops directory import");
    check(cancelChecks.load()>0&&callbacksOnCaller.load(),"progress and cancellation callbacks stay on the caller thread");
    check(sourceCount(service)==0,"cancelled preparation publishes no partial source batches");
    std::vector<DataEntry> curves;check(service.listData("curves",0,curves,error)&&curves.empty(),"cancelled workers publish no partial curves");
    Database db(project);check(db.scalar("SELECT count(*) FROM source_scan_cache").toLongLong()==0,"cancelled workers publish no cache entries");
}

void legacyMigration(const QString& root){
    const QString project=root+"/migration-project",path=root+"/HZ95-1-1.las";const QByteArray bytes=las("HZ95-1-1");writeFile(path,bytes);
    DataService service;std::string error;check(service.createProject(utf8(project),"legacy migration",error),error);
    ImportOptions options;options.copySources=false;ImportReport report;
    check(service.importFile(utf8(path),options,report,error),error);
    const auto id=onlyCurve(service);check(service.updateCurveValue(id,0,84,error),error);
    std::vector<SourceEntry> sources;check(service.listSources(sources,error)&&sources.size()==1,"migration source fixture");const auto sourceId=sources.front().id;
    service.closeProject();
    {
        Database legacy(project,true);
        legacy.execute("DROP TABLE source_scan_cache");
        legacy.execute("DELETE FROM ds_meta WHERE key='import_performance_schema'");
    }
    check(service.openProject(utf8(project),error),error);
    check(onlyCurve(service)==id,"migration retains curve ID");checkEditedCurve(service,id);
    check(service.listSources(sources,error)&&sources.size()==1&&sources.front().id==sourceId,"migration retains source ID");
    check(service.importFile(utf8(path),options,report,error)&&report.skipped==1&&report.cached==0&&report.hashedBytes==bytes.size(),"legacy source is fully checked once before cache population");
    check(service.importFile(utf8(path),options,report,error)&&report.skipped==1&&report.cached==1&&report.hashedBytes==0,"migrated source is cached after first verified repeat");
    checkEditedCurve(service,id);
    Database migrated(project);
    check(migrated.scalar("SELECT value FROM ds_meta WHERE key='import_performance_schema'").toString()=="2","performance migration recorded");
    check(migrated.scalar("SELECT count(*) FROM sqlite_master WHERE type='index' AND name='log_sets_source'").toInt()==1,"source-filter index created");
    check(migrated.scalar("SELECT count(*) FROM source_scan_cache").toInt()==1,"migration populated cache");
    check(migrated.scalar("PRAGMA integrity_check").toString()=="ok","migration maintains database integrity");
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temp;check(temp.isValid(),"temporary fixture root");
        cacheAndEdits(temp.path());comprehensiveDepthOptionInvalidatesCache(temp.path());changedAndMissing(temp.path());sampledChangedBytes(temp.path());
        failedFileRetry(temp.path());directoryProgressAndCancel(temp.path());changedDuringParse(temp.path());legacyMigration(temp.path());
        parallelEquivalent(temp.path());cancelPreparedWorkers(temp.path());
        std::cout<<"PASS import performance: cache/reopen/manual edits, strict verification, options/force batches, comprehensive depth option invalidation, changed/missing source, sampled changes, failed retries, staged progress, scan cancellation, unstable source, legacy migration, serial/parallel equivalence, staging budget, worker cancellation\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL import performance: "<<e.what()<<'\n';return 1;}
}
