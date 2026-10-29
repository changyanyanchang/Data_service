#include "interpretation_store.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonParseError>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace paleo::data::detail::interpretation {
QString newId(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
QString now(){return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);}
QString digest(const QByteArray& b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
QByteArray bytes(const QJsonObject& o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
bool parse(const QByteArray& payload,const QString& expected,QJsonObject& out,std::string& error){
    out={};if(digest(payload)!=expected){error="解释数据校验失败";return false;}
    QJsonParseError pe;const auto d=QJsonDocument::fromJson(payload,&pe);
    if(pe.error!=QJsonParseError::NoError||!d.isObject()||(d.object().value("schema").toInt()!=1&&d.object().value("schema").toInt()!=2)){error="解释数据格式或版本不受支持";return false;}
    out=d.object();return true;
}
bool ensureSchema(ProjectStore& st,std::string& error){
    QSqlQuery check;if(!st.exec("SELECT value FROM ds_meta WHERE key='interpretation_schema'",{},error,&check))return false;
    if(check.next()){const auto version=check.value(0).toString();if(version!="1"&&version!="2"){error="解释存储版本不受支持";return false;}}check.finish();
    check.finish();Transaction tx(st,error);if(!tx.active)return false;
    if(!st.exec("SELECT value FROM ds_meta WHERE key='interpretation_schema'",{},error,&check))return false;
    if(check.next()){const auto version=check.value(0).toString();if(version!="1"&&version!="2"){error="解释存储版本不受支持";return false;}}check.finish();
    const QStringList sql={
      "CREATE TABLE IF NOT EXISTS correlation_input_snapshots(id TEXT PRIMARY KEY,project_id TEXT NOT NULL,fingerprint TEXT NOT NULL,created TEXT NOT NULL,payload BLOB NOT NULL,digest TEXT NOT NULL)",
      "CREATE TABLE IF NOT EXISTS interpretations(id TEXT PRIMARY KEY,name TEXT NOT NULL,head_revision TEXT NOT NULL,created TEXT NOT NULL,well_count INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS interpretation_revisions(revision TEXT PRIMARY KEY,interpretation_id TEXT NOT NULL REFERENCES interpretations(id),parent_revision TEXT NOT NULL,created TEXT NOT NULL,author TEXT NOT NULL,note TEXT NOT NULL,payload BLOB NOT NULL,digest TEXT NOT NULL)",
      "CREATE INDEX IF NOT EXISTS interpretation_revision_owner ON interpretation_revisions(interpretation_id,created)",
      "CREATE TABLE IF NOT EXISTS interpretation_commits(idempotency_key TEXT PRIMARY KEY,request_hash TEXT NOT NULL,interpretation_id TEXT NOT NULL REFERENCES interpretations(id),revision TEXT NOT NULL REFERENCES interpretation_revisions(revision))",
      "CREATE TABLE IF NOT EXISTS interpretation_templates(id TEXT NOT NULL,version TEXT NOT NULL,name TEXT NOT NULL,payload BLOB NOT NULL,digest TEXT NOT NULL,PRIMARY KEY(id,version))",
      "CREATE TABLE IF NOT EXISTS interpretation_clusterings(id TEXT NOT NULL,version TEXT NOT NULL,name TEXT NOT NULL,payload BLOB NOT NULL,digest TEXT NOT NULL,PRIMARY KEY(id,version))",
      "CREATE TABLE IF NOT EXISTS interpretation_payloads(hash TEXT PRIMARY KEY,kind TEXT NOT NULL,payload BLOB NOT NULL,created TEXT NOT NULL)"
    };
    for(const auto& statement:sql)if(!st.exec(statement,{},error))return false;
    if(!st.exec("CREATE INDEX IF NOT EXISTS horizon_xy_region ON horizon_points(horizon_id,x,y)",{},error))return false;
    if(!st.exec("INSERT OR IGNORE INTO ds_meta(key,value) VALUES('project_uuid',?)",{newId()},error)||
       !st.exec("INSERT OR REPLACE INTO ds_meta(key,value) VALUES('interpretation_schema','2')",{},error))return false;
    return tx.commit(error);
}
bool projectId(ProjectStore& st,std::string& out,std::string& error){QSqlQuery r;out.clear();if(!st.exec("SELECT value FROM ds_meta WHERE key='project_uuid'",{},error,&r)||!r.next()){if(error.empty())error="工区缺少稳定标识";return false;}out=s(r.value(0).toString());return !out.empty();}

namespace {
QJsonValue num(double d){return std::isfinite(d)?QJsonValue(d):QJsonValue(QJsonValue::Null);}
double real(const QJsonValue& v){return v.isDouble()?v.toDouble():Missing;}
long long integer(const QJsonValue& v){return v.isString()?v.toString().toLongLong():static_cast<long long>(v.toDouble());}
QJsonArray doubles(const std::vector<double>& v){QJsonArray a;for(double n:v)a.append(num(n));return a;}
QJsonArray ints(const std::vector<int>& v){QJsonArray a;for(int n:v)a.append(n);return a;}
QJsonArray flags(const std::vector<unsigned char>& v){QJsonArray a;for(auto n:v)a.append(int(n));return a;}
QJsonArray strings(const std::vector<std::string>& v){QJsonArray a;for(const auto& n:v)a.append(q(n));return a;}
std::vector<double> readDoubles(const QJsonValue& v){std::vector<double> a;for(const auto& n:v.toArray())a.push_back(real(n));return a;}
std::vector<int> readInts(const QJsonValue& v){std::vector<int> a;for(const auto& n:v.toArray())a.push_back(n.toInt());return a;}
std::vector<unsigned char> readFlags(const QJsonValue& v){std::vector<unsigned char> a;for(const auto& n:v.toArray())a.push_back(static_cast<unsigned char>(n.toInt()));return a;}
std::vector<std::string> readStrings(const QJsonValue& v){std::vector<std::string> a;for(const auto& n:v.toArray())a.push_back(s(n.toString()));return a;}
#define S(f) o.insert(#f,q(v.f))
#define I(f) o.insert(#f,QString::number(v.f))
#define N(f) o.insert(#f,num(v.f))
#define B(f) o.insert(#f,v.f)
#define D(f) o.insert(#f,doubles(v.f))
#define SS(f) o.insert(#f,strings(v.f))
#define RS(f) v.f=s(o.value(#f).toString())
#define RI(f) v.f=integer(o.value(#f))
#define RN(f) v.f=real(o.value(#f))
#define RB(f) v.f=o.value(#f).toBool()
#define RD(f) v.f=readDoubles(o.value(#f))
#define RSS(f) v.f=readStrings(o.value(#f))
QJsonObject write(const InterpretationSeries& v){QJsonObject o;S(id);S(name);S(kind);S(unit);S(axisKind);S(axisUnit);D(axis);D(values);o["quality"]=flags(v.quality);S(runId);S(segmentId);S(parametersJson);S(inputFingerprint);S(status);return o;}
void read(const QJsonObject& o,InterpretationSeries& v){RS(id);RS(name);RS(kind);RS(unit);RS(axisKind);RS(axisUnit);RD(axis);RD(values);v.quality=readFlags(o["quality"]);RS(runId);RS(segmentId);RS(parametersJson);RS(inputFingerprint);if(o.contains("status"))RS(status);}
QJsonObject write(const InterpretationBoundary& v){QJsonObject o;S(id);S(name);S(role);S(source);S(status);N(md);N(originalMd);N(time);B(locked);S(evidenceJson);return o;}
void read(const QJsonObject& o,InterpretationBoundary& v){RS(id);RS(name);RS(role);RS(source);RS(status);RN(md);RN(originalMd);RN(time);RB(locked);RS(evidenceJson);}
QJsonObject write(const InterpretationCycle& v){QJsonObject o;S(id);S(name);S(segmentId);S(topBoundaryId);S(bottomBoundaryId);S(parentId);S(templateIdentity);S(templateVersion);S(source);S(status);B(level);B(direction);return o;}
void read(const QJsonObject& o,InterpretationCycle& v){RS(id);RS(name);RS(segmentId);RS(topBoundaryId);RS(bottomBoundaryId);RS(parentId);RS(templateIdentity);RS(templateVersion);RS(source);RS(status);v.level=o["level"].toInt();v.direction=o["direction"].toInt();}
QJsonObject write(const InterpretationEdit& v){QJsonObject o;S(id);S(targetId);S(action);S(beforeJson);S(afterJson);S(author);S(created);return o;}
void read(const QJsonObject& o,InterpretationEdit& v){RS(id);RS(targetId);RS(action);RS(beforeJson);RS(afterJson);RS(author);RS(created);}
QJsonObject write(const WellInfo& v){QJsonObject o;S(wellName);S(fieldName);S(wellType);N(wellheadX);N(wellheadY);S(coordinateSystem);S(coordinateUnit);N(totalDrilledDepth);S(remark);return o;}
void read(const QJsonObject& o,WellInfo& v){RS(wellName);RS(fieldName);RS(wellType);RN(wellheadX);RN(wellheadY);RS(coordinateSystem);RS(coordinateUnit);RN(totalDrilledDepth);RS(remark);}
QJsonObject write(const IntervalTrack& v){QJsonObject o;S(trackName);S(trackType);S(sourceFile);S(sourceSheet);o["depthInfo"]=depthJson(v.depthInfo);QJsonArray a;for(const auto& i:v.intervals){QJsonObject p{{"id",i.intervalId},{"name",q(i.intervalName)},{"order",i.displayOrder},{"facies",q(i.faciesType)},{"topUnconformity",i.topUnconformity},{"bottomUnconformity",i.bottomUnconformity},{"bottomNotPenetrated",i.bottomNotPenetrated},{"top",num(i.topDepth)},{"bottom",num(i.bottomDepth)},{"topTvd",num(i.topTvd)},{"bottomTvd",num(i.bottomTvd)},{"topTvdss",num(i.topTvdss)},{"bottomTvdss",num(i.bottomTvdss)},{"sourceRow",i.sourceRow}};QJsonArray m;for(const auto& x:i.properties)m.append(QJsonObject{{"name",q(x.measurementName)},{"unit",q(x.measurementUnit)},{"value",num(x.numericValue)},{"text",q(x.originalText)}});p["properties"]=m;a.append(p);}o["intervals"]=a;return o;}
void read(const QJsonObject& o,IntervalTrack& v){RS(trackName);RS(trackType);RS(sourceFile);RS(sourceSheet);v.depthInfo=readDepth(o["depthInfo"].toObject());for(const auto& n:o["intervals"].toArray()){auto p=n.toObject();GeologicalInterval i;i.intervalId=p["id"].toInt();i.intervalName=s(p["name"].toString());i.displayOrder=p["order"].toInt();i.faciesType=s(p["facies"].toString());i.topUnconformity=p["topUnconformity"].toInt(-1);i.bottomUnconformity=p["bottomUnconformity"].toInt(-1);i.bottomNotPenetrated=p["bottomNotPenetrated"].toInt(-1);i.topDepth=real(p["top"]);i.bottomDepth=real(p["bottom"]);i.topTvd=real(p["topTvd"]);i.bottomTvd=real(p["bottomTvd"]);i.topTvdss=real(p["topTvdss"]);i.bottomTvdss=real(p["bottomTvdss"]);i.sourceRow=p["sourceRow"].toInt();for(const auto& n:p["properties"].toArray()){auto x=n.toObject();i.properties.push_back({s(x["name"].toString()),s(x["unit"].toString()),real(x["value"]),s(x["text"].toString())});}v.intervals.push_back(std::move(i));}}
QJsonObject write(const TrajectoryPosition& v){QJsonObject o;N(md);N(x);N(y);N(tvd);N(tvdss);return o;}
void read(const QJsonObject& o,TrajectoryPosition& v){RN(md);RN(x);RN(y);RN(tvd);RN(tvdss);}
QJsonObject write(const TrajectorySurvey& v){QJsonObject o;S(surveyName);S(azimuthReference);QJsonArray a;for(const auto& p:v.surveyPoints)a.append(QJsonObject{{"md",num(p.measuredDepth)},{"inclination",num(p.inclinationDegrees)},{"azimuth",num(p.azimuthDegrees)}});o["points"]=a;return o;}
void read(const QJsonObject& o,TrajectorySurvey& v){RS(surveyName);RS(azimuthReference);for(const auto& e:o["points"].toArray()){auto p=e.toObject();v.surveyPoints.push_back({real(p["md"]),real(p["inclination"]),real(p["azimuth"])});}}
QJsonObject write(const TimeDepthTable& v){QJsonObject o;S(relationName);o["depthInfo"]=depthJson(v.depthInfo);S(timeUnit);S(timeType);S(timeReference);S(sourceFile);S(sourceSheet);QJsonArray a;for(const auto& p:v.depthTimePairs)a.append(QJsonObject{{"depth",num(p.depthValue)},{"time",num(p.timeValue)},{"x",num(p.x)},{"y",num(p.y)},{"z",num(p.z)}});o["pairs"]=a;o["positionDepthInfo"]=depthJson(v.positionDepthInfo);S(coordinateSystem);S(coordinateUnit);return o;}
void read(const QJsonObject& o,TimeDepthTable& v){RS(relationName);v.depthInfo=readDepth(o["depthInfo"].toObject());RS(timeUnit);RS(timeType);RS(timeReference);RS(sourceFile);RS(sourceSheet);for(const auto& e:o["pairs"].toArray()){auto p=e.toObject();TimeDepthPoint t;t.depthValue=real(p["depth"]);t.timeValue=real(p["time"]);t.x=real(p["x"]);t.y=real(p["y"]);t.z=real(p["z"]);v.depthTimePairs.push_back(t);}v.positionDepthInfo=readDepth(o["positionDepthInfo"].toObject());RS(coordinateSystem);RS(coordinateUnit);}
QJsonObject write(const SeismicHorizon& v){QJsonObject o;S(horizonName);B(parentVolumeId);S(coordinateSystem);S(coordinateUnit);S(timeUnit);S(timeType);S(timeReference);S(sourceFile);QJsonArray a;for(const auto& p:v.points)a.append(QJsonObject{{"inline",p.inlineNumber},{"crossline",p.crosslineNumber},{"x",num(p.x)},{"y",num(p.y)},{"time",num(p.zTime)}});o["points"]=a;return o;}
void read(const QJsonObject& o,SeismicHorizon& v){RS(horizonName);v.parentVolumeId=o["parentVolumeId"].toInt();RS(coordinateSystem);RS(coordinateUnit);RS(timeUnit);RS(timeType);RS(timeReference);RS(sourceFile);for(const auto& e:o["points"].toArray()){auto p=e.toObject();v.points.push_back({p["inline"].toInt(),p["crossline"].toInt(),real(p["x"]),real(p["y"]),real(p["time"])});}}
QJsonObject write(const CorrelationHorizon& v){QJsonObject o;I(resourceId);S(role);S(inputFingerprint);o["surface"]=write(v.surface);S(supportRegionsJson);return o;}
void read(const QJsonObject& o,CorrelationHorizon& v){RI(resourceId);RS(role);RS(inputFingerprint);read(o["surface"].toObject(),v.surface);RS(supportRegionsJson);}
QJsonObject write(const CorrelationAlgorithmProfile& v){QJsonObject o;S(id);S(version);o["arOrders"]=ints(v.arOrders);N(sampleStep);N(maxGap);N(padding);N(minimumThickness);B(resample);B(invertPhase);B(normalize);B(usePace);B(useResidual);B(usePhase);N(paceWeight);N(residualWeight);N(phaseWeight);N(referenceWeight);S(direction);B(allowTemplateGuidedCandidates);return o;}
void read(const QJsonObject& o,CorrelationAlgorithmProfile& v){RS(id);RS(version);v.arOrders=readInts(o["arOrders"]);RN(sampleStep);RN(maxGap);RN(padding);RN(minimumThickness);RB(resample);RB(invertPhase);RB(normalize);RB(usePace);RB(useResidual);RB(usePhase);RN(paceWeight);RN(residualWeight);RN(phaseWeight);RN(referenceWeight);RS(direction);RB(allowTemplateGuidedCandidates);}
QJsonObject write(const InterpretationTemplateSegment& v){QJsonObject o;S(segmentId);D(boundaryFractions);o["directions"]=ints(v.directions);o["parentGroups"]=ints(v.parentGroups);o["parentDirections"]=ints(v.parentDirections);SS(sequenceIdentities);return o;}
void read(const QJsonObject& o,InterpretationTemplateSegment& v){RS(segmentId);RD(boundaryFractions);v.directions=readInts(o["directions"]);v.parentGroups=readInts(o["parentGroups"]);v.parentDirections=readInts(o["parentDirections"]);RSS(sequenceIdentities);}
QJsonObject write(const InterpretationClusterMember& v){QJsonObject o;I(wellId);B(group);B(manual);return o;}
void read(const QJsonObject& o,InterpretationClusterMember& v){RI(wellId);v.group=o["group"].toInt();RB(manual);}
QJsonObject write(const InterpretationRun& v){QJsonObject o;S(id);S(kind);S(status);S(created);S(completed);S(inputFingerprint);S(snapshotId);S(parametersJson);S(diagnosticsJson);return o;}
void read(const QJsonObject& o,InterpretationRun& v){RS(id);RS(kind);RS(status);RS(created);RS(completed);RS(inputFingerprint);RS(snapshotId);RS(parametersJson);RS(diagnosticsJson);}
template<class T> QJsonArray objects(const std::vector<T>& a){QJsonArray out;for(const auto& v:a)out.append(write(v));return out;}
template<class T> void readObjects(const QJsonValue& a,std::vector<T>& out){out.clear();for(const auto& n:a.toArray()){T v;read(n.toObject(),v);out.push_back(std::move(v));}}
QJsonObject write(const CorrelationWellInput& v){QJsonObject o;I(wellId);I(curveId);I(logSetId);I(timeDepthId);I(trajectoryId);o["well"]=write(v.well);o["depthInfo"]=depthJson(v.depthInfo);o["log"]=write(v.log);o["timeDepth"]=write(v.timeDepth);o["survey"]=write(v.survey);o["positions"]=objects(v.positions);o["markers"]=objects(v.markers);S(inputFingerprint);S(sourceRefsJson);B(confirmedVertical);SS(warnings);o["surveyDepthInfo"]=depthJson(v.surveyDepthInfo);D(rawLogValues);o["sourceQuality"]=flags(v.sourceQuality);B(processed);I(intervalTrackId);o["manualTrack"]=write(v.manualTrack);I(formationTrackId);o["formationTrack"]=write(v.formationTrack);I(manualLevel3TrackId);o["manualLevel3Track"]=write(v.manualLevel3Track);D(logTvdValues);D(logTvdssValues);return o;}
void read(const QJsonObject& o,CorrelationWellInput& v){RI(wellId);RI(curveId);RI(logSetId);RI(timeDepthId);RI(trajectoryId);read(o["well"].toObject(),v.well);v.depthInfo=readDepth(o["depthInfo"].toObject());read(o["log"].toObject(),v.log);read(o["timeDepth"].toObject(),v.timeDepth);read(o["survey"].toObject(),v.survey);readObjects(o["positions"],v.positions);readObjects(o["markers"],v.markers);RS(inputFingerprint);RS(sourceRefsJson);RB(confirmedVertical);RSS(warnings);v.surveyDepthInfo=readDepth(o["surveyDepthInfo"].toObject());RD(rawLogValues);v.sourceQuality=readFlags(o["sourceQuality"]);RB(processed);RI(intervalTrackId);read(o["manualTrack"].toObject(),v.manualTrack);RI(formationTrackId);read(o["formationTrack"].toObject(),v.formationTrack);RI(manualLevel3TrackId);read(o["manualLevel3Track"].toObject(),v.manualLevel3Track);RD(logTvdValues);RD(logTvdssValues);}
QJsonObject write(const WellInterpretation& v){QJsonObject o;I(wellId);S(name);o["input"]=write(v.input);o["series"]=objects(v.series);o["boundaries"]=objects(v.boundaries);o["cycles"]=objects(v.cycles);SS(warnings);return o;}
void read(const QJsonObject& o,WellInterpretation& v){RI(wellId);RS(name);read(o["input"].toObject(),v.input);readObjects(o["series"],v.series);readObjects(o["boundaries"],v.boundaries);readObjects(o["cycles"],v.cycles);RSS(warnings);}
// These vectors instantiate after the matching write/read overloads are declared.
QJsonArray wells(const std::vector<CorrelationWellInput>& a){QJsonArray out;for(const auto& v:a)out.append(write(v));return out;}
QJsonArray interpretedWells(const std::vector<WellInterpretation>& a){QJsonArray out;for(const auto& v:a)out.append(write(v));return out;}
bool finiteOrMissing(double v){return !std::isinf(v);}
bool seriesValid(const InterpretationSeries& v,std::string& error){
    if(v.axis.size()!=v.values.size()||(!v.quality.empty()&&v.quality.size()!=v.values.size())){error="曲线坐标、数值与质量数组长度不一致";return false;}
    for(size_t i=0;i<v.axis.size();++i){if(!std::isfinite(v.axis[i])||!finiteOrMissing(v.values[i])){error="曲线坐标或数值包含无穷值/无效坐标";return false;}}
    return true;
}
}
QJsonObject encode(const CorrelationInputSnapshot& v){QJsonObject o{{"schema",1}};S(id);S(projectId);S(fingerprint);S(created);o["wells"]=wells(v.wells);o["horizons"]=objects(v.horizons);SS(warnings);return o;}
bool decode(const QJsonObject& o,CorrelationInputSnapshot& v,std::string& error){
    v={};if(!o["wells"].isArray()||!o["horizons"].isArray()){error="输入快照数组缺失";return false;}RS(id);RS(projectId);RS(fingerprint);RS(created);for(const auto& n:o["wells"].toArray()){CorrelationWellInput a;read(n.toObject(),a);if(!seriesValid(a.log,error))return false;v.wells.push_back(std::move(a));}readObjects(o["horizons"],v.horizons);RSS(warnings);return true;
}
QJsonObject encode(const InterpretationTemplate& v){QJsonObject o{{"schema",1}};S(id);S(version);S(name);S(status);S(sourceRevision);I(referenceWellId);o["segments"]=objects(v.segments);S(sourceReconciliationJson);return o;}
bool decode(const QJsonObject& o,InterpretationTemplate& v,std::string& error){v={};RS(id);RS(version);RS(name);RS(status);RS(sourceRevision);RI(referenceWellId);readObjects(o["segments"],v.segments);RS(sourceReconciliationJson);return validate(v,error);}
QJsonObject encode(const InterpretationClustering& v){QJsonObject o{{"schema",1}};S(id);S(version);S(name);S(inputFingerprint);S(method);S(parametersJson);o["members"]=objects(v.members);return o;}
bool decode(const QJsonObject& o,InterpretationClustering& v,std::string& error){v={};RS(id);RS(version);RS(name);RS(inputFingerprint);RS(method);RS(parametersJson);readObjects(o["members"],v.members);return validate(v,error);}
QJsonObject encode(const InterpretationDocument& v){QJsonObject o{{"schema",1}};S(id);S(name);S(revision);S(parentRevision);S(projectId);S(created);S(author);S(note);o["profile"]=write(v.profile);o["snapshot"]=encode(v.snapshot);o["wells"]=interpretedWells(v.wells);o["runs"]=objects(v.runs);o["edits"]=objects(v.edits);QJsonArray ts,cs;for(const auto& t:v.templates)ts.append(encode(t));for(const auto& c:v.clusterings)cs.append(encode(c));o["templates"]=ts;o["clusterings"]=cs;S(activeTemplateId);S(activeClusteringId);S(viewStateJson);return o;}
bool decode(const QJsonObject& o,InterpretationDocument& v,std::string& error){v={};if(!o["wells"].isArray()){error="解释井集合缺失";return false;}RS(id);RS(name);RS(revision);RS(parentRevision);RS(projectId);RS(created);RS(author);RS(note);read(o["profile"].toObject(),v.profile);if(!decode(o["snapshot"].toObject(),v.snapshot,error))return false;for(const auto& n:o["wells"].toArray()){WellInterpretation a;read(n.toObject(),a);v.wells.push_back(std::move(a));}readObjects(o["runs"],v.runs);readObjects(o["edits"],v.edits);for(const auto& n:o["templates"].toArray()){InterpretationTemplate a;if(!decode(n.toObject(),a,error))return false;v.templates.push_back(std::move(a));}for(const auto& n:o["clusterings"].toArray()){InterpretationClustering a;if(!decode(n.toObject(),a,error))return false;v.clusterings.push_back(std::move(a));}RS(activeTemplateId);RS(activeClusteringId);RS(viewStateJson);return validate(v,error);}

bool validate(const InterpretationTemplate& v,std::string& error){
    if(v.id.empty()||v.version.empty()){error="模板必须有稳定 ID 和版本";return false;}std::set<std::string> ids;
    for(const auto& a:v.segments){if(a.segmentId.empty()||!ids.insert(a.segmentId).second||a.boundaryFractions.size()<2||a.boundaryFractions.front()!=0.0||a.boundaryFractions.back()!=1.0){error="模板段或端点定义无效";return false;}
      for(size_t i=0;i<a.boundaryFractions.size();++i)if(!std::isfinite(a.boundaryFractions[i])||a.boundaryFractions[i]<0||a.boundaryFractions[i]>1||(i&&a.boundaryFractions[i]<=a.boundaryFractions[i-1])){error="模板边界比例必须严格递增且位于0至1";return false;}
      const auto n=a.boundaryFractions.size()-1;if((!a.directions.empty()&&a.directions.size()!=n)||(!a.parentGroups.empty()&&a.parentGroups.size()!=n)||(!a.parentDirections.empty()&&a.parentDirections.size()!=n)||(!a.sequenceIdentities.empty()&&a.sequenceIdentities.size()!=n)){error="模板层序属性数量不匹配";return false;}
      std::unordered_map<int,int> parentDirection;for(size_t i=0;i<a.parentGroups.size()&&!a.parentDirections.empty();++i){const auto group=a.parentGroups[i],direction=a.parentDirections[i];if(direction<-1||direction>1){error="父层序方向无效";return false;}const auto [it,inserted]=parentDirection.emplace(group,direction);if(!inserted&&it->second!=direction){error="同一父层序不能有冲突方向";return false;}}
    }return true;
}
bool validate(const InterpretationClustering& v,std::string& error){if(v.id.empty()||v.version.empty()){error="分组结果必须有稳定 ID 和版本";return false;}std::set<long long> wells;for(const auto& a:v.members)if(a.wellId<=0||!wells.insert(a.wellId).second){error="分组井 ID 无效或重复";return false;}return true;}
bool validate(const InterpretationDocument& v,std::string& error){
    if(v.name.empty()){error="解释方案名称不能为空";return false;}std::set<long long> wellIds;
    for(const auto& w:v.wells){if(w.wellId<=0||!wellIds.insert(w.wellId).second||w.input.wellId!=w.wellId){error="解释井 ID 无效、重复或与输入不一致";return false;}if(!seriesValid(w.input.log,error))return false;
      if((!w.input.rawLogValues.empty()&&w.input.rawLogValues.size()!=w.input.log.values.size())||(!w.input.sourceQuality.empty()&&w.input.sourceQuality.size()!=w.input.log.values.size())){error="原始曲线及来源质量数组与输入轴不一致";return false;}
      if((!w.input.logTvdValues.empty()&&w.input.logTvdValues.size()!=w.input.log.values.size())||(!w.input.logTvdssValues.empty()&&w.input.logTvdssValues.size()!=w.input.log.values.size())){error="TVD/TVDSS数值轴与输入主轴不一致";return false;}
      std::set<std::string> seriesIds;for(const auto& a:w.series)if(a.id.empty()||!seriesIds.insert(a.id).second||!seriesValid(a,error)){if(error.empty())error="派生曲线 ID 无效或重复";return false;}
      std::unordered_map<std::string,const InterpretationBoundary*> boundaries;for(const auto& b:w.boundaries){if(b.id.empty()||!boundaries.emplace(b.id,&b).second||!finiteOrMissing(b.md)||!finiteOrMissing(b.originalMd)||!finiteOrMissing(b.time)){error="边界 ID 或数值无效";return false;}if(b.locked&&!std::isfinite(b.md)){error="锁定边界必须具有有效井深";return false;}}
      std::unordered_map<std::string,const InterpretationCycle*> cycles;for(const auto& c:w.cycles){auto t=boundaries.find(c.topBoundaryId),b=boundaries.find(c.bottomBoundaryId);if(c.id.empty()||!cycles.emplace(c.id,&c).second||t==boundaries.end()||b==boundaries.end()||!std::isfinite(t->second->md)||!std::isfinite(b->second->md)||t->second->md>=b->second->md){error="旋回引用不存在、重复或顶底顺序无效";return false;}}
      for(const auto& c:w.cycles){if(c.parentId.empty())continue;auto p=cycles.find(c.parentId);if(p==cycles.end()||p->second->level>=c.level||boundaries.at(c.topBoundaryId)->md<boundaries.at(p->second->topBoundaryId)->md||boundaries.at(c.bottomBoundaryId)->md>boundaries.at(p->second->bottomBoundaryId)->md){error="旋回父子关系或范围无效";return false;}if((boundaries.at(c.topBoundaryId)->md==boundaries.at(p->second->topBoundaryId)->md&&c.topBoundaryId!=p->second->topBoundaryId)||(boundaries.at(c.bottomBoundaryId)->md==boundaries.at(p->second->bottomBoundaryId)->md&&c.bottomBoundaryId!=p->second->bottomBoundaryId)){error="父子旋回重合端点必须共享边界身份";return false;}}
      // Same-level touching intervals must share identity, not duplicate equal-depth picks.
      for(size_t i=0;i<w.cycles.size();++i)for(size_t j=i+1;j<w.cycles.size();++j){const auto& a=w.cycles[i];const auto& b=w.cycles[j];if(a.level!=b.level||a.segmentId!=b.segmentId)continue;const auto at=boundaries.at(a.topBoundaryId)->md,ab=boundaries.at(a.bottomBoundaryId)->md,bt=boundaries.at(b.topBoundaryId)->md,bb=boundaries.at(b.bottomBoundaryId)->md;if(std::max(at,bt)<std::min(ab,bb)){error="同段同级旋回不能重叠";return false;}if((ab==bt&&a.bottomBoundaryId!=b.topBoundaryId)||(bb==at&&b.bottomBoundaryId!=a.topBoundaryId)){error="相邻旋回必须引用同一公共边界";return false;}}
    }
    for(const auto& t:v.templates)if(!validate(t,error))return false;for(const auto& c:v.clusterings)if(!validate(c,error))return false;
    if(!v.activeTemplateId.empty()&&std::count_if(v.templates.begin(),v.templates.end(),[&](const auto& t){return t.id==v.activeTemplateId;})!=1){error="采用模板必须明确对应一个已保存版本";return false;}
    if(!v.activeClusteringId.empty()&&std::count_if(v.clusterings.begin(),v.clusterings.end(),[&](const auto& c){return c.id==v.activeClusteringId;})!=1){error="采用分组必须明确对应一个已保存版本";return false;}
    return true;
}
#undef S
#undef I
#undef N
#undef B
#undef D
#undef SS
#undef RS
#undef RI
#undef RN
#undef RB
#undef RD
#undef RSS
}
