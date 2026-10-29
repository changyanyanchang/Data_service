#include "paleo_stratigraphy/workflow.h"
#include "paleo_stratigraphy/conventions.h"
#include "../core/internal.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <atomic>
#include <chrono>
#include <set>

namespace paleo::stratigraphy {
namespace {
using namespace detail;
QJsonObject object(const std::string& json){return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();}
std::string json(const QJsonObject& value){return QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString();}
// Carry original assumptions, not an entire previous audit tree. Repeated
// subdivision keeps bounded evidence rather than nesting every earlier run.
QJsonArray flatAlignments(const QJsonObject& source){
    QJsonArray result;std::set<std::string> seen;
    std::function<void(const QJsonObject&,int)> visit=[&](const QJsonObject& value,int depth){
        if(depth>24||value.isEmpty())return;
        auto original=value;for(const auto* field:{"inheritedAlignment","regionalBoundaryAlignment","sourceAlignments","alignmentAudits"})original.remove(field);
        bool substantive=false;for(const auto& entry:original.value("entries").toArray()){
            const auto row=entry.toObject();const auto field=row.value("field").toString();
            if(field.contains("Coordinate")||field.contains("Time reference")||field.contains("timeSign")||field=="MD reference"||field=="surfaceInterpolation")substantive=true;
        }
        if(substantive&&original.value("reviewRequired").toBool()){const auto key=json(original);if(seen.insert(key).second)result.append(original);}
        visit(value.value("inheritedAlignment").toObject(),depth+1);
        for(const auto* field:{"regionalBoundaryAlignment","sourceAlignments","alignmentAudits"})for(const auto& child:value.value(field).toArray())visit(child.toObject(),depth+1);
    };visit(source,0);return result;
}
void appendConventionAudit(InterpretationRun& run,const ConventionResolution& resolution){
    if(!resolution.geometry.resolveFromSources)return;auto parameters=object(run.parametersJson);auto entries=parameters.value("autoConventions").toArray();auto current=object(resolution.evidenceJson);
    for(const auto& entry:entries)if(entry.toObject()==current)return;
    entries.append(current);parameters["autoConventions"]=entries;run.parametersJson=json(parameters);
}
void annotate(InterpretationBoundary& boundary,const ConventionResolution& resolution){
    if(!resolution.geometry.resolveFromSources)return;auto evidence=object(boundary.evidenceJson);auto audit=object(resolution.evidenceJson);const auto inherited=evidence.value("autoConventions").toObject();
    if(inherited.value("reviewRequired").toBool()){audit["sourceAlignments"]=flatAlignments(inherited);audit["reviewRequired"]=true;}
    evidence["autoConventions"]=audit;boundary.evidenceJson=json(evidence);if(audit.value("reviewRequired").toBool())boundary.status="review";
}
void inheritSegmentReview(ConventionResolution& resolution,const WellInterpretation& well,const std::string& segment){
    if(!resolution.geometry.resolveFromSources)return;auto [top,bottom]=endpoints(segment);QJsonArray parents;
    for(const auto& boundary:well.boundaries)if(boundary.role==top||boundary.role==bottom){const auto audit=object(boundary.evidenceJson).value("autoConventions").toObject();if(audit.value("reviewRequired").toBool())parents.append(QJsonObject{{"boundaryId",QString::fromStdString(boundary.id)},{"alignmentAudits",flatAlignments(audit)}});}
    if(parents.isEmpty())return;resolution.reviewRequired=true;auto audit=object(resolution.evidenceJson);audit["reviewRequired"]=true;audit["regionalBoundaryAlignment"]=parents;resolution.evidenceJson=json(audit);
}
void inheritExtrapolation(std::vector<InterpretationBoundary>& output,const WellInterpretation& well,const std::string& segment){
    const auto [top,bottom]=endpoints(segment);QJsonArray evidence;
    for(const auto& b:well.boundaries)if(b.role==top||b.role==bottom){
        const auto extrapolation=object(b.evidenceJson).value("timeDepthExtrapolation").toObject();
        if(extrapolation.value("applied").toBool())evidence.append(QJsonObject{{"boundaryId",QString::fromStdString(b.id)},{"role",QString::fromStdString(b.role)},{"timeDepthExtrapolation",extrapolation}});
    }
    if(evidence.isEmpty())return;
    for(auto& b:output)if(b.role=="cycle"){auto audit=object(b.evidenceJson);audit["regionalExtrapolation"]=evidence;b.evidenceJson=json(audit);b.status="review";}
}
void clearResolvedWarnings(WellInterpretation& well){
    std::erase_if(well.warnings,[](const auto& message){return message=="曲线深度类型或单位尚未确认"||message.find("时深表已有沿井位置点；其坐标或深度约定尚需确认")!=std::string::npos||message.find("局部层位仅按数值坐标提取候选；坐标系/单位尚未确认")!=std::string::npos;});
}
std::string timestamp() {
    using namespace std::chrono;
    const auto now=floor<milliseconds>(system_clock::now());
    const auto day=floor<days>(now); const year_month_day date{day};
    const hh_mm_ss time{now-day}; std::ostringstream out;
    out<<std::setfill('0')<<std::setw(4)<<int(date.year())<<'-'<<std::setw(2)<<unsigned(date.month())<<'-'<<std::setw(2)<<unsigned(date.day())
       <<'T'<<std::setw(2)<<time.hours().count()<<':'<<std::setw(2)<<time.minutes().count()<<':'<<std::setw(2)<<time.seconds().count()<<'.'<<std::setw(3)<<time.subseconds().count()<<'Z';
    return out.str();
}
void checkpoint(const CancelCheck& cancel) {if(cancel&&cancel())throw WorkflowCancelled();}
void report(const ProgressCallback& progress,const CancelCheck& cancel,const std::string& stage,size_t done,size_t total,long long well=0,const std::string& segment={},const std::string& message={}) {
    checkpoint(cancel); if(progress)progress({stage,done,total,well,segment,message}); checkpoint(cancel);
}
std::string geometryJson(const GeometryOptions& g) {
    return json(QJsonObject{
        {"allowBottomExtrapolation",g.allowBottomExtrapolation},
        {"allowTopExtrapolation",g.allowTopExtrapolation},
        {"allowOneWayConversion",g.allowOneWayConversion},
        {"maximumBottomExtrapolationM",g.maximumBottomExtrapolationM},
        {"bottomExtrapolationFitWindowM",g.bottomExtrapolationFitWindowM},
        {"maximumTopExtrapolationM",g.maximumTopExtrapolationM},
        {"topExtrapolationFitWindowM",g.topExtrapolationFitWindowM},
        {"maximumLogExtrapolationM",g.maximumLogExtrapolationM},
        {"wellTimeSign",g.wellTimeSign},
        {"horizonTimeSign",g.horizonTimeSign},
        {"wellTimeOffsetMs",g.wellTimeOffsetMs},
        {"horizonTimeOffsetMs",g.horizonTimeOffsetMs},
        {"coordinateSystem",QString::fromStdString(g.confirmedCoordinateSystem)},
        {"coordinateUnit",QString::fromStdString(g.confirmedCoordinateUnit)},
        {"timeReference",QString::fromStdString(g.confirmedTimeReference)},
        {"mdReference",QString::fromStdString(g.confirmedMdReference)},
        {"wellTimeUnit",QString::fromStdString(g.confirmedWellTimeUnit)},
        {"horizonTimeUnit",QString::fromStdString(g.confirmedHorizonTimeUnit)},
        {"wellTimeType",QString::fromStdString(g.confirmedWellTimeType)},
        {"horizonTimeType",QString::fromStdString(g.confirmedHorizonTimeType)},
        {"depthUnit",QString::fromStdString(g.confirmedDepthUnit)},
        {"timeDepthBasis",QString::fromStdString(g.confirmedDepthType)},
        {"logDepthBasis",QString::fromStdString(g.confirmedLogDepthType)},
        {"verticalDepthReference",QString::fromStdString(g.confirmedVerticalDepthReference)},
        {"maximumSurfaceEdge",finite(g.maximumSurfaceEdge)?QJsonValue(g.maximumSurfaceEdge):QJsonValue(QJsonValue::Null)}
    });
}

std::string profileJson(const CorrelationAlgorithmProfile& p) {
    std::string orders="[";for(int order:p.arOrders){if(orders.size()>1)orders+=',';orders+=std::to_string(order);}orders+=']';
    auto boolean=[](bool value){return value?"true":"false";};
    return "{\"id\":"+quote(p.id)+",\"version\":"+quote(p.version)+",\"arOrders\":"+orders+",\"sampleStepM\":"+number(p.sampleStep)+",\"maxGapM\":"+number(p.maxGap)+",\"paddingM\":"+number(p.padding)+",\"minimumThicknessM\":"+number(p.minimumThickness)+",\"resample\":"+boolean(p.resample)+",\"invertPhase\":"+boolean(p.invertPhase)+",\"normalize\":"+boolean(p.normalize)+",\"usePace\":"+boolean(p.usePace)+",\"useResidual\":"+boolean(p.useResidual)+",\"usePhase\":"+boolean(p.usePhase)+",\"paceWeight\":"+number(p.paceWeight)+",\"residualWeight\":"+number(p.residualWeight)+",\"phaseWeight\":"+number(p.phaseWeight)+",\"referenceWeight\":"+number(p.referenceWeight)+",\"direction\":"+quote(p.direction)+",\"allowTemplateGuidedCandidates\":"+boolean(p.allowTemplateGuidedCandidates)+'}';
}
InterpretationRun startRun(const InterpretationDocument& doc,const std::string& kind,const GeometryOptions& g,const std::string& filter,const std::string& extra="{}") {
    static std::atomic<unsigned long long> sequence{0}; InterpretationRun r;r.kind=kind;r.created=timestamp();
    r.id="workflow-"+kind+'-'+key(r.created+std::to_string(++sequence)+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    r.snapshotId=doc.snapshot.id;r.inputFingerprint=doc.snapshot.fingerprint;
    r.parametersJson="{\"serviceVersion\":1,\"profile\":"+profileJson(doc.profile)+",\"geometry\":"+geometryJson(g)+",\"segmentFilter\":"+quote(filter)+",\"selection\":"+extra+'}';return r;
}
void finish(InterpretationDocument& doc,InterpretationRun run,size_t succeeded,size_t failed,const std::vector<std::string>& diagnostics,const CancelCheck& cancel) {
    checkpoint(cancel);run.completed=timestamp();run.status=failed?(succeeded?"partial":"failed"):"complete";
    std::string rows="[";for(const auto& d:diagnostics){if(rows.size()>1)rows+=',';rows+=quote(d);}rows+=']';
    run.diagnosticsJson="{\"succeeded\":"+std::to_string(succeeded)+",\"failed\":"+std::to_string(failed)+",\"messages\":"+rows+'}';doc.runs.push_back(std::move(run));
}
void prepare(InterpretationDocument& doc) {
    std::set<long long> ids;
    for(const auto& w:doc.wells)if(w.wellId<=0||!ids.insert(w.wellId).second)throw std::invalid_argument("Workflow requires unique positive well IDs");
    std::set<long long> snapshotIds;
    for(const auto& input:doc.snapshot.wells){if(input.wellId<=0||!snapshotIds.insert(input.wellId).second)throw std::invalid_argument("Snapshot well IDs must be unique and positive");if(ids.insert(input.wellId).second){WellInterpretation w;w.wellId=input.wellId;w.name=input.well.wellName;w.input=input;w.warnings=input.warnings;doc.wells.push_back(std::move(w));}}
}
void filterCheck(const std::string& filter){if(!filter.empty())endpoints(filter);}
std::vector<std::string> segments(const std::string& filter){return filter.empty()?std::vector<std::string>{"E12","E3","E4"}:std::vector<std::string>{filter};}
InterpretationSeries calculationLog(const CorrelationWellInput& input,const GeometryOptions& g) {
    auto log=input.log;
    auto confirm=[](const std::string& source,const std::string& value,const std::string& label){if(!source.empty()&&!value.empty()&&metadataToken(source,label)!=metadataToken(value,label))throw std::invalid_argument(label+" confirmation conflicts with source metadata");auto answer=source.empty()?value:source;if(answer.empty())throw std::invalid_argument(label+" requires explicit confirmation");return metadataToken(answer,label);};
    log.axisUnit=confirm(log.axisUnit,g.confirmedDepthUnit,"Log depth unit");
    log.axisKind=confirm(log.axisKind,g.confirmedLogDepthType,"Log depth basis");
    if(log.axisUnit=="ft"){for(double& md:log.axis)md*=.3048;log.axisUnit="m";}
    checkAxis(log);return log;
}
void recordFailure(WellInterpretation& well,const std::string& stage,const std::string& segment,const std::exception& e,std::vector<std::string>& diagnostics) {
    const auto message=stage+" well "+std::to_string(well.wellId)+(segment.empty()?"":" / "+segment)+": "+e.what();well.warnings.push_back(message);diagnostics.push_back(message);
}
void replaceDerived(WellInterpretation& well,std::vector<InterpretationSeries> computed,const std::string& segment,const std::string& workflowRun) {
    // CDMT is a cycle-thickness product and is not replaced by log computations.
    std::erase_if(well.series,[&](const auto& s){return s.segmentId==segment&&s.kind!="FISCHER_CDMT";});
    for(auto& s:computed){s.segmentId=segment;s.status="current";if(s.id.find('@')==std::string::npos)s.id+='@'+segment;
        s.parametersJson="{\"workflowRunId\":"+quote(workflowRun)+",\"algorithmRunId\":"+quote(s.runId)+",\"algorithm\":"+(s.parametersJson.empty()?"{}":s.parametersJson)+'}';
        s.runId=workflowRun;well.series.push_back(std::move(s));}
}
bool refreshCdmt(WellInterpretation& well,const std::string& segment,const std::string& runId,std::vector<std::string>& diagnostics) {
    bool complete=true;
    for(int level:{3,4}) {
        if(std::none_of(well.cycles.begin(),well.cycles.end(),[&](const auto& c){return c.segmentId==segment&&c.level==level;}))continue;
        const auto id="FISCHER_CDMT-"+segment+"-L"+std::to_string(level);
        try {auto computed=fischerCdmt(well,segment,level);computed.runId=runId;computed.status="current";
            auto existing=std::find_if(well.series.begin(),well.series.end(),[&](const auto& s){return s.id==id;});
            if(existing==well.series.end())well.series.push_back(std::move(computed));else *existing=std::move(computed);
        } catch(const std::exception& e) {complete=false;
            for(auto& s:well.series)if(s.id==id)s.status="stale";
            recordFailure(well,"Fischer CDMT L"+std::to_string(level),segment,e,diagnostics);
        }
    }
    return complete;
}
bool protectedPoint(const InterpretationBoundary& b){return b.locked||b.source=="manual"||b.source=="imported_interpretation"||b.status=="confirmed";}
void addCandidate(WellInterpretation& well,InterpretationBoundary b,const std::string& role,const std::string& run) {
    b.name=role+" candidate";b.role="candidate:"+role;b.status="review";b.locked=false;
    b.id+="-candidate-"+key(run);well.boundaries.push_back(std::move(b));
}
void ensureTemplate(InterpretationDocument& doc,const InterpretationTemplate& t) {
    if(t.id.empty()||t.version.empty())throw std::invalid_argument("Assigned template needs a stable ID and version");
    for(const auto& previous:doc.templates)if(previous.id==t.id){
        if(previous.version!=t.version)throw std::invalid_argument("Different versions of one template ID cannot be active in a document; assign a distinct versioned ID");
        if(previous.referenceWellId!=t.referenceWellId||previous.sourceRevision!=t.sourceRevision||previous.sourceReconciliationJson!=t.sourceReconciliationJson||previous.segments.size()!=t.segments.size())throw std::invalid_argument("Assigned template identity conflicts with saved contents");
        for(size_t i=0;i<t.segments.size();++i){const auto& a=previous.segments[i];const auto& b=t.segments[i];if(a.segmentId!=b.segmentId||a.boundaryFractions!=b.boundaryFractions||a.directions!=b.directions||a.parentGroups!=b.parentGroups||a.parentDirections!=b.parentDirections||a.sequenceIdentities!=b.sequenceIdentities)throw std::invalid_argument("Assigned template ID/version has different sequence contents");}return;
    }doc.templates.push_back(t);
}
}

InterpretationDocument calculateHorizons(const InterpretationDocument& document,const GeometryOptions& geometry,CancelCheck cancel,ProgressCallback progress) {
    checkpoint(cancel);auto doc=document;prepare(doc);auto run=startRun(doc,"horizons",geometry,{});size_t success=0,failed=0,done=0;std::vector<std::string> diagnostics;
    report(progress,cancel,run.kind,0,doc.wells.size());
    for(auto& well:doc.wells){checkpoint(cancel);try{
        if(well.input.timeDepth.depthTimePairs.empty())throw std::invalid_argument("未选出可用的时深关系；请查看本井的时深优选与覆盖检查原因");
        const auto resolved=resolveWellConventions(well.input,doc.snapshot.horizons,geometry);appendConventionAudit(run,resolved);
        std::vector<CorrelationHorizon> supported;const auto* surfaces=&doc.snapshot.horizons;
        if(!resolved.excludedHorizonIds.empty()){for(const auto& h:doc.snapshot.horizons)if(std::find(resolved.excludedHorizonIds.begin(),resolved.excludedHorizonIds.end(),h.resourceId)==resolved.excludedHorizonIds.end())supported.push_back(h);surfaces=&supported;}
        auto found=intersectHorizons(well.input,*surfaces,resolved.geometry);auto candidate=well;
        if(geometry.resolveFromSources)clearResolvedWarnings(candidate);
        candidate.warnings.insert(candidate.warnings.end(),resolved.messages.begin(),resolved.messages.end());
        for(auto& b:found.boundaries)annotate(b,resolved);
        candidate.warnings.insert(candidate.warnings.end(),found.warnings.begin(),found.warnings.end());
        std::map<std::string,std::vector<InterpretationBoundary>> groups;for(auto& b:found.boundaries)groups[b.name].push_back(std::move(b));
        // Replace only prior unselected candidates; active/manual points survive.
        std::erase_if(candidate.boundaries,[](const auto& b){return b.role.starts_with("candidate:")&&!protectedPoint(b);});
        for(auto& [role,points]:groups){auto current=std::find_if(candidate.boundaries.begin(),candidate.boundaries.end(),[&](const auto& b){return b.name==role||b.role==role;});
            if(points.size()!=1||(current!=candidate.boundaries.end()&&protectedPoint(*current))){for(auto& b:points)addCandidate(candidate,std::move(b),role,run.id);continue;}
            auto proposed=candidate;auto point=points.front();
            if(current==candidate.boundaries.end())proposed.boundaries.push_back(point);
            else {auto& target=proposed.boundaries[size_t(current-candidate.boundaries.begin())];point.id=target.id;point.originalMd=target.originalMd;target=point;}
            const auto errors=validateInterpretation(proposed);
            if(!errors.empty()){addCandidate(candidate,std::move(point),role,run.id);candidate.warnings.push_back(role+": new intersection retained as candidate because existing interpretation would be invalid: "+errors.front());continue;}
            // Any changed regional anchor invalidates only its adjacent segments.
            for(const auto& seg:segments({})){auto [a,b]=endpoints(seg);if(role==a||role==b){for(auto& s:proposed.series)if(s.segmentId.empty()||s.segmentId==seg)s.status="stale";for(auto& c:proposed.cycles)if(c.segmentId==seg)c.status="review";}}
            candidate=std::move(proposed);
        }
        if(found.boundaries.empty()){
            well.warnings=candidate.warnings;
            std::string message="当前时深及空间覆盖范围内没有有效的井—层位交点；已有解释已保留";
            for(const auto& warning:found.warnings)message+="；"+warning;
            throw std::runtime_error(message);
        }
        bool incomplete=false;for(const auto& requested:doc.snapshot.horizons){const auto at=groups.find(requested.role);if(at==groups.end()||at->second.size()!=1){incomplete=true;const auto message=well.name+"："+requested.role+((resolved.geometry.allowBottomExtrapolation||resolved.geometry.allowTopExtrapolation)?" 在实测及允许的两端外推范围内未得到唯一交点；其他界面结果已保留。":" 在当前时深/空间覆盖内未得到唯一交点；其他界面结果已保留。");candidate.warnings.push_back(message);diagnostics.push_back(message);}}
        if(incomplete)++failed;
        well=std::move(candidate);++success;
    }catch(const std::exception& e){recordFailure(well,run.kind,{},e,diagnostics);++failed;}
        report(progress,cancel,run.kind,++done,doc.wells.size(),well.wellId);
    }finish(doc,std::move(run),success,failed,diagnostics,cancel);return doc;
}

InterpretationDocument deriveForSegments(const InterpretationDocument& document,const GeometryOptions& geometry,CancelCheck cancel,ProgressCallback progress,const std::string& segmentFilter) {
    checkpoint(cancel);filterCheck(segmentFilter);auto doc=document;prepare(doc);auto run=startRun(doc,"derived_curves",geometry,segmentFilter);const auto selected=segments(segmentFilter);size_t done=0,success=0,failed=0,total=doc.wells.size()*selected.size();std::vector<std::string> diagnostics;
    report(progress,cancel,run.kind,0,total);
    for(auto& well:doc.wells)for(const auto& segment:selected){checkpoint(cancel);try{auto [a,b]=endpoints(segment);const double top=named(well,a).md,bottom=named(well,b).md;
        auto resolved=resolveWellConventions(well.input,{},geometry,false);inheritSegmentReview(resolved,well,segment);appendConventionAudit(run,resolved);
        auto derived=deriveCurves(calculationLog(well.input,resolved.geometry),doc.profile,top,bottom);auto candidate=well;
        replaceDerived(candidate,std::move(derived.series),segment,run.id);candidate.warnings.insert(candidate.warnings.end(),derived.warnings.begin(),derived.warnings.end());
        // Manual edits change thickness without changing GR. Rebuild CDMT from
        // accepted cycles independently; a gap must not discard valid GR work.
        if(!refreshCdmt(candidate,segment,run.id,diagnostics))++failed;
        well=std::move(candidate);++success;
    }catch(const std::exception& e){recordFailure(well,run.kind,segment,e,diagnostics);++failed;}
        report(progress,cancel,run.kind,++done,total,well.wellId,segment);
    }finish(doc,std::move(run),success,failed,diagnostics,cancel);return doc;
}

InterpretationDocument propagateTemplates(const InterpretationDocument& document,const std::vector<TemplateAssignment>& assignments,const GeometryOptions& geometry,CancelCheck cancel,ProgressCallback progress,const std::string& segmentFilter) {
    checkpoint(cancel);filterCheck(segmentFilter);auto doc=document;prepare(doc);
    struct Job{size_t wellIndex;std::string segment;InterpretationTemplate reference;};std::vector<Job> jobs;std::set<std::pair<long long,std::string>> used;std::string selection="[";
    for(const auto& assignment:assignments){checkpoint(cancel);auto it=std::find_if(doc.wells.begin(),doc.wells.end(),[&](const auto& w){return w.wellId==assignment.wellId;});if(it==doc.wells.end())throw std::invalid_argument("Template assignment refers to an absent well");if(!assignment.segmentId.empty())endpoints(assignment.segmentId);
        for(const auto& segment:assignment.reference.segments){if(!assignment.segmentId.empty()&&segment.segmentId!=assignment.segmentId)continue;if(!segmentFilter.empty()&&segment.segmentId!=segmentFilter)continue;endpoints(segment.segmentId);
            if(!used.insert({assignment.wellId,segment.segmentId}).second)throw std::invalid_argument("Duplicate template assignment for one well/segment");ensureTemplate(doc,assignment.reference);jobs.push_back({size_t(it-doc.wells.begin()),segment.segmentId,assignment.reference});
            if(selection.size()>1)selection+=',';selection+="{\"wellId\":"+std::to_string(assignment.wellId)+",\"segment\":"+quote(segment.segmentId)+",\"templateId\":"+quote(assignment.reference.id)+",\"templateVersion\":"+quote(assignment.reference.version)+",\"referenceWellId\":"+std::to_string(assignment.reference.referenceWellId)+'}';
        }
        if(!assignment.segmentId.empty()&&std::none_of(assignment.reference.segments.begin(),assignment.reference.segments.end(),[&](const auto& s){return s.segmentId==assignment.segmentId;}))throw std::invalid_argument("Assigned reference lacks the requested segment");
    }selection+=']';auto run=startRun(doc,"template_propagation",geometry,segmentFilter,selection);size_t done=0,success=0,failed=0;std::vector<std::string> diagnostics;report(progress,cancel,run.kind,0,jobs.size());
    for(const auto& job:jobs){checkpoint(cancel);auto& well=doc.wells[job.wellIndex];try{
        if(std::any_of(well.cycles.begin(),well.cycles.end(),[&](const auto& c){return c.segmentId==job.segment&&(c.source=="manual"||c.source=="imported_interpretation"||c.status=="confirmed");}))throw std::invalid_argument("This segment contains manual/confirmed cycles; preserve or explicitly archive that interpretation before propagation");
        auto resolved=resolveWellConventions(well.input,{},geometry,false);inheritSegmentReview(resolved,well,job.segment);appendConventionAudit(run,resolved);
        auto calculation=well;calculation.input.log=calculationLog(well.input,resolved.geometry);auto result=divideSegment(calculation,job.reference,job.segment,doc.profile);auto candidate=well;
        for(auto& b:result.boundaries)annotate(b,resolved);
        inheritExtrapolation(result.boundaries,well,job.segment);
        std::set<std::string> obsolete;for(const auto& c:candidate.cycles)if(c.segmentId==job.segment){obsolete.insert(c.topBoundaryId);obsolete.insert(c.bottomBoundaryId);}
        std::erase_if(candidate.cycles,[&](const auto& c){return c.segmentId==job.segment;});
        for(auto& b:result.boundaries){auto found=std::find_if(candidate.boundaries.begin(),candidate.boundaries.end(),[&](const auto& old){return old.id==b.id;});if(found==candidate.boundaries.end())candidate.boundaries.push_back(std::move(b));else *found=std::move(b);}
        candidate.cycles.insert(candidate.cycles.end(),result.cycles.begin(),result.cycles.end());
        std::set<std::string> retained;for(const auto& c:candidate.cycles){retained.insert(c.topBoundaryId);retained.insert(c.bottomBoundaryId);}
        std::erase_if(candidate.boundaries,[&](const auto& b){return obsolete.count(b.id)&&!retained.count(b.id)&&!protectedPoint(b);});
        const auto errors=validateInterpretation(candidate);if(!errors.empty())throw std::runtime_error(errors.front());
        replaceDerived(candidate,std::move(result.usedSeries),job.segment,run.id);
        // CDMT is recomputed from the actual accepted cycle thicknesses.
        std::erase_if(candidate.series,[&](const auto& s){return s.segmentId==job.segment&&s.kind=="FISCHER_CDMT";});
        for(int level:{3,4})if(std::any_of(candidate.cycles.begin(),candidate.cycles.end(),[&](const auto& c){return c.segmentId==job.segment&&c.level==level;})){auto cdmt=fischerCdmt(candidate,job.segment,level);cdmt.runId=run.id;candidate.series.push_back(std::move(cdmt));}
        candidate.warnings.insert(candidate.warnings.end(),result.warnings.begin(),result.warnings.end());well=std::move(candidate);++success;
    }catch(const std::exception& e){recordFailure(well,run.kind,job.segment,e,diagnostics);++failed;}
        report(progress,cancel,run.kind,++done,jobs.size(),well.wellId,job.segment);
    }finish(doc,std::move(run),success,failed,diagnostics,cancel);return doc;
}
InterpretationDocument propagateTemplates(const InterpretationDocument& document,const std::map<long long,InterpretationTemplate>& assignments,const GeometryOptions& geometry,CancelCheck cancel,ProgressCallback progress,const std::string& segmentFilter) {
    std::vector<TemplateAssignment> selected;for(const auto& [well,reference]:assignments)selected.push_back({well,{},reference});return propagateTemplates(document,selected,geometry,std::move(cancel),std::move(progress),segmentFilter);
}
}
