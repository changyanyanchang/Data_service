#include "paleo_data/service.h"
#include "store/project_store.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
std::string u(const QString& text){return text.toUtf8().toStdString();}
void require(bool value,const std::string& error){if(!value)throw std::runtime_error(error);}
struct Fixture {
    QTemporaryDir temporary;DataService service,reader;std::string error,sha;long long sourceId=0,tableId=0;
    Fixture() {
        require(temporary.isValid(),"temporary directory");const auto directory=temporary.path()+"/project",file=temporary.path()+"/source.txt";
        require(service.createProject(u(directory),"numeric fixture",error),error);
        QFile input(file);require(input.open(QIODevice::WriteOnly)&&input.write("fixture")==7,"source fixture");input.close();
        sha=QCryptographicHash::hash("fixture",QCryptographicHash::Sha256).toHex().toStdString();
        ParsedFile parsed;parsed.kind="table-fixture";parsed.status="parsed";SourceTable table;table.sheetName="same name";table.rowCount=4;table.columnCount=3;
        table.cells={{1,1,"-2.5"},{2,1,"0"},{3,1,"-999.25"},{4,1,"+4.5"},{1,2,"1"},{2,2,"0"},{4,2,"1"},{1,3,"unused text"}};
        parsed.sourceTables={table,table};require(service.importPreparedFile(u(file),u(file),u(file),sha,parsed,sourceId,error),error);
        std::vector<DataEntry> tables;require(service.listSourceData("tables",sourceId,tables,error)&&tables.size()==2,error);tableId=tables[1].id;
        require(service.upgradeNumericTableProjectionSchema(error),error);require(reader.openProjectReadOnly(service.projectPath(),error),error);
    }
    ModelChangeHead head() {ModelChangeHead value;require(service.readModelChangeHead(value,error)&&value.available,error);return value;}
    NumericTableProjection definition()const {
        NumericTableProjection p;p.tableId=tableId;p.sourceId=sourceId;p.sourceSha256=sha;p.rowCount=4;
        NumericTableColumn number;number.id="amplitude";number.label="Amplitude";number.unit="mV";number.sourceColumn=1;number.nullTexts={"-999.25"};
        NumericTableColumn boolean;boolean.id="accepted";boolean.label="Accepted";boolean.unit="1";boolean.sourceColumn=2;boolean.dtype=NumericTableDtype::Boolean;
        p.columns={number,boolean};return p;
    }
    NumericTableDescriptor confirm(const NumericTableProjection& p,const std::string& previous={}) {
        std::string revision;require(service.confirmNumericTableProjection(p,head(),previous,revision,error)&&!revision.empty(),error);
        NumericTableDescriptor descriptor;require(reader.describeNumericTableProjection(tableId,descriptor,error),error);require(descriptor.revision==revision,"confirmed revision");return descriptor;
    }
    void edit(const QString& sql,const QVariantList& args={}) {
        detail::ProjectStore writer;require(writer.open(QString::fromStdString(service.projectPath()),false,{},error),error);
        detail::Transaction transaction(writer,error);require(transaction.active,error);require(writer.exec(sql,args,error),error);require(transaction.commit(error),error);
    }
    NumericTableWindowQuery query(const NumericTableDescriptor& d)const {return {d.cut,{"amplitude","accepted"},0,4};}
};
void valuesAndIdentity() {
    Fixture f;const auto declaration=f.definition();const auto descriptor=f.confirm(declaration);NumericTableWindow result;NumericTableReadStats usage;
    require(f.reader.readNumericTableWindow(f.query(descriptor),result,f.error,{},&usage),f.error);
    require(result.sourceRows==std::vector<std::int64_t>({1,2,3,4}),"stable one-based physical row axis");
    require(result.columns[0].numbers==std::vector<double>({-2.5,0,0,4.5})&&result.columns[0].nullMask==std::vector<unsigned char>({0,0,1,0}),"declared null precedes numeric parsing, negatives and zero preserved");
    require(result.columns[1].booleans==std::vector<unsigned char>({1,0,0,1})&&result.columns[1].nullMask==std::vector<unsigned char>({0,0,1,0}),"bool and missing sparse row");
    require(result.columns[1].numbers.empty()&&result.columns[0].booleans.empty(),"typed storage");
    require(result.descriptor.cut.projectionRevision==descriptor.revision&&result.descriptor.cut.model.sequence==descriptor.cut.model.sequence,"actual consumed read cut");
    require(usage.cellsRead==7&&usage.cellTextBytesRead>0&&usage.metadataBytesRead>0&&usage.outputBytes==76&&usage.reservedBytes>=usage.outputBytes,"measured work and reservation");
    const auto before=f.head();std::string revision;require(f.service.confirmNumericTableProjection(declaration,before,descriptor.revision,revision,f.error),f.error);
    require(revision==descriptor.revision&&f.head().sequence==before.sequence,"identical confirmation is a no-op");
    require(!f.service.confirmNumericTableProjection(declaration,f.head(),{},revision,f.error)&&revision.empty()&&f.error=="NumericTableProjectionConflict","create cannot replace projection");
    auto wrong=declaration;wrong.sourceId+=100;require(!f.service.confirmNumericTableProjection(wrong,f.head(),descriptor.revision,revision,f.error),"foreign source rejected");
    auto query=f.query(descriptor);query.columnIds={"accepted","amplitude"};query.firstRow=1;query.rowCount=2;
    require(f.reader.readNumericTableWindow(query,result,f.error)&&result.sourceRows==std::vector<std::int64_t>({2,3})&&result.columns[0].column.id=="accepted",f.error);
    query.columnIds={"amplitude","amplitude"};require(!f.reader.readNumericTableWindow(query,result,f.error)&&result.sourceRows.empty(),"duplicate columns reject and clear");
    query=f.query(descriptor);query.firstRow=4;query.rowCount=0;require(f.reader.readNumericTableWindow(query,result,f.error)&&result.sourceRows.empty(),"valid empty window");
    query.rowCount=1;require(!f.reader.readNumericTableWindow(query,result,f.error)&&result.columns.empty(),"out-of-window does not clamp");
    DataTable display;require(f.service.readTable("tables",f.tableId,0,4,display,f.error)&&display.rows[2][1]=="-999.25","display contract/raw values unchanged");
}
void revisionsRollbackAndReopen() {
    Fixture f;auto p=f.definition();auto d=f.confirm(p);const auto old=d.cut;const auto initial=f.head();std::string revision;
    p.columns[0].unit="V";int checks=0;
    require(!f.service.confirmNumericTableProjection(p,initial,d.revision,revision,f.error,[&]{return ++checks==5;})&&revision.empty(),"cancel during metadata writes");
    NumericTableDescriptor same;require(f.reader.describeNumericTableProjection(f.tableId,same,f.error)&&same.revision==d.revision&&f.head().sequence==initial.sequence,"rollback retains revision and head");
    f.edit("UPDATE source_tables SET name='renamed' WHERE id=?",{f.tableId});
    require(f.reader.describeNumericTableProjection(f.tableId,same,f.error)&&same.revision==d.revision,"display rename preserves semantic revision");
    NumericTableWindow result;require(!f.reader.readNumericTableWindow({old,{"amplitude"},0,1},result,f.error)&&f.error=="NumericTableStaleReadCut","old model cut does not follow latest");
    f.edit("UPDATE source_cells SET text='9.5' WHERE table_id=? AND row_num=1 AND col_num=1",{f.tableId});
    require(!f.reader.describeNumericTableProjection(f.tableId,same,f.error)&&same.revision.empty()&&f.error=="NumericTableSourceChanged","numeric edit invalidates projection");
    d=f.confirm(f.definition(),d.revision);require(d.revision!=old.projectionRevision,"explicit reconfirmation mints a new revision");
    require(f.reader.readNumericTableWindow(f.query(d),result,f.error)&&result.columns[0].numbers[0]==9.5,f.error);
    const auto directory=f.service.projectPath();f.reader.closeProject();f.service.closeProject();require(f.reader.openProjectReadOnly(directory,f.error),f.error);
    require(f.reader.describeNumericTableProjection(f.tableId,same,f.error)&&same.revision==d.revision,"RO reopening preserves persisted declaration");
    require(!f.reader.upgradeNumericTableProjectionSchema(f.error)&&f.error=="NumericTableReadOnly","RO cannot upgrade");
    require(!f.reader.confirmNumericTableProjection(f.definition(),same.cut.model,same.revision,revision,f.error)&&revision.empty()&&f.error=="NumericTableReadOnly","RO cannot confirm");
}
void snapshotAndCancellation() {
    Fixture f;const auto descriptor=f.confirm(f.definition());const auto query=f.query(descriptor);NumericTableWindow result;NumericTableReadStats usage;
    NumericTableReadOptions options;int checks=0;bool changed=false;
    options.cancelled=[&] {
        if(++checks==4){f.edit("UPDATE source_cells SET text='77' WHERE table_id=? AND row_num=1 AND col_num=1",{f.tableId});changed=true;}return false;
    };
    require(f.reader.readNumericTableWindow(query,result,f.error,options,&usage)&&changed&&result.columns[0].numbers[0]==-2.5,"snapshot returns original cut despite concurrent committed write");
    require(result.descriptor.cut.model.sequence==descriptor.cut.model.sequence,"returned cut is the read snapshot, not post-read current head");
    const auto current=f.confirm(f.definition(),descriptor.revision);
    options.cancelled=[]{return true;};require(!f.reader.readNumericTableWindow(f.query(current),result,f.error,options,&usage)&&result.columns.empty()&&usage.cellsRead==0,"cancel before payload");
    checks=0;options.cancelled=[&]{return ++checks==6;};
    require(!f.reader.readNumericTableWindow(f.query(current),result,f.error,options,&usage)&&f.error=="NumericTableCancelled"&&result.columns.empty(),"cancel during reading delivers no partial columns");
}
void legacyAndInvalidDeclarations() {
    QTemporaryDir root;DataService service;std::string error;require(service.createProject(u(root.path()+"/project"),"legacy",error),error);
    DataService reader;require(reader.openProjectReadOnly(service.projectPath(),error),error);NumericTableDescriptor descriptor;
    require(!reader.describeNumericTableProjection(1,descriptor,error)&&error=="NumericTableSchemaUnavailable","legacy RO has no inferred numeric capability");
    NumericTableReadOptions options;options.metadata.maxFieldBytes=0;require(!reader.describeNumericTableProjection(1,descriptor,error,options)&&error=="NumericTableInvalidBudget","bad budget before schema read");
    Fixture f;auto p=f.definition();std::string revision;p.columns[0].unit="unknown";
    require(!f.service.confirmNumericTableProjection(p,f.head(),{},revision,f.error)&&f.error=="NumericTableUnitRequired","unknown units not guessed");
    p=f.definition();p.columns[1].unit="m";require(!f.service.confirmNumericTableProjection(p,f.head(),{},revision,f.error),"bool explicitly dimensionless");
    p=f.definition();p.columns[0].id="=formula";require(!f.service.confirmNumericTableProjection(p,f.head(),{},revision,f.error),"unsafe column id");
    p=f.definition();p.columns[0].sourceColumn=999;require(!f.service.confirmNumericTableProjection(p,f.head(),{},revision,f.error),"foreign physical column");
}
void budgetsAndStrictTypes() {
    Fixture f;auto d=f.confirm(f.definition());NumericTableWindow result;NumericTableReadStats usage;NumericTableReadOptions options;
    options.maxOutputBytes=1;
    require(!f.reader.readNumericTableWindow(f.query(d),result,f.error,options,&usage)&&usage.cellsRead==0&&result.columns.empty(),"output reservation precedes payload reads");
    options={};options.maxWorkingBytes=1;
    require(!f.reader.readNumericTableWindow(f.query(d),result,f.error,options,&usage)&&usage.cellsRead==0,"working reservation precedes payload reads");
    options={};options.metadata.maxFieldBytes=8;
    NumericTableDescriptor rejected;require(!f.reader.describeNumericTableProjection(f.tableId,rejected,f.error,options,&usage)&&usage.metadataBytesRead==0,"metadata field guard before driver text");
    options={};options.maxReadBytes=1;
    require(!f.reader.readNumericTableWindow(f.query(d),result,f.error,options,&usage)&&usage.metadataBytesRead==0&&usage.cellsRead==0,"tiny aggregate limit protects metadata too");
    options={};options.maxCellBytes=2;
    require(!f.reader.readNumericTableWindow(f.query(d),result,f.error,options,&usage)&&f.error=="NumericTableCellBudgetExceeded"&&usage.cellTextBytesRead==0,"column length preflight rejects before raw text delivery");
    for(const auto& text:std::vector<QString>{"NaN","Inf","1,000","+-1","abc"}) {
        f.edit("UPDATE source_cells SET text=? WHERE table_id=? AND row_num=1 AND col_num=1",{text,f.tableId});
        d=f.confirm(f.definition(),d.revision);options={};
        require(!f.reader.readNumericTableWindow(f.query(d),result,f.error,options,&usage)&&f.error=="NumericTableCellTypeInvalid"&&result.columns.empty(),"invalid numeric token returns no partial table");
    }
    f.edit("UPDATE source_cells SET text='-2.5' WHERE table_id=? AND row_num=1 AND col_num=1",{f.tableId});
    f.edit("UPDATE source_cells SET text='true' WHERE table_id=? AND row_num=1 AND col_num=2",{f.tableId});d=f.confirm(f.definition(),d.revision);
    require(!f.reader.readNumericTableWindow(f.query(d),result,f.error)&&result.sourceRows.empty(),"bool is explicit 0/1, not text inference");
    f.edit("UPDATE source_cells SET text='1' WHERE table_id=? AND row_num=1 AND col_num=2",{f.tableId});auto required=f.definition();required.columns[1].missingIsNull=false;
    d=f.confirm(required,d.revision);require(!f.reader.readNumericTableWindow(f.query(d),result,f.error)&&f.error=="NumericTableMissingCell","missing sparse cell needs an explicit null rule");
}
void unselectedLargeCellsAndIndex() {
    Fixture f;auto p=f.definition();p.columns.resize(1);auto d=f.confirm(p);
    f.edit("UPDATE source_cells SET text=? WHERE table_id=? AND row_num=1 AND col_num=3",{QString(16*1024*1024,QChar('x')),f.tableId});d=f.confirm(p,d.revision);
    NumericTableWindow result;NumericTableReadStats usage;NumericTableWindowQuery query{d.cut,{"amplitude"},0,4};
    require(f.reader.readNumericTableWindow(query,result,f.error,{},&usage)&&usage.cellsRead==4&&usage.cellTextBytesRead<64,"unselected 16MiB field is not fetched");
    detail::ProjectStore probe;require(probe.open(QString::fromStdString(f.service.projectPath()),false,{},f.error,true),f.error);QSqlQuery explain;
    require(probe.exec("EXPLAIN QUERY PLAN SELECT row_num,text FROM source_cells INDEXED BY numeric_table_column_window WHERE table_id=? AND col_num=? AND row_num>=? AND row_num<=? ORDER BY row_num",{f.tableId,1,1,4},f.error,&explain),f.error);
    QString plan;while(explain.next())plan+=explain.value(3).toString();
    require(plan.contains("numeric_table_column_window")&&plan.contains("table_id=?")&&plan.contains("col_num=?")&&plan.contains("row_num>?")&&!plan.contains("SCAN"),"column and row window seek: "+u(plan));
    f.edit("UPDATE source_cells SET text=? WHERE table_id=? AND row_num=1 AND col_num=1",{QString(16*1024*1024,QChar('x')),f.tableId});d=f.confirm(p,d.revision);query.expected=d.cut;
    require(!f.reader.readNumericTableWindow(query,result,f.error,{},&usage)&&usage.cellTextBytesRead==0&&result.columns.empty(),"selected oversized field rejected before materialization");
}
void schemaUpgradeCancellation() {
    QTemporaryDir root;DataService service;std::string error;require(service.createProject(u(root.path()+"/project"),"upgrade",error),error);
    int checks=0;require(!service.upgradeNumericTableProjectionSchema(error,[&]{return ++checks==4;})&&error=="NumericTableCancelled","schema upgrade cancellation");
    DataService reader;require(reader.openProjectReadOnly(service.projectPath(),error),error);NumericTableDescriptor descriptor;
    require(!reader.describeNumericTableProjection(1,descriptor,error)&&error=="NumericTableSchemaUnavailable","cancelled DDL is not published");
    require(service.upgradeNumericTableProjectionSchema(error),error);ModelChangeHead before,after;require(service.readModelChangeHead(before,error),error);
    require(service.upgradeNumericTableProjectionSchema(error)&&service.readModelChangeHead(after,error)&&after.sequence==before.sequence,"repeat schema upgrade is a no-op");
}
void shapeRepairAndReusedIds() {
    Fixture f;const auto p=f.definition();auto descriptor=f.confirm(p);NumericTableDescriptor value;
    f.edit("UPDATE source_tables SET rows=5 WHERE id=?",{f.tableId});
    require(!f.reader.describeNumericTableProjection(f.tableId,value,f.error)&&f.error=="NumericTableSourceChanged","shape changes invalidate the view");
    descriptor=f.confirm(p,descriptor.revision);
    f.edit("DROP INDEX numeric_table_column_window");
    require(!f.reader.describeNumericTableProjection(f.tableId,value,f.error)&&f.error=="NumericTableSchemaUnavailable","missing window index cannot cause a scan fallback");
    require(f.service.upgradeNumericTableProjectionSchema(f.error),f.error);
    require(!f.reader.describeNumericTableProjection(f.tableId,value,f.error)&&f.error=="NumericTableSourceChanged","repair does not bless old tracking");
    descriptor=f.confirm(p,descriptor.revision);const auto old=descriptor.revision;
    f.edit("DELETE FROM source_tables WHERE id=?",{f.tableId});
    f.edit("INSERT INTO source_tables(id,source_id,name,rows,columns) VALUES(?,?,'replacement',4,3)",{f.tableId,f.sourceId});
    std::string revision;require(!f.service.confirmNumericTableProjection(p,f.head(),old,revision,f.error)&&revision.empty(),"deleted projection revision cannot bind a reused id");
    descriptor=f.confirm(p);require(descriptor.revision!=old,"reused table id gets a distinct projection identity");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {valuesAndIdentity();revisionsRollbackAndReopen();snapshotAndCancellation();legacyAndInvalidDeclarations();budgetsAndStrictTypes();unselectedLargeCellsAndIndex();schemaUpgradeCancellation();shapeRepairAndReusedIds();std::cout<<"numeric_table_contract passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
