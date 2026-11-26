#include "wellbore_identity_store.h"
#include "model_change_journal.h"
#include "project_store.h"

namespace paleo::data::detail {
namespace {
bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}
bool validText(const std::string& text) {
    if (text.empty() || text.size() > 512 || text.find('\0') != std::string::npos) return false;
    const auto value = q(text);
    return value.trimmed() == value && s(value) == text;
}
bool valid(const ExternalWellboreKey& key) {
    return validText(key.system) && validText(key.project) && validText(key.object);
}
bool valid(const SourceWellboreKey& key) { return key.sourceId > 0 && validText(key.member); }

// A missing marker is a legacy project. Reading it never creates tables.
bool schemaPresent(const ProjectStore& store, bool& present, std::string& error) {
    QSqlQuery row;
    if (!store.exec("SELECT value FROM ds_meta WHERE key='wellbore_identity_schema'", {}, error, &row)) return false;
    present = row.next();
    return !present || row.value(0).toString() == "1" || fail(error, "Unsupported wellbore identity schema");
}
bool createSchema(ProjectStore& store, std::string& error) {
    const QStringList statements{
        "CREATE TABLE wellbore_identities(well_id INTEGER NOT NULL REFERENCES wells(id) ON DELETE CASCADE,"
            "bore_key TEXT NOT NULL,display_name TEXT NOT NULL,PRIMARY KEY(well_id,bore_key))",
        "CREATE TABLE wellbore_external_bindings(system_key TEXT NOT NULL,project_key TEXT NOT NULL,object_key TEXT NOT NULL,"
            "well_id INTEGER NOT NULL,bore_key TEXT NOT NULL,PRIMARY KEY(system_key,project_key,object_key),"
            "FOREIGN KEY(well_id,bore_key) REFERENCES wellbore_identities(well_id,bore_key) ON DELETE CASCADE)",
        "CREATE TABLE wellbore_source_bindings(source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,member_key TEXT NOT NULL,"
            "well_id INTEGER NOT NULL,bore_key TEXT NOT NULL,PRIMARY KEY(source_id,member_key),"
            "FOREIGN KEY(well_id,bore_key) REFERENCES wellbore_identities(well_id,bore_key) ON DELETE CASCADE)",
        "INSERT INTO ds_meta(key,value) VALUES('wellbore_identity_schema','1')"
    };
    for (const auto& statement : statements) if (!store.exec(statement, {}, error)) return false;
    return true;
}
bool requireWell(const ProjectStore& store, long long wellId, std::string& error) {
    QSqlQuery row;
    if (!store.exec("SELECT 1 FROM wells WHERE id=?", {wellId}, error, &row)) return false;
    return row.next() || fail(error, "Unknown parent well");
}
struct BindingQuery {
    QString select, insert;
    QVariantList values;
};
BindingQuery bindingQuery(const ExternalWellboreKey& key) {
    return {"SELECT well_id,bore_key FROM wellbore_external_bindings WHERE system_key=? AND project_key=? AND object_key=?",
            "INSERT INTO wellbore_external_bindings(system_key,project_key,object_key,well_id,bore_key) VALUES(?,?,?,?,?)",
            {q(key.system), q(key.project), q(key.object)}};
}
BindingQuery bindingQuery(const SourceWellboreKey& key) {
    return {"SELECT well_id,bore_key FROM wellbore_source_bindings WHERE source_id=? AND member_key=?",
            "INSERT INTO wellbore_source_bindings(source_id,member_key,well_id,bore_key) VALUES(?,?,?,?)",
            {key.sourceId, q(key.member)}};
}
bool lookup(const ProjectStore& store, const BindingQuery& query, std::optional<WellboreRef>& result, std::string& error) {
    QSqlQuery row;
    if (!store.exec(query.select, query.values, error, &row)) return false;
    if (row.next()) result = WellboreRef{row.value(0).toLongLong(), s(row.value(1).toString())};
    return true;
}
bool bind(ProjectStore& store, BindingQuery query, const WellboreRef& target, std::string& error) {
    std::optional<WellboreRef> previous;
    if (!lookup(store, query, previous, error)) return false;
    if (previous) return *previous == target || fail(error, "WellboreConflict: identity already bound to another bore");
    query.values << target.wellId << q(target.key);
    return store.exec(query.insert, query.values, error);
}
bool declareBore(ProjectStore& store, const WellboreIdentity& bore, std::string& error) {
    QSqlQuery existing;
    if (!store.exec("SELECT display_name FROM wellbore_identities WHERE well_id=? AND bore_key=?",
                    {bore.ref.wellId, q(bore.ref.key)}, error, &existing)) return false;
    if (existing.next()) return s(existing.value(0).toString()) == bore.displayName ||
        fail(error, "WellboreConflict: bore key already has a different declaration");
    existing.finish();
    return store.exec("INSERT INTO wellbore_identities(well_id,bore_key,display_name) VALUES(?,?,?)",
                      {bore.ref.wellId, q(bore.ref.key), q(bore.displayName)}, error);
}
template<typename Key>
bool readBinding(const ProjectStore& store, const Key& key, std::optional<WellboreRef>& result, std::string& error) {
    result.reset();
    error.clear();
    if (!valid(key)) return fail(error, "Invalid wellbore lookup key");
    bool present = false;
    if (!schemaPresent(store, present, error)) return false;
    return !present || lookup(store, bindingQuery(key), result, error);
}
}

bool SourceWellboreConfirmation::matchesModel(const ProjectStore& store, std::string& error) const {
    ModelChangeHead head;
    if (!readJournalHead(store, head, error)) return false;
    return (head.available && head.storeId == storeId && head.generation == generation) ||
        fail(error, "StaleModelGeneration");
}

bool registerBore(ProjectStore& store, const WellboreDeclaration& declaration, std::string& error,
                  const SourceWellboreConfirmation* expected) {
    error.clear();
    if (store.readOnly) return fail(error, "Read-only model");
    const auto& bore = declaration.wellbore;
    if (bore.ref.wellId <= 0 || !validText(bore.ref.key) || !validText(bore.displayName) ||
        (declaration.external && !valid(*declaration.external)) || (declaration.source && !valid(*declaration.source)))
        return fail(error, "Invalid wellbore declaration");
    Transaction transaction(store, error);
    if (!transaction.active) return false;
    // Writable open may rebuild the generation after an unmanaged writer commits.
    // Recheck the caller's model under the same SQLite write lock as the binding.
    if (expected && !expected->matchesModel(store, error)) return false;
    if (!requireWell(store, bore.ref.wellId, error)) return false;
    if (declaration.source) {
        QSqlQuery row;
        if (!store.exec("SELECT hash FROM sources WHERE id=? AND active<>0", {declaration.source->sourceId}, error, &row)) return false;
        if (!row.next()) return fail(error, "Unknown or inactive source");
        if (expected && s(row.value(0).toString()) != expected->fingerprint)
            return fail(error, "SourceRevisionChanged");
    }
    bool present = false;
    if (!schemaPresent(store, present, error) || (!present && !createSchema(store, error))) return false;
    if (!declareBore(store, bore, error)) return false;
    if (declaration.external && !bind(store, bindingQuery(*declaration.external), bore.ref, error)) return false;
    if (declaration.source && !bind(store, bindingQuery(*declaration.source), bore.ref, error)) return false;
    return transaction.commit(error);
}
bool listBores(const ProjectStore& store, long long wellId, const std::string& afterKey, int limit,
               WellborePage& result, std::string& error) {
    // The caller may pass result.nextKey back into this same page object.
    const std::string after = afterKey;
    result = {};
    error.clear();
    if (wellId <= 0 || (!after.empty() && !validText(after)) || limit < 1 || limit > 512)
        return fail(error, "Invalid wellbore page request");
    bool present = false;
    if (!schemaPresent(store, present, error) || !requireWell(store, wellId, error)) return false;
    if (!present) {
        if (after < "main") result.items.push_back({{wellId, "main"}, "主井筒"});
        return true;
    }
    // Older wells need no row backfill. An explicit main-bore declaration
    // takes precedence over the legacy display label without changing its key.
    QSqlQuery rows;
    const auto cursor = after.empty() ? QStringLiteral("") : q(after);
    const auto sql = "SELECT bore_key,display_name,declared FROM ("
        "SELECT bore_key,display_name,1 AS declared FROM wellbore_identities WHERE well_id=? AND bore_key>? "
        "UNION ALL SELECT 'main','主井筒',0 WHERE 'main'>? AND NOT EXISTS("
        "SELECT 1 FROM wellbore_identities WHERE well_id=? AND bore_key='main')) ORDER BY bore_key LIMIT ?";
    if (!store.exec(QString::fromUtf8(sql), {wellId, cursor, cursor, wellId, limit + 1}, error, &rows)) return false;
    while (rows.next()) {
        if (int(result.items.size()) == limit) {
            result.nextKey = result.items.back().ref.key;
            break;
        }
        result.items.push_back({{wellId, s(rows.value(0).toString())}, s(rows.value(1).toString()), rows.value(2).toBool()});
    }
    return true;
}
bool findBore(const ProjectStore& store, const ExternalWellboreKey& key, std::optional<WellboreRef>& result, std::string& error) {
    return readBinding(store, key, result, error);
}
bool findBore(const ProjectStore& store, const SourceWellboreKey& key, std::optional<WellboreRef>& result, std::string& error) {
    return readBinding(store, key, result, error);
}
}
