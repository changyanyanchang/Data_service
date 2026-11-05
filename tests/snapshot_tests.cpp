// Synthetic databases and isolated process copies only; no private source data.
#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QVariant>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace paleo::data;
namespace {
std::string u(const QString& value){return value.toUtf8().toStdString();}
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
QString workerName(){return QStringLiteral("paleo_data_snapshot_worker")
#ifdef Q_OS_WIN
    +QStringLiteral(".exe")
#endif
    ;}
void hidden(QProcess& process){
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args){args->flags|=CREATE_NO_WINDOW;});
#else
    (void)process;
#endif
}
QByteArray hashFile(const QString& path){
    QFile file(path);check(file.open(QIODevice::ReadOnly),"open synthetic fingerprint");QCryptographicHash digest(QCryptographicHash::Sha256);
    check(digest.addData(&file),"hash synthetic database");return digest.result();
}
QStringList stages(const QString& parent){return QDir(parent).entryList({".paleo-snapshot-*"},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot);}
void sql(QSqlDatabase& database,const QString& statement){QSqlQuery query(database);check(query.exec(statement),u(query.lastError().text()));}
struct Fixture {
    QTemporaryDir temp;
    QString project,database,connection;
    QSqlDatabase writer;
    DataService reader;
    std::string error;
    QByteArray originalMain,originalWal;
    Fixture(){
        check(temp.isValid(),"temporary snapshot fixture");project=temp.filePath("model");database=QDir(project).filePath("data.sqlite");
        DataService create;check(create.createProject(u(project),"snapshot synthetic",error),error);create.closeProject();
        connection="snapshot_fixture_"+QUuid::createUuid().toString(QUuid::WithoutBraces);writer=QSqlDatabase::addDatabase("QSQLITE",connection);
        writer.setDatabaseName(database);check(writer.open(),u(writer.lastError().text()));
        sql(writer,"PRAGMA journal_mode=WAL");sql(writer,"PRAGMA wal_autocheckpoint=0");
        sql(writer,"CREATE TABLE snapshot_values(id INTEGER PRIMARY KEY,payload BLOB)");
        sql(writer,"INSERT INTO snapshot_values VALUES(1,zeroblob(16384)),(2,'raw signed -120 / source text')");
        check(reader.openProjectReadOnly(u(project),error),error);check(reader.isReadOnly(),"read-only source session");
        check(QFileInfo(database+"-wal").size()>0,"committed source data remains in WAL");
        originalMain=hashFile(database);originalWal=hashFile(database+"-wal");
    }
    ~Fixture(){reader.closeProject();writer.close();writer=QSqlDatabase();QSqlDatabase::removeDatabase(connection);}
    void unchanged(){check(hashFile(database)==originalMain&&hashFile(database+"-wal")==originalWal,"snapshot must not rewrite source database or WAL");}
    void copiedValues(const QString& path,int count=2){
        const auto name="snapshot_verify_"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            auto copy=QSqlDatabase::addDatabase("QSQLITE",name);copy.setDatabaseName(path);copy.setConnectOptions("QSQLITE_OPEN_READONLY");
            check(copy.open(),u(copy.lastError().text()));
            {QSqlQuery query(copy);check(query.exec("SELECT count(*),length(max(CASE WHEN id=1 THEN payload END)),max(CASE WHEN id=2 THEN CAST(payload AS TEXT) END) FROM snapshot_values")&&query.next(),u(query.lastError().text()));
             check(query.value(0).toInt()==count&&query.value(1).toInt()==16384&&query.value(2).toString()=="raw signed -120 / source text","snapshot preserves IDs, BLOBs and committed raw values");}
            {QSqlQuery query(copy);check(query.exec("PRAGMA quick_check")&&query.next()&&query.value(0).toString()=="ok","snapshot database is complete");}
            copy.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
};

void walAndExistingDestination(){
    Fixture fixture;const auto output=fixture.temp.filePath("snapshot.sqlite");
    check(fixture.reader.snapshotDatabase(u(output),fixture.error),fixture.error);fixture.copiedValues(output);fixture.unchanged();
    check(stages(fixture.temp.path()).empty(),"successful snapshot removes only its private staging directory");
    const auto before=hashFile(output);
    check(!fixture.reader.snapshotDatabaseCancellable(u(output),fixture.error,{},10000)&&hashFile(output)==before,"existing target is never overwritten or removed");
    sql(fixture.writer,"BEGIN");sql(fixture.writer,"INSERT INTO snapshot_values VALUES(3,'not committed')");
    const auto committed=fixture.temp.filePath("committed.sqlite");
    check(fixture.reader.snapshotDatabaseCancellable(u(committed),fixture.error,{},10000),fixture.error);
    fixture.copiedValues(committed);sql(fixture.writer,"ROLLBACK");
}
void cancellationCleanup(){
    Fixture fixture;const auto output=fixture.temp.filePath("cancel.sqlite");
    check(!fixture.reader.snapshotDatabaseCancellable(u(output),fixture.error,[]{return true;},10000)&&!QFileInfo::exists(output),"pre-cancel leaves target absent");
    bool sawOutput=false;QElapsedTimer elapsed;elapsed.start();
    const auto cancelled=[&] {
        for(const auto& stage:stages(fixture.temp.path()))if(QFileInfo::exists(QDir(fixture.temp.filePath(stage)).filePath("data.sqlite")))sawOutput=true;
        return sawOutput||elapsed.elapsed()>5000;
    };
    check(!fixture.reader.snapshotDatabaseCancellable(u(output),fixture.error,cancelled,10000)&&sawOutput&&
          fixture.error.find("取消")!=std::string::npos,"cancellation after worker creates output is reported");
    check(!QFileInfo::exists(output)&&stages(fixture.temp.path()).empty(),"cancelled output and private stage are removed");fixture.unchanged();
}
void copyExecutable(const QString& from,const QString& to){
    check(QFile::copy(from,to),"copy isolated test executable");
    check(QFile::setPermissions(to,QFile::permissions(to)|QFileDevice::ExeOwner|QFileDevice::ExeUser),"set copied executable permissions");
}
void runClient(const QString& executable,const QString& mode,const QString& source,const QString& destination,const QString& fake={}){
    QProcess process;hidden(process);auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert("PATH",QCoreApplication::applicationDirPath()+QDir::listSeparator()+environment.value("PATH"));
    if(!fake.isEmpty())environment.insert("PDS_TEST_FAKE_SNAPSHOT",fake);process.setProcessEnvironment(environment);
    QStringList args{"--snapshot-client",mode,source,destination};
    for(const auto& path:QCoreApplication::libraryPaths())args<<"--plugin-path"<<path;
    process.start(executable,args,QIODevice::ReadOnly);const bool finished=process.waitForFinished(15000);
    if(!finished){process.kill();process.waitForFinished(3000);}
    check(finished&&process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0,
          "isolated client "+u(mode)+": "+u(QString::fromUtf8(process.readAllStandardError())));
}
int client(const QStringList& args){
    check(args.size()>=5,"client arguments");QStringList plugins;
    for(int i=5;i+1<args.size();i+=2){check(args[i]=="--plugin-path","client plugin argument");plugins.push_back(args[i+1]);}
    QCoreApplication::setLibraryPaths(plugins);
    DataService service;std::string error;check(service.openProjectReadOnly(u(args[3]),error),error);
    const bool result=service.snapshotDatabaseCancellable(u(args[4]),error,{},args[2]=="timeout"?100:10000);
    if(args[2]=="success")check(result,error);
    else {
        check(!result&&!QFileInfo::exists(args[4])&&stages(QFileInfo(args[4]).absolutePath()).empty(),"failed helper never leaves requested output or stage");
        if(args[2]=="missing")check(error.find("paleo_data_snapshot_worker")!=std::string::npos,"missing helper explains deployment issue");
        if(args[2]=="timeout")check(error.find("超时")!=std::string::npos,"worker timeout is explicit");
    }
    return 0;
}
void helperAvailabilityTimeoutAndCrash(){
    Fixture fixture;QTemporaryDir isolated;check(isolated.isValid(),"isolated app root");
    const auto bin=isolated.filePath("bin");check(QDir().mkpath(bin),"isolated bin");
    const auto executable=QDir(bin).filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName());
    copyExecutable(QCoreApplication::applicationFilePath(),executable);
    runClient(executable,"missing",fixture.project,fixture.temp.filePath("missing.sqlite"));
    // Verify the controlled install-relative libexec location independently of PATH.
    const auto libexec=isolated.filePath("libexec");check(QDir().mkpath(libexec),"isolated libexec");
    const auto helper=QDir(libexec).filePath(workerName());
    copyExecutable(QDir(QCoreApplication::applicationDirPath()).filePath(workerName()),helper);
    const auto success=fixture.temp.filePath("installed.sqlite");runClient(executable,"success",fixture.project,success);fixture.copiedValues(success);
    check(QFile::remove(helper),"remove only copied test helper");
    // The fake helper is another copy of this test executable, never a production hook.
    copyExecutable(QCoreApplication::applicationFilePath(),QDir(bin).filePath(workerName()));
    runClient(executable,"timeout",fixture.project,fixture.temp.filePath("timeout.sqlite"),"sleep");
    runClient(executable,"crash",fixture.project,fixture.temp.filePath("partial.sqlite"),"partial");
    runClient(executable,"invalid-success",fixture.project,fixture.temp.filePath("invalid.sqlite"),"invalid-success");fixture.unchanged();
}
int fakeHelper(const QStringList& args){
    const auto mode=qEnvironmentVariable("PDS_TEST_FAKE_SNAPSHOT");
    if(mode=="partial"||mode=="invalid-success") {
        const auto at=args.indexOf("--destination");check(at>=0&&at+1<args.size(),"fake destination");QFile file(args[at+1]);
        check(file.open(QIODevice::WriteOnly)&&file.write(QByteArray(128,'x'))==128,"write synthetic incomplete output");file.close();
        std::fputs("paleo-snapshot-complete-v1\n",stdout);return mode=="partial"?7:0;
    }
    QThread::msleep(2000);return 9;
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try {
        if(QFileInfo(app.applicationFilePath()).fileName()==workerName())return fakeHelper(app.arguments());
        if(app.arguments().size()>1&&app.arguments()[1]=="--snapshot-client")return client(app.arguments());
        walAndExistingDestination();cancellationCleanup();helperAvailabilityTimeoutAndCrash();
        std::cout<<"snapshot worker contract passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
