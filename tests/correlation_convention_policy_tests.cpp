// Public SDK behavior with synthetic inputs. SQL is used only to inject a
// corrupt policy and an audit failure, not to implement application behavior.
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
std::string u(const QString& v){return v.toUtf8().toStdString();}
void require(bool value,const std::string& error){if(!value)throw std::runtime_error(error);}
bool same(const CorrelationProjectConventions& a,const CorrelationProjectConventions& b){
    return a.depthType==b.depthType&&a.depthUnit==b.depthUnit&&a.timeUnit==b.timeUnit
        &&a.evidence==b.evidence&&a.allowProvisionalAlignment==b.allowProvisionalAlignment;
}
bool same(const std::vector<double>& a,const std::vector<double>& b){
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[](double x,double y){return x==y||(std::isnan(x)&&std::isnan(y));});
}
QVariant sql(const QString& project,const QString& command,const QVariantList& values={}){
    const auto name=QStringLiteral("policy-test-")+QUuid::createUuid().toString();QVariant result;std::string failure;
    {
        auto database=QSqlDatabase::addDatabase("QSQLITE",name);database.setDatabaseName(project+"/data.sqlite");
        if(!database.open())failure=u(database.lastError().text());
        else{QSqlQuery query(database);if(!query.prepare(command))failure=u(query.lastError().text());else{
            for(const auto& value:values)query.addBindValue(value);
            if(!query.exec())failure=u(query.lastError().text());else if(query.next())result=query.value(0);
        }}
        database.close();
    }
    QSqlDatabase::removeDatabase(name);require(failure.empty(),failure);return result;
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir temp;require(temp.isValid(),"temporary project directory");
    const auto project=temp.path()+QStringLiteral("/工区"),fileName=temp.path()+QStringLiteral("/SYNTH-1.las");
    const QByteArray source="~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nDEPT.m\nGR.API\n~Ascii\n100 5\n101 -999.25\n102 8\n";
    {QFile file(fileName);require(file.open(QIODevice::WriteOnly),"create synthetic LAS");require(file.write(source)==source.size(),"write synthetic LAS");}
    DataService service;std::string error;require(service.createProject(u(project),"SYNTHETIC policy test",error),error);
    CorrelationProjectConventions empty;require(service.readCorrelationProjectConventions(empty,error),error);require(same(empty,{}),"missing policy must remain unknown");
    ImportOptions options;options.copySources=false;ImportReport report;require(service.importFile(u(fileName),options,report,error),error);
    std::vector<WellSummary> wells;require(service.listWells(wells,error)&&wells.size()==1,"one synthetic well");
    std::vector<DataEntry> curves;require(service.listData("curves",wells[0].id,curves,error)&&curves.size()==1,"one GR curve");
    const auto curveId=curves[0].id;CurveSlice before;require(service.readCurveSlice(curveId,Missing,Missing,true,before,error),error);
    CorrelationInputRequest request;CorrelationWellSelection selected;selected.wellId=wells[0].id;selected.curveId=curveId;request.wells.push_back(selected);
    CorrelationInputSnapshot original;require(service.captureCorrelationInputs(request,original,error),error);
    CorrelationProjectConventions policy;policy.depthType="MD";policy.depthUnit="m";policy.timeUnit="ms";
    policy.evidence="Synthetic author explicitly declared units; provisional coordinates remain reviewable";policy.allowProvisionalAlignment=true;
    require(service.saveCorrelationProjectConventions(policy,error),error);require(service.saveCorrelationProjectConventions(policy,error),error);
    require(sql(project,"SELECT count(*) FROM changes WHERE kind='correlation_project_conventions'").toInt()==1,"identical policy must not duplicate audit history");
    const auto audit=QJsonDocument::fromJson(sql(project,"SELECT description FROM changes WHERE kind='correlation_project_conventions'").toString().toUtf8()).object();
    require(audit.value("before").isNull()&&audit.value("after").toObject().value("depthType").toString()=="MD","policy before/after audit missing");
    require(!audit.value("sourceMetadataChanged").toBool(true)&&!audit.value("sourceArraysChanged").toBool(true),"audit must distinguish policy from source facts");
    for(const auto field:{0,1,2,3}){
        auto invalid=policy;if(field==0)invalid.depthType="TWT";if(field==1)invalid.depthUnit="yards";if(field==2)invalid.timeUnit="days";if(field==3)invalid.evidence=" \n ";
        require(!service.saveCorrelationProjectConventions(invalid,error)&&!error.empty(),"invalid policy accepted");
        CorrelationProjectConventions unchanged;require(service.readCorrelationProjectConventions(unchanged,error)&&same(unchanged,policy),"invalid request changed policy");
    }
    // The policy upsert and audit event must roll back together.
    sql(project,"CREATE TRIGGER reject_policy_audit BEFORE INSERT ON changes WHEN NEW.kind='correlation_project_conventions' BEGIN SELECT RAISE(ABORT,'synthetic audit failure'); END");
    auto replacement=policy;replacement.timeUnit="s";replacement.evidence="Synthetic policy revision";
    require(!service.saveCorrelationProjectConventions(replacement,error),"audit failure must reject the transaction");
    CorrelationProjectConventions rolledBack;require(service.readCorrelationProjectConventions(rolledBack,error)&&same(rolledBack,policy),"audit failure left a partial policy update");
    sql(project,"DROP TRIGGER reject_policy_audit");
    service.closeProject();require(service.openProject(u(project),error),error);CorrelationProjectConventions restored;
    require(service.readCorrelationProjectConventions(restored,error)&&same(restored,policy),"policy did not survive reopening");
    CurveSlice after;require(service.readCurveSlice(curveId,Missing,Missing,true,after,error),error);
    require(same(before.depths,after.depths)&&same(before.rawValues,after.rawValues)&&same(before.values,after.values)&&before.quality==after.quality,"source samples or quality changed");
    CorrelationInputSnapshot history;require(service.readCorrelationSnapshot(original.id,history,error),error);
    require(history.fingerprint==original.fingerprint&&history.wells[0].depthInfo.depthType==original.wells[0].depthInfo.depthType,"historical source snapshot changed");
    CorrelationInputSnapshot current;require(service.captureCorrelationInputs(request,current,error),error);
    require(current.wells[0].inputFingerprint==original.wells[0].inputFingerprint&&current.wells[0].log.axisKind==original.wells[0].log.axisKind,"project rules silently rewrote source metadata");
    {QFile file(fileName);require(file.open(QIODevice::ReadOnly)&&file.readAll()==source,"original LAS bytes changed");}
    const auto stored=sql(project,"SELECT value FROM ds_meta WHERE key='correlation_project_conventions_v1'").toString();
    auto corrupt=QJsonDocument::fromJson(stored.toUtf8()).object();corrupt.insert("unknownScientificDefault","not accepted");
    sql(project,"UPDATE ds_meta SET value=? WHERE key='correlation_project_conventions_v1'",{QString::fromUtf8(QJsonDocument(corrupt).toJson(QJsonDocument::Compact))});
    CorrelationProjectConventions invalidRead=policy;require(!service.readCorrelationProjectConventions(invalidRead,error)&&same(invalidRead,{}),"unknown stored fields silently accepted");
    require(!service.saveCorrelationProjectConventions(policy,error),"save must not silently replace an unreadable prior policy");
    sql(project,"UPDATE ds_meta SET value=? WHERE key='correlation_project_conventions_v1'",{stored});
    require(service.readCorrelationProjectConventions(restored,error)&&same(restored,policy),"restore valid stored policy");
    DataService unopened;require(!unopened.readCorrelationProjectConventions(restored,error),"unopened project cannot read defaults");
    std::cout<<"PASS project conventions: explicit policy, audit rollback, reopen, source preservation and malformed policy rejection\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
