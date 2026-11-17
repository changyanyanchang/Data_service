#include "numeric_table_projection.h"
#include "bounded_metadata.h"
#include "model_change_journal.h"
#include "project_store.h"
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QStringDecoder>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace paleo::data::detail {
namespace {
const QStringList triggerNames{"pds_numeric_cells_i","pds_numeric_cells_u","pds_numeric_cells_d",
    "pds_numeric_table_u","pds_numeric_columns_i","pds_numeric_columns_u","pds_numeric_columns_d"};
bool utf8(const std::string& text,std::size_t maximum,bool empty=false) {
    if((!empty&&text.empty())||text.size()>maximum||text.find('\0')!=std::string::npos)return false;
    QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);
    const QString value=decoder(QByteArrayView(text.data(),qsizetype(text.size())));Q_UNUSED(value);return !decoder.hasError();
}
bool hash(const std::string& text) {
    return text.size()==64&&std::all_of(text.begin(),text.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
bool identifier(const std::string& text) {
    if(text.empty()||text.size()>64||text=="source_row"||text[0]<'a'||text[0]>'z')return false;
    return std::all_of(text.begin(),text.end(),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';});
}
bool revisionValid(const std::string& text) {
    return utf8(text,36)&&text.size()==36&&s(QUuid(q(text)).toString(QUuid::WithoutBraces))==text;
}
QByteArray encodedNulls(const NumericTableColumn& column) {
    QJsonArray values;for(const auto& text:column.nullTexts)values.append(q(text));
    return QJsonDocument(values).toJson(QJsonDocument::Compact);
}
bool declarationValid(const NumericTableProjection& p,std::string& error) {
    if(p.tableId<=0||p.sourceId<=0||!hash(p.sourceSha256)||p.firstSourceRow<1||p.rowCount<0||
       p.rowCount>std::numeric_limits<std::int64_t>::max()-p.firstSourceRow||p.columns.empty()||p.columns.size()>NumericTableMaxColumns) {
        error="NumericTableInvalidDeclaration";return false;
    }
    QSet<QString> names;QSet<int> positions;
    std::size_t metadata=p.sourceSha256.size()+36+sizeof(NumericTableContract)-1;
    for(const auto& column:p.columns) {
        if(!identifier(column.id)||!utf8(column.label,256,true)||!utf8(column.unit,128)||column.sourceColumn<1||
           (column.dtype!=NumericTableDtype::Float64&&column.dtype!=NumericTableDtype::Boolean)||
           (column.dtype==NumericTableDtype::Boolean&&column.unit!="1")||column.nullTexts.size()>8||
           names.contains(q(column.id))||positions.contains(column.sourceColumn)) {error="NumericTableInvalidColumn";return false;}
        const auto unit=q(column.unit).trimmed().toCaseFolded();
        if(unit.isEmpty()||QStringList{"unknown","unspecified","undefined","?","n/a"}.contains(unit)) {error="NumericTableUnitRequired";return false;}
        names.insert(q(column.id));positions.insert(column.sourceColumn);metadata+=column.id.size()+column.label.size()+column.unit.size()+128;
        QSet<QString> nulls;std::size_t nullBudget=0;
        for(const auto& text:column.nullTexts) {
            if(!utf8(text,64,true)||nulls.contains(q(text))){error="NumericTableInvalidNullRule";return false;}
            nulls.insert(q(text));nullBudget+=text.size()+8;
        }
        // Include persisted JSON escaping without relaxing the original conservative allowance.
        metadata+=std::max(nullBudget,std::size_t(encodedNulls(column).size()));
    }
    if(metadata>16*1024){error="NumericTableMetadataBudgetExceeded";return false;}return true;
}
bool sameHead(const ModelChangeHead& a,const ModelChangeHead& b) {
    return a.available&&b.available&&a.storeId==b.storeId&&a.generation==b.generation&&a.sequence==b.sequence;
}
struct SourceShape {std::int64_t source=0,rows=0,columns=0;std::string sha;};
bool sourceShape(const ProjectStore& store,std::int64_t id,SourceShape& result,std::string& error) {
    QSqlQuery row;
    if(!store.exec("SELECT t.source_id,t.rows,t.columns,CASE WHEN typeof(s.hash)='text' AND octet_length(s.hash)=64 THEN CAST(s.hash AS BLOB) END,s.active,typeof(t.rows),typeof(t.columns) FROM source_tables t JOIN sources s ON s.id=t.source_id WHERE t.id=?",
        {qlonglong(id)},error,&row))return false;
    if(!row.next()){error="NumericTableNotFound";return false;}
    result={row.value(0).toLongLong(),row.value(1).toLongLong(),row.value(2).toLongLong(),row.value(3).toByteArray().toStdString()};
    if(!row.value(4).toBool()||row.value(5).toString()!="integer"||row.value(6).toString()!="integer"||
       result.rows<0||result.columns<0||!hash(result.sha)){error="NumericTableSourceUnavailable";return false;}return true;
}
}
bool numericTableOptionsValid(const NumericTableReadOptions& options,std::string& error) {
    if(!metadataBudgetValid(options.metadata)||options.metadata.maxTextBytes>64*1024||options.metadata.maxFieldBytes>8192||
       options.maxReadBytes<1||options.maxReadBytes>8*1024*1024||options.maxOutputBytes<1||options.maxOutputBytes>2*1024*1024||
       options.maxWorkingBytes<1||options.maxWorkingBytes>8*1024*1024||options.maxCellBytes<1||options.maxCellBytes>4096) {
        error="NumericTableInvalidBudget";return false;
    }return true;
}
bool numericTableStopped(const NumericTableReadOptions& options,std::string& error) {
    if(options.cancelled&&options.cancelled()){error="NumericTableCancelled";return true;}return false;
}
bool numericTableSchemaReady(const ProjectStore& store,bool& ready,std::string& error) {
    ready=false;QSqlQuery row;
    if(!store.exec("SELECT CASE WHEN typeof(value)='text' AND octet_length(value)<=16 THEN value END FROM ds_meta WHERE key='numeric_table_schema'",{},error,&row))return false;
    if(!row.next())return true;
    if(row.value(0).toString()!="1"){error="NumericTableUnsupportedSchema";return false;}row.finish();
    QStringList names=triggerNames;names<<"numeric_table_projections"<<"numeric_table_columns"<<"numeric_table_column_window";
    QStringList marks;QVariantList values;for(const auto& name:names){marks.append("?");values.append(name);}
    if(!store.exec("SELECT count(*) FROM sqlite_master WHERE name IN ("+marks.join(',')+")",values,error,&row)||!row.next())return false;
    ready=row.value(0).toInt()==names.size();row.finish();if(!ready)return true;
    if(!store.exec("PRAGMA index_info(numeric_table_column_window)",{},error,&row))return false;
    QStringList columns;while(row.next())columns.append(row.value(2).toString());
    if(row.lastError().isValid()){error="NumericTableReadFailed: "+s(row.lastError().text());return false;}
    ready=columns==QStringList{"table_id","col_num","row_num"};return true;
}
bool ensureNumericTableSchema(ProjectStore& store,std::string& error,const std::function<bool()>& cancelled) {
    error.clear();if(store.readOnly){error="NumericTableReadOnly";return false;}
    NumericTableReadOptions options;options.cancelled=cancelled;if(numericTableStopped(options,error))return false;
    bool ready=false;if(!numericTableSchemaReady(store,ready,error))return false;if(ready)return true;
    Transaction transaction(store,error);if(!transaction.active)return false;
    if(!numericTableSchemaReady(store,ready,error))return false;if(ready)return transaction.commit(error);
    const QStringList ddl{
        "CREATE TABLE IF NOT EXISTS numeric_table_projections(table_id INTEGER PRIMARY KEY REFERENCES source_tables(id) ON DELETE CASCADE,source_id INTEGER NOT NULL REFERENCES sources(id),source_sha256 TEXT NOT NULL,revision TEXT NOT NULL UNIQUE,contract TEXT NOT NULL,first_row INTEGER NOT NULL,row_count INTEGER NOT NULL,base_rows INTEGER NOT NULL,base_columns INTEGER NOT NULL,column_count INTEGER NOT NULL,stale INTEGER NOT NULL CHECK(stale IN(0,1)))",
        "CREATE TABLE IF NOT EXISTS numeric_table_columns(table_id INTEGER NOT NULL REFERENCES numeric_table_projections(table_id) ON DELETE CASCADE,ordinal INTEGER NOT NULL,id TEXT NOT NULL,source_column INTEGER NOT NULL,dtype INTEGER NOT NULL CHECK(dtype IN(0,1)),label TEXT NOT NULL,unit TEXT NOT NULL,missing_is_null INTEGER NOT NULL CHECK(missing_is_null IN(0,1)),nulls_json TEXT NOT NULL,PRIMARY KEY(table_id,ordinal),UNIQUE(table_id,id),UNIQUE(table_id,source_column))",
        "DROP INDEX IF EXISTS numeric_table_column_window",
        "CREATE INDEX IF NOT EXISTS numeric_table_column_window ON source_cells(table_id,col_num,row_num)",
        "CREATE TRIGGER IF NOT EXISTS pds_numeric_cells_i AFTER INSERT ON source_cells BEGIN UPDATE numeric_table_projections SET stale=1 WHERE table_id=NEW.table_id; END",
        "CREATE TRIGGER IF NOT EXISTS pds_numeric_cells_u AFTER UPDATE ON source_cells WHEN OLD.table_id IS NOT NEW.table_id OR OLD.row_num IS NOT NEW.row_num OR OLD.col_num IS NOT NEW.col_num OR OLD.text IS NOT NEW.text BEGIN UPDATE numeric_table_projections SET stale=1 WHERE table_id IN(OLD.table_id,NEW.table_id); END",
        "CREATE TRIGGER IF NOT EXISTS pds_numeric_cells_d AFTER DELETE ON source_cells BEGIN UPDATE numeric_table_projections SET stale=1 WHERE table_id=OLD.table_id; END",
        "CREATE TRIGGER IF NOT EXISTS pds_numeric_table_u AFTER UPDATE ON source_tables WHEN OLD.id IS NOT NEW.id OR OLD.source_id IS NOT NEW.source_id OR OLD.rows IS NOT NEW.rows OR OLD.columns IS NOT NEW.columns BEGIN UPDATE numeric_table_projections SET stale=1 WHERE table_id IN(OLD.id,NEW.id); END",
        "CREATE TRIGGER IF NOT EXISTS pds_numeric_columns_i AFTER INSERT ON numeric_table_columns BEGIN UPDATE numeric_table_projections SET stale=1 WHERE table_id=NEW.table_id; END",
        "CREATE TRIGGER IF NOT EXISTS pds_numeric_columns_u AFTER UPDATE ON numeric_table_columns BEGIN UPDATE numeric_table_projections SET stale=1 WHERE table_id IN(OLD.table_id,NEW.table_id); END",
        "CREATE TRIGGER IF NOT EXISTS pds_numeric_columns_d AFTER DELETE ON numeric_table_columns BEGIN UPDATE numeric_table_projections SET stale=1 WHERE table_id=OLD.table_id; END",
        "UPDATE numeric_table_projections SET stale=1",
        "INSERT OR REPLACE INTO ds_meta(key,value) VALUES('numeric_table_schema','1')"};
    for(const auto& sql:ddl)if(numericTableStopped(options,error)||!store.exec(sql,{},error))return false;
    if(numericTableStopped(options,error))return false;return transaction.commit(error);
}
bool loadNumericTableDescriptor(const ProjectStore& store,std::int64_t id,NumericTableDescriptor& result,
    const NumericTableReadOptions& options,NumericTableReadStats& stats,std::string& error) {
    bool ready=false;if(!numericTableSchemaReady(store,ready,error))return false;
    if(!ready){error="NumericTableSchemaUnavailable";return false;}
    if(!metadataBudgetAvailable(store,error,"NumericTableMetadata"))return false;
    ModelChangeHead head;if(!readJournalHead(store,head,error))return false;
    if(!head.available){error="NumericTableReadCutUnavailable";return false;}
    auto metadata=options.metadata;metadata.maxTextBytes=std::min<std::int64_t>(metadata.maxTextBytes,options.maxReadBytes);
    const QStringList fields{"revision","source_sha256"};const auto guard=metadataGuard(fields,metadata);
    QSqlQuery row;
    if(!store.exec("SELECT source_id,first_row,row_count,base_rows,base_columns,column_count,stale,contract="+QString("'numeric_table.v1'")+","+guard+","+guardedMetadataColumns(fields,guard)+" FROM numeric_table_projections WHERE table_id=?",{qlonglong(id)},error,&row))return false;
    if(!row.next()){error="NumericTableProjectionUnavailable";return false;}
    if(!row.value(8).toBool()){error="NumericTableMetadataBudgetExceeded";return false;}
    QVector<QByteArray> texts;long long bytes=0;if(!metadataTextValues(row,9,fields,metadata,texts,bytes,error,"NumericTableMetadata"))return false;
    stats.metadataBytesRead+=bytes;
    if(row.value(6).toBool()){error="NumericTableSourceChanged";return false;}
    if(!row.value(7).toBool()){error="NumericTableUnsupportedContract";return false;}
    NumericTableDescriptor pending;auto& p=pending.projection;p.tableId=id;p.sourceId=row.value(0).toLongLong();p.firstSourceRow=row.value(1).toLongLong();p.rowCount=row.value(2).toLongLong();
    const auto baseRows=row.value(3).toLongLong(),baseColumns=row.value(4).toLongLong(),count=row.value(5).toLongLong();
    pending.revision=texts[0].toStdString();p.sourceSha256=texts[1].toStdString();row.finish();
    if(count<1||count>NumericTableMaxColumns||!revisionValid(pending.revision)){error="NumericTableProjectionCorrupt";return false;}
    if(stats.metadataBytesRead>metadata.maxTextBytes-64){error="NumericTableMetadataBudgetExceeded";return false;}
    SourceShape shape;if(!sourceShape(store,id,shape,error))return false;stats.metadataBytesRead+=shape.sha.size();
    if(shape.source!=p.sourceId||shape.sha!=p.sourceSha256||shape.rows!=baseRows||shape.columns!=baseColumns){error="NumericTableSourceChanged";return false;}
    if(!store.exec("SELECT ordinal FROM numeric_table_columns WHERE table_id=? ORDER BY ordinal LIMIT 16",{qlonglong(id)},error,&row))return false;
    int observed=0;while(row.next())if(row.value(0).toInt()!=observed++ || observed>count){error="NumericTableProjectionCorrupt";return false;}
    if(row.lastError().isValid()){error="NumericTableReadFailed: "+s(row.lastError().text());return false;}
    row.finish();if(observed!=count){error="NumericTableProjectionCorrupt";return false;}
    const QStringList columns{"id","label","unit","nulls_json"};
    for(int ordinal=0;ordinal<count;++ordinal) {
        if(numericTableStopped(options,error))return false;
        auto budget=metadata;budget.maxTextBytes=metadata.maxTextBytes-stats.metadataBytesRead;
        if(budget.maxTextBytes<1){error="NumericTableMetadataBudgetExceeded";return false;}
        const auto columnGuard=metadataGuard(columns,budget);
        if(!store.exec("SELECT ordinal,source_column,dtype,missing_is_null,"+columnGuard+","+guardedMetadataColumns(columns,columnGuard)+" FROM numeric_table_columns WHERE table_id=? AND ordinal=?",{qlonglong(id),ordinal},error,&row)||!row.next())return false;
        if(!row.value(4).toBool()){error="NumericTableMetadataBudgetExceeded";return false;}
        if(!metadataTextValues(row,5,columns,budget,texts,bytes,error,"NumericTableMetadata"))return false;
        stats.metadataBytesRead+=bytes;
        NumericTableColumn column;column.sourceColumn=row.value(1).toInt();column.dtype=NumericTableDtype(row.value(2).toInt());column.missingIsNull=row.value(3).toBool();
        column.id=texts[0].toStdString();column.label=texts[1].toStdString();column.unit=texts[2].toStdString();
        const auto nulls=QJsonDocument::fromJson(texts[3]);if(!nulls.isArray()||nulls.array().size()>8){error="NumericTableProjectionCorrupt";return false;}
        for(const auto& value:nulls.array()) {
            if(!value.isString()||QString::fromUtf8(value.toString().toUtf8())!=value.toString()){error="NumericTableProjectionCorrupt";return false;}
            column.nullTexts.push_back(s(value.toString()));
        }
        p.columns.push_back(std::move(column));row.finish();
    }
    if(p.columns.size()!=static_cast<std::size_t>(count)){error="NumericTableProjectionCorrupt";return false;}
    if(!declarationValid(p,error))return false;
    if(p.firstSourceRow-1>shape.rows||p.rowCount>shape.rows-(p.firstSourceRow-1)){error="NumericTableSourceChanged";return false;}
    for(const auto& column:p.columns)if(column.sourceColumn>shape.columns){error="NumericTableProjectionCorrupt";return false;}
    if(stats.metadataBytesRead>options.maxReadBytes){error="NumericTableReadBudgetExceeded";return false;}
    pending.cut={head,p.tableId,p.sourceId,p.sourceSha256,pending.revision};result=std::move(pending);return true;
}
bool confirmNumericTable(ProjectStore& store,const NumericTableProjection& declaration,const ModelChangeHead& expected,
    const std::string& previous,std::string& revision,std::string& error,const std::function<bool()>& cancelled) {
    revision.clear();error.clear();if(store.readOnly){error="NumericTableReadOnly";return false;}
    if(!previous.empty()&&!revisionValid(previous)){error="NumericTableInvalidRevision";return false;}
    if(!declarationValid(declaration,error))return false;
    NumericTableReadOptions options;options.cancelled=cancelled;if(numericTableStopped(options,error))return false;
    bool ready=false;if(!numericTableSchemaReady(store,ready,error))return false;if(!ready){error="NumericTableSchemaUnavailable";return false;}
    if(!metadataBudgetAvailable(store,error,"NumericTableMetadata"))return false;
    Transaction transaction(store,error);if(!transaction.active)return false;
    ModelChangeHead head;if(!readJournalHead(store,head,error))return false;
    if(!sameHead(head,expected)){error="NumericTableStaleReadCut";return false;}
    SourceShape shape;if(!sourceShape(store,declaration.tableId,shape,error))return false;
    if(shape.source!=declaration.sourceId||shape.sha!=declaration.sourceSha256||declaration.firstSourceRow-1>shape.rows||
       declaration.rowCount>shape.rows-(declaration.firstSourceRow-1)){error="NumericTableSourceChanged";return false;}
    for(const auto& column:declaration.columns)if(column.sourceColumn>shape.columns){error="NumericTableInvalidColumn";return false;}
    QSqlQuery row;if(!store.exec("SELECT CASE WHEN octet_length(revision)<=64 THEN revision END,stale FROM numeric_table_projections WHERE table_id=?",{qlonglong(declaration.tableId)},error,&row))return false;
    const bool exists=row.next();const auto old=exists?s(row.value(0).toString()):std::string();const bool stale=exists&&row.value(1).toBool();row.finish();
    if((!exists&&!previous.empty())||(exists&&(previous.empty()||!revisionValid(old)||previous!=old))){error="NumericTableProjectionConflict";return false;}
    if(exists&&!stale) {
        NumericTableDescriptor current;NumericTableReadStats usage;
        auto comparisonOptions=options;
        comparisonOptions.metadata.maxFieldBytes=8192;comparisonOptions.metadata.maxTextBytes=64*1024;
        if(!loadNumericTableDescriptor(store,declaration.tableId,current,comparisonOptions,usage,error))return false;
        if(current.projection==declaration){if(numericTableStopped(options,error)||!transaction.commit(error))return false;revision=old;return true;}
    }
    const auto next=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!store.exec("INSERT INTO numeric_table_projections VALUES(?,?,?,?,?,?,?,?,?,?,1) ON CONFLICT(table_id) DO UPDATE SET source_id=excluded.source_id,source_sha256=excluded.source_sha256,revision=excluded.revision,contract=excluded.contract,first_row=excluded.first_row,row_count=excluded.row_count,base_rows=excluded.base_rows,base_columns=excluded.base_columns,column_count=excluded.column_count,stale=1",
        {qlonglong(declaration.tableId),qlonglong(declaration.sourceId),q(declaration.sourceSha256),next,QLatin1String(NumericTableContract),qlonglong(declaration.firstSourceRow),qlonglong(declaration.rowCount),qlonglong(shape.rows),qlonglong(shape.columns),int(declaration.columns.size())},error)||
       !store.exec("DELETE FROM numeric_table_columns WHERE table_id=?",{qlonglong(declaration.tableId)},error))return false;
    int ordinal=0;for(const auto& column:declaration.columns) {
        if(numericTableStopped(options,error)||!store.exec("INSERT INTO numeric_table_columns VALUES(?,?,?,?,?,?,?,?,?)",
            {qlonglong(declaration.tableId),ordinal++,q(column.id),column.sourceColumn,int(column.dtype),q(column.label),q(column.unit),column.missingIsNull,QString::fromUtf8(encodedNulls(column))},error))return false;
    }
    if(numericTableStopped(options,error)||!store.exec("UPDATE numeric_table_projections SET stale=0 WHERE table_id=?",{qlonglong(declaration.tableId)},error)||!transaction.commit(error))return false;
    revision=s(next);return true;
}
}
