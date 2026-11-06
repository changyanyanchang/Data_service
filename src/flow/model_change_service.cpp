#include "store/project_store.h"
#include "store/model_change_journal.h"
#include <limits>
#include <QUuid>

namespace paleo::data {
bool DataService::readModelChangeHead(ModelChangeHead& result,std::string& error)const {
    return detail::readJournalHead(impl_->store,result,error);
}
bool DataService::readModelChanges(const ModelChangeQuery& query,ModelChangePage& result,std::string& error)const {
    result={};error.clear();
    if(query.afterSequence<0||query.sequence<0||query.afterOrdinal<0||query.maxItems<1||query.maxItems>2000||query.maxBytes<1||query.maxBytes>16*1024*1024){error="Invalid model change cursor/budget";return false;}
    auto& store=impl_->store;if(!store.exec("SAVEPOINT model_change_page",{},error))return false;
    struct Guard {detail::ProjectStore& store;bool active=true;~Guard(){if(active){std::string ignored;store.exec("ROLLBACK TO model_change_page",{},ignored);store.exec("RELEASE model_change_page",{},ignored);}}} guard{store};
    ModelChangePage page;if(!detail::readJournalHead(store,page.head,error))return false;
    if(!page.head.available){error="ModelChangeUnavailable: legacy store requires a writable upgrade";return false;}
    if(!query.expectedGeneration.empty()&&query.expectedGeneration!=page.head.generation){error="ModelGenerationChanged: bootstrap required";return false;}
    QSqlQuery batch;
    if(query.sequence){if(!store.exec("SELECT sequence,operation_id FROM pds_model_batches WHERE sequence=?",{qlonglong(query.sequence)},error,&batch))return false;}
    else if(!store.exec("SELECT sequence,operation_id FROM pds_model_batches WHERE sequence>? ORDER BY sequence LIMIT 1",{qlonglong(query.afterSequence)},error,&batch))return false;
    if(batch.next()){
        page.sequence=batch.value(0).toLongLong();page.operationId=detail::s(batch.value(1).toString());batch.finish();
        QSqlQuery headers;if(!store.exec("SELECT ordinal,object_id,source_id,kind,deleted,length(CAST(metadata AS BLOB)) FROM pds_model_items WHERE sequence=? AND ordinal>? ORDER BY ordinal LIMIT ?",
            {qlonglong(page.sequence),qlonglong(query.afterOrdinal),query.maxItems+1},error,&headers))return false;
        long long bytes=0;page.nextOrdinal=query.afterOrdinal;
        while(headers.next()){
            const auto size=headers.value(5).toLongLong();
            if(page.items.size()>=static_cast<size_t>(query.maxItems)||size>query.maxBytes-bytes){
                if(page.items.empty()){error="ModelChangeBudgetExceeded: metadata item does not fit page";return false;}page.complete=false;break;
            }
            ModelChangeItem item;item.ordinal=headers.value(0).toLongLong();item.objectId=headers.value(1).toLongLong();item.sourceId=headers.value(2).toLongLong();item.kind=detail::s(headers.value(3).toString());item.deleted=headers.value(4).toBool();
            QSqlQuery image;if(!store.exec("SELECT metadata FROM pds_model_items WHERE sequence=? AND ordinal=?",{qlonglong(page.sequence),qlonglong(item.ordinal)},error,&image)||!image.next())return false;
            item.metadataJson=detail::s(image.value(0).toString());bytes+=size;page.nextOrdinal=item.ordinal;page.items.push_back(std::move(item));
        }
    }else if(query.sequence){error="ModelChangeBatchMissing: bootstrap required";return false;}
    batch.finish();if(!store.exec("RELEASE model_change_page",{},error))return false;guard.active=false;result=std::move(page);return true;
}
bool DataService::forkModelGeneration(const ModelChangeHead& expected,ModelChangeHead& renewed,std::string& error) {
    renewed={};error.clear();auto& store=impl_->store;if(store.readOnly){error="Read-only model cannot fork generation";return false;}
    detail::Transaction transaction(store,error);if(!transaction.active)return false;ModelChangeHead head;
    if(!detail::readJournalHead(store,head,error))return false;
    if(!expected.available||!head.available||head.storeId!=expected.storeId||head.generation!=expected.generation||head.sequence!=expected.sequence){error="StaleModelGeneration: publication baseline changed";return false;}
    const auto generation=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!store.exec("DELETE FROM pds_model_items",{},error)||!store.exec("DELETE FROM pds_model_batches",{},error)||!store.exec("DELETE FROM pds_model_dirty",{},error)||
       !store.exec("UPDATE pds_model_state SET generation=?,sequence=0 WHERE singleton=1",{generation},error)||!transaction.commit(error))return false;
    return detail::readJournalHead(store,renewed,error);
}
}
