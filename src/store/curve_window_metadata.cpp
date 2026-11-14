#include "curve_window_metadata.h"
#include "bounded_metadata.h"
#include <algorithm>
#include <limits>

namespace paleo::data::detail {
bool boundedCurveMetadata(ProjectStore &store,long long id,const CurveReadOptions &options,
    CurveReadStats &stats,CurveWindowMetadata &metadata,QStringList &text,std::string &error) {
    text.clear();MetadataReadOptions budget;budget.maxFieldBytes=options.maxMetadataFieldBytes;
    budget.maxTextBytes=std::min(options.maxMetadataBytes,options.maxReadBytes);
    if(budget.maxTextBytes<1){error="CurveWindowBudgetExceeded: metadata exceeds total read budget";return false;}
    if(!metadataBudgetValid(budget)){error="CurveWindowBudgetInvalid: metadata limits";return false;}
    if(!metadataBudgetAvailable(store,error,"CurveWindowMetadata"))return false;
    const QStringList fields{"c.name","c.unit","l.depth_json","s.kind","s.options","s.original_path","l.encoding"};
    auto guard=metadataGuard(fields,budget)+" AND typeof(l.sample_count)='integer'";
    for(const auto &column:QStringList{"l.depths","l.tvd","l.tvdss","c.raw_values","c.processed_values","c.quality"})
        guard+=" AND typeof("+column+") IN ('blob','null')";
    const QStringList numeric{"l.id","l.sample_count","coalesce(octet_length(l.depths),0)","coalesce(octet_length(l.tvd),0)",
        "coalesce(octet_length(l.tvdss),0)","coalesce(octet_length(c.raw_values),0)","coalesce(octet_length(c.processed_values),0)",
        "coalesce(octet_length(c.quality),0)","c.processed_values IS NOT NULL"};
    QStringList selected{guard};for(const auto &column:numeric)selected.append("CASE WHEN "+guard+" THEN "+column+" END");
    selected.append(guardedMetadataColumns(fields,guard));QSqlQuery query;
    if(!store.exec("SELECT "+selected.join(',')+" FROM curves c JOIN log_sets l ON l.id=c.set_id LEFT JOIN sources s ON s.id=l.source_id WHERE c.id=?",{id},error,&query))return false;
    if(!query.next()){error="curve does not exist";return false;}
    if(!query.value(0).toBool()){error="CurveWindowBudgetExceeded: metadata text or type exceeds limits";return false;}
    CurveWindowMetadata next;next.setId=query.value(1).toLongLong();next.count=query.value(2).toLongLong();
    next.depthBytes=query.value(3).toLongLong();next.tvdBytes=query.value(4).toLongLong();next.ssBytes=query.value(5).toLongLong();
    next.rawBytes=query.value(6).toLongLong();next.processedBytes=query.value(7).toLongLong();next.qualityBytes=query.value(8).toLongLong();next.hasProcessed=query.value(9).toBool();
    QVector<QByteArray> bytes;long long total=0;if(!metadataTextValues(query,10,fields,budget,bytes,total,error,"CurveWindow"))return false;
    stats.metadataBytesRead+=total;
    if(bytes[6]!="float64-le"){error="CurveWindowUnsupportedEncoding";return false;}
    if(next.count<0||next.count>std::numeric_limits<long long>::max()/8||next.depthBytes!=next.count*8||
        next.rawBytes!=next.count*8||next.qualityBytes!=next.count||(next.hasProcessed&&next.processedBytes!=next.count*8)||
        (next.tvdBytes&&next.tvdBytes!=next.count*8)||(next.ssBytes&&next.ssBytes!=next.count*8)) {
        error="curve array lengths are corrupt";return false;
    }
    for(int i=0;i<6;++i)text.append(QString::fromUtf8(bytes[i]));metadata=next;return true;
}
}
