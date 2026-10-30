// Synthetic preferences are configuration; no source arrays/metadata are edited.
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
void require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
QVariant sql(const QString& project,const QString& command,const QVariantList& values={}){
    const auto name=QStringLiteral("preference-test-")+QUuid::createUuid().toString();QVariant out;std::string error;
    {auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(project+"/data.sqlite");if(!db.open())error=u(db.lastError().text());else{QSqlQuery query(db);if(!query.prepare(command))error=u(query.lastError().text());else{for(const auto& value:values)query.addBindValue(value);if(!query.exec())error=u(query.lastError().text());else if(query.next())out=query.value(0);}}db.close();}QSqlDatabase::removeDatabase(name);require(error.empty(),error);return out;
}
bool same(const std::vector<CorrelationTimeDepthPreference>& a,const std::vector<CorrelationTimeDepthPreference>& b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(a[i].wellId!=b[i].wellId||a[i].timeDepthId!=b[i].timeDepthId||a[i].evidenceJson!=b[i].evidenceJson)return false;return true;}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir temp;require(temp.isValid(),"Temporary project");const auto project=temp.path()+"/project",path=temp.path()+"/synthetic.checkshots";
    const QByteArray bytes="# Petrel checkshots format\n# Unit in depth: m\n# Unit in time: ms\n# Unit in X and Y direction: m\nVERSION 1\nBEGIN HEADER\nX\nY\nZ\nTWT picked\nMD\nWell\nEND HEADER\n100 200 0 0 0 \"SYNTH-A\"\n100 200 -100 -80 100 \"SYNTH-A\"\n100 200 -200 -150 200 \"SYNTH-A\"\n300 400 0 0 0 \"SYNTH-B\"\n300 400 -100 -85 100 \"SYNTH-B\"\n300 400 -200 -160 200 \"SYNTH-B\"\n";
    {QFile f(path);require(f.open(QIODevice::WriteOnly|QIODevice::NewOnly)&&f.write(bytes)==bytes.size(),"Write synthetic relation");}
    DataService service;std::string error;require(service.createProject(u(project),"SYNTHETIC preference tests",error),error);std::vector<CorrelationTimeDepthPreference> values;require(service.readCorrelationTimeDepthPreferences(values,error)&&values.empty(),"Missing preferences read empty");ImportReport report;require(service.importFile(u(path),{},report,error)&&report.failed==0,error);
    std::vector<WellSummary> wells;require(service.listWells(wells,error)&&wells.size()==2,"Two synthetic wells");std::sort(wells.begin(),wells.end(),[](const auto& a,const auto& b){return a.id<b.id;});std::vector<DataEntry> a,b;require(service.listData("time_depth",wells[0].id,a,error)&&a.size()==1,"First TD");require(service.listData("time_depth",wells[1].id,b,error)&&b.size()==1,"Second TD");
    const auto original=sql(project,"SELECT payload FROM well_objects WHERE id=?",{a[0].id}).toString();
    values={{wells[0].id,a[0].id,"{\"grade\":\"synthetic provisional\",\"source\":\"author fixture\"}"},{wells[1].id,b[0].id,"{\"source\":\"independent synthetic author\"}"}};require(service.saveCorrelationTimeDepthPreferences(values,error),error);std::vector<CorrelationTimeDepthPreference> saved;require(service.readCorrelationTimeDepthPreferences(saved,error),error);
    auto reversed=values;std::reverse(reversed.begin(),reversed.end());require(service.saveCorrelationTimeDepthPreferences(reversed,error),error);require(sql(project,"SELECT count(*) FROM changes WHERE kind='correlation_time_depth_preferences'").toInt()==1,"Identical bindings are order-independent and idempotent");
    const auto audit=QJsonDocument::fromJson(sql(project,"SELECT description FROM changes WHERE kind='correlation_time_depth_preferences'").toString().toUtf8()).object();require(audit.value("before").isNull()&&audit.value("after").isObject(),"Binding audit before/after");require(!audit.value("sourceMetadataChanged").toBool(true)&&!audit.value("sourceArraysChanged").toBool(true),"Audit distinguishes binding from source changes");
    for(int test=0;test<5;++test){auto invalid=saved;if(test==0)invalid[0].timeDepthId=b[0].id;if(test==1)invalid[0].timeDepthId=999999;if(test==2)invalid[0].wellId=999999;if(test==3)invalid[0].evidenceJson="{}";if(test==4)invalid.push_back(invalid[0]);require(!service.saveCorrelationTimeDepthPreferences(invalid,error)&&!error.empty(),"Invalid ownership/identity/evidence/duplicate accepted");std::vector<CorrelationTimeDepthPreference> current;require(service.readCorrelationTimeDepthPreferences(current,error)&&same(current,saved),"Invalid binding changed prior configuration");}
    sql(project,"CREATE TRIGGER reject_preference_audit BEFORE INSERT ON changes WHEN NEW.kind='correlation_time_depth_preferences' BEGIN SELECT RAISE(ABORT,'synthetic audit failure'); END");auto replacement=saved;replacement[0].evidenceJson="{\"source\":\"synthetic revision\"}";require(!service.saveCorrelationTimeDepthPreferences(replacement,error),"Audit failure must abort binding save");std::vector<CorrelationTimeDepthPreference> rolledBack;require(service.readCorrelationTimeDepthPreferences(rolledBack,error)&&same(rolledBack,saved),"Binding save did not roll back with audit");sql(project,"DROP TRIGGER reject_preference_audit");
    service.closeProject();require(service.openProject(u(project),error),error);std::vector<CorrelationTimeDepthPreference> restored;require(service.readCorrelationTimeDepthPreferences(restored,error)&&same(restored,saved),"Bindings survive reopen");require(sql(project,"SELECT payload FROM well_objects WHERE id=?",{a[0].id}).toString()==original,"Source TD samples or metadata were modified");{QFile file(path);require(file.open(QIODevice::ReadOnly)&&file.readAll()==bytes,"Source file bytes unchanged");}
    const auto stored=sql(project,"SELECT value FROM ds_meta WHERE key='correlation_time_depth_preferences_v1'").toString();sql(project,"UPDATE ds_meta SET value='{}' WHERE key='correlation_time_depth_preferences_v1'");require(!service.readCorrelationTimeDepthPreferences(restored,error)&&restored.empty(),"Malformed persisted binding must fail");require(!service.saveCorrelationTimeDepthPreferences(saved,error),"Unreadable prior audit state cannot be silently replaced");sql(project,"UPDATE ds_meta SET value=? WHERE key='correlation_time_depth_preferences_v1'",{stored});
    require(service.deleteData("time_depth",b[0].id,error),error);require(service.readCorrelationTimeDepthPreferences(restored,error)&&same(restored,saved),"A deleted TD must not make other wells' historical preferences unreadable");require(!service.saveCorrelationTimeDepthPreferences(saved,error),"Save must still reject stale relation bindings");require(service.saveCorrelationTimeDepthPreferences({saved[0]},error),error);require(service.readCorrelationTimeDepthPreferences(restored,error)&&restored.size()==1&&restored[0].timeDepthId==a[0].id,"Stale binding may be explicitly removed without affecting a valid well");
    require(service.saveCorrelationTimeDepthPreferences({},error),error);require(service.readCorrelationTimeDepthPreferences(restored,error)&&restored.empty(),"Explicit empty replacement clears preferences");
    DataService unopened;require(!unopened.readCorrelationTimeDepthPreferences(restored,error),"Unopened service cannot read defaults");std::cout<<"PASS preferred TD ownership, persistence, audit rollback, idempotence and source preservation\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
