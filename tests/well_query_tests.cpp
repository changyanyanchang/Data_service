#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
void check(bool good,const std::string& message) {if(!good)throw std::runtime_error(message);}
QString sql(const QString& root,const QString& statement) {
    QString result;const auto name=QStringLiteral("well_query_test");
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(QDir(root).filePath("data.sqlite"));
        check(db.open(),"open SQL probe");
        {QSqlQuery query(db);check(query.exec(statement),"execute SQL probe");while(query.next())
            for(int column=0;column<query.record().count();++column)result+=query.value(column).toString()+" ";}
        db.close();
    }
    QSqlDatabase::removeDatabase(name);return result;
}
}
int runWellQueryMetadataContract() {
    try {
        QTemporaryDir temporary;check(temporary.isValid(),"temporary directory");
        const auto root=temporary.filePath("project");DataService writer;std::string error;
        check(writer.createProject(root.toStdString(),"well query",error),error);
        ModelChangeHead baseline;check(writer.readModelChangeHead(baseline,error)&&baseline.available,error);
        std::vector<long long> expected;
        for(const auto& name:QStringList{"A-1","A_1","a 1",QString::fromUtf8("A\xE3\x80\x80" "1")}) {
            WellInfo well;well.wellName=name.toUtf8().toStdString();well.totalDrilledDepth=123;
            long long id=0;check(writer.createWell(well,id,error),error);expected.push_back(id);
        }
        DataService reader;check(reader.openProjectReadOnly(root.toStdString(),error),error);
        ModelChangeHead initialHead;check(reader.readModelChangeHead(initialHead,error)&&initialHead.available,error);
        ModelChangeQuery changeOptions;changeOptions.expectedGeneration=initialHead.generation;changeOptions.afterSequence=baseline.sequence;ModelChangePage changes;
        do {
            check(reader.readModelChanges(changeOptions,changes,error)&&changes.complete,error);
            for(const auto& item:changes.items)check(item.kind!="store","derived query index must not publish store rebuild items: "+item.metadataJson);
            changeOptions.afterSequence=changes.sequence;
        }while(changes.sequence&&changes.sequence<initialHead.sequence);
        std::vector<long long> all,page;bool more=false;long long after=0;
        do {
            check(reader.findWellIdsByQueryName(" a- _1 ",after,2,page,more,error),error);
            check(page.size()<=2,"bounded name page");all.insert(all.end(),page.begin(),page.end());
            if(!page.empty())after=page.back();
        }while(more);
        check(all==expected,"Unicode normalized name page reconstruction");
        WellInfo punctuation;punctuation.wellName="- _";punctuation.totalDrilledDepth=-15;long long punctuationId=0;
        check(writer.createWell(punctuation,punctuationId,error),error);
        check(reader.findWellIdsByQueryName("_-",0,8,page,more,error)&&page==std::vector<long long>{punctuationId},"empty normalized key is a non-null key");
        WellQueryValue row;bool found=false;
        check(reader.readWellQueryValue(punctuationId,{},row,found,error)&&found&&row.name=="- _"&&row.totalDepth==-15,"raw negative TD and name preserved");
        WellInfo missing;missing.wellName="NoDepth";long long missingId=0;
        check(writer.createWell(missing,missingId,error),error);
        check(reader.readWellQueryValue(missingId,{},row,found,error)&&found&&std::isnan(row.totalDepth),"missing TD remains NaN");
        check(reader.readWellQueryValue(expected.front(),{},row,found,error)&&found&&row.totalDepth==123,"optional point read");
        check(reader.readWellQueryValue(999999,{},row,found,error)&&!found,"missing point is not an I/O failure");
        check(!reader.findWellIdsByQueryName("a1",0,0,page,more,error)&&error=="InvalidWellQueryBudget","bad page budget");
        check(!reader.findWellIdsByQueryName(std::string(5000,'a'),0,1,page,more,error)&&error=="InvalidWellQueryBudget","bounded query input");
        WellInfo large;large.wellName=std::string(5000,'x');long long largeId=0;
        check(writer.createWell(large,largeId,error),error);
        check(!reader.readWellQueryValue(largeId,{},row,found,error)&&error=="WellQueryValueBudgetExceeded","oversized label is not truncated");
        const auto plan=sql(root,"EXPLAIN QUERY PLAN SELECT well_id FROM pds_well_query_names WHERE name_key='a1' AND well_id>0 ORDER BY well_id LIMIT 3");
        check(plan.contains("pds_well_query_name_key")&&!plan.contains("SCAN"),"query must seek the compound name/ID index");
        WellInfo collision;check(writer.readWellInfo(expected.front(),collision,error),error);collision.wellName="A_1";
        check(!writer.saveWellInfo(expected.front(),collision,error),"duplicate rename must roll back");
        check(reader.findWellIdsByQueryName("a1",0,8,page,more,error)&&page==expected,"failed write must preserve name index");
        reader.closeProject();sql(root,"UPDATE wells SET name='new-name' WHERE id="+QString::number(expected.front()));
        sql(root,"CREATE TRIGGER pds_model_pds_well_query_names_d AFTER DELETE ON pds_well_query_names BEGIN UPDATE pds_model_state SET unmanaged=1; END");
        check(reader.openProjectReadOnly(root.toStdString(),error),error);
        check(!reader.findWellIdsByQueryName("newname",0,8,page,more,error)&&error.find("WellQueryIndexUnavailable")==0,"unprojected old writer must refuse");
        reader.closeProject();writer.closeProject();check(writer.openProject(root.toStdString(),error),error);
        check(reader.openProjectReadOnly(root.toStdString(),error),error);
        ModelChangeHead reopenedHead;check(reader.readModelChangeHead(reopenedHead,error)&&reopenedHead.available,error);
        check(reader.findWellIdsByQueryName("new name",0,8,page,more,error)&&page==std::vector<long long>{expected.front()},"writable rebuild restores exact metadata index");
        reader.closeProject();writer.closeProject();
        sql(root,"DROP TABLE pds_well_query_names");check(reader.openProjectReadOnly(root.toStdString(),error),error);
        check(!reader.findWellIdsByQueryName("a1",0,8,page,more,error),"read-only legacy query never builds an index");
        check(sql(root,"SELECT name FROM sqlite_master WHERE type='table' AND name='pds_well_query_names'").isEmpty(),"read-only query must not migrate schema");
        reader.closeProject();check(writer.openProject(root.toStdString(),error),error);
        check(writer.findWellIdsByQueryName("newname",0,8,page,more,error)&&page.size()==1,"legacy additive rebuild");
        std::cout<<"well query metadata contract passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#ifndef PALEO_EMBED_WELL_QUERY_TEST
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    return runWellQueryMetadataContract();
}
#endif
