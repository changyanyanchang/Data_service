#pragma once
#include "paleo_data/interpretation.h"
#include <string>
#include <vector>

namespace paleo::stratigraphy {
using namespace paleo::data;

struct DerivedResult {
    std::vector<InterpretationSeries> series;
    std::vector<std::string> warnings;
};
struct CycleEvent {
    double md=Missing, score=0;
    std::string evidenceJson;
};
struct SegmentationResult {
    std::vector<InterpretationBoundary> boundaries;
    std::vector<InterpretationCycle> cycles;
    std::vector<CycleEvent> events;
    std::vector<InterpretationSeries> usedSeries;
    std::vector<std::string> warnings;
};
struct GeometryOptions {
    // Explicit positive-down canonical TWT milliseconds. No sign is inferred.
    double horizonTimeSign=1.0, wellTimeSign=1.0;
    double horizonTimeOffsetMs=0.0, wellTimeOffsetMs=0.0;
    bool allowOneWayConversion=false;
    // Empty means matching nonempty source metadata is mandatory.
    std::string confirmedCoordinateSystem, confirmedCoordinateUnit;
    std::string confirmedTimeReference;
    std::string confirmedMdReference;
    std::string confirmedWellTimeUnit, confirmedHorizonTimeUnit;
    std::string confirmedWellTimeType, confirmedHorizonTimeType;
    std::string confirmedDepthUnit;
    std::string confirmedDepthType; // Time-depth table basis, e.g. MD or TVD.
    std::string confirmedLogDepthType; // Needed when the log axis basis is unknown.
    std::string confirmedVerticalDepthReference;
    // Retained for source compatibility with the early sampled solver. The v2
    // piecewise-linear triangle solver is exact on its representation and does
    // not use these to skip samples or merge distinct intersections.
    double searchStepMd=2.0, intersectionToleranceMd=0.01;
    double maximumSurfaceEdge=Missing;
    // Opt-in business-layer resolution. The pure core remains strict; source
    // values are never rewritten. Provisional alignment produces review drafts.
    bool resolveFromSources=false;
    bool allowProvisionalAlignment=false;
    std::string policyEvidence;
    bool allowIrregularSurface=false;
    // Optional endpoint extension of an MD/TWT relation on calculation copies.
    // Missing XY positions follow the corresponding observed endpoint tangent;
    // horizon space and GR values are never extrapolated. Every extended pick
    // remains a review candidate with quantitative evidence.
    bool allowBottomExtrapolation=false;
    // Zero removes this TD/trajectory distance cap; the effective GR bounds
    // plus maximumLogExtrapolationM still provide a finite calculation extent.
    double maximumBottomExtrapolationM=500.0;
    double bottomExtrapolationFitWindowM=200.0;
    bool allowTopExtrapolation=false;
    double maximumTopExtrapolationM=0.0;
    double topExtrapolationFitWindowM=200.0;
    // Expand only an explicitly enabled side, measured from first/last valid
    // GR samples (finite values, not missing/unstable quality). Zero preserves
    // the earlier opt-in bottom behavior; the business layer may select 100 m.
    double maximumLogExtrapolationM=0.0;
};
struct HorizonIntersectionResult {
    std::vector<InterpretationBoundary> boundaries;
    std::vector<std::string> warnings;
};

// All functions consume owned public data objects, never files or a database.
// Invalid conventions/inputs throw std::invalid_argument; no feasible solution
// throws std::runtime_error. Raw input objects are never changed.
std::vector<double> burgPace(const std::vector<double>& values, int order,
                            const std::string& direction="reverse");
std::vector<double> cosinePhase(const std::vector<double>& values, bool invert=true);
DerivedResult deriveCurves(const InterpretationSeries& input,
                          const CorrelationAlgorithmProfile& profile,
                          double topMd, double bottomMd);
std::vector<CycleEvent> detectCycleEvents(const std::vector<InterpretationSeries>& series,
                                        double topMd, double bottomMd,
                                        const CorrelationAlgorithmProfile& profile);
InterpretationTemplate extractTemplate(const WellInterpretation& reference,
                                      const std::string& id, const std::string& version,
                                      double sourceReconciliationToleranceM=0.0);
SegmentationResult divideSegment(const WellInterpretation& well,
                                 const InterpretationTemplate& reference,
                                 const std::string& segmentId,
                                 const CorrelationAlgorithmProfile& profile);
InterpretationSeries fischerCdmt(const WellInterpretation& well,
                                const std::string& segmentId, int level=4);
std::vector<std::string> validateInterpretation(const WellInterpretation& well);
// Validates before committing; on rejection the caller's object is unchanged.
void moveBoundary(WellInterpretation& well, const std::string& boundaryId, double md);
std::vector<TrajectoryPosition> buildTrajectory(const CorrelationWellInput& well);
WellInterpretation makeManualInterpretation(const CorrelationWellInput& input,
                                           const GeometryOptions& options={});
InterpretationClustering clusterWells(const std::vector<WellInterpretation>& wells,
                                     int groupCount);
HorizonIntersectionResult intersectHorizons(const CorrelationWellInput& well,
                                            const std::vector<CorrelationHorizon>& horizons,
                                            const GeometryOptions& options={});
}
