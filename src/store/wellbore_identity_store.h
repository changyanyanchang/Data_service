#pragma once
#include "paleo_data/wellbore_identity.h"

namespace paleo::data::detail {
class ProjectStore;
struct SourceWellboreConfirmation {
    const std::string& storeId;
    const std::string& generation;
    const std::string& fingerprint;
    bool matchesModel(const ProjectStore&, std::string& error) const;
};
bool registerBore(ProjectStore&, const WellboreDeclaration&, std::string&, const SourceWellboreConfirmation* expected = nullptr);
bool listBores(const ProjectStore&, long long, const std::string&, int, WellborePage&, std::string&);
bool findBore(const ProjectStore&, const ExternalWellboreKey&, std::optional<WellboreRef>&, std::string&);
bool findBore(const ProjectStore&, const SourceWellboreKey&, std::optional<WellboreRef>&, std::string&);
}
