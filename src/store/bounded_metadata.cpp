#include "bounded_metadata.h"
#include "project_store.h"
#include <QStringDecoder>
#include <atomic>

namespace paleo::data::detail {
bool metadataBudgetValid(const MetadataReadOptions &options) {
    return options.maxFieldBytes>0&&options.maxFieldBytes<=64*1024&&options.maxTextBytes>0&&options.maxTextBytes<=256*1024;
}
bool metadataBudgetAvailable(const ProjectStore &store,std::string &error,const char *prefix) {
    QSqlQuery query;
    if(!store.exec("SELECT octet_length('')",{},error,&query)||!query.next()) {
        error=std::string(prefix)+"Unavailable: SQLite octet_length is required";return false;
    }
    query.finish();
    if(!store.exec("PRAGMA encoding",{},error,&query)||!query.next()||query.value(0).toString()!="UTF-8") {
        error=std::string(prefix)+"Unavailable: UTF-8 database encoding is required";return false;
    }
    return true;
}
QString metadataGuard(const QStringList &columns,const MetadataReadOptions &options) {
    QStringList conditions,lengths;
    for(const auto &column:columns) {
        const auto length="coalesce(octet_length("+column+"),0)";lengths.append(length);
        conditions.append("typeof("+column+") IN ('text','null')");
        conditions.append(length+"<="+QString::number(options.maxFieldBytes));
    }
    conditions.append('('+lengths.join('+')+")<="+QString::number(options.maxTextBytes));
    return '('+conditions.join(" AND ")+')';
}
QString guardedMetadataColumns(const QStringList &columns,const QString &guard) {
    QStringList result;
    for(const auto &column:columns)result.append("CASE WHEN "+guard+" THEN CAST("+column+" AS BLOB) END");
    return result.join(',');
}
bool metadataTextValues(const QSqlQuery &query,int first,const QStringList &columns,
    const MetadataReadOptions &options,QVector<QByteArray> &values,long long &bytes,std::string &error,const char *prefix) {
    values.clear();bytes=0;QVector<QByteArray> pending;
    for(int i=0;i<columns.size();++i) {
        const auto value=query.value(first+i).toByteArray();
        if(value.size()>options.maxFieldBytes||value.size()>options.maxTextBytes-bytes){error=std::string(prefix)+"BudgetExceeded";return false;}
        QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);
        const QString decoded=decoder(value);Q_UNUSED(decoded);
        if(decoder.hasError()){error=std::string(prefix)+"EncodingInvalid";return false;}
        pending.append(value);bytes+=value.size();
    }
    values=std::move(pending);return true;
}
MetadataReadSnapshot::MetadataReadSnapshot(const ProjectStore &store,std::string &error):store_(store) {
    static std::atomic<unsigned long long> sequence{0};name_="bounded_metadata_"+QString::number(++sequence);
    active_=store_.exec("SAVEPOINT "+name_,{},error);
}
MetadataReadSnapshot::~MetadataReadSnapshot() {
    if(active_){std::string ignored;store_.exec("ROLLBACK TO "+name_,{},ignored);store_.exec("RELEASE "+name_,{},ignored);}
}
bool MetadataReadSnapshot::finish(std::string &error) {
    if(!store_.exec("RELEASE "+name_,{},error))return false;active_=false;return true;
}
}
