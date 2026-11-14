#include "store/project_store.h"
#include "store/bounded_metadata.h"
#include <array>

namespace paleo::data {
namespace {
const QStringList Fields{"original_path","stored_path","kind","status","message","hash"};
QString guardedSourceQuery(const SourceIndexReadOptions &options) {
    const auto allowed=detail::metadataGuard(Fields,options)+" AND typeof(bytes)='integer' AND typeof(active)='integer'";
    return "SELECT "+allowed+",id,CASE WHEN "+allowed+" THEN bytes END,CASE WHEN "+allowed+" THEN active END,"
        +detail::guardedMetadataColumns(Fields,allowed)+" FROM sources WHERE id=?";
}
}
bool DataService::readSourceIndexEntry(long long id,SourceIndexEntry &out,std::string &error,
    const SourceIndexReadOptions &options,SourceIndexReadStats *stats) const {
    out={};error.clear();if(stats)*stats={};
    if(id<=0||!detail::metadataBudgetValid(options)){error="SourceIndexValueBudgetInvalid";return false;}
    if(!detail::metadataBudgetAvailable(impl_->store,error,"SourceIndexValueBudget"))return false;
    QSqlQuery query;if(!impl_->store.exec(guardedSourceQuery(options),{id},error,&query))return false;
    if(!query.next()){error="SourceIndexValueNotFound";return false;}
    if(!query.value(0).toBool()){error="SourceIndexValueBudgetExceeded";return false;}
    QVector<QByteArray> text;long long total=0;
    if(!detail::metadataTextValues(query,4,Fields,options,text,total,error,"SourceIndexValue"))return false;
    SourceIndexEntry result;result.id=query.value(1).toLongLong();result.bytes=query.value(2).toLongLong();result.active=query.value(3).toBool();
    const std::array<std::string*,6> fields{&result.path,&result.storedPath,&result.kind,&result.status,&result.message,&result.sha256};
    for(size_t i=0;i<fields.size();++i)fields[i]->assign(text[int(i)].constData(),size_t(text[int(i)].size()));
    out=std::move(result);if(stats)stats->textBytesRead=total;return true;
}
}
