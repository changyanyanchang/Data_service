#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace paleo::data;
static void check(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}
static std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }

static void confirmWhileLegacyWriterCommits(const std::string& project, long long wellId, long long sourceId) {
    DataService reader;
    std::string error;
    check(reader.openProjectReadOnly(project, error), error);
    ModelChangeHead before, after;
    SourceIndexEntry source;
    check(reader.readModelChangeHead(before, error) && reader.readSourceIndexEntry(sourceId, source, error), error);
    WellboreDeclaration branch{{{wellId, "legacy-race"}, "Legacy race"}, {},
                               SourceWellboreKey{sourceId, "legacy-member"}};

    // ProjectStore numbers successive Qt connections. Once the writable connection
    // appears, the read-only check has completed. SQLite still holds our legacy write.
    const auto connections = QSqlDatabase::connectionNames().filter("paleo_data_");
    check(connections.size() == 1, "single review reader before concurrent confirmation");
    const auto readerNumber = connections.front().section('_', -1).toLongLong();
    const auto writableConnection = QStringLiteral("paleo_data_%1").arg(readerNumber + 2);
    bool accepted = false;
    std::string decision;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy-wellbore-writer");
        db.setDatabaseName(QString::fromStdString(project) + "/data.sqlite");
        check(db.open(), "open legacy database connection");
        QSqlQuery query(db);
        check(query.exec("BEGIN IMMEDIATE") &&
              query.exec("UPDATE wells SET remark='changed by legacy client'"), "start legacy change");
        std::jthread confirmation([&] {
            accepted = confirmSourceWellbore(project, before.storeId, before.generation, source.sha256, branch, decision);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!QSqlDatabase::contains(writableConnection) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool reachedWritableOpen = QSqlDatabase::contains(writableConnection);
        check(query.exec("COMMIT"), "commit legacy change");
        confirmation.join();
        check(reachedWritableOpen, "confirmation reached writable open after read-only preflight");
    }
    QSqlDatabase::removeDatabase("legacy-wellbore-writer");
    check(!accepted && decision == "StaleModelGeneration", "old generation confirmation rejected after rebuild");
    check(reader.readModelChangeHead(after, error) && before.generation != after.generation, "legacy edit rebuilt the generation");
    std::optional<WellboreRef> found;
    check(reader.findWellbore(*branch.source, found, error) && !found, "stale confirmation left no binding");
    check(confirmSourceWellbore(project, after.storeId, after.generation, source.sha256, branch, error), error);
    check(reader.findWellbore(*branch.source, found, error) && found == branch.wellbore.ref, "fresh confirmation persisted");
}

static void runContract() {
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory");
    const auto project = utf8(dir.filePath("工区"));
    std::string error;
    long long wellId = 0, sourceId = 0;
    {
        DataService writer;
        check(writer.createProject(project, "wellbores", error), error);
        WellInfo well; well.wellName = "A-1";
        check(writer.createWell(well, wellId, error), error);
        QFile source(dir.filePath("来源.txt"));
        const QByteArray bytes("two independently identified worksheet members");
        check(source.open(QIODevice::WriteOnly) && source.write(bytes) == bytes.size(), "write source"); source.close();
        ParsedFile parsed; parsed.kind = "synthetic"; parsed.status = "parsed";
        WellDataset dataset; dataset.wellInfo = well; parsed.wells.push_back(dataset);
        const auto path = utf8(source.fileName());
        const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
        check(writer.importPreparedFile(path, path, path, digest, parsed, sourceId, error), error);
    }
    const WellboreRef main{wellId, "main"}, first{wellId, "side-a"}, second{wellId, "side-b"};
    const ExternalWellboreKey external{"system", "project-a", "same-object"};
    const SourceWellboreKey member{sourceId, "sheet:1"};
    WellboreDeclaration branch{{first, "重名分支"}, external, member};
    WellborePage page;
    std::optional<WellboreRef> found;
    {
        DataService legacy;
        check(legacy.openProjectReadOnly(project, error), error);
        check(legacy.listWellbores(wellId, {}, 1, page, error), error);
        check(page.items.size() == 1 && page.items[0].ref == main &&
              page.items[0].displayName == "主井筒" && page.nextKey.empty(), "legacy main key");
        check(legacy.findWellbore(member, found, error) && !found, "legacy file did not declare a bore member");
        check(!legacy.registerWellbore(branch, error), "read-only registration");
    }
    {
        DataService writer;
        check(writer.openProject(project, error), error);
        ModelChangeHead before, after, repeat;
        check(writer.readModelChangeHead(before, error), error);
        SourceIndexEntry source;
        check(writer.readSourceIndexEntry(sourceId, source, error), error);
        check(!confirmSourceWellbore(project, before.storeId, "another-generation", source.sha256, branch, error) &&
              error == "StaleModelGeneration", "stale generation must not receive the binding");
        check(!confirmSourceWellbore(project, before.storeId, before.generation, "changed", branch, error) &&
              error == "SourceRevisionChanged", "stale source must not receive the binding");
        check(confirmSourceWellbore(project, before.storeId, before.generation, source.sha256, branch, error), error);
        check(writer.readModelChangeHead(after, error) && after.sequence > before.sequence, "identity change published");
        check(writer.registerWellbore(branch, error) && writer.readModelChangeHead(repeat, error) &&
              repeat.sequence == after.sequence, "repeat does not create a new model change");
        auto conflict = branch;
        conflict.wellbore.ref = second;
        conflict.external->project = "other-project";
        check(!writer.registerWellbore(conflict, error) && error.find("WellboreConflict") != std::string::npos,
              "source member already belongs to first branch");
        check(writer.findWellbore(*conflict.external, found, error) && !found, "external registration was rolled back");
        check(writer.listWellbores(wellId, {}, 10, page, error) && page.items.size() == 2, "conflicting branch was rolled back");
        conflict.source->member = "sheet:2";
        check(writer.registerWellbore(conflict, error), error);
        check(writer.findWellbore(*conflict.external, found, error) && found == second, "external project scopes the key");
        conflict.external = external;
        check(!writer.registerWellbore(conflict, error) && error.find("WellboreConflict") != std::string::npos,
              "external identity cannot switch branches");
        branch.wellbore.displayName = "different";
        check(!writer.registerWellbore(branch, error), "same stable key with contradictory name");
        auto missing = branch; missing.source->sourceId += 1000;
        check(!writer.registerWellbore(missing, error), "missing source");
        check(!writer.listWellbores(wellId + 10000, {}, 1, page, error), "missing parent well");
        check(!writer.listWellbores(wellId, {}, 0, page, error), "invalid limit");
    }
    {
        DataService reader;
        check(reader.openProjectReadOnly(project, error), error);
        check(reader.findWellbore(external, found, error) && found == first, "read external after reopening");
        check(reader.findWellbore(member, found, error) && found == first, "read source member after reopening");
        check(reader.listWellbores(wellId, {}, 2, page, error) && page.items.size() == 2 &&
              page.items[0].ref == main && page.items[1].ref == first && page.nextKey == first.key, "first page");
        check(!page.items[0].declared && page.items[1].declared, "implicit main and confirmed branch");
        check(reader.listWellbores(wellId, page.nextKey, 2, page, error) && page.items.size() == 1 &&
              page.items[0].ref == second && page.items[0].displayName == "重名分支" && page.nextKey.empty(), "second page");
        WellInfo old;
        check(reader.readWellInfo(wellId, old, error) && old.wellName == "A-1", "legacy well API");
    }
    confirmWhileLegacyWriterCommits(project, wellId, sourceId);
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "future-wellbore");
        db.setDatabaseName(dir.filePath("工区/data.sqlite"));
        check(db.open(), "open version fixture");
        QSqlQuery version(db);
        check(version.exec("UPDATE ds_meta SET value=2 WHERE key='wellbore_identity_schema'"), "future schema fixture");
    }
    QSqlDatabase::removeDatabase("future-wellbore");
    DataService future;
    check(future.openProjectReadOnly(project, error), error);
    check(!future.listWellbores(wellId, {}, 1, page, error) && error.find("Unsupported") != std::string::npos,
          "future schema is not interpreted as legacy");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try { runContract(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
