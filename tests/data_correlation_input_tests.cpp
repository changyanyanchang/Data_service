// Synthetic input preservation tests. No private well data or local paths are bundled.
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
std::string u(const QString& s){return s.toUtf8().toStdString();}
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void write(const QString& path,const QByteArray& bytes){QFile f(path);check(f.open(QIODevice::WriteOnly),"open fixture");check(f.write(bytes)==bytes.size(),"write fixture");}
QByteArray read(const QString& path){QFile f(path);check(f.open(QIODevice::ReadOnly),"read fixture");return f.readAll();}
bool issue(const ParsedFile& p,const char* code){for(const auto& v:p.issues)if(v.code==code)return true;return false;}
}

int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temp;check(temp.isValid(),"temporary directory");
        const auto path=temp.path()+"/checkshots";
        QByteArray contents="# Petrel checkshots format\n# Unit in X and Y direction: m\n# Unit in depth: m\nVERSION 1\nBEGIN HEADER\nX\nY\nZ\nTWT picked\nMD\nWell\nEND HEADER\n";
        // More than the parser's source preview: every XYZ sample must be stored, not just a header excerpt.
        for(int i=0;i<64;++i)contents+=QByteArray::number(1000+i)+" "+QByteArray::number(2000+2*i)+" "+QByteArray::number(-999-i)+" "+QByteArray::number(-999-2*i)+" "+QByteArray::number(100+5*i)+" \"SYNTH-1\"\n";
        write(path,contents);
        auto parsed=parseFile(u(path));
        check(parsed.wells.size()==1,"one well grouped");
        const auto& original=parsed.wells[0].timeDepthRelations.at(0);
        check(original.depthTimePairs.size()==64,"all joint samples preserved");
        check(original.depthTimePairs[0].timeValue==-999&&original.depthTimePairs[0].z==-999,"undeclared negative numeric values are not null sentinels");
        check(original.coordinateUnit=="m"&&original.positionDepthInfo.depthUnit=="m","source-declared units retained");
        check(original.coordinateSystem.empty()&&original.timeUnit.empty()&&original.timeReference.empty()&&original.positionDepthInfo.depthType.empty(),"unknown conventions not inferred");
        check(issue(parsed,"TIME_UNIT_UNKNOWN")&&issue(parsed,"TIME_DEPTH_COORDINATES_REVIEW")&&issue(parsed,"TIME_DEPTH_Z_REVIEW"),"missing geometry metadata visible");
        std::string error;DataService service;const auto project=temp.path()+"/project";
        check(service.createProject(u(project),"Synthetic input preservation",error),error);
        ImportOptions options;options.copySources=false;ImportReport report;
        check(service.importFile(u(path),options,report,error),error);
        std::vector<DataEntry> entries;check(service.listData("time_depth",0,entries,error)&&entries.size()==1,"one persisted time relation");
        const auto id=entries[0].id;
        service.closeProject();check(service.openProject(u(project),error),error);
        TimeDepthTable result;check(service.readTimeDepth(id,result,error),error);
        check(result.depthTimePairs.size()==64,"reopen sample count");
        const auto& end=result.depthTimePairs.back();
        check(end.depthValue==415&&end.timeValue==-1125&&end.x==1063&&end.y==2126&&end.z==-1062,"XYZ/MD/TWT remain paired after reopen");
        check(result.positionDepthInfo.depthUnit=="m"&&result.coordinateUnit=="m"&&result.timeUnit.empty(),"reopen conventions preserved");
        check(read(path)==contents,"original input unchanged");

        const auto declared=temp.path()+"/declared_checkshots";
        write(declared,"# Petrel checkshots format\n# Unit in depth: m\n# Unit in X and Y direction: m\n# Coordinate reference system X, Y: SYNTHETIC_CRS\n# Unit in time: ms\n# Time reference: SYNTHETIC_DATUM\n# MD reference: SYNTHETIC_KB\n# Z depth type: TVDSS\n# Z reference: SYNTHETIC_DATUM\n# Z positive direction: up\n# Null value: -7777\nBEGIN HEADER\nX\nY\nZ\nTWT picked\nMD\nWell\nEND HEADER\n1 2 -25 -15 30 \"Well 1\"\n-7777 3 -35 -25 40 \"Well 1\"\n");
        check(service.importFile(u(declared),options,report,error),error);
        check(service.listData("time_depth",0,entries,error)&&entries.size()==2,"unassigned table stored");
        long long pending=0;for(const auto& e:entries)if(e.wellId==0)pending=e.id;
        check(pending&&service.readTimeDepth(pending,result,error),"pending time table readable");
        check(result.coordinateSystem=="SYNTHETIC_CRS"&&result.timeUnit=="ms"&&result.timeReference=="SYNTHETIC_DATUM"&&result.depthInfo.mdReference=="SYNTHETIC_KB","explicit conventions preserved");
        check(result.positionDepthInfo.depthType=="TVDSS"&&result.positionDepthInfo.tvdssReference=="SYNTHETIC_DATUM"&&result.positionDepthInfo.tvdssDirection=="up","Z conventions separate from MD");
        check(result.depthTimePairs[0].x==1&&std::isnan(result.depthTimePairs[1].x)&&result.depthTimePairs[1].y==3,"source-declared null only affects its own field");

        const auto horizon=temp.path()+"/T70";
        const QByteArray surface="  INLINE : 10 XLINE : 20 1000 2000 200\nINLINE : 10 XLINE : 21 1000 2010 210\nINLINE : 11 XLINE : 20 1010 2000 220\n";
        write(horizon,surface);
        auto surfaceParsed=parseFile(u(horizon));
        check(surfaceParsed.kind=="horizon"&&surfaceParsed.horizons.size()==1&&surfaceParsed.horizons[0].points.size()==3,"extensionless horizon content recognized");
        check(surfaceParsed.horizons[0].timeUnit.empty()&&surfaceParsed.horizons[0].coordinateSystem.empty(),"horizon units not guessed");
        check(service.importFile(u(horizon),options,report,error),error);
        check(service.listData("horizons",0,entries,error)&&entries.size()==1,"horizon imported through public service");
        const auto horizonId=entries[0].id;SeismicHorizon h;
        service.closeProject();check(service.openProject(u(project),error),error);
        check(service.readHorizon(horizonId,1,2,h,error)&&h.points.size()==2&&h.points[1].inlineNumber==11&&h.points[1].zTime==220,"horizon pagination and reopen");
        check(read(horizon)==surface,"horizon input unchanged");

        // Simulate a database written by the previous SDK: no spatial keys at all.
        service.closeProject();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","legacy-input-fixture");db.setDatabaseName(project+"/data.sqlite");check(db.open(),u(db.lastError().text()));
            QSqlQuery q(db);q.prepare("UPDATE well_objects SET payload=? WHERE id=?");
            q.addBindValue(QStringLiteral("{\"depth\":{\"type\":\"MD\",\"unit\":\"m\"},\"timeType\":\"TWT\",\"points\":[{\"depth\":123,\"time\":-45}]}"));q.addBindValue(id);
            check(q.exec(),u(q.lastError().text()));db.close();
        }
        QSqlDatabase::removeDatabase("legacy-input-fixture");
        check(service.openProject(u(project),error)&&service.readTimeDepth(id,result,error),error);
        check(result.depthTimePairs.size()==1&&result.depthTimePairs[0].timeValue==-45&&std::isnan(result.depthTimePairs[0].x)&&std::isnan(result.depthTimePairs[0].z)&&result.coordinateSystem.empty(),"legacy two-column payload remains valid without fabricated geometry");
        std::cout<<"PASS correlation inputs: extensionless horizons, full XYZ/MD/TWT roundtrip, explicit and unknown conventions, nulls, pending wells, legacy compatibility\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
