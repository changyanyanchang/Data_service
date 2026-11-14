#include <paleo_data/service.h>
#include "store/bounded_metadata.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
void require(bool ok,const std::string &why){if(!ok)throw std::runtime_error(why);}
std::string utf8(const QString &value){return value.toUtf8().toStdString();}
struct Fixture {
    QTemporaryDir temporary;QString project,connection;DataService service;QSqlDatabase database;long long well=0,curve=0,second=0;
    Fixture() {
        project=temporary.filePath("project");std::string error;require(service.createProject(utf8(project),"metadata budget",error),error);
        QFile source(temporary.filePath("source.data"));const QByteArray bytes="typed bounded metadata";
        require(source.open(QIODevice::WriteOnly)&&source.write(bytes)==bytes.size(),"write source");source.close();
        ParsedFile parsed;parsed.kind="logs";parsed.status="parsed";WellDataset dataset;dataset.wellInfo.wellName="W";
        LogSet logs;logs.logSetName="raw";logs.depthInfo={"MD","m","KB","","",""};
        LogCurve left,right;left.curveName="GR";left.curveUnit="API";right.curveName="DT";right.curveUnit="us/m";
        for(int i=0;i<128;++i){logs.samplingAxis.depthValues.push_back(i);left.sampleValues.push_back(i);right.sampleValues.push_back(2*i);}
        logs.logCurves={left,right};dataset.wellbore.logSets={logs};parsed.wells={dataset};long long sourceId=0;
        require(service.importPreparedFile(utf8(source.fileName()),utf8(source.fileName()),utf8(source.fileName()),
            QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().toStdString(),parsed,sourceId,error),error);
        std::vector<DataEntry> entries;require(service.listData("curves",0,entries,error)&&entries.size()==2,"typed curve fixture");
        curve=entries[0].id;second=entries[1].id;well=entries[0].wellId;
        connection=QUuid::createUuid().toString();database=QSqlDatabase::addDatabase("QSQLITE",connection);
        database.setDatabaseName(project+"/data.sqlite");require(database.open(),"open metadata writer");
    }
    ~Fixture(){database.close();database={};QSqlDatabase::removeDatabase(connection);}
    QString value(const QString &table,const QString &column,long long id=1) {
        QSqlQuery query(database);query.prepare("SELECT "+column+" FROM "+table+" WHERE id=?");query.addBindValue(id);require(query.exec()&&query.next(),"read small fixture metadata");return query.value(0).toString();
    }
    void set(const QString &table,const QString &column,const QString &text,long long id=1) {
        QSqlQuery query(database);query.prepare("UPDATE "+table+" SET "+column+"=? WHERE id=?");query.addBindValue(text);query.addBindValue(id);require(query.exec(),"write metadata mutation");
    }
};
CurveReadOptions options() {
    CurveReadOptions out;out.maxSamples=0;out.maxLegacyBytes=0;out.maxReadBytes=8192;
    out.maxMetadataBytes=8192;out.maxMetadataFieldBytes=8192;return out;
}
void windowDenied(Fixture &fixture,const CurveReadOptions &budget) {
    CurveWindowQuery query;query.curveId=fixture.curve;query.sampleCount=0;query.includeAuxiliaryAxes=false;
    CurveWindowResult result;result.curve.name="old";CurveReadStats stats;std::string error;
    require(!fixture.service.readCurveWindow(query,result,error,budget,&stats)&&error.starts_with("CurveWindowBudgetExceeded:"),error);
    require(result.curve.name.empty()&&result.curve.values.empty()&&result.totalSamples==0,"failed window clears output");
    require(stats.metadataBytesRead==0&&stats.payloadBytesRead==0&&stats.axisBlocksRead==0&&stats.valueBlocksRead==0,"denied metadata never returns text or arrays");
}
void curveBudget(Fixture &fixture) {
    CurveWindowQuery query;query.curveId=fixture.curve;query.sampleCount=0;query.includeAuxiliaryAxes=false;
    auto budget=options();CurveReadStats stats;CurveWindowResult result;std::string error;
    require(fixture.service.readCurveWindow(query,result,error,budget,&stats),error);
    require(stats.metadataBytesRead>0&&stats.metadataBytesRead<=8192&&stats.payloadBytesRead==0&&stats.axisBlocksRead==0&&stats.valueBlocksRead==0,"metadata-only window reads zero arrays");
    auto small=budget;small.maxMetadataBytes=stats.metadataBytesRead-1;windowDenied(fixture,small);
    for(const auto &field:std::vector<std::pair<QString,QString>>{{"curves","name"},{"curves","unit"},{"log_sets","depth_json"},
        {"log_sets","encoding"},{"sources","kind"},{"sources","options"},{"sources","original_path"}}) {
        const auto original=fixture.value(field.first,field.second);
        fixture.set(field.first,field.second,QString(16*1024*1024,QChar('x')));windowDenied(fixture,budget);
        fixture.set(field.first,field.second,original);
    }
    fixture.set("curves","name",QString::fromUtf8("曲线🌍"));
    require(fixture.service.readCurveWindow(query,result,error,budget,&stats),error);require(result.curve.name==utf8(QString::fromUtf8("曲线🌍")),"window UTF-8 preserved");
}
void pageBudget(Fixture &fixture) {
    MetadataReadOptions budget;budget.maxFieldBytes=8192;budget.maxTextBytes=128*1024;MetadataReadStats stats;
    std::vector<DataEntry> page;bool more=false;std::string error;
    require(fixture.service.listDataPage("curves",fixture.well,0,2,page,more,error,budget,&stats)&&page.size()==2&&!more,error);
    long long expected=0;for(const auto &row:page)expected+=row.name.size()+row.detail.size();require(stats.textBytesRead==expected,"page bytes count exact returned name/detail UTF-8");
    auto small=budget;small.maxTextBytes=expected-1;
    require(!fixture.service.listDataPage("curves",fixture.well,0,2,page,more,error,small,&stats)&&error=="DataPageValueBudgetExceeded",error);
    require(page.empty()&&!more&&stats.textBytesRead==0,"aggregate page denial clears output/stats");
    for(const auto &field:std::vector<std::pair<QString,QString>>{{"curves","name"},{"curves","unit"},{"log_sets","name"}}) {
        const auto original=fixture.value(field.first,field.second);
        fixture.set(field.first,field.second,QString(16*1024*1024,QChar('x')));
        require(!fixture.service.listDataPage("curves",fixture.well,0,2,page,more,error,budget,&stats)&&error=="DataPageValueBudgetExceeded",error);
        require(page.empty()&&!more&&stats.textBytesRead==0,"large point field never returns a page");fixture.set(field.first,field.second,original);
    }
    const auto original=fixture.value("curves","name",fixture.second);fixture.set("curves","name",QString(16*1024*1024,QChar('x')),fixture.second);
    require(fixture.service.listDataPage("curves",fixture.well,0,1,page,more,error,budget,&stats)&&page.size()==1&&more,"numeric-only continuation probe does not materialize next TEXT");
    require(!fixture.service.listDataPage("curves",fixture.well,fixture.curve,1,page,more,error,budget,&stats)&&error=="DataPageValueBudgetExceeded",error);
    fixture.set("curves","name",original,fixture.second);
}
void opcodeGuard(Fixture &fixture) {
    const QStringList fields{"message"};const auto guard=detail::metadataGuard(fields,MetadataReadOptions{});
    QSqlQuery query(fixture.database);require(query.exec("EXPLAIN SELECT "+detail::guardedMetadataColumns(fields,guard)+" FROM sources WHERE id=1"),"explain production SQL guard");
    std::vector<std::pair<int,int>> jumps;bool metadataOnly=false,guardedPayload=false;
    while(query.next()) {
        const auto address=query.value(0).toInt();const auto op=query.value(1).toString();
        if(op=="Column"&&query.value(6).toInt()==192)metadataOnly=true;
        if(op=="Column"&&query.value(6).toInt()==0) {
            for(const auto &[before,target]:jumps)if(before<address&&target>address)guardedPayload=true;
        }
        if(op=="IfNot"||op=="If"||op=="Ne"||op=="Gt"||op=="Ge")jumps.emplace_back(address,query.value(3).toInt());
    }
    require(metadataOnly&&guardedPayload,"SQLite opcode checks metadata before branching around full TEXT Column");
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        Fixture fixture;SourceIndexEntry source;SourceIndexReadStats sourceStats;std::string capabilityError;
        if(!fixture.service.readSourceIndexEntry(1,source,capabilityError,SourceIndexReadOptions{},&sourceStats)) {
            require(capabilityError.starts_with("SourceIndexValueBudgetUnavailable:"),capabilityError);
            CurveWindowQuery query;query.curveId=fixture.curve;query.sampleCount=0;query.includeAuxiliaryAxes=false;
            CurveWindowResult value;CurveReadStats stats;std::string error;std::vector<DataEntry> page;bool more=false;MetadataReadStats pageStats;
            require(!fixture.service.readCurveWindow(query,value,error,options(),&stats)&&error.starts_with("CurveWindowMetadataUnavailable:"),"bounded curve unavailable explicitly");
            require(!fixture.service.listDataPage("curves",fixture.well,0,1,page,more,error,MetadataReadOptions{},&pageStats)&&error.starts_with("DataPageMetadataUnavailable:"),"bounded page unavailable explicitly");
            require(fixture.service.readCurveWindow(query,value,error,CurveReadOptions{},&stats),"legacy curve remains available");
            require(fixture.service.listDataPage("curves",fixture.well,0,1,page,more,error),"legacy page remains available");
            std::cout<<"metadata_budget unavailable with legacy compatibility verified: "<<capabilityError<<'\n';return 0;
        }
        curveBudget(fixture);pageBudget(fixture);opcodeGuard(fixture);
        Fixture arrays;auto budget=options();budget.maxSamples=128;budget.maxReadBytes=1024*1024;
        CurveWindowQuery query;query.curveId=arrays.curve;query.sampleCount=4;query.includeAuxiliaryAxes=false;CurveReadStats stats;CurveWindowResult value;std::string error;
        require(arrays.service.readCurveWindow(query,value,error,budget,&stats),error);require(stats.payloadBytesRead>0&&stats.metadataBytesRead>0,"numeric and metadata counters separate");
        budget.maxReadBytes=stats.payloadBytesRead+stats.metadataBytesRead-1;
        require(!arrays.service.readCurveWindow(query,value,error,budget,&stats)&&error.starts_with("CurveWindowBudgetExceeded:"),"total budget charges metadata plus numeric payload");
        require(value.curve.values.empty(),"aggregate read failure returns no arrays");
        std::cout<<"metadata_budget passed: seven 16MiB window fields, three 16MiB page fields, UTF-8/aggregate/opcode/zero-array/continuation checks\n";return 0;
    } catch(const std::exception &failure){std::cerr<<failure.what()<<'\n';return 1;}
}
