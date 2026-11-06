#include "paleo_data/service.h"
#include "store/project_store.h"
#include "store/model_change_journal.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <future>
#include <chrono>

using namespace paleo::data;
namespace {
std::string u(const QString& text){return text.toUtf8().toStdString();}
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
QVariant sql(const QString& project,const QString& command,const QVariantList& values={}) {
    const auto name="journal-test-"+QUuid::createUuid().toString();QVariant out;std::string error;
    {auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(project+"/data.sqlite");
     if(!db.open())error=u(db.lastError().text());else{QSqlQuery query(db);if(!query.prepare(command))error=u(query.lastError().text());else{for(const auto& value:values)query.addBindValue(value);if(!query.exec())error=u(query.lastError().text());else if(query.next())out=query.value(0);}}db.close();}
    QSqlDatabase::removeDatabase(name);require(error.empty(),error);return out;
}
ModelChangeHead head(DataService& service) {ModelChangeHead value;std::string error;require(service.readModelChangeHead(value,error),error);return value;}
std::string sourceImage(DataService& service,const ModelChangeHead& target) {
    ModelChangeQuery query;query.expectedGeneration=target.generation;query.sequence=target.sequence;query.maxItems=1;std::string error,image;
    do{ModelChangePage page;require(service.readModelChanges(query,page,error),error);require(page.sequence==target.sequence,"Cursor keeps the exact batch");
       for(const auto& item:page.items)if(item.kind=="source")image=item.metadataJson;
       if(page.complete)break;require(page.nextOrdinal>query.afterOrdinal,"Page cursor advances");query.afterOrdinal=page.nextOrdinal;
    }while(true);return image;
}
void versionChangesWhileWaitingForWriteLock(const QString& project) {
    detail::ProjectStore store;std::string error;require(store.open(project,false,{},error),error);
    const auto originalSchema=sql(project,"PRAGMA schema_version");
    std::promise<void> ready,start;auto readyResult=ready.get_future();auto startResult=start.get_future();std::exception_ptr writerError;
    std::jthread writer([&]{
        const auto connection="future-journal-writer-"+QUuid::createUuid().toString();
        try{
            {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(project+"/data.sqlite");require(db.open(),u(db.lastError().text()));
             QSqlQuery query(db);require(query.exec("BEGIN IMMEDIATE"),u(query.lastError().text()));
             require(query.exec("UPDATE ds_meta SET value='future-during-lock' WHERE key='model_change_schema'"),u(query.lastError().text()));
             ready.set_value();startResult.wait();
             // Keep the incompatible change uncommitted while the SDK attempts
             // BEGIN IMMEDIATE; readers can still see the old WAL snapshot.
             std::this_thread::sleep_for(std::chrono::milliseconds(150));
             require(query.exec("COMMIT"),u(query.lastError().text()));db.close();}
        }catch(...){writerError=std::current_exception();try{ready.set_exception(writerError);}catch(...) {}}
        QSqlDatabase::removeDatabase(connection);
    });
    readyResult.get();start.set_value();const bool upgraded=detail::ensureModelChangeJournal(store,error);writer.join();
    if(writerError)std::rethrow_exception(writerError);
    require(!upgraded&&error.find("UnsupportedModelChangeVersion")!=std::string::npos,"Recheck future journal format after SQLite grants the write lock");
    require(sql(project,"SELECT value FROM ds_meta WHERE key='model_change_schema'").toString()=="future-during-lock","Failed upgrade must not overwrite the newer format marker");
    require(sql(project,"PRAGMA schema_version")==originalSchema,"Unknown format is rejected before any schema DDL");
    sql(project,"UPDATE ds_meta SET value='1' WHERE key='model_change_schema'");
    require(store.exec("BEGIN IMMEDIATE",{},error)&&store.exec("ROLLBACK",{},error),"Rejected upgrade releases the write lock and preserves connection usability");
}
void writerFencingAndStableReopen(const QString& parent) {
    const auto project=parent+"/fenced";DataService old,publisher;std::string error;
    require(old.createProject(u(project),"Fencing",error),error);const auto original=head(old);
    require(publisher.openProject(u(project),error),error);require(head(publisher).generation==original.generation,"Ordinary writable reopen preserves generation");
    const auto baseline=head(publisher);ModelChangeHead forked;
    require(publisher.forkModelGeneration(baseline,forked,error),error);
    WellInfo value;value.wellName="CURRENT-WRITER";long long wellId=0;
    require(publisher.createWell(value,wellId,error),error);require(head(publisher).generation==forked.generation,"The instance performing its own fork remains a valid writer");
    value.wellName="STALE-WRITER";
    require(!old.createWell(value,wellId,error)&&error.find("StaleModelGeneration")!=std::string::npos,"Foreign generation invalidates an already-open explicit writer");
    require(!old.setProjectCrs("EPSG:32649","m",error)&&error.find("StaleModelGeneration")!=std::string::npos,"Foreign generation also fences automatic single-statement DML");
    require(sql(project,"SELECT count(*) FROM wells WHERE name='STALE-WRITER'").toInt()==0,"Rejected stale writer leaves no value behind");
    old.closeProject();publisher.closeProject();
    const auto stableSchema=sql(project,"PRAGMA schema_version");
    for(int pass=0;pass<3;++pass){
        require(publisher.openProject(u(project),error),error);const auto reopened=head(publisher);
        require(reopened.generation==forked.generation,"Repeated ordinary reopen never fabricates an unmanaged generation change");
        if(pass==0){value.wellName="AFTER-REOPEN";require(publisher.createWell(value,wellId,error),error);}
        publisher.closeProject();
        require(sql(project,"PRAGMA schema_version")==stableSchema,"Ordinary reopen does not drop/recreate unchanged journal triggers");
    }
    require(sql(project,"SELECT count(*) FROM wells WHERE name='AFTER-REOPEN'").toInt()==1,"Reopened writer edits survive later opens");
    {detail::ProjectStore store;require(store.open(project,false,{},error),error);ModelChangeHead before;
     QSqlQuery durability;require(store.exec("PRAGMA synchronous",{},error,&durability)&&durability.next()&&durability.value(0).toInt()==2,"Writable reopen explicitly uses synchronous=FULL");durability.finish();
     require(detail::readJournalHead(store,before,error),error);
     require(store.exec("INSERT OR IGNORE INTO ds_meta(key,value) VALUES('project_uuid','ignored-random-uuid')",{},error),error);
     ModelChangeHead after;require(detail::readJournalHead(store,after,error),error);
     require(after.generation==before.generation&&after.sequence==before.sequence,"Ignored metadata insertion has no unmanaged flag or phantom receipt");}
#ifndef Q_OS_WIN
    // POSIX permits renaming a directory while an old SQLite connection remains
    // open. A copied database can even retain the same store/generation tokens;
    // the physical file identity must still fence the retained connection.
    const auto active=parent+"/active",prepared=parent+"/replacement",retained=parent+"/retained";
    require(old.createProject(u(active),"Rename fencing",error),error);
    sql(active,"PRAGMA wal_checkpoint(TRUNCATE)");require(QDir().mkpath(prepared),"Create replacement fixture directory");
    require(QFile::copy(active+"/data.sqlite",prepared+"/data.sqlite"),"Copy only the synthetic checkpointed database");
    auto activeGate=tryAcquireModelWriteLease(u(active),0,error);require(bool(activeGate),error);
    require(QDir().rename(active,retained)&&QDir().rename(prepared,active),"Publish replacement directory while old SQLite handle remains open");
    require(publisher.openProject(u(active),error),error);
    value.wellName="OLD-HANDLE";require(!old.createWell(value,wellId,error)&&error.find("StaleModelGeneration")!=std::string::npos,"Physical identity fences a replaced database even with identical generation tokens");
    const auto current=head(publisher);require(publisher.forkModelGeneration(current,forked,error),error);
    value.wellName="NEW-HANDLE";require(publisher.createWell(value,wellId,error),error);
    publisher.closeProject();old.closeProject();activeGate={};
    require(sql(retained,"SELECT count(*) FROM wells WHERE name='OLD-HANDLE'").toInt()==0&&sql(active,"SELECT count(*) FROM wells WHERE name='NEW-HANDLE'").toInt()==1,"Publication cannot redirect old writer edits into retained backup");
#endif
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temp;require(temp.isValid(),"Temporary directory");const auto project=temp.path()+"/project";
        writerFencingAndStableReopen(temp.path());
        DataService service;std::string error;require(service.createProject(u(project),"Journal fixture",error),error);auto initial=head(service);require(initial.available&&!initial.storeId.empty()&&!initial.generation.empty(),"New store exposes generation and sequence");
        const auto source=temp.path()+"/SYNTH-J1.las";QFile file(source);require(file.open(QIODevice::WriteOnly),"Source fixture");file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-J1\nNULL. -999.25\n~Curve\nDEPT.M\nGR.API\n~ASCII\n100 12\n101 13\n");file.close();
        ImportReport imported;require(service.importFile(u(source),{},imported,error),error);const auto b1=head(service);require(b1.sequence==initial.sequence+1,"One source transaction publishes one batch");
        const auto image1=sourceImage(service,b1);require(!image1.empty()&&image1.find("SYNTH-J1")!=std::string::npos,"B1 persists its own source card");
        std::vector<WellSummary> wells;require(service.listWells(wells,error)&&wells.size()==1,error);WellInfo well;require(service.readWellInfo(wells.front().id,well,error),error);well.wellName="RENAMED-J1";
        require(QFile::remove(source),"Remove only synthetic source to prove replay is file-independent");
        require(service.saveWellInfo(wells.front().id,well,error),error);const auto b2=head(service);require(b2.sequence==b1.sequence+1,"Well edit is a distinct batch");
        require(sourceImage(service,b1)==image1,"B1 after-image stays identical after B2 and does not read the source");
        require(sourceImage(service,b2).find("RENAMED-J1")!=std::string::npos,"Well metadata edit updates its related source card");
        ModelChangeQuery query;query.expectedGeneration=b2.generation;query.afterSequence=b1.sequence;ModelChangePage page;
        require(service.readModelChanges(query,page,error)&&page.sequence==b2.sequence,error);
        query.sequence=b1.sequence;query.maxBytes=1;require(!service.readModelChanges(query,page,error)&&page.items.empty()&&error.find("BudgetExceeded")!=std::string::npos,"Byte budget checks stored metadata before delivery");
        // Journal writes must not change the last inserted business-object identity.
        {detail::ProjectStore store;require(store.open(project,false,{},error),error);QSqlQuery inserted;
         require(store.exec("INSERT INTO wells(name) VALUES('AUTOMATIC-WELL')",{},error,&inserted),error);
         require(inserted.lastInsertId().toLongLong()==sql(project,"SELECT id FROM wells WHERE name='AUTOMATIC-WELL'").toLongLong(),"Automatic commit preserves QSqlQuery::lastInsertId");
         const auto before=head(service);require(store.exec("BEGIN",{},error)&&store.exec("SELECT count(*) FROM wells",{},error)&&store.exec("COMMIT",{},error),error);require(head(service).sequence==before.sequence,"Read-only BEGIN/COMMIT has no event");
         require(store.begin(error)&&store.commit(error),error);require(head(service).sequence==before.sequence,"Empty write transaction has no event");
         require(store.exec("SAVEPOINT cancelled_group",{},error)&&store.exec("UPDATE wells SET name='ROLLED-BACK' WHERE name='AUTOMATIC-WELL'",{},error)&&store.exec("ROLLBACK TO cancelled_group",{},error)&&store.exec("RELEASE cancelled_group",{},error),error);
         require(head(service).sequence==before.sequence,"Savepoint rollback discards dirty set and receipt");}
        const auto beforeFailure=head(service);sql(project,"CREATE TRIGGER reject_journal BEFORE INSERT ON pds_model_batches BEGIN SELECT RAISE(ABORT,'injected receipt failure'); END");
        well.wellName="MUST-ROLL-BACK";require(!service.saveWellInfo(wells.front().id,well,error),"Receipt failure rejects domain commit");sql(project,"DROP TRIGGER reject_journal");
        require(head(service).sequence==beforeFailure.sequence&&sql(project,"SELECT name FROM wells WHERE id=?",{wells.front().id}).toString()=="RENAMED-J1","Receipt failure rolls back domain value and head");
        {auto lease=tryAcquireModelWriteLease(u(project),0,error);require(bool(lease),error);auto moved=std::move(lease);require(!lease&&bool(moved),"Move-only lease transfers ownership");
         auto nested=tryAcquireModelWriteLease(u(project),0,error);require(bool(nested),"Same-thread nested leases share the gate");std::atomic<bool> rejected=false;std::thread other([&]{std::string message;rejected=!tryAcquireModelWriteLease(u(project),0,message);});other.join();require(rejected,"Other thread cannot enter a held writer gate");}
        const auto beforeExternal=head(service);sql(project,"UPDATE wells SET remark='external edit' WHERE id=?",{wells.front().id});ModelChangeHead unavailable;
        require(!service.readModelChangeHead(unavailable,error)&&error.find("RebuildRequired")!=std::string::npos,"External SQL without SDK context is detected explicitly");
        service.closeProject();require(service.openProjectReadOnly(u(project),error),error);require(!service.readModelChangeHead(unavailable,error),"Read-only open does not repair incomplete history");
        service.closeProject();require(service.openProject(u(project),error),error);const auto rebuilt=head(service);require(rebuilt.generation!=beforeExternal.generation&&rebuilt.storeId==beforeExternal.storeId,"Writable reopen starts a new generation after unmanaged SQL");
        query={};query.expectedGeneration=beforeExternal.generation;require(!service.readModelChanges(query,page,error)&&error.find("GenerationChanged")!=std::string::npos,"Old generation cannot be mixed with repaired history");
        ModelChangeHead forked;require(service.forkModelGeneration(rebuilt,forked,error),error);require(forked.generation!=rebuilt.generation&&forked.storeId==rebuilt.storeId&&forked.sequence==0,"Prepared generation fork preserves store identity and values");
        require(!service.forkModelGeneration(rebuilt,unavailable,error),"Generation fork requires an exact CAS baseline");
        require(sql(project,"SELECT remark FROM wells WHERE id=?",{wells.front().id}).toString()=="external edit","Generation fork never rewrites values");
        versionChangesWhileWaitingForWriteLock(project);
        service.closeProject();sql(project,"UPDATE ds_meta SET value='future-version' WHERE key='model_change_schema'");
        require(!service.openProject(u(project),error)&&error.find("UnsupportedModelChangeVersion")!=std::string::npos,"Unknown journal version refuses writable open");
        require(service.openProjectReadOnly(u(project),error),error);require(!service.readModelChangeHead(unavailable,error),"Unknown version cannot claim journal availability");service.closeProject();
        const auto old=temp.path()+"/old";DataService oldService;require(oldService.createProject(u(old),"Legacy",error),error);oldService.closeProject();sql(old,"DELETE FROM ds_meta WHERE key='model_change_schema'");
        require(oldService.openProjectReadOnly(u(old),error),error);require(oldService.readModelChangeHead(unavailable,error)&&!unavailable.available,"Legacy read-only open explicitly reports unavailable and does not upgrade");
        require(!oldService.readModelChanges({},page,error)&&page.items.empty(),"Legacy history query cannot fabricate an empty successful batch");
        std::cout<<"Model change journal checks passed\n";return 0;
    }catch(const std::exception& exception){std::cerr<<exception.what()<<'\n';return 1;}
}
