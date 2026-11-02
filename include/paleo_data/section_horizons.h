#pragma once
#include "model.h"
#include <string>
#include <vector>

namespace paleo::data {
struct SectionHorizonColumn {
    int inlineNumber=0, crosslineNumber=0;
    bool present=true; // false for a missing seismic trace; never bridge it.
};
struct SectionHorizonQuery {
    long long volumeId=0;
    std::vector<SectionHorizonColumn> columns; // exact displayed order, duplicates retained
    std::string schemeId="default", revision;
    // Explicit display convention for sources without declarations. Empty = skip.
    std::string undeclaredTimeUnit, undeclaredTimeType;
    int interpolationRadius=2; // bounded line-number neighbourhood; 0 = exact only
};
struct SectionHorizonCurve {
    std::string id, name, intervalId;
    long long horizonId=0;
    int internalEdge=0; // 0 = imported major horizon; 1..3 = S1/S2, S2/S3, S3/S4
    bool provisional=false;
    std::vector<double> timesMs; // one value per displayed column; NaN breaks the curve
};
struct SectionHorizonResult {
    std::string schemeId, revision;
    std::vector<SectionHorizonCurve> curves;
    std::vector<std::string> warnings;
};
}
