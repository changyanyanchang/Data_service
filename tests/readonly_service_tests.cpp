#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
static void check(bool ok,const std::string& message) { if(!ok)throw std::runtime_error(message); }
static std::string u(const QString& value) { return value.toUtf8().toStdString(); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir directory; check(directory.isValid(),"temporary directory");
        const auto project=directory.filePath("工区"); std::string error;
        long long wellId=0;
        {
            DataService writer; check(writer.createProject(u(project),"read-only contract",error),error);
            WellInfo well; well.wellName="SYNTH-1";
            check(writer.createWell(well,wellId,error),error);
        }
        // Simulate an older additive schema. Read-only open must not upgrade it.
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","readonly_schema");
            db.setDatabaseName(QDir(project).filePath("data.sqlite"));
            check(db.open(),"open old schema fixture"); QSqlQuery sql(db);
            check(sql.exec("DROP TABLE source_scan_cache"),"remove additive table");
            check(sql.exec("DELETE FROM ds_meta WHERE key='import_performance_schema'"),"remove additive marker");
        }
        QSqlDatabase::removeDatabase("readonly_schema");
        DataService reader;
        check(reader.openProjectReadOnly(u(project),error)&&reader.isReadOnly(),error);
        WellInfo before; check(reader.readWellInfo(wellId,before,error),error);
        WellInfo changed=before; changed.remark="must not persist";
        check(!reader.saveWellInfo(wellId,changed,error)&&!error.empty(),"readonly edit rejected");
        const auto input=directory.filePath("input.las"); QFile file(input);
        check(file.open(QIODevice::WriteOnly),"fixture open");
        file.write("~Version\nVERS. 2.0\n~Well\nWELL. SYNTH-1\n~Curve\nDEPT.M\nGR.API\n~Ascii\n1 42\n"); file.close();
        const auto filesBefore=QDir(project).entryList(QDir::AllEntries|QDir::NoDotAndDotDot);
        ImportOptions options; ImportReport report;
        check(!reader.importFile(u(input),options,report,error)&&report.seen==0,"readonly import rejected before staging");
        check(!reader.importDirectory(u(directory.path()),options,report,error),"readonly directory import rejected");
        check(!reader.reimportSource(1,{},report,error),"readonly reimport rejected");
        check(QDir(project).entryList(QDir::AllEntries|QDir::NoDotAndDotDot)==filesBefore,"no staged source copies");
        reader.closeProject();
        DataService reopened; check(reopened.openProjectReadOnly(u(project),error),error);
        WellInfo after; check(reopened.readWellInfo(wellId,after,error)&&after.remark==before.remark,"reopen retains original data");
        check(!reader.openProjectReadOnly(u(directory.filePath("missing")),error),"missing project rejected");
        check(!QFile::exists(directory.filePath("missing")),"readonly open never creates a project");
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","readonly_schema_check");
            db.setDatabaseName(QDir(project).filePath("data.sqlite"));
            db.setConnectOptions("QSQLITE_OPEN_READONLY"); check(db.open(),"check schema");
            QSqlQuery sql(db);
            check(sql.exec("SELECT name FROM sqlite_master WHERE name='source_scan_cache'")&&!sql.next(),"read-only open did not migrate schema");
        }
        QSqlDatabase::removeDatabase("readonly_schema_check");
        std::cout<<"readonly service contract passed\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
