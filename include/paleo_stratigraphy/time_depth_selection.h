#pragma once
#include "quick_workflow.h"

namespace paleo::stratigraphy {
struct TimeDepthCandidateEvaluation {
    long long wellId=0, timeDepthId=0;
    std::string name;
    bool usable=false, selected=false, explicitSelection=false;
    std::vector<std::string> interfaceRoles, anchorRoles;
    std::vector<InterpretationBoundary> boundaries;
    double anchorRmseM=Missing, anchorMaxAbsM=Missing;
    double grFirstMd=Missing, grLastMd=Missing;
    int observedInterfaces=0, extrapolatedInterfaces=0;
    double totalExtrapolationM=0;
    std::string reason, evidenceJson;
};
struct TimeDepthSelectionPlan {
    CorrelationInputRequest request;
    std::vector<TimeDepthCandidateEvaluation> candidates;
    std::vector<QuickIssue> issues;
    std::string auditJson;
    // At least one usable well and no global request error. Failed individual
    // wells retain timeDepthId=0 plus their own issue; filter before capture.
    bool ready() const;
};
// A single-well SDK snapshot is evaluated without altering its source arrays.
// Anchor error is unshifted MD residual in metres, not the historical experiment's
// TWT RMSE. Only explicit manual/imported/confirmed anchors are eligible.
TimeDepthCandidateEvaluation evaluateTimeDepthCandidate(
    const CorrelationInputSnapshot& snapshot,const GeometryOptions& geometry,
    const std::vector<InterpretationBoundary>& independentAnchors={});
// Existing nonzero timeDepthId is an explicit binding and is retained if valid.
// Next retain a usable historical preference when it covers as many interfaces
// as the best current candidate. Otherwise choose a common interface mask before
// comparing scores and record why the historical coverage was insufficient.
// No fitted time offset, fabricated GR or source metadata mutation is permitted.
TimeDepthSelectionPlan selectTimeDepthCandidates(
    paleo::data::DataService& service,const CorrelationInputRequest& request,
    const GeometryOptions& geometry,CancelCheck cancel={},
    const std::vector<WellInterpretation>& independentAnchors={});
}
