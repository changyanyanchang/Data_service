#include <paleo_data/catalog.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
static void check(bool ok,const std::string& message) {
    if(!ok) throw std::runtime_error(message);
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir directory;
        check(directory.isValid(),"temporary directory");
        const auto path=directory.filePath("catalog.sqlite");
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","catalog_contract");
            db.setDatabaseName(path); check(db.open(),db.lastError().text().toStdString());
            QSqlQuery sql(db);
            for(const auto& statement:QString::fromUtf8(CatalogStorage::schemaSql()).split(';',Qt::SkipEmptyParts))
                check(sql.exec(statement),sql.lastError().text().toStdString());
            CatalogStorage store("catalog_contract"); std::string error;
            CatalogEntity well; well.id="well-1"; well.entityType="well";
            well.name="井-一"; well.extraJson=R"({"unknown":{"keep":true}})";
            CatalogAsset asset{"ast-1","well_log","las","输入.las"};
            CatalogVersion version; version.id="ver-1"; version.assetId=asset.id;
            version.path="artifacts/RAW/ast-1/ver-1/输入.las";
            version.parentVersionIdsJson=R"(["earlier-version"])";
            CatalogLink link; link.entityType="well"; link.entityId=well.id;
            link.assetId=asset.id; link.role="well_log";
            check(db.transaction(),"begin");
            check(store.upsert(well,error),error); check(store.upsert(asset,error),error);
            check(store.upsert(version,error),error); check(store.upsert(link,error),error);
            check(store.writeMeta({{"catalog_revision","1"}},error),error);
            check(db.commit(),"commit");
            CatalogSnapshot snapshot;
            check(store.load(snapshot,error),error);
            check(snapshot.entities.size()==1&&snapshot.entities[0].name==well.name&&
                  snapshot.entities[0].extraJson==well.extraJson,"UTF-8 and unknown metadata");
            check(snapshot.versions.size()==1&&snapshot.versions[0].parentVersionIdsJson==version.parentVersionIdsJson,
                  "version identity and lineage");
            check(db.transaction(),"begin rollback"); well.name="discard";
            check(store.upsert(well,error),error); check(store.eraseAsset(asset.id,error),error);
            check(db.rollback(),"rollback");
            check(store.load(snapshot,error)&&snapshot.assets.size()==1&&snapshot.entities[0].name!="discard",
                  "rollback preserves the complete previous state");
            db.close(); check(db.open(),"reopen");
            check(store.load(snapshot,error)&&snapshot.links.size()==1,"reopen retains link");
            db.close();
        }
        QSqlDatabase::removeDatabase("catalog_contract");
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","catalog_readonly");
            db.setDatabaseName(path); db.setConnectOptions("QSQLITE_OPEN_READONLY");
            check(db.open(),"readonly open"); CatalogStorage store("catalog_readonly");
            std::string error; CatalogSnapshot snapshot;
            check(store.load(snapshot,error)&&snapshot.entities.size()==1,"readonly load");
            check(!store.clearRecords(error)&&!error.empty(),"readonly write is rejected");
            check(store.load(snapshot,error)&&snapshot.entities.size()==1,"rejected write retains data");
        }
        QSqlDatabase::removeDatabase("catalog_readonly");
        std::string error; CatalogSnapshot snapshot;
        check(!CatalogStorage("not-open").load(snapshot,error),"missing connection never implicitly opens");
        std::cout<<"catalog storage contract passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
