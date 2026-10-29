// Repository fixtures are generated synthetic examples; no private source data is bundled.
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace paleo::data;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static std::string u(const QString& v){return v.toUtf8().toStdString();}
static QString write(const QString& directory,const QString& name,const QByteArray& bytes){
    const auto path=directory+"/"+name;QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);check(f.open(QIODevice::WriteOnly),"fixture open");check(f.write(bytes)==bytes.size(),"fixture write");return path;
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir temp;check(temp.isValid(),"temp directory");std::string error;ImportReport report;DataService service;
    const auto project=temp.path()+"/project";check(service.createProject(u(project),"新格式验证",error),error.c_str());
    const auto xml=write(temp.path(),"HZ1-1-1.xml",R"XML(<?xml version="1.0"?><Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet"><Worksheet ss:Name="测井曲线-HZ1-1-1井"><Table>
<Row><Cell><Data>井号</Data></Cell><Cell><Data>深度</Data></Cell><Cell><Data>TVD</Data></Cell><Cell><Data>TVDSS</Data></Cell><Cell><Data>GR</Data></Cell><Cell><Data>SP</Data></Cell></Row>
<Row><Cell><Data>HZ1-1-1</Data></Cell><Cell><Data>100</Data></Cell><Cell><Data>99</Data></Cell><Cell><Data>-20</Data></Cell><Cell><Data>12</Data></Cell><Cell><Data>-31</Data></Cell></Row>
<Row><Cell><Data>HZ1-1-1</Data></Cell><Cell><Data>100.5</Data></Cell><Cell><Data>99.5</Data></Cell><Cell><Data>-19.5</Data></Cell><Cell><Data>-9999</Data></Cell><Cell><Data>-32</Data></Cell></Row>
</Table></Worksheet></Workbook>)XML");
    check(service.importFile(u(xml),{},report,error),error.c_str());std::vector<DataEntry> entries;
    check(service.listData("curves",0,entries,error)&&entries.size()==2,"XML persisted curves");const auto group=entries[0].parentId;
    LogSet set;check(service.readLogSet(group,false,set,error),error.c_str());check(set.sourceSheet=="测井曲线-HZ1-1-1井"&&set.samplingAxis.tvdValues[0]==99&&set.samplingAxis.tvdssValues[0]==-20,"sheet and extra depth axes persisted");check(set.logCurves[0].sampleValues[1]==-9999&&set.logCurves[1].sampleValues[0]==-31,"raw signed values persist");
    check(service.readLogSet(group,true,set,error)&&std::isnan(set.logCurves[0].sampleValues[1]),"processed missing separate");
    check(service.updateCurveValue(entries[0].id,0,99,error),error.c_str());check(service.importFile(u(xml),{},report,error)&&report.skipped==1,"repeat import skipped");
    check(service.readLogSet(group,true,set,error)&&set.logCurves[0].sampleValues[0]==99,"repeat import preserves edit");
    const auto png=QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jQ4kAAAAASUVORK5CYII=");
    const auto photo=write(temp.path(),"岩心/XJ88-8-B1/照片/1-4.png",png);check(service.importFile(u(photo),{},report,error),error.c_str());
    check(service.listData("core_photos",0,entries,error)&&entries.size()==1,"core photo category persisted");CorePhotoTrack track;const auto photoId=entries[0].id,photoSource=entries[0].sourceId;
    check(service.readCorePhotoTrack(photoId,track,error)&&track.photos.size()==1,"typed photo array");
    check(std::isnan(track.photos[0].topDepth),"box number not depth");check(QString::fromUtf8(track.photos[0].imagePath).startsWith(project),"managed relative path resolved");
    const auto relocated=write(temp.path(),"relocated/image.png",png);check(service.relocateSource(photoSource,u(relocated),error),error.c_str());check(service.readCorePhotoTrack(photoId,track,error)&&track.photos[0].imagePath==u(relocated),"relocate rewrites typed photo path");
    DataTable table;check(service.readTable("core_photos",photoId,0,200,table,error)&&table.totalRows==1&&table.columns.size()==5,"photo table usable");
    const auto attachment=write(temp.path(),"岩心/XJ88-8-B1/报告.pdf","PDF placeholder for metadata registration");check(service.importFile(u(attachment),{},report,error),error.c_str());check(service.listData("attachments",0,entries,error)&&entries.size()==1&&entries[0].count==1,"attachment count");Attachment a;check(service.readAttachment(entries[0].id,a,error)&&QFileInfo::exists(QString::fromUtf8(a.filePath)),"attachment returned");
    service.closeProject();check(service.openProject(u(project),error)&&service.readLogSet(group,true,set,error)&&set.logCurves[0].sampleValues[0]==99,"reopen preserves edits");service.closeProject();
    // Simulate the previous additive schema, then check upgrading leaves values and IDs intact.
    {auto db=QSqlDatabase::addDatabase("QSQLITE","old-schema-test");db.setDatabaseName(project+"/data.sqlite");check(db.open(),"old db open");QSqlQuery q(db);check(q.exec("ALTER TABLE log_sets DROP COLUMN sheet"),"drop new optional column");db.close();}QSqlDatabase::removeDatabase("old-schema-test");
    check(service.openProject(u(project),error)&&service.readLogSet(group,true,set,error)&&set.logCurves[0].sampleValues[0]==99,"old schema upgraded without loss");
    const auto las=write(temp.path(),"XJ88-8-B1.las","~Version\n WRAP . YES : Wrapped\n~Well\n WELL . WELL : XJ88-8-B1\n NULL . -999.25 : Missing\n~Curve\n DEPT .M : Depth\n GR .API : Gamma\n SP .MV : Potential\n~ASCII\n100 20\n-30\n101 -999.25 -31\n");
    auto parsed=parseFile(u(las));check(parsed.wells.size()==1&&parsed.wells[0].wellInfo.wellName=="XJ88-8-B1"&&parsed.wells[0].wellbore.logSets[0].logCurves.size()==2,"spaced LAS 1.2 header / wrapped records / branch well");
    const auto tsv=write(temp.path(),"1X.txt","\"MD\n(m)\"\t\"TVD\n(m)\"\tX-cor\tY-cor\n0\t0\t1000.0\t2000.0\n10.25\t10.25\t1001.0\t2000.0\n");
    parsed=parseFile(u(tsv));check(parsed.status=="review"&&parsed.wells.empty()&&parsed.sourceTables.size()==1&&parsed.sourceTables[0].cells.size()==12,"multiline tab-separated coordinate table preserved without fake angles");
    const auto legacyProject=temp.path()+"/legacy";DataService legacy;check(legacy.createProject(u(legacyProject),"旧格式升级",error)&&legacy.importFile(u(xml),{},report,error),error.c_str());legacy.closeProject();
    {auto db=QSqlDatabase::addDatabase("QSQLITE","legacy-registration");db.setDatabaseName(legacyProject+"/data.sqlite");check(db.open(),"legacy registration open");QSqlQuery q(db);
        check(q.exec("DELETE FROM curves")&&q.exec("DELETE FROM log_sets")&&q.exec("UPDATE sources SET options=json_set(options,'$.parser_version',1),kind='attachment',status='registered'"),"simulate old XML registration");db.close();}QSqlDatabase::removeDatabase("legacy-registration");
    check(legacy.openProject(u(legacyProject),error)&&legacy.importFile(u(xml),{},report,error)&&report.skipped==0,"old registration upgraded");
    check(legacy.listData("curves",0,entries,error)&&entries.size()==2,"upgrade has exactly two curves");const auto upgradedCurve=entries[0].id;
    check(legacy.updateCurveValue(upgradedCurve,0,78,error),error.c_str());legacy.closeProject();
    {auto db=QSqlDatabase::addDatabase("QSQLITE","legacy-structured");db.setDatabaseName(legacyProject+"/data.sqlite");check(db.open(),"legacy structured open");QSqlQuery q(db);check(q.exec("UPDATE sources SET options=json_set(options,'$.parser_version',1) WHERE active=1"),"simulate previous structured parser");db.close();}QSqlDatabase::removeDatabase("legacy-structured");
    check(legacy.openProject(u(legacyProject),error)&&legacy.importFile(u(xml),{},report,error)&&report.skipped==1,"structured previous data not overwritten by parser upgrade");CurveSlice saved;check(legacy.readCurveSlice(upgradedCurve,Missing,Missing,true,saved,error)&&saved.values[0]==78,"manual edit preserved during upgrade");
    // A copied photo must retain its original well association on explicit reimport.
    check(service.listData("core_photos",0,entries,error)&&entries.size()==1,"photo before reimport");
    check(service.reimportSource(entries[0].sourceId,{},report,error),error.c_str());
    check(service.listData("core_photos",0,entries,error)&&entries.size()==1&&service.readCorePhotoTrack(entries[0].id,track,error)&&track.photos.size()==1,"cached reimport retains photo and well context");
    std::cout<<"Format compatibility persistence checks passed\n";
    if(argc>1){int files=0;long long points=0,curves=0;QDirIterator it(app.arguments()[1],QDir::Files,QDirIterator::Subdirectories);while(it.hasNext()){const auto path=it.next();if(QFileInfo(path).suffix().compare("las",Qt::CaseInsensitive)!=0)continue;auto p=parseFile(u(path));std::cout<<u(QFileInfo(path).fileName())<<" "<<p.status<<" "<<p.message<<'\n';check(p.status!="failed"&&p.wells.size()==1,"real LAS file parsed");++files;for(const auto& log:p.wells[0].wellbore.logSets){curves+=log.logCurves.size();points+=log.samplingAxis.depthValues.size();}}std::cout<<"LAS files="<<files<<" curves="<<curves<<" shared_depth_points="<<points<<'\n';}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
