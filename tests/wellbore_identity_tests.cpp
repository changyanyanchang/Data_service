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

static void confirmWholeLasOwner() {
    QTemporaryDir dir;
    const auto project = utf8(dir.filePath("owner-project"));
    std::string error;
    DataService writer;
    check(writer.createProject(project, "owners", error), error);
    const auto saveSource = [&](const QString& name, const std::vector<std::string>& owners) {
        const QString path = dir.filePath(name);
        const QByteArray bytes = "prepared LAS " + name.toUtf8();
        QFile file(path);
        check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write source fixture");
        file.close();
        ParsedFile parsed;
        parsed.kind = "logs";
        parsed.status = "parsed";
        for (const auto& owner : owners) {
            WellDataset well;
            well.wellInfo.wellName = owner;
            LogSet log;
            log.logSetName = "Gamma";
            log.samplingAxis.depthValues = {100, 101};
            LogCurve curve;
            curve.curveName = "GR";
            curve.sampleValues = {12, 13};
            log.logCurves.push_back(curve);
            well.wellbore.logSets.push_back(log);
            parsed.wells.push_back(well);
        }
        long long id = 0;
        const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
        check(writer.importPreparedFile(utf8(path), utf8(path), utf8(path), digest, parsed, id, error), error);
        check(QFile::remove(path), "remove original; only persisted parent records may be consulted");
        return id;
    };
    const auto single = saveSource("A.LAS", {"A"});
    const auto multiple = saveSource("multiple.las", {"A", "B"});
    const auto empty = saveSource("empty.las", {});
    std::vector<long long> owners, both;
    check(writer.listSourceWells(single, owners, error) && owners.size() == 1, "single stored parent");
    check(writer.listSourceWells(multiple, both, error) && both.size() == 2, "two stored parents");
    const long long a = owners.front(), b = both.front() == a ? both.back() : both.front();
    check(writer.listSourceWells(empty, owners, error) && owners.empty(), "no stored parent");
    const ExternalWellboreKey external{"legacy", "field", "external-b"};
    WellboreDeclaration wrong{{{b, "branch-b"}, "B branch"}, external, SourceWellboreKey{single, "file"}};
    WellboreDeclaration correct{{{a, "branch-a"}, "A branch"}, {}, SourceWellboreKey{single, "file"}};
    const auto confirm = [&](const WellboreDeclaration& declaration) {
        SourceIndexEntry source;
        ModelChangeHead head;
        check(writer.readSourceIndexEntry(declaration.source->sourceId, source, error), error);
        check(writer.readModelChangeHead(head, error), error);
        return confirmSourceWellbore(project, head.storeId, head.generation, source.sha256, declaration, error);
    };
    std::optional<WellboreRef> found;
    for (const auto id : {single, multiple, empty}) {
        wrong.source->sourceId = id;
        check(!confirm(wrong) && error == "SourceWellMismatch", "wrong, ambiguous or absent parent rejected");
        check(!writer.registerWellbore(wrong, error) && error == "SourceWellMismatch", "all LAS file writes check the parent");
        check(writer.findWellbore(*wrong.source, found, error) && !found, "rejected file left no binding");
    }
    auto firstOwner = correct;
    firstOwner.source->sourceId = multiple;
    firstOwner.wellbore.ref.wellId = both.front();
    check(!confirm(firstOwner) && error == "SourceWellMismatch", "first of two stored parents is still ambiguous");
    WellborePage page;
    check(writer.findWellbore(external, found, error) && !found, "rejected confirmation left no external identity");
    check(writer.listWellbores(b, {}, 10, page, error) && page.items.size() == 1 && !page.items[0].declared,
          "rejected confirmation left no bore");

    // Existing bindings from before parent validation can be corrected only by
    // explicitly confirming the actual parent of this file.
    wrong.source->sourceId = single;
    auto oldBore = wrong;
    oldBore.source.reset();
    check(writer.registerWellbore(oldBore, error), error);
    auto otherMember = oldBore;
    otherMember.source = SourceWellboreKey{single, "legacy-member"};
    check(confirm(otherMember), "named members retain their own declared parent");
    auto otherFile = oldBore;
    otherFile.source = SourceWellboreKey{saveSource("bundle.txt", {"A", "B"}), "file"};
    check(confirm(otherFile), "non-LAS bindings retain their declaration semantics");
    writer.closeProject();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "old-cross-well-binding");
        db.setDatabaseName(QString::fromStdString(project) + "/data.sqlite");
        check(db.open(), "open historical fixture");
        QSqlQuery row(db);
        check(row.prepare("INSERT INTO wellbore_source_bindings(source_id,member_key,well_id,bore_key) VALUES(?,'file',?,'branch-b')"),
              "prepare historical binding fixture");
        row.addBindValue(single); row.addBindValue(b);
        check(row.exec(), "seed binding written by the historical version");
    }
    QSqlDatabase::removeDatabase("old-cross-well-binding");
    check(writer.openProject(project, error), error);
    check(!confirm(wrong) && error == "SourceWellMismatch", "legacy wrong parent cannot be reaffirmed");
    check(!writer.registerWellbore(correct, error), "generic registration cannot overwrite a bound file");
    auto conflictingExternal = correct;
    conflictingExternal.external = external;
    check(!confirm(conflictingExternal), "repair cannot move an existing external identity");
    check(writer.findWellbore(*wrong.source, found, error) && found == wrong.wellbore.ref, "failed repair rolled back");
    check(confirm(correct), error);
    ModelChangeHead repaired, repeat;
    check(writer.readModelChangeHead(repaired, error) && confirm(correct) && writer.readModelChangeHead(repeat, error) &&
          repeat.sequence == repaired.sequence, "confirming the repaired parent is idempotent");
    auto otherBore = correct;
    otherBore.wellbore = {{a, "other"}, "Another A bore"};
    check(!confirm(otherBore) && error.starts_with("WellboreConflict"), "same-parent bore conflict is preserved");
    writer.closeProject();
    DataService reader;
    check(reader.openProjectReadOnly(project, error), error);
    check(reader.findWellbore(*correct.source, found, error) && found == correct.wellbore.ref, "repaired file after reopen");
    check(reader.findWellbore(external, found, error) && found == wrong.wellbore.ref, "original external binding preserved");
    check(reader.findWellbore(*otherMember.source, found, error) && found == wrong.wellbore.ref, "other member preserved");
    check(reader.listWellbores(b, {}, 10, page, error) && page.items.size() == 2, "original declared bore preserved");
}

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
    try { runContract(); confirmWholeLasOwner(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
