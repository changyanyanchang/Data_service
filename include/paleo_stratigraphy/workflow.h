#pragma once
#include "core.h"
#include <functional>
#include <map>
#include <stdexcept>

namespace paleo::stratigraphy {
struct WorkflowProgress {
    std::string stage;
    std::size_t completed=0, total=0;
    long long wellId=0;
    std::string segmentId, message;
};
using CancelCheck=std::function<bool()>;
using ProgressCallback=std::function<void(const WorkflowProgress&)>;
class WorkflowCancelled : public std::runtime_error {
public: WorkflowCancelled():std::runtime_error("Stratigraphy workflow cancelled; no document changes returned"){}
};
struct TemplateAssignment {
    long long wellId=0;
    std::string segmentId; // Empty means all segments in this reference template.
    InterpretationTemplate reference;
};

// Public transactional orchestration. Input is never modified. Cancellation or
// a thrown callback returns no partial document. Per-item calculation failures
// are retained as warnings and recorded in a run with status partial/failed.
// Callbacks run on the caller's thread between individual well/segment jobs.
// These functions do not save: publish the returned complete document using the
// SDK's revision/idempotency-aware interpretation commit API.
InterpretationDocument calculateHorizons(const InterpretationDocument& document,
                                         const GeometryOptions& geometry,
                                         CancelCheck cancel={}, ProgressCallback progress={});
InterpretationDocument deriveForSegments(const InterpretationDocument& document,
                                         const GeometryOptions& geometry,
                                         CancelCheck cancel={}, ProgressCallback progress={},
                                         const std::string& segmentFilter={});
InterpretationDocument propagateTemplates(const InterpretationDocument& document,
                                          const std::vector<TemplateAssignment>& assignments,
                                          const GeometryOptions& geometry,
                                          CancelCheck cancel={}, ProgressCallback progress={},
                                          const std::string& segmentFilter={});
InterpretationDocument propagateTemplates(const InterpretationDocument& document,
                                          const std::map<long long,InterpretationTemplate>& assignments,
                                          const GeometryOptions& geometry,
                                          CancelCheck cancel={}, ProgressCallback progress={},
                                          const std::string& segmentFilter={});
}
