#include "store/project_store.h"
#include "store/wellbore_identity_store.h"

namespace paleo::data {
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
