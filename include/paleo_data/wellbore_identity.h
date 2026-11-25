#pragma once
#include <optional>
#include <string>
#include <vector>

namespace paleo::data {
// The well ID belongs to this model store. Every existing well has a stable
// "main" key, including read-only projects created before this API.
struct WellboreRef {
    long long wellId = 0;
    std::string key;
    bool operator==(const WellboreRef&) const = default;
};
struct WellboreIdentity {
    WellboreRef ref;
    std::string displayName;
    bool declared = false; // Output: false for the implicit legacy main bore.
};
struct ExternalWellboreKey {
    std::string system, project, object;
};
struct SourceWellboreKey {
    long long sourceId = 0;
    // A stable member within this source, e.g. a worksheet/record group.
    // A caller can declare "file" for an explicitly single-bore source.
    std::string member;
};
struct WellboreDeclaration {
    WellboreIdentity wellbore;
    std::optional<ExternalWellboreKey> external;
    std::optional<SourceWellboreKey> source;
};
// Confirm an indexed source in a specific model generation. The fingerprint
// is the persisted source revision, including its prefix for a quick digest.
bool confirmSourceWellbore(const std::string& directory, const std::string& storeId,
                           const std::string& generation, const std::string& sourceFingerprint,
                           const WellboreDeclaration& declaration, std::string& error);
struct WellborePage {
    std::vector<WellboreIdentity> items;
    // Empty at the last page; otherwise pass unchanged to the next request.
    std::string nextKey;
};
}
