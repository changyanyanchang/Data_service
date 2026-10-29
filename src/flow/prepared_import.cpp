#include "store/project_store.h"
#include "file_digest.h"
#include <QFileInfo>
#include <QSqlQuery>

namespace paleo::data {
using namespace detail;
bool DataService::importPreparedFile(const std::string& readablePath,
    const std::string& identityPath,const std::string& storedReference,
    const std::string& expectedSha256,const ParsedFile& model,long long& sourceId,
    std::string& error,const CancelCheck& cancelled) {
    sourceId=0; error.clear(); auto& store=impl_->store;
    if(store.readOnly||!store.db.isOpen()) { error="prepared import requires a writable data project"; return false; }
    if(identityPath.empty()||storedReference.empty()||expectedSha256.size()!=64||
       (model.status!="parsed"&&model.status!="review"&&model.status!="registered")) {
        error="invalid prepared source identity or status"; return false;
    }
    if(cancelled&&cancelled()) { error="cancelled"; return false; }
    QString digest;
    if(!fullFileSha256(q(readablePath),digest,error,cancelled))return false;
    if(digest.compare(q(expectedSha256),Qt::CaseInsensitive)!=0) {
        error="prepared model does not match source SHA-256"; return false;
    }
    Transaction transaction(store,error); if(!transaction.active)return false;
    QSqlQuery source;
    if(!store.exec("INSERT INTO sources(original_path,stored_path,hash,options,kind,status,message,bytes,document) VALUES(?,?,?,'{\"host_prepared\":1}',?,?,?,?,?)",
        {q(identityPath),q(storedReference),digest,q(model.kind),q(model.status),q(model.message),
         QFileInfo(q(readablePath)).size(),q(model.documentText)},error,&source))return false;
    const auto id=source.lastInsertId().toLongLong();
    // Only this trusted host API bypasses guessed null tokens. Keep the public
    // ParseOptions layout and ordinary file-import defaults unchanged.
    if(!store.saveParsed(id,model,{},error,cancelled,true))return false;
    // Storage can discover conflicts with existing wells even when the host's
    // model was valid on its own. Do not publish a partially accepted model.
    QSqlQuery issues;
    if(!store.exec("SELECT code,message,blocking FROM issues WHERE source_id=? AND resolved=0 ORDER BY id",
        {id},error,&issues))return false;
    bool needsReview=false;
    while(issues.next()) {
        if(issues.value(2).toBool()) {
            error="prepared import blocked by "+s(issues.value(0).toString())+": "+s(issues.value(1).toString());
            return false; // Transaction rolls back this source, its issues and all objects.
        }
        needsReview=true;
    }
    issues.finish();
    if(needsReview&&!store.exec("UPDATE sources SET status='review' WHERE id=?",{id},error))return false;
    // Recheck the bytes before publication; failure rolls back objects and source.
    if(!fullFileSha256(q(readablePath),digest,error,cancelled))return false;
    if(digest.compare(q(expectedSha256),Qt::CaseInsensitive)!=0) {
        error="source changed while storing the prepared model"; return false;
    }
    if(cancelled&&cancelled()) { error="cancelled"; return false; }
    if(!transaction.commit(error))return false;
    sourceId=id; return true;
}
}
