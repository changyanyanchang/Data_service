#pragma once
#include "model.h"
#include <cstdint>

namespace paleo::data {

// Project-scoped business rules, separate from source metadata and snapshots.
// Empty fields remain unknown. Evidence describes the explicit authority for
// these rules. Provisional alignment permits reviewable candidates only; it
// does not confirm a coordinate system, datum, or common seismic time zero.
struct CorrelationProjectConventions {
    std::string depthType, depthUnit, timeUnit, evidence;
    bool allowProvisionalAlignment=false;
};

// A sourced, project-specific choice of an existing time/depth resource.
// Choosing a relation does not confirm its datum or change any source samples.
struct CorrelationTimeDepthPreference {
    long long wellId=0, timeDepthId=0;
    std::string evidenceJson; // Nonempty JSON object, including selection provenance.
};

// Public, file-independent objects shared by the data SDK and correlation core.
// IDs are stable within a project. Arrays own their storage; missing values are NaN.
struct InterpretationSeries {
    std::string id, name, kind, unit;
    std::string axisKind="MD", axisUnit="m";
    std::vector<double> axis, values;
    std::vector<unsigned char> quality; // 0 valid, 1 missing, 2 interpolated, 4 edge/unstable
    std::string runId, segmentId, parametersJson, inputFingerprint;
    std::string status="current"; // stale retains numeric arrays for history and undo.
};
struct InterpretationBoundary {
    std::string id, name, role, source="manual", status="review";
    double md=Missing, originalMd=Missing, time=Missing;
    bool locked=false;
    std::string evidenceJson;
};
struct InterpretationCycle {
    std::string id, name, segmentId, topBoundaryId, bottomBoundaryId, parentId;
    std::string templateIdentity, templateVersion, source="algorithm", status="review";
    int level=4, direction=0;
};
struct InterpretationEdit {
    std::string id, targetId, action, beforeJson, afterJson, author, created;
};
struct TrajectoryPosition {
    double md=Missing, x=Missing, y=Missing, tvd=Missing, tvdss=Missing;
};
struct CorrelationWellInput {
    long long wellId=0, curveId=0, logSetId=0, timeDepthId=0, trajectoryId=0;
    WellInfo well;
    DepthInfo depthInfo;
    InterpretationSeries log;
    TimeDepthTable timeDepth;
    TrajectorySurvey survey;
    std::vector<TrajectoryPosition> positions;
    std::vector<InterpretationBoundary> markers;
    std::string inputFingerprint, sourceRefsJson;
    bool confirmedVertical=false;
    std::vector<std::string> warnings;
    DepthInfo surveyDepthInfo; // Survey MD uses the well's default depth convention.
    std::vector<double> rawLogValues; // Exact source values, including finite missing sentinels.
    std::vector<unsigned char> sourceQuality; // SDK flags: 1 missing, 2 abnormal, 4 manually edited.
    bool processed=true;
    long long intervalTrackId=0;
    IntervalTrack manualTrack; // Selected manual interpretation, including all properties and its depth convention.
    long long formationTrackId=0;
    IntervalTrack formationTrack; // Explicit geological-unit track, separate from the L3/L4 symbol track.
    long long manualLevel3TrackId=0;
    IntervalTrack manualLevel3Track; // Explicit L3 track; manualTrack selects L4 when separate tracks exist.
    std::vector<double> logTvdValues, logTvdssValues; // Optional original depth axes, aligned with log.axis.
};
struct CorrelationHorizon {
    long long resourceId=0;
    std::string role, inputFingerprint;
    SeismicHorizon surface;
    std::string supportRegionsJson; // Exact requested regions, well IDs and coordinate-confirmation status.
};
struct CorrelationInputSnapshot {
    std::string id, projectId, fingerprint, created;
    std::vector<CorrelationWellInput> wells;
    std::vector<CorrelationHorizon> horizons;
    std::vector<std::string> warnings;
};
struct CorrelationWellSelection {
    long long wellId=0, curveId=0, timeDepthId=0, trajectoryId=0;
    bool processed=true, confirmedVertical=false;
    long long intervalTrackId=0;
    long long formationTrackId=0;
    long long manualLevel3TrackId=0;
};
struct CorrelationHorizonSelection { long long resourceId=0; std::string role; };
struct CorrelationInputRequest {
    std::vector<CorrelationWellSelection> wells;
    std::vector<CorrelationHorizonSelection> horizons;
    double firstDepth=Missing, lastDepth=Missing;
    double surfaceSearchRadius=1500.0; // Padding in the selected horizon's coordinate units; never silently resample.
};
struct CorrelationAlgorithmProfile {
    std::string id="web-compatible-v1", version="1";
    std::vector<int> arOrders{24,60,120};
    double sampleStep=1.0, maxGap=5.0, padding=120.0, minimumThickness=5.0;
    bool resample=true, invertPhase=true, normalize=false;
    bool usePace=true, useResidual=true, usePhase=true;
    double paceWeight=1.0, residualWeight=0.35, phaseWeight=0.5, referenceWeight=0.7;
    std::string direction="reverse";
    // Permit reviewed template-position candidates where observed events are
    // insufficient. Counts/hierarchy still come exclusively from the template.
    bool allowTemplateGuidedCandidates=false;
};
struct InterpretationTemplateSegment {
    std::string segmentId;
    std::vector<double> boundaryFractions; // includes 0 and 1; derived from approved reference
    std::vector<int> directions, parentGroups;
    std::vector<int> parentDirections; // Explicit L3 direction for each L4; no majority inference.
    std::vector<std::string> sequenceIdentities;
};
struct InterpretationTemplate {
    std::string id, version, name, status="draft", sourceRevision;
    long long referenceWellId=0;
    std::vector<InterpretationTemplateSegment> segments;
    std::string sourceReconciliationJson; // Explicit tolerance and original/manual endpoint reconciliation.
};
struct InterpretationClusterMember { long long wellId=0; int group=0; bool manual=false; };
struct InterpretationClustering {
    std::string id, version, name, inputFingerprint, method, parametersJson;
    std::vector<InterpretationClusterMember> members;
};
struct WellInterpretation {
    long long wellId=0;
    std::string name;
    CorrelationWellInput input;
    std::vector<InterpretationSeries> series;
    std::vector<InterpretationBoundary> boundaries;
    std::vector<InterpretationCycle> cycles;
    std::vector<std::string> warnings;
};
struct InterpretationRun {
    std::string id, kind, status="complete", created, completed;
    std::string inputFingerprint, snapshotId, parametersJson, diagnosticsJson;
};
struct InterpretationDocument {
    std::string id, name, revision, parentRevision, projectId, created, author, note;
    CorrelationAlgorithmProfile profile;
    CorrelationInputSnapshot snapshot;
    std::vector<WellInterpretation> wells;
    std::vector<InterpretationRun> runs;
    std::vector<InterpretationEdit> edits;
    std::vector<InterpretationTemplate> templates;
    std::vector<InterpretationClustering> clusterings;
    std::string activeTemplateId, activeClusteringId, viewStateJson;
};
struct InterpretationSummary { std::string id,name,revision,created; int wellCount=0; };
struct InterpretationRevisionSummary { std::string revision,parentRevision,created,author,note; };
struct InterpretationCommitOptions {
    std::string expectedHeadRevision, idempotencyKey;
};
struct InterpretationCommitResult { std::string interpretationId, revision; bool reused=false; };

}
