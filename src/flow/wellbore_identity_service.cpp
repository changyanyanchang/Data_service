#include "store/project_store.h"
#include "store/wellbore_identity_store.h"

namespace paleo::data {
bool confirmSourceWellbore(const std::string& directory, const std::string& storeId,
                          const std::string& generation, const std::string& sourceFingerprint,
                          const WellboreDeclaration& declaration, std::string& error) {
    if (!declaration.source || directory.empty() || storeId.empty() || generation.empty() || sourceFingerprint.empty()) {
        error = "Missing source wellbore confirmation context";
        return false;
    }
    auto lease = tryAcquireModelWriteLease(directory, 0, error);
    if (!lease) return false;
    detail::ProjectStore store;
    if (!store.open(detail::q(directory), false, {}, error, true)) return false;
    const detail::SourceWellboreConfirmation expected{storeId, generation, sourceFingerprint};
    if (!expected.matchesModel(store, error)) return false;
    // Keep the directory lease across reopening and the transaction. This
    // metadata command needs no well-wide coordinate scan or payload parsing.
    if (!store.open(detail::q(directory), false, {}, error)) return false;
    return detail::registerBore(store, declaration, error, &expected);
}
bool DataService::registerWellbore(const WellboreDeclaration& declaration, std::string& error) {
    return detail::registerBore(impl_->store, declaration, error);
}
bool DataService::listWellbores(long long wellId, const std::string& afterKey, int limit,
                              WellborePage& result, std::string& error) const {
    return detail::listBores(impl_->store, wellId, afterKey, limit, result, error);
}
bool DataService::findWellbore(const ExternalWellboreKey& key, std::optional<WellboreRef>& result, std::string& error) const {
    return detail::findBore(impl_->store, key, result, error);
}
bool DataService::findWellbore(const SourceWellboreKey& key, std::optional<WellboreRef>& result, std::string& error) const {
    return detail::findBore(impl_->store, key, result, error);
}
}
