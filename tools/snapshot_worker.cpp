// Dedicated process: only Qt's own QSQLITE implementation opens these handles.
#include <QCoreApplication>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <cstdio>

namespace {
bool makeSnapshot(const QString& source,const QString& destination,QString& error) {
    auto input=QSqlDatabase::addDatabase("QSQLITE","snapshot_source");
    input.setDatabaseName(source);input.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
    if(!input.open()){error=input.lastError().text();return false;}
    // VACUUM INTO takes its own consistent read snapshot, including committed
    // WAL. No source migration, checkpoint, journal setting or writer is used.
    QSqlQuery copy(input);
    if(!copy.prepare("VACUUM INTO ?")){error=copy.lastError().text();return false;}
    copy.addBindValue(destination);
    if(!copy.exec()){error=copy.lastError().text();return false;}
    copy.finish();input.close();
    auto output=QSqlDatabase::addDatabase("QSQLITE","snapshot_check");
    output.setDatabaseName(destination);output.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
    if(!output.open()){error=output.lastError().text();return false;}
    QSqlQuery check(output);
    if(!check.exec("SELECT value FROM ds_meta WHERE key='schema_version'")||!check.next()||check.value(0).toString()!="1") {
        error="Snapshot schema verification failed";return false;
    }
    check.finish();output.close();return true;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();
    QString source,destination;QStringList plugins;
    for(int i=1;i<args.size();++i) {
        if(i+1>=args.size())return 2;
        const auto name=args[i],value=args[++i];
        if(name=="--source"&&source.isEmpty())source=value;
        else if(name=="--destination"&&destination.isEmpty())destination=value;
        else if(name=="--plugin-path")plugins.push_back(value);
        else return 2;
    }
    if(!plugins.isEmpty())QCoreApplication::setLibraryPaths(plugins);
    if(source.isEmpty()||destination.isEmpty()||!QFileInfo(source).isFile()||QFileInfo::exists(destination)||QFileInfo(destination).isSymLink())return 2;
    QString error;
    if(!makeSnapshot(source,destination,error)) {
        const auto bytes=error.toUtf8().left(2048);std::fwrite(bytes.constData(),1,size_t(bytes.size()),stderr);return 3;
    }
    std::fputs("paleo-snapshot-complete-v1\n",stdout);return 0;
}
