#include "object_revision_schema.h"
#include "project_store.h"
#include <QUuid>

namespace paleo::data::detail {
namespace {
const QStringList triggerNames{
    "pds_revision_time_depth_ai","pds_revision_time_depth_au","pds_revision_time_depth_ad",
    "pds_revision_tracks_ai","pds_revision_tracks_au","pds_revision_tracks_ad",
    "pds_revision_intervals_ai","pds_revision_intervals_au","pds_revision_intervals_ad",
    "pds_revision_volumes_ai","pds_revision_volumes_au","pds_revision_volumes_ad"};
QString bump(const QString& kind,const QString& id) {
    return "INSERT INTO object_revisions(kind,object_id,revision) VALUES('"+kind+"',"+id+",1) "
        "ON CONFLICT(kind,object_id) DO UPDATE SET revision=revision+1;";
}
bool cancelledNow(const std::function<bool()>& cancelled,std::string& error) {
    if(cancelled&&cancelled()){error="对象修订元数据升级已取消";return true;}return false;
}
QStringList triggers() {
    QStringList out;
    const auto trigger=[&](const QString& name,const QString& event,const QString& table,const QString& body,const QString& when=QString()) {
        out.append("CREATE TRIGGER "+name+" AFTER "+event+" ON "+table+(when.isEmpty()?QString():" WHEN "+when)+" BEGIN "+body+" END");
    };
    trigger(triggerNames[0],"INSERT","well_objects",bump("time_depth","NEW.id"),"NEW.kind='time_depth'");
    trigger(triggerNames[1],"UPDATE","well_objects",
        "INSERT INTO object_revisions SELECT 'time_depth',OLD.id,1 WHERE OLD.kind='time_depth' ON CONFLICT(kind,object_id) DO UPDATE SET revision=revision+1;"
        "INSERT INTO object_revisions SELECT 'time_depth',NEW.id,1 WHERE NEW.kind='time_depth' AND (OLD.kind<>'time_depth' OR OLD.id<>NEW.id) ON CONFLICT(kind,object_id) DO UPDATE SET revision=revision+1;",
        "OLD.id IS NOT NEW.id OR OLD.kind IS NOT NEW.kind OR OLD.source_id IS NOT NEW.source_id OR OLD.well_id IS NOT NEW.well_id OR OLD.payload IS NOT NEW.payload");
    trigger(triggerNames[2],"DELETE","well_objects",bump("time_depth","OLD.id"),"OLD.kind='time_depth'");
    for(const auto& pair:{std::pair{QString("tracks"),QString("intervals")},std::pair{QString("volumes"),QString("volumes")}}) {
        const auto& table=pair.first;const auto& kind=pair.second;
        trigger("pds_revision_"+table+"_ai","INSERT",table,bump(kind,"NEW.id"));
        const auto changed=table=="volumes"?
            QString("OLD.id IS NOT NEW.id OR OLD.source_id IS NOT NEW.source_id OR OLD.geometry IS NOT NEW.geometry OR OLD.sampling IS NOT NEW.sampling OR OLD.format IS NOT NEW.format OR OLD.amplitude_unit IS NOT NEW.amplitude_unit"):
            QString("OLD.id IS NOT NEW.id OR OLD.source_id IS NOT NEW.source_id OR OLD.well_id IS NOT NEW.well_id OR OLD.type IS NOT NEW.type OR OLD.depth_json IS NOT NEW.depth_json OR OLD.sheet IS NOT NEW.sheet");
        trigger("pds_revision_"+table+"_au","UPDATE",table,bump(kind,"OLD.id")+
            "INSERT INTO object_revisions SELECT '"+kind+"',NEW.id,1 WHERE NEW.id<>OLD.id ON CONFLICT(kind,object_id) DO UPDATE SET revision=revision+1;",changed);
        trigger("pds_revision_"+table+"_ad","DELETE",table,bump(kind,"OLD.id"));
    }
    trigger(triggerNames[6],"INSERT","intervals",bump("intervals","NEW.track_id"));
    trigger(triggerNames[7],"UPDATE","intervals",bump("intervals","OLD.track_id")+
        "INSERT INTO object_revisions SELECT 'intervals',NEW.track_id,1 WHERE NEW.track_id<>OLD.track_id ON CONFLICT(kind,object_id) DO UPDATE SET revision=revision+1;");
    trigger(triggerNames[8],"DELETE","intervals",bump("intervals","OLD.track_id"));
    return out;
}
}
bool objectRevisionSchema(const ProjectStore& store,QString& epoch,bool& ready,std::string& error) {
    epoch.clear();ready=false;QSqlQuery query;
    if(!store.exec("SELECT value FROM ds_meta WHERE key='object_revision_schema'",{},error,&query))return false;
    if(!query.next())return true;
    const auto version=query.value(0).toString();query.finish();
    if(version=="1")return true; // Earlier tracking counted locator/display edits.
    if(version!="2"){error="对象修订格式不受支持";return false;}
    if(!store.exec("SELECT value FROM ds_meta WHERE key='object_revision_epoch'",{},error,&query))return false;
    if(query.next())epoch=query.value(0).toString();query.finish();if(epoch.isEmpty())return true;
    if(!store.exec("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='object_revisions'",{},error,&query)||!query.next())return false;
    const bool table=query.value(0).toInt()==1;query.finish();if(!table)return true;
    QStringList placeholders;QVariantList args;for(const auto& name:triggerNames){placeholders.append("?");args.append(name);}
    if(!store.exec("SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN ("+placeholders.join(',')+")",args,error,&query)||!query.next())return false;
    ready=query.value(0).toInt()==triggerNames.size();return true;
}
bool ensureObjectRevisionSchema(ProjectStore& store,std::string& error,const std::function<bool()>& cancelled) {
    error.clear();if(store.readOnly){error="只读工区不能升级对象修订元数据";return false;}
    if(cancelledNow(cancelled,error))return false;
    bool ready=false;QString epoch;if(!objectRevisionSchema(store,epoch,ready,error))return false;if(ready)return true;
    Transaction transaction(store,error);if(!transaction.active)return false;
    // A second writer may already have completed the upgrade while we waited.
    if(!objectRevisionSchema(store,epoch,ready,error))return false;if(ready)return transaction.commit(error);
    if(!store.exec("CREATE TABLE IF NOT EXISTS object_revisions(kind TEXT NOT NULL,object_id INTEGER NOT NULL,revision INTEGER NOT NULL CHECK(revision>0),PRIMARY KEY(kind,object_id))",{},error))return false;
    for(const auto& name:triggerNames)if(!store.exec("DROP TRIGGER IF EXISTS "+name,{},error))return false;
    // Repairing incomplete tracking starts a new epoch: old tokens cannot be trusted.
    if(!store.exec("DELETE FROM object_revisions",{},error))return false;
    for(const auto& sql:QStringList{
        "INSERT INTO object_revisions SELECT 'time_depth',id,1 FROM well_objects WHERE kind='time_depth'",
        "INSERT INTO object_revisions SELECT 'intervals',id,1 FROM tracks",
        "INSERT INTO object_revisions SELECT 'volumes',id,1 FROM volumes"})if(!store.exec(sql,{},error))return false;
    for(const auto& sql:triggers())if(!store.exec(sql,{},error))return false;
    if(!store.exec("INSERT OR REPLACE INTO ds_meta(key,value) VALUES('object_revision_schema','2'),('object_revision_epoch',?)",
        {QUuid::createUuid().toString(QUuid::WithoutBraces)},error))return false;
    if(cancelledNow(cancelled,error))return false;return transaction.commit(error);
}
}
