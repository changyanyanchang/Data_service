#pragma once
#include "paleo_data/wellbore_identity.h"

namespace paleo::data::detail {
class ProjectStore;
bool registerBore(ProjectStore&, const WellboreDeclaration&, std::string&);
bool listBores(const ProjectStore&, long long, const std::string&, int, WellborePage&, std::string&);
bool findBore(const ProjectStore&, const ExternalWellboreKey&, std::optional<WellboreRef>&, std::string&);
bool findBore(const ProjectStore&, const SourceWellboreKey&, std::optional<WellboreRef>&, std::string&);
}
