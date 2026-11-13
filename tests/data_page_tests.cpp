#include <paleo_data/service.h>
#include "store/well_data_query_index.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QCryptographicHash>
#include <limits>
#include <iostream>
#include <stdexcept>
using namespace paleo::data;
namespace {
void check(bool ok,const std::string &message){if(!ok)throw std::runtime_error(message);}
ParsedFile fixture(int firstCount=1000) {
    ParsedFile parsed;parsed.kind="synthetic";parsed.status="parsed";
    for(int wellIndex=0;wellIndex<2;++wellIndex) {
        WellDataset well;well.wellInfo.wellName="W-"+std::to_string(wellIndex);
        well.defaultDepthInfo={"MD","ft","KB"};LogSet logs;logs.logSetName="Original";
        logs.depthInfo=well.defaultDepthInfo;logs.samplingAxis.depthValues={-2.,0.,2.};
        for(int i=0;i<(wellIndex?3:firstCount);++i) {
            LogCurve curve;curve.curveName="C-"+std::to_string(i);curve.curveUnit="mV";
            curve.sampleValues={-4.,double(i),7.};logs.logCurves.push_back(curve);
            IntervalTrack track;track.trackName="T-"+std::to_string(i);track.trackType="Declared lithology";
            track.depthInfo=well.defaultDepthInfo;GeologicalInterval interval;interval.intervalName="raw";
            interval.topDepth=-2.;interval.bottomDepth=2.;track.intervals.push_back(interval);
            well.wellbore.intervalTracks.push_back(track);
        }
        well.wellbore.logSets.push_back(logs);parsed.wells.push_back(well);
    }
    return parsed;
}
void indexedLargeInventory(const QString& root,const QString& source) {
    DataService writer;std::string error;check(writer.createProject(root.toStdString(),"large indexed",error),error);
    const auto hash=QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex().toStdString();long long sourceId=0;
    check(writer.importPreparedFile(source.toStdString(),source.toStdString(),source.toStdString(),hash,fixture(10000),sourceId,error),error);
    std::vector<long long>wells;check(writer.listSourceWells(sourceId,wells,error)&&wells.size()==2,error);writer.closeProject();
    DataService reader;check(reader.openProjectReadOnly(root.toStdString(),error),error);
    const auto name=QStringLiteral("data_page_index_probe");{
        auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(root+"/data.sqlite");check(db.open(),"index probe open");
        const auto statement=[&](const QString& sql){QSqlQuery query(db);check(query.exec(sql),query.lastError().text().toStdString());return query;};
        for(const auto& kind:{std::string("curves"),std::string("intervals")}) {
            QSqlQuery plan(db);check(plan.prepare("EXPLAIN QUERY PLAN "+paleo::data::detail::wellDataPageSql(kind,true)),"prepare actual producer plan");
            plan.addBindValue(wells[1]);plan.addBindValue(0LL);plan.addBindValue(3);check(plan.exec(),"run actual producer plan");QString explanation;
            while(plan.next())explanation+=plan.value(3).toString()+"\n";
            const auto index=kind=="curves"?"pds_well_query_curve_well":"pds_well_query_track_well";
            check(explanation.contains(index)&&explanation.contains("well_id=?")&&explanation.contains(kind=="curves"?"curve_id>?":"track_id>?"),"exact well/keyset must seek compound index: "+explanation.toStdString());
            check(!explanation.contains("SCAN i")&&!explanation.contains("SCAN c")&&!explanation.contains("SCAN t")&&!explanation.contains("CORRELATED"),"no other-well or interval-count scan: "+explanation.toStdString());
            std::vector<DataEntry> page;bool more=false;check(reader.listDataPage(kind,wells[1],0,2,page,more,error)&&page.size()==2&&more,error);
            check(page.front().name==(kind=="curves"?"C-0":"T-0")&&page.front().wellId==wells[1],"last small well after 10000 other-well objects");
        }
        statement("PRAGMA foreign_keys=ON");statement("BEGIN");
        statement("UPDATE log_sets SET well_id="+QString::number(wells[1])+" WHERE well_id="+QString::number(wells[0]));
        auto count=statement("SELECT count(*) FROM pds_well_query_curves WHERE well_id="+QString::number(wells[1]));check(count.next()&&count.value(0).toInt()==10003,"log-set reassignment updates every exact curve mapping");count.finish();
        statement("UPDATE tracks SET well_id="+QString::number(wells[1])+" WHERE well_id="+QString::number(wells[0]));
        count=statement("SELECT count(*) FROM pds_well_query_tracks WHERE well_id="+QString::number(wells[1]));check(count.next()&&count.value(0).toInt()==10003,"track reassignment index");count.finish();
        statement("INSERT INTO intervals(track_id,ordinal,name,top,bottom) VALUES((SELECT max(id) FROM tracks),2,'new',3,4)");
        count=statement("SELECT record_count FROM pds_well_query_tracks WHERE track_id=(SELECT max(id) FROM tracks)");check(count.next()&&count.value(0).toInt()==2,"increment interval count without scans");count.finish();
        statement("DELETE FROM intervals WHERE id=(SELECT max(id) FROM intervals)");
        count=statement("SELECT record_count FROM pds_well_query_tracks WHERE track_id=(SELECT max(id) FROM tracks)");check(count.next()&&count.value(0).toInt()==1,"decrement interval count");count.finish();
        statement("UPDATE intervals SET track_id=(SELECT min(id) FROM tracks) WHERE id=(SELECT max(id) FROM intervals)");
        count=statement("SELECT record_count FROM pds_well_query_tracks WHERE track_id=(SELECT max(id) FROM tracks)");check(count.next()&&count.value(0).toInt()==0,"moving an interval decrements its old track");count.finish();
        count=statement("SELECT record_count FROM pds_well_query_tracks WHERE track_id=(SELECT min(id) FROM tracks)");check(count.next()&&count.value(0).toInt()==2,"moving an interval increments its exact new track");count.finish();
        statement("DELETE FROM log_sets WHERE id=(SELECT min(id) FROM log_sets)");
        count=statement("SELECT count(*) FROM pds_well_query_curves");check(count.next()&&count.value(0).toInt()==3,"cascade deletion removes derived curve mappings");count.finish();
        statement("ROLLBACK");
        statement("DROP INDEX pds_well_query_curve_well");
    }QSqlDatabase::removeDatabase(name);
    std::vector<DataEntry> page;bool more=false;
    check(!reader.listDataPage("curves",wells[1],0,2,page,more,error)&&error.find("DataPageIndexUnavailable")==0,"old read-only/missing index has no scan fallback");reader.closeProject();
    check(writer.openProject(root.toStdString(),error),error);writer.closeProject();check(reader.openProjectReadOnly(root.toStdString(),error),error);
    check(reader.listDataPage("curves",wells[1],0,4,page,more,error)&&page.size()==3&&!more,"writable migration rebuilds exact index after rollback");
    check(reader.listDataPage("intervals",wells[1],0,4,page,more,error)&&page.size()==3&&page.front().count==1,"cached counts survive rollback and rebuild");
}
void verifyPages(DataService &reader,const std::string &kind,long long well,int limit) {
    std::vector<DataEntry> expected,actual,page;std::string error;bool more=true;long long after=0;
    check(reader.listData(kind,well,expected,error),error);
    do {
        check(reader.listDataPage(kind,well,after,limit,page,more,error),error);
        check(page.size()<=size_t(limit),"producer returned too many rows");
        for(const auto &row:page){check(row.id>after,"strictly monotonic cursor");after=row.id;actual.push_back(row);}
        check(!more||!page.empty(),"hasMore must progress");
    }while(more);
    check(actual.size()==expected.size(),"complete reconstruction");
    for(size_t i=0;i<actual.size();++i){const auto &a=actual[i],&e=expected[i];
        check(a.id==e.id&&a.wellId==e.wellId&&a.sourceId==e.sourceId&&a.parentId==e.parentId&&
            a.name==e.name&&a.detail==e.detail&&a.count==e.count&&a.kind==e.kind,"preserve actual metadata values");}
}
}
int main(int argc,char **argv) {
    QCoreApplication application(argc,argv);
    try {
        QTemporaryDir temporary;check(temporary.isValid(),"temporary directory");DataService writer;std::string error;
        check(writer.createProject(temporary.filePath("project").toStdString(),"bounded",error),error);
        const auto source=temporary.filePath("synthetic");QFile file(source);check(file.open(QIODevice::WriteOnly),"source");file.write("synthetic");file.close();
        const auto fingerprint=QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex().toStdString();
        long long sourceId=0;check(writer.importPreparedFile(source.toStdString(),source.toStdString(),source.toStdString(),fingerprint,fixture(),sourceId,error),error);
        std::vector<long long>wells;check(writer.listSourceWells(sourceId,wells,error)&&wells.size()==2,error);
        const auto project=writer.projectPath();writer.closeProject();DataService reader;check(reader.openProjectReadOnly(project,error),error);
        ModelChangeHead before,after;check(reader.readModelChangeHead(before,error),error);
        for(const auto &kind:{std::string("curves"),std::string("intervals")}) {
            verifyPages(reader,kind,wells[0],37);verifyPages(reader,kind,0,512);verifyPages(reader,kind,wells[1],1);
            std::vector<DataEntry> page;bool more=true;
            check(reader.listDataPage(kind,0,std::numeric_limits<long long>::max(),1,page,more,error)&&page.empty()&&!more,"terminal cursor");
            for(int limit:{0,513})check(!reader.listDataPage(kind,0,0,limit,page,more,error)&&page.empty()&&!more&&error=="InvalidDataPageBudget","invalid limit");
            check(!reader.listDataPage(kind,0,-1,1,page,more,error)&&page.empty()&&!more,"negative cursor");
            check(reader.listDataPage(kind,999999,0,1,page,more,error)&&page.empty()&&!more,"exact unknown well");
        }
        std::vector<DataEntry> page;bool more=true;
        check(!reader.listDataPage("volumes",0,0,2,page,more,error)&&page.empty()&&!more&&error=="UnsupportedDataPageKind","unknown kind");
        check(reader.readModelChangeHead(after,error)&&before.sequence==after.sequence&&before.generation==after.generation,"read-only query does not change journal");
        reader.closeProject();
        const auto connection=QStringLiteral("data_page_probe");{
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(QString::fromStdString(project)+"/data.sqlite");check(db.open(),"probe open");
            QSqlQuery query(db);check(query.exec("UPDATE curves SET name=replace(hex(zeroblob(3000)),'0','x') WHERE id=(SELECT max(id) FROM curves)"),"oversized metadata probe");
        }QSqlDatabase::removeDatabase(connection);
        check(reader.openProjectReadOnly(project,error),error);
        check(!reader.listDataPage("curves",wells[1],0,512,page,more,error)&&page.empty()&&!more&&error=="DataPageValueBudgetExceeded","whole page rejects oversized text without truncation");
        reader.closeProject();indexedLargeInventory(temporary.filePath("large"),source);
        std::cout<<"data page contract passed: 1000 curves/tracks, exact reconstruction, bounds, reopening, immutable values\n";
        return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
