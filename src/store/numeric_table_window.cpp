#include "numeric_table_projection.h"
#include "bounded_metadata.h"
#include "project_store.h"
#include <QStringDecoder>
#include <QSqlError>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

namespace paleo::data::detail {
namespace {
bool identifier(const std::string& value) {
    if(value.empty()||value.size()>64||value[0]<'a'||value[0]>'z')return false;
    return std::all_of(value.begin(),value.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';});
}
bool sameCut(const NumericTableReadCut& a,const NumericTableReadCut& b) {
    return a.model.available&&b.model.available&&a.model.storeId==b.model.storeId&&a.model.generation==b.model.generation&&
        a.model.sequence==b.model.sequence&&a.tableId==b.tableId&&a.sourceId==b.sourceId&&
        a.sourceSha256==b.sourceSha256&&a.projectionRevision==b.projectionRevision;
}
bool parse(const QByteArray& raw,const NumericTableColumn& column,double& number,unsigned char& boolean,bool& null) {
    QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);
    const QString text=decoder(raw);Q_UNUSED(text);if(decoder.hasError()||raw.contains('\0'))return false;
    const std::string bytes=raw.toStdString();null=std::find(column.nullTexts.begin(),column.nullTexts.end(),bytes)!=column.nullTexts.end();
    if(null)return true;
    std::string_view value(bytes);const auto blank=[](char c){return c==' '||c=='\t'||c=='\r'||c=='\n';};
    while(!value.empty()&&blank(value.front()))value.remove_prefix(1);
    while(!value.empty()&&blank(value.back()))value.remove_suffix(1);
    if(column.dtype==NumericTableDtype::Boolean) {
        if(value!="0"&&value!="1")return false;boolean=value=="1";return true;
    }
    if(!value.empty()&&value.front()=='+') {
        value.remove_prefix(1);if(!value.empty()&&(value.front()=='+'||value.front()=='-'))return false;
    }
    if(value.empty())return false;
    const auto converted=std::from_chars(value.data(),value.data()+value.size(),number,std::chars_format::general);
    return converted.ec==std::errc()&&converted.ptr==value.data()+value.size()&&std::isfinite(number);
}
bool readColumn(const ProjectStore& store,std::int64_t tableId,std::int64_t first,std::int64_t count,
    NumericTableColumnValues& column,const NumericTableReadOptions& options,NumericTableReadStats& stats,std::string& error) {
    if(count==0)return true;
    const auto last=first+count-1;QSqlQuery rows;
    if(!store.exec("SELECT count(*),coalesce(max(octet_length(text)),0),coalesce(sum(octet_length(text)),0),coalesce(sum(typeof(text)<>'text'),0) FROM source_cells INDEXED BY numeric_table_column_window WHERE table_id=? AND col_num=? AND row_num>=? AND row_num<=?",
        {qlonglong(tableId),column.column.sourceColumn,qlonglong(first),qlonglong(last)},error,&rows)||!rows.next())return false;
    const auto cellCount=rows.value(0).toLongLong(),maximum=rows.value(1).toLongLong(),total=rows.value(2).toLongLong();
    if(cellCount>count||rows.value(3).toLongLong()!=0){error="NumericTableCellTypeInvalid";return false;}
    if(maximum>options.maxCellBytes){error="NumericTableCellBudgetExceeded";return false;}
    if(total<0||total>options.maxReadBytes-stats.metadataBytesRead-stats.cellTextBytesRead){error="NumericTableReadBudgetExceeded";return false;}
    rows.finish();
    if(!store.exec("SELECT row_num,typeof(text)='text' AND octet_length(text)<=?,CASE WHEN typeof(text)='text' AND octet_length(text)<=? THEN CAST(text AS BLOB) END FROM source_cells INDEXED BY numeric_table_column_window WHERE table_id=? AND col_num=? AND row_num>=? AND row_num<=? ORDER BY row_num",
        {qlonglong(options.maxCellBytes),qlonglong(options.maxCellBytes),qlonglong(tableId),column.column.sourceColumn,qlonglong(first),qlonglong(last)},error,&rows))return false;
    std::vector<unsigned char> seen(std::size_t(count),0);
    while(rows.next()) {
        if(numericTableStopped(options,error))return false;
        const auto ordinal=rows.value(0).toLongLong()-first;
        if(ordinal<0||ordinal>=count||seen[std::size_t(ordinal)]){error="NumericTableCellIdentityInvalid";return false;}
        if(!rows.value(1).toBool()){error="NumericTableCellBudgetExceeded";return false;}
        const auto raw=rows.value(2).toByteArray();
        if(raw.size()>options.maxCellBytes||raw.size()>options.maxReadBytes-stats.metadataBytesRead-stats.cellTextBytesRead) {
            error="NumericTableReadBudgetExceeded";return false;
        }
        stats.cellTextBytesRead+=raw.size();++stats.cellsRead;seen[std::size_t(ordinal)]=1;
        double number=0;unsigned char boolean=0;bool null=false;
        if(!parse(raw,column.column,number,boolean,null)){error="NumericTableCellTypeInvalid";return false;}
        column.nullMask[std::size_t(ordinal)]=null?1:0;
        if(!null) {
            if(column.column.dtype==NumericTableDtype::Float64)column.numbers[std::size_t(ordinal)]=number;
            else column.booleans[std::size_t(ordinal)]=boolean;
        }
    }
    if(rows.lastError().isValid()){error="NumericTableReadFailed: "+s(rows.lastError().text());return false;}
    if(!column.column.missingIsNull&&std::find(seen.begin(),seen.end(),0)!=seen.end()){error="NumericTableMissingCell";return false;}
    return true;
}
}
bool readNumericTable(const ProjectStore& store,const NumericTableWindowQuery& query,NumericTableWindow& result,
    const NumericTableReadOptions& options,NumericTableReadStats& stats,std::string& error) {
    if(!numericTableOptionsValid(options,error))return false;
    const auto& cut=query.expected;const auto& head=cut.model;
    if(!head.available||head.storeId.empty()||head.storeId.size()>128||head.generation.empty()||head.generation.size()>128||head.sequence<0||
       cut.tableId<=0||cut.sourceId<=0||cut.sourceSha256.size()!=64||cut.projectionRevision.size()!=36||query.firstRow<0||
       query.rowCount<0||query.rowCount>NumericTableMaxRows||query.columnIds.empty()||query.columnIds.size()>NumericTableMaxColumns) {
        error="NumericTableInvalidRequest";return false;
    }
    std::vector<std::string> names;
    for(const auto& id:query.columnIds) {
        if(!identifier(id)||std::find(names.begin(),names.end(),id)!=names.end()){error="NumericTableInvalidColumn";return false;}names.push_back(id);
    }
    if(numericTableStopped(options,error))return false;
    MetadataReadSnapshot snapshot(store,error);if(!snapshot.active())return false;
    NumericTableWindow pending;if(!loadNumericTableDescriptor(store,cut.tableId,pending.descriptor,options,stats,error))return false;
    if(!sameCut(cut,pending.descriptor.cut)){error="NumericTableStaleReadCut";return false;}
    const auto& projection=pending.descriptor.projection;
    if(query.firstRow>projection.rowCount||query.rowCount>projection.rowCount-query.firstRow){error="NumericTableWindowOutsideProjection";return false;}
    std::int64_t bytes=query.rowCount*8;
    for(const auto& id:query.columnIds) {
        const auto column=std::find_if(projection.columns.begin(),projection.columns.end(),[&](const auto& value){return value.id==id;});
        if(column==projection.columns.end()){error="NumericTableInvalidColumn";return false;}
        bytes+=query.rowCount*(column->dtype==NumericTableDtype::Float64?9:2);NumericTableColumnValues output;output.column=*column;pending.columns.push_back(std::move(output));
    }
    const auto reserved=bytes+query.rowCount+options.maxCellBytes*4+options.metadata.maxTextBytes*8;
    if(bytes>options.maxOutputBytes||reserved>options.maxWorkingBytes){error="NumericTableOutputBudgetExceeded";return false;}
    stats.reservedBytes=reserved;pending.firstRow=query.firstRow;pending.sourceRows.reserve(std::size_t(query.rowCount));
    const auto first=projection.firstSourceRow+query.firstRow;
    for(std::int64_t i=0;i<query.rowCount;++i)pending.sourceRows.push_back(first+i);
    for(auto& column:pending.columns) {
        if(numericTableStopped(options,error))return false;
        column.nullMask.assign(std::size_t(query.rowCount),1);
        if(column.column.dtype==NumericTableDtype::Float64)column.numbers.assign(std::size_t(query.rowCount),0);
        else column.booleans.assign(std::size_t(query.rowCount),0);
        if(!readColumn(store,projection.tableId,first,query.rowCount,column,options,stats,error))return false;
    }
    if(numericTableStopped(options,error)||!snapshot.finish(error))return false;
    stats.outputBytes=bytes;stats.rowsReturned=query.rowCount;result=std::move(pending);return true;
}
}
