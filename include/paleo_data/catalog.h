#pragma once
#include <string>
#include <utility>
#include <vector>

namespace paleo::data {
// Lossless artifact/version catalog, complementary to the typed survey objects.
// IDs belong to the host; filenames, CRS and geological semantics are never inferred.
// JSON columns are preserved verbatim so host extensions survive round trips.
struct CatalogEntity {
    std::string id, entityType, name;
    double surfaceX=0, surfaceY=0;
    bool hasSurface=false;
    double kb=0, td=0;
    std::string coordinateStatus;
    double inlineMin=0, inlineMax=0, xlineMin=0, xlineMax=0;
    double sampleIntervalUs=0, startTimeMs=0;
    std::string cornersJson="[]", extraJson="{}";
};
struct CatalogAsset { std::string id, type, format, displayName; };
struct CatalogVersion {
    std::string id, assetId, stage;
    int versionNumber=1;
    bool managed=true;
    std::string path, sourceUri, sha256, fileName;
    std::string parentVersionIdsJson="[]", extraJson="{}";
};
struct CatalogLink {
    int ord=0;
    std::string entityType, entityId, assetId, role;
    bool isPrimary=true, unresolved=false;
    int ordinal=0;
    std::string note;
};
struct CatalogSnapshot {
    std::vector<CatalogEntity> entities;
    std::vector<CatalogAsset> assets;
    std::vector<CatalogVersion> versions;
    std::vector<CatalogLink> links;
    std::vector<std::pair<std::string,std::string>> meta;
};

// Qt-SQL embedding adapter. Borrows an already-open named QSQLITE connection on
// the calling thread; does not reopen it, change its access mode, own its lifetime,
// migrate files, or commit the host's transaction. This permits atomic host
// journals and existing backup/recovery policies without a second writable copy.
// Public records and errors remain standard C++/UTF-8. No workstation dependency.
class CatalogStorage {
public:
    explicit CatalogStorage(std::string qtConnectionName);
    static const char* schemaSql(); // existing compatible schema; no implicit DDL
    bool load(CatalogSnapshot& result, std::string& error) const;
    bool upsert(const CatalogEntity& value, std::string& error) const;
    bool upsert(const CatalogAsset& value, std::string& error) const;
    bool upsert(const CatalogVersion& value, std::string& error) const;
    bool upsert(const CatalogLink& value, std::string& error) const;
    bool eraseAsset(const std::string& id, std::string& error) const;
    bool eraseVersion(const std::string& id, std::string& error) const;
    bool clearLinks(std::string& error) const;
    bool clearRecords(std::string& error) const;
    bool writeMeta(const std::vector<std::pair<std::string,std::string>>& rows,
                   std::string& error) const;
private:
    std::string connection_;
};
}
