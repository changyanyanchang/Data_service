#include "store/project_store.h"
#include "store/bounded_metadata.h"
#include "store/well_data_query_index.h"
#include <QSqlError>

namespace paleo::data {
namespace {
QString idPage(const std::string &kind,bool filterWell) {
    const auto filter=filterWell?QString("well_id=? AND "):QString();
    if(kind=="curves")return "SELECT curve_id FROM pds_well_query_curves "+QString(filterWell?"INDEXED BY pds_well_query_curve_well ":"")+
        "WHERE "+filter+"curve_id>? ORDER BY curve_id LIMIT ?";
    if(kind=="intervals")return "SELECT track_id FROM pds_well_query_tracks "+QString(filterWell?"INDEXED BY pds_well_query_track_well ":"")+
        "WHERE "+filter+"track_id>? ORDER BY track_id LIMIT ?";
    return {};
}
QString rowQuery(const std::string &kind,const MetadataReadOptions &options,QStringList &fields) {
    const bool curves=kind=="curves";
    fields=curves?QStringList{"c.name","c.unit","l.name"}:QStringList{"t.name","t.type"};
    auto budget=options;if(curves)budget.maxTextBytes-=3;
    auto guard=detail::metadataGuard(fields,budget);
    if(curves)guard+=" AND (coalesce(octet_length(c.unit),0)+coalesce(octet_length(l.name),0)+3)<="+QString::number(options.maxFieldBytes);
    const auto numeric=curves?QStringList{"l.well_id","l.source_id","l.id","l.sample_count"}:QStringList{"t.well_id","t.source_id","0","i.record_count"};
    for(const auto &column:numeric)guard+=" AND typeof("+column+") IN ('integer','null')";
    QStringList selected{guard};for(const auto &column:numeric)selected.append("CASE WHEN "+guard+" THEN "+column+" END");
    selected.append(detail::guardedMetadataColumns(fields,guard));
    return "SELECT "+selected.join(',')+(curves?" FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE c.id=?":
        " FROM tracks t JOIN pds_well_query_tracks i ON i.track_id=t.id WHERE t.id=?");
}
}
bool DataService::listDataPage(const std::string &kind,long long wellId,long long afterId,int limit,
    std::vector<DataEntry> &out,bool &hasMore,std::string &error,const MetadataReadOptions &options,MetadataReadStats *stats) const {
    out.clear();hasMore=false;error.clear();if(stats)*stats={};
    if(wellId<0||afterId<0||limit<1||limit>512||!detail::metadataBudgetValid(options)){error="InvalidDataPageBudget";return false;}
    const auto idsSql=idPage(kind,wellId>0);if(idsSql.isEmpty()){error="UnsupportedDataPageKind";return false;}
    if(!detail::metadataBudgetAvailable(impl_->store,error,"DataPageMetadata")||!detail::wellDataQueryIndexReady(impl_->store,error))return false;
    detail::MetadataReadSnapshot snapshot(impl_->store,error);if(!snapshot.active())return false;
    QVariantList arguments;if(wellId>0)arguments<<wellId;arguments<<afterId<<limit+1;
    QSqlQuery ids;if(!impl_->store.exec(idsSql,arguments,error,&ids))return false;
    std::vector<long long> selected;selected.reserve(size_t(limit+1));while(ids.next())selected.push_back(ids.value(0).toLongLong());
    if(ids.lastError().isValid()){error=detail::s(ids.lastError().text());return false;}ids.finish();
    const bool more=selected.size()>size_t(limit);if(more)selected.resize(size_t(limit));
    std::vector<DataEntry> pending;pending.reserve(selected.size());long long total=0;
    for(const auto id:selected) {
        auto rowBudget=options;rowBudget.maxTextBytes-=total;
        if(rowBudget.maxTextBytes<(kind=="curves"?3:1)){error="DataPageValueBudgetExceeded";return false;}
        QStringList fields;const auto sql=rowQuery(kind,rowBudget,fields);QSqlQuery row;
        if(!impl_->store.exec(sql,{id},error,&row)||!row.next())return false;
        if(!row.value(0).toBool()){error="DataPageValueBudgetExceeded";return false;}
        QVector<QByteArray> text;long long bytes=0;
        if(!detail::metadataTextValues(row,5,fields,rowBudget,text,bytes,error,"DataPageValue"))return false;
        DataEntry value;value.id=id;value.wellId=row.value(1).toLongLong();value.sourceId=row.value(2).toLongLong();
        value.parentId=row.value(3).toLongLong();value.count=row.value(4).toLongLong();value.kind=kind;
        value.name=text[0].toStdString();
        value.detail=kind=="curves"?(text[1]+" / "+text[2]).toStdString():text[1].toStdString();
        if(kind=="curves")bytes+=3;
        if(value.name.size()>size_t(rowBudget.maxFieldBytes)||value.detail.size()>size_t(rowBudget.maxFieldBytes)||bytes>rowBudget.maxTextBytes) {
            error="DataPageValueBudgetExceeded";return false;
        }
        total+=bytes;pending.push_back(std::move(value));
    }
    if(!snapshot.finish(error))return false;out=std::move(pending);hasMore=more;if(stats)stats->textBytesRead=total;return true;
}
}
