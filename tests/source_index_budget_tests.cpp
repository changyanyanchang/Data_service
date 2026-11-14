#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
void require(bool ok,const std::string &why){if(!ok)throw std::runtime_error(why);}
std::string utf8(const QString &text){return text.toUtf8().toStdString();}
void update(const QString &project,const QString &column,const QVariant &value) {
    const auto name=QUuid::createUuid().toString();
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(project+"/data.sqlite");require(db.open(),"open test writer");
        QSqlQuery query(db);require(query.prepare("UPDATE sources SET "+column+"=? WHERE id=1"),"prepare metadata mutation");
        query.addBindValue(value);require(query.exec(),"write metadata mutation");query.finish();db.close();
    }
    QSqlDatabase::removeDatabase(name);
}
long long bytes(const SourceIndexEntry &value) {
    return value.path.size()+value.storedPath.size()+value.kind.size()+value.status.size()+value.message.size()+value.sha256.size();
}
void invalidUtf8(const QString &project,const char *hex) {
    const auto name=QUuid::createUuid().toString();
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(project+"/data.sqlite");require(db.open(),"open UTF-8 fixture");
        QSqlQuery query(db);require(query.exec("UPDATE sources SET message=CAST(X'"+QString::fromLatin1(hex)+"' AS TEXT) WHERE id=1"),"write invalid UTF-8 fixture");query.finish();db.close();
    }
    QSqlDatabase::removeDatabase(name);
}
void denied(DataService &service,const SourceIndexReadOptions &options,const std::string &code) {
    SourceIndexEntry value;value.path="old";value.message="old";value.id=42;SourceIndexReadStats stats;stats.textBytesRead=999;
    std::string error;require(!service.readSourceIndexEntry(1,value,error,options,&stats),"budget must reject");
    require(error==code,"unexpected budget failure: "+error);require(value.id==0&&value.path.empty()&&value.message.empty(),"rejection clears complete result");
    require(stats.textBytesRead==0,"rejection reports no returned text");
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir temp;require(temp.isValid(),"temporary fixture");const auto project=temp.filePath("project");
        DataService service;std::string error;require(service.createProject(utf8(project),"source budget",error),error);
        QFile source(temp.filePath("source.data"));const QByteArray content="bounded source identity";
        require(source.open(QIODevice::WriteOnly)&&source.write(content)==content.size(),"write source");source.close();
        ParsedFile parsed;parsed.kind="test";parsed.status="parsed";long long id=0;
        const auto hash=QCryptographicHash::hash(content,QCryptographicHash::Sha256).toHex().toStdString();
        require(service.importPreparedFile(utf8(source.fileName()),utf8(source.fileName()),utf8(source.fileName()),hash,parsed,id,error),error);
        require(id==1,"stable first source");SourceIndexEntry original;require(service.readSourceIndexEntry(id,original,error),error);
        SourceIndexReadOptions budget;budget.maxFieldBytes=8192;budget.maxTextBytes=8192;SourceIndexReadStats stats;SourceIndexEntry value;
        if(!service.readSourceIndexEntry(id,value,error,budget,&stats)) {
            require(error.starts_with("SourceIndexValueBudgetUnavailable:"),error);
            require(value.id==0&&stats.textBytesRead==0,"unsupported SQLite is fail closed");
            std::cout<<"source_index_budget unavailable: "<<error<<'\n';return 0;
        }
        require(value.id==id&&value.path==original.path&&value.sha256==original.sha256,"bounded values equal original identity");
        require(stats.textBytesRead==bytes(original),"count all six returned UTF-8 fields");
        const auto unicode=QString::fromUtf8("来源🌍记录").repeated(25);update(project,"message",unicode);
        require(service.readSourceIndexEntry(id,value,error,budget,&stats),error);
        require(value.message==utf8(unicode)&&stats.textBytesRead==bytes(value),"Unicode bytes preserved and charged");
        auto exact=budget;exact.maxTextBytes=bytes(value);require(service.readSourceIndexEntry(id,value,error,exact,&stats),"exact total budget succeeds");
        --exact.maxTextBytes;denied(service,exact,"SourceIndexValueBudgetExceeded");
        auto field=budget;field.maxFieldBytes=unicode.toUtf8().size()-1;denied(service,field,"SourceIndexValueBudgetExceeded");
        update(project,"message",QString());
        for(const auto &column:{"original_path","stored_path","kind","status","message","hash"}) {
            const std::string name(column);
            const auto old=name=="original_path"?original.path:name=="stored_path"?original.storedPath:
                name=="kind"?original.kind:name=="status"?original.status:name=="hash"?original.sha256:std::string();
            update(project,QString::fromLatin1(column),QString(16*1024*1024,QChar('x')));
            denied(service,budget,"SourceIndexValueBudgetExceeded");
            update(project,QString::fromLatin1(column),QString::fromUtf8(old.data(),qsizetype(old.size())));
        }
        update(project,"bytes",QString(16*1024*1024,QChar('9')));denied(service,budget,"SourceIndexValueBudgetExceeded");
        update(project,"bytes",qlonglong(original.bytes));
        update(project,"message",QString(32768,QChar('y')));SourceIndexEntry compatibility;
        require(service.readSourceIndexEntry(id,compatibility,error)&&compatibility.message.size()==32768,"legacy signature semantics remain unchanged");
        denied(service,budget,"SourceIndexValueBudgetExceeded");update(project,"message",QString());
        for(const auto *hex:{"FF","C3","E282","F09F92"}) {
            invalidUtf8(project,hex);denied(service,budget,"SourceIndexValueEncodingInvalid");
        }
        update(project,"message",QString());
        auto invalid=budget;invalid.maxFieldBytes=0;denied(service,invalid,"SourceIndexValueBudgetInvalid");
        invalid=budget;invalid.maxTextBytes=256*1024+1;denied(service,invalid,"SourceIndexValueBudgetInvalid");
        DataService reader;require(reader.openProjectReadOnly(utf8(project),error),error);
        require(reader.readSourceIndexEntry(id,value,error,budget,&stats),error);require(stats.textBytesRead==bytes(value),"read-only same bounds");
        require(!reader.readSourceIndexEntry(999,value,error,budget,&stats)&&error=="SourceIndexValueNotFound"&&value.id==0&&stats.textBytesRead==0,"missing clears output");
        std::cout<<"source_index_budget passed: SQLite guards six 16MiB fields before materialization; Unicode/truncated-UTF8/exact/aggregate/readonly/legacy checks\n";
        return 0;
    } catch(const std::exception &failure){std::cerr<<failure.what()<<'\n';return 1;}
}
