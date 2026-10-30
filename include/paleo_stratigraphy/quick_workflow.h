#pragma once
#include "workflow.h"
#include "paleo_data/service.h"

namespace paleo::stratigraphy {
// Matching identifies resources; it never supplies unknown scientific metadata.
// A nonzero preferred resource ID is an explicit choice and is validated against
// its well/category. Missing optional manual tracks do not block horizon work.
struct QuickIssue {
    long long wellId=0;
    std::string field, message;
    bool blocking=false;
    std::vector<long long> candidateIds;
};
struct QuickWellSummary {
    long long wellId=0;
    std::string name, coordinateSource; // timeDepthXYZ / trajectory / confirmedVertical / unavailable
};
struct QuickInputPlan {
    CorrelationInputRequest request;
    std::vector<QuickIssue> issues;
    std::vector<QuickWellSummary> wellSummaries;
    bool ready() const;
};
QuickInputPlan planQuickInputs(paleo::data::DataService& service,
                              const std::vector<long long>& wellIds,
                              const CorrelationInputRequest& preferred={});

struct QuickReferenceDiscovery {
    std::vector<InterpretationTemplate> templates;
    std::vector<WellInterpretation> manualReferences;
    std::vector<QuickIssue> issues;
};
// Discover real manual standards in the whole project, including unselected
// wells. Captures immutable SDK snapshots; never edits source tracks or saves
// a recommended template as the project's active template.
QuickReferenceDiscovery discoverQuickReferences(paleo::data::DataService& service,
                                                const GeometryOptions& geometry={},
                                                CancelCheck cancel={},
                                                double sourceReconciliationTolerance=2.0);

struct QuickSegmentTemplatePlan {
    std::string segmentId, templateId, templateVersion;
    long long referenceWellId=0;
    int level3Count=0, level4Count=0;
    std::vector<int> parentGroups;
    std::string selectionMode, reason; // active_template / preferred_reference / auto_recommended
};
struct QuickTemplatePlan {
    std::vector<TemplateAssignment> assignments;
    std::vector<InterpretationTemplate> candidates;
    // Exact manual source interpretations used for candidate extraction. They
    // are not silently applied over the caller's current regional boundaries.
    std::vector<WellInterpretation> manualReferences;
    std::vector<QuickIssue> issues;
    // Chosen once per regional segment, never changed by target well/cluster.
    std::vector<QuickSegmentTemplatePlan> segmentPlans;
    bool usesRecommendation=false, hasManualConflicts=false;
    std::string recommendationJson;
    bool ready() const;
};
// An active template is binding for all wells and cannot silently fall back.
// Without one, recommend a common reference by complete segment coverage and
// source completeness, filling missing segments from a common secondary source.
// The business policy allows at most 2 m of source ordinal reconciliation;
// original manual depths are retained, and the tolerance is recorded in audit.
QuickTemplatePlan planQuickTemplates(const InterpretationDocument& document,
                                    const GeometryOptions& geometry={},
                                    long long preferredReferenceWellId=0,
                                    const std::string& segmentFilter={},
                                    double sourceReconciliationTolerance=2.0,
                                    const std::vector<WellInterpretation>& discoveredReferences={});
}
