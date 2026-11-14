#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
void require(bool value,const std::string& reason){if(!value)throw std::runtime_error(reason);}
std::string text(const QString& value){return value.toUtf8().toStdString();}
void edit(const QString& project,const QString& statement,const QVariantList& arguments={}) {
    const auto name=QUuid::createUuid().toString();bool ok=false;
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(project+"/data.sqlite");
        if(db.open()){
            QSqlQuery query(db);ok=query.prepare(statement);
            for(const auto& argument:arguments)query.addBindValue(argument);
            ok=ok&&query.exec();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);require(ok,"Edit synthetic metadata");
}
void origins(const CurveWindowResult& value,CurveDepthOrigin type,CurveDepthOrigin unit,CurveDepthOrigin reference) {
    require(value.depthOrigins.depthType==type,"Depth type origin");
    require(value.depthOrigins.depthUnit==unit,"Depth unit origin");
    require(value.depthOrigins.mdReference==reference,"MD reference origin");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir temporary;require(temporary.isValid(),"Temporary fixture");
        const auto project=temporary.path()+"/project";DataService service;std::string error;
        require(service.createProject(text(project),"Depth origins",error),error);
        const auto path=temporary.path()+"/declared.las";
        {
            QFile file(path);require(file.open(QIODevice::WriteOnly),"Write LAS");
            file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. ORIGIN-1 : well\nNULL. -999.25\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~ASCII\n100 20\n101 80\n102 40\n");
        }
        ImportReport imported;ImportOptions options;
        require(service.importFile(text(path),options,imported,error),error);require(imported.imported==1,"Actual LAS import");
        std::vector<DataEntry> entries;require(service.listData("curves",0,entries,error)&&entries.size()==1,error);
        const auto selected=entries.front();DepthInfo confirmed;confirmed.depthType="MD";confirmed.depthUnit="M";confirmed.mdReference="KB";
        require(service.saveLogSetDepthInfo(selected.parentId,confirmed,error),error);
        CurveWindowQuery query;query.curveId=selected.id;query.sampleCount=2;query.includeAuxiliaryAxes=false;
        CurveReadOptions budget;budget.maxSamples=2;budget.maxLegacyBytes=0;CurveReadStats usage;CurveWindowResult result;
        require(service.readCurveWindow(query,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Declared,CurveDepthOrigin::Declared,CurveDepthOrigin::Declared);
        require(result.curve.values==std::vector<double>({20,80})&&usage.legacyBytesMaterialized==0,"Exact bounded LAS values");
        require(service.readCurveDepthWindow(selected.id,100,101,true,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Declared,CurveDepthOrigin::Declared,CurveDepthOrigin::Declared);
        edit(project,"UPDATE log_sets SET depth_json=? WHERE id=?",{QString("{\"unit\":\"m\",\"md\":\"KB\"}"),selected.parentId});
        query.sampleCount=0;require(service.readCurveWindow(query,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Inferred,CurveDepthOrigin::Declared,CurveDepthOrigin::Declared);
        require(result.depthInfo.depthType=="MD"&&usage.payloadBytesRead==0,"Metadata-only legacy LAS type recovery");
        edit(project,"UPDATE log_sets SET depth_json=? WHERE id=?",{QString("{\"md\":\"KB\"}"),selected.parentId});
        require(service.readCurveWindow(query,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Inferred,CurveDepthOrigin::Unknown,CurveDepthOrigin::Declared);
        require(result.depthInfo.depthUnit.empty(),"LAS never guesses units");
        edit(project,"UPDATE sources SET kind='workbook',options='{\"comprehensiveWellMetreDepth\":true}' WHERE id=?",{selected.sourceId});
        require(service.readCurveWindow(query,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Inferred,CurveDepthOrigin::Inferred,CurveDepthOrigin::Declared);
        edit(project,"UPDATE log_sets SET depth_json=? WHERE id=?",{QString("{\"type\":\"MD\",\"md\":\"KB\"}"),selected.parentId});
        require(service.readCurveWindow(query,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Declared,CurveDepthOrigin::Inferred,CurveDepthOrigin::Declared);
        edit(project,"UPDATE log_sets SET depth_json=? WHERE id=?",{QString("{\"unit\":\"ft\"}"),selected.parentId});
        require(service.readCurveWindow(query,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Inferred,CurveDepthOrigin::Declared,CurveDepthOrigin::Unknown);
        require(result.depthInfo.depthUnit=="ft","Workbook preserves explicit units");
        edit(project,"UPDATE sources SET kind='table',options='{}' WHERE id=?",{selected.sourceId});
        require(service.readCurveWindow(query,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Unknown,CurveDepthOrigin::Declared,CurveDepthOrigin::Unknown);
        budget.cancelled=[]{return true;};require(!service.readCurveWindow(query,result,error,budget,&usage),"Cancel rejects");
        origins(result,CurveDepthOrigin::Unknown,CurveDepthOrigin::Unknown,CurveDepthOrigin::Unknown);
        budget.cancelled={};budget.maxReadBytes=1;query.sampleCount=2;
        require(!service.readCurveWindow(query,result,error,budget,&usage),"Budget rejects before payload");
        origins(result,CurveDepthOrigin::Unknown,CurveDepthOrigin::Unknown,CurveDepthOrigin::Unknown);
        budget.maxReadBytes=64*1024*1024;service.closeProject();require(service.openProjectReadOnly(text(project),error),error);
        require(service.readCurveWindow(query,result,error,budget,&usage),error);
        origins(result,CurveDepthOrigin::Unknown,CurveDepthOrigin::Declared,CurveDepthOrigin::Unknown);
        std::cout<<"Curve depth origin checks passed (12 scenarios, bounded/metadata-only/read-only)\n";return 0;
    } catch(const std::exception& exception) {std::cerr<<exception.what()<<'\n';return 1;}
}
