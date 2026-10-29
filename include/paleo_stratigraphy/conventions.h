#pragma once
#include "core.h"

namespace paleo::stratigraphy {
struct ConventionResolution {
    GeometryOptions geometry;
    std::string evidenceJson;
    bool reviewRequired=false;
    std::vector<std::string> messages;
    // Candidate surfaces outside observed coordinate/time support are omitted
    // independently, so one missing deep interface cannot block a shallow one.
    std::vector<long long> excludedHorizonIds;
};
// Resolves a calculation copy, never changes source metadata or numeric arrays.
// Explicit contradictions and insufficient evidence throw invalid_argument.
// requireSpatial=false supports curves/manual tracks without requiring seismic
// coordinates or a time-depth table. Opt-in is controlled by requested flags.
ConventionResolution resolveWellConventions(const CorrelationWellInput& input,
                                            const std::vector<CorrelationHorizon>& horizons,
                                            const GeometryOptions& requested,
                                            bool requireSpatial=true);
}
