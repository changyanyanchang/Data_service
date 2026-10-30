#include "paleo_data/service.h"
#include "store/interpretation_store.h"
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <bit>

namespace paleo::data {
using namespace detail;
namespace ix=detail::interpretation;
namespace {
class ReadTransaction {
    ProjectStore& store_;
public:
    bool active;
    ReadTransaction(ProjectStore& s,std::string& e):store_(s),active(s.exec("BEGIN",{},e)){}
    ~ReadTransaction(){if(active)store_.rollback();}
    bool finish(std::string& e){if(!active||!store_.commit(e))return false;active=false;return true;}
};
bool stopped(const CancelCheck& cancelled,std::string& error){if(cancelled&&cancelled()){error="操作已取消";return true;}return false;}
bool belongs(DataService& service,const std::string& kind,long long id,long long wellId,DataEntry& found,std::string& error){
    std::vector<DataEntry> entries;if(!service.listData(kind,wellId,entries,error))return false;
    const auto it=std::find_if(entries.begin(),entries.end(),[=](const auto& v){return v.id==id&&v.wellId==wellId;});
    if(it==entries.end()){error="选中的"+kind+"资料不存在或不属于该井";return false;}found=*it;return true;
}
QJsonObject ref(const DataEntry& entry){return {{"kind",q(entry.kind)},{"id",QString::number(entry.id)},{"wellId",QString::number(entry.wellId)},{"sourceId",QString::number(entry.sourceId)},{"parentId",QString::number(entry.parentId)}};}
bool saveSnapshot(ProjectStore& st,const CorrelationInputSnapshot& value,std::string& error){
    if(value.id.empty()||value.projectId.empty()||value.fingerprint.empty()){error="输入快照身份不完整";return false;}
    const auto payload=ix::bytes(ix::encode(value));const auto checksum=ix::digest(payload);QSqlQuery exists;
    if(!st.exec("SELECT digest,payload FROM correlation_input_snapshots WHERE id=?",{q(value.id)},error,&exists))return false;
    if(exists.next()){
        QJsonObject original;CorrelationInputSnapshot stored;
        if(!ix::parse(exists.value(1).toByteArray(),exists.value(0).toString(),original,error)||!ix::decode(original,stored,error))return false;
        // Additive optional fields may be absent in older payloads; compare the typed meaning,
        // while retaining the original immutable bytes and checksum in the database.
        if(ix::digest(ix::bytes(ix::encode(stored)))!=checksum){error="输入快照不可覆盖";return false;}return true;
    }
    return st.exec("INSERT INTO correlation_input_snapshots(id,project_id,fingerprint,created,payload,digest) VALUES(?,?,?,?,?,?)",{q(value.id),q(value.projectId),q(value.fingerprint),q(value.created),payload,checksum},error);
}
template<class T> bool saveNamed(ProjectStore& st,const QString& table,const T& value,std::string& error){
    if(!ix::validate(value,error))return false;const auto payload=ix::bytes(ix::encode(value));const auto checksum=ix::digest(payload);QSqlQuery r;
    if(!st.exec("SELECT digest,payload FROM "+table+" WHERE id=? AND version=?",{q(value.id),q(value.version)},error,&r))return false;
    if(r.next()){QJsonObject original;T stored;if(!ix::parse(r.value(1).toByteArray(),r.value(0).toString(),original,error)||!ix::decode(original,stored,error))return false;if(ix::digest(ix::bytes(ix::encode(stored)))!=checksum){error="已有版本不可覆盖，请创建新版本";return false;}return true;}
    return st.exec("INSERT INTO "+table+"(id,version,name,payload,digest) VALUES(?,?,?,?,?)",{q(value.id),q(value.version),q(value.name),payload,checksum},error);
}
template<class T> bool readNamed(ProjectStore& st,const QString& table,const std::string& id,const std::string& version,T& out,std::string& error){
    out={};error.clear();if(id.empty()||version.empty()){error="请明确指定对象 ID 和版本";return false;}QSqlQuery r;
    if(!st.exec("SELECT payload,digest FROM "+table+" WHERE id=? AND version=?",{q(id),q(version)},error,&r))return false;
    if(!r.next()){error="指定版本不存在";return false;}QJsonObject o;T tmp;if(!ix::parse(r.value(0).toByteArray(),r.value(1).toString(),o,error)||!ix::decode(o,tmp,error))return false;
    if(tmp.id!=id||tmp.version!=version){error="对象身份与记录不一致";return false;}out=std::move(tmp);return true;
}
template<class T> bool listNamed(ProjectStore& st,const QString& table,std::vector<T>& out,std::string& error){
    out.clear();error.clear();QSqlQuery r;if(!st.exec("SELECT payload,digest FROM "+table+" ORDER BY name,id,version",{},error,&r))return false;std::vector<T> values;
    while(r.next()){QJsonObject o;T a;if(!ix::parse(r.value(0).toByteArray(),r.value(1).toString(),o,error)||!ix::decode(o,a,error))return false;values.push_back(std::move(a));}out=std::move(values);return true;
}
}

bool DataService::captureCorrelationInputs(const CorrelationInputRequest& request,CorrelationInputSnapshot& result,std::string& error,const CancelCheck& cancelled){
    result={};error.clear();if(request.wells.empty()||request.wells.size()>256){error="请选择1至256口井";return false;}
    if(std::isinf(request.firstDepth)||std::isinf(request.lastDepth)||(std::isfinite(request.firstDepth)&&std::isfinite(request.lastDepth)&&request.firstDepth>request.lastDepth)){error="输入深度范围无效";return false;}
    if(!std::isfinite(request.surfaceSearchRadius)||request.surfaceSearchRadius<=0){error="层位搜索半径必须为有限正数";return false;}
    auto& st=impl_->store;ReadTransaction readTx(st,error);if(!readTx.active)return false;
    std::vector<CorrelationTimeDepthPreference> timeDepthPreferences;
    if(std::any_of(request.wells.begin(),request.wells.end(),[](const auto& well){return well.timeDepthId>0;})&&!readCorrelationTimeDepthPreferences(timeDepthPreferences,error))return false;
    CorrelationInputSnapshot snapshot;if(!ix::projectId(st,snapshot.projectId,error))return false;snapshot.id=s(ix::newId());snapshot.created=s(ix::now());
    std::set<long long> wells;std::set<std::string> roles;long long totalValues=0;
    for(const auto& selection:request.wells){
        if(stopped(cancelled,error))return false;if(selection.wellId<=0||selection.curveId<=0||!wells.insert(selection.wellId).second){error="井或曲线 ID 无效，或重复选择井";return false;}
        CorrelationWellInput input;input.wellId=selection.wellId;input.curveId=selection.curveId;input.timeDepthId=selection.timeDepthId;input.trajectoryId=selection.trajectoryId;input.intervalTrackId=selection.intervalTrackId;input.processed=selection.processed;input.confirmedVertical=selection.confirmedVertical;
        DataEntry curve;if(!belongs(*this,"curves",selection.curveId,selection.wellId,curve,error)||!readWellInfo(selection.wellId,input.well,error))return false;input.logSetId=curve.parentId;QJsonArray refs;refs.append(ref(curve));
        LogSet logs;if(!readLogSet(curve.parentId,selection.processed,logs,error))return false;input.depthInfo=logs.depthInfo;
        QSqlQuery defaults;if(!st.exec("SELECT depth_json FROM wells WHERE id=?",{selection.wellId},error,&defaults)||!defaults.next()){if(error.empty())error="井默认深度约定不可读取";return false;}input.surveyDepthInfo=readDepth(object(defaults.value(0).toString()));defaults.finish();
        CurveSlice slice;if(!readCurveSlice(selection.curveId,request.firstDepth,request.lastDepth,selection.processed,slice,error))return false;
        totalValues+=static_cast<long long>(slice.depths.size());if(totalValues>16000000){error="输入曲线超过本次1600万样点预算，请缩小井组或深度区间";return false;}
        if(slice.depths.empty()){error="所选曲线在请求区间没有样点";return false;}
        const auto& axis=logs.samplingAxis;
        if((!axis.tvdValues.empty()&&axis.tvdValues.size()!=axis.depthValues.size())||(!axis.tvdssValues.empty()&&axis.tvdssValues.size()!=axis.depthValues.size())){error="原始TVD/TVDSS与测井主轴长度不一致";return false;}
        for(size_t i=0;i<axis.depthValues.size();++i){const auto d=axis.depthValues[i];if((std::isfinite(request.firstDepth)&&(!std::isfinite(d)||d<request.firstDepth))||(std::isfinite(request.lastDepth)&&(!std::isfinite(d)||d>request.lastDepth)))continue;if(!axis.tvdValues.empty())input.logTvdValues.push_back(axis.tvdValues[i]);if(!axis.tvdssValues.empty())input.logTvdssValues.push_back(axis.tvdssValues[i]);}
        input.rawLogValues=slice.rawValues;input.sourceQuality=slice.quality;input.log.id="source-curve-"+std::to_string(slice.curveId);input.log.name=slice.name;input.log.kind="SOURCE_LOG";input.log.unit=slice.unit;input.log.axisKind=input.depthInfo.depthType;input.log.axisUnit=input.depthInfo.depthUnit;input.log.axis=slice.depths;input.log.values=slice.values;input.log.quality.resize(slice.values.size(),0);
        for(size_t i=0;i<input.log.values.size();++i){if(!std::isfinite(input.log.axis[i])){error="输入曲线含无效深度；请先核对源资料";return false;}if((slice.quality[i]&1)||!std::isfinite(slice.values[i])){input.log.values[i]=Missing;input.log.quality[i]=1;}}
        if(input.depthInfo.depthType.empty()||input.depthInfo.depthUnit.empty())input.warnings.push_back("曲线深度类型或单位尚未确认");
        if(selection.timeDepthId){
            DataEntry e;if(!belongs(*this,"time_depth",selection.timeDepthId,selection.wellId,e,error)||!readTimeDepth(selection.timeDepthId,input.timeDepth,error))return false;
            auto reference=ref(e);const auto preferred=std::find_if(timeDepthPreferences.begin(),timeDepthPreferences.end(),[&](const auto& value){return value.wellId==selection.wellId&&value.timeDepthId==selection.timeDepthId;});
            // Preserve sourced business assumptions as audit evidence, never as
            // confirmed source metadata. Source refs participate in both hashes.
            if(preferred!=timeDepthPreferences.end())reference.insert("businessConventionEvidence",QJsonDocument::fromJson(QByteArray::fromStdString(preferred->evidenceJson)).object());
            refs.append(reference);
        }else input.warnings.push_back("未选择时深关系");
        if(selection.trajectoryId){DataEntry e;if(!belongs(*this,"trajectories",selection.trajectoryId,selection.wellId,e,error)||!readTrajectory(selection.trajectoryId,input.survey,error))return false;refs.append(ref(e));}
        else if(!selection.confirmedVertical){
            std::set<double> positionedDepths;for(const auto& point:input.timeDepth.depthTimePairs)if(std::isfinite(point.depthValue)&&std::isfinite(point.x)&&std::isfinite(point.y))positionedDepths.insert(point.depthValue);
            if(positionedDepths.size()<2)input.warnings.push_back("缺少足够沿井时深位置点，且未选择测斜或确认直井");
            else if(input.timeDepth.coordinateSystem.empty()||input.timeDepth.coordinateUnit.empty()||input.timeDepth.depthInfo.depthType.empty()||input.timeDepth.depthInfo.depthUnit.empty())input.warnings.push_back("时深表已有沿井位置点；其坐标或深度约定尚需确认，无需另行假定直井");
        }
        if(selection.intervalTrackId){
            DataEntry e;std::vector<IntervalRecord> records;if(!belongs(*this,"intervals",selection.intervalTrackId,selection.wellId,e,error)||!readIntervalTrackInfo(selection.intervalTrackId,input.manualTrack,error)||!readIntervals(selection.intervalTrackId,Missing,Missing,records,error))return false;refs.append(ref(e));
            for(const auto& record:records){input.manualTrack.intervals.push_back(record.value);if(input.manualTrack.depthInfo.depthType!="MD"||input.manualTrack.depthInfo.depthUnit!=input.depthInfo.depthUnit||input.manualTrack.depthInfo.mdReference!=input.depthInfo.mdReference)continue;
              for(int side=0;side<2;++side){const double depth=side?record.value.bottomDepth:record.value.topDepth;if(!std::isfinite(depth)||(side&&record.value.bottomNotPenetrated==1))continue;InterpretationBoundary marker;marker.id="manual-"+std::to_string(record.id)+(side?"-bottom":"-top");marker.name=record.value.intervalName+(side?"底":"顶");marker.role="manual_reference";marker.source="manual_track";marker.status="reference";marker.md=depth;marker.originalMd=depth;marker.locked=true;marker.evidenceJson=s(ix::bytes(QJsonObject{{"trackId",QString::number(selection.intervalTrackId)},{"intervalId",QString::number(record.id)},{"side",side?"bottom":"top"}}));input.markers.push_back(std::move(marker));}
            }
        }
        if(selection.formationTrackId){DataEntry e;std::vector<IntervalRecord> records;if(!belongs(*this,"intervals",selection.formationTrackId,selection.wellId,e,error)||!readIntervalTrackInfo(selection.formationTrackId,input.formationTrack,error)||!readIntervals(selection.formationTrackId,Missing,Missing,records,error))return false;input.formationTrackId=selection.formationTrackId;refs.append(ref(e));for(const auto& record:records)input.formationTrack.intervals.push_back(record.value);}
        if(selection.manualLevel3TrackId){DataEntry e;std::vector<IntervalRecord> records;if(!belongs(*this,"intervals",selection.manualLevel3TrackId,selection.wellId,e,error)||!readIntervalTrackInfo(selection.manualLevel3TrackId,input.manualLevel3Track,error)||!readIntervals(selection.manualLevel3TrackId,Missing,Missing,records,error))return false;input.manualLevel3TrackId=selection.manualLevel3TrackId;refs.append(ref(e));for(const auto& record:records)input.manualLevel3Track.intervals.push_back(record.value);}
        input.sourceRefsJson=s(QJsonDocument(refs).toJson(QJsonDocument::Compact));snapshot.wells.push_back(std::move(input));
    }
    constexpr long long horizonBudget=2000000;long long horizonPoints=0;
    for(const auto& selection:request.horizons){
        if(stopped(cancelled,error))return false;if(selection.resourceId<=0||selection.role.empty()||!roles.insert(selection.role).second){error="层位资源 ID 或角色无效，或角色重复";return false;}
        CorrelationHorizon horizon;horizon.resourceId=selection.resourceId;horizon.role=selection.role;
        if(!readHorizon(selection.resourceId,0,1,horizon.surface,error))return false;horizon.surface.points.clear();QJsonArray regions;
        std::set<std::tuple<int,int,double,double,std::uint64_t>> included;
        for(auto& well:snapshot.wells){
            if(stopped(cancelled,error))return false;
            double minX=std::numeric_limits<double>::infinity(),minY=minX,maxX=-minX,maxY=-minX;std::string crs,unit,positionSource;
            for(const auto& p:well.timeDepth.depthTimePairs)if(std::isfinite(p.x)&&std::isfinite(p.y)){minX=std::min(minX,p.x);minY=std::min(minY,p.y);maxX=std::max(maxX,p.x);maxY=std::max(maxY,p.y);}
            if(std::isfinite(minX)){crs=well.timeDepth.coordinateSystem;unit=well.timeDepth.coordinateUnit;positionSource="time_depth_xyz";}
            else if(std::isfinite(well.well.wellheadX)&&std::isfinite(well.well.wellheadY)){minX=maxX=well.well.wellheadX;minY=maxY=well.well.wellheadY;crs=well.well.coordinateSystem;unit=well.well.coordinateUnit;positionSource="wellhead";}
            else{well.warnings.push_back(selection.role+"未提取局部层位：缺少时深XYZ和井口坐标");continue;}
            if(!crs.empty()&&!horizon.surface.coordinateSystem.empty()&&crs!=horizon.surface.coordinateSystem){well.warnings.push_back(selection.role+"未提取局部层位：已知坐标系不一致");continue;}
            double factor=1.0;const auto targetUnit=horizon.surface.coordinateUnit;
            if(!unit.empty()&&!targetUnit.empty()&&unit!=targetUnit){if(unit=="ft"&&targetUnit=="m")factor=0.3048;else if(unit=="m"&&targetUnit=="ft")factor=1.0/0.3048;else{well.warnings.push_back(selection.role+"未提取局部层位：坐标单位无法明确换算");continue;}}
            const bool confirmed=!crs.empty()&&!horizon.surface.coordinateSystem.empty()&&!unit.empty()&&!targetUnit.empty();
            if(!confirmed)well.warnings.push_back(selection.role+"局部层位仅按数值坐标提取候选；坐标系/单位尚未确认，不能直接自动落井");
            minX=minX*factor-request.surfaceSearchRadius;maxX=maxX*factor+request.surfaceSearchRadius;minY=minY*factor-request.surfaceSearchRadius;maxY=maxY*factor+request.surfaceSearchRadius;
            SeismicHorizon local;if(!readHorizonRegion(selection.resourceId,minX,minY,maxX,maxY,500000,local,error))return false;
            regions.append(QJsonObject{{"wellId",QString::number(well.wellId)},{"positionSource",q(positionSource)},{"xMin",minX},{"yMin",minY},{"xMax",maxX},{"yMax",maxY},{"padding",request.surfaceSearchRadius},{"coordinateUnit",q(targetUnit)},{"conventionsConfirmed",confirmed},{"sourcePointCount",int(local.points.size())}});
            if(local.points.empty())well.warnings.push_back(selection.role+"在所选井局部范围内没有层位支持点");
            for(const auto& p:local.points){const auto z=std::isnan(p.zTime)?std::uint64_t(0x7ff8000000000000ULL):std::bit_cast<std::uint64_t>(p.zTime);if(!included.emplace(p.inlineNumber,p.crosslineNumber,p.x,p.y,z).second)continue;horizon.surface.points.push_back(p);if(++horizonPoints>horizonBudget){error="局部层位支持点超过本次200万点预算；请减少井数或缩小搜索半径";return false;}}
        }
        horizon.supportRegionsJson=s(QJsonDocument(regions).toJson(QJsonDocument::Compact));
        if(horizon.surface.points.empty())snapshot.warnings.push_back(selection.role+"层位没有可用点");CorrelationInputSnapshot single;single.projectId=snapshot.projectId;single.horizons.push_back(horizon);horizon.inputFingerprint=s(ix::digest(ix::bytes(ix::encode(single))));snapshot.horizons.push_back(std::move(horizon));
    }
    for(auto& well:snapshot.wells){CorrelationInputSnapshot single;single.projectId=snapshot.projectId;single.wells.push_back(well);well.inputFingerprint=s(ix::digest(ix::bytes(ix::encode(single))));well.log.inputFingerprint=well.inputFingerprint;}
    if(stopped(cancelled,error)||!readTx.finish(error))return false;
    auto content=ix::encode(snapshot);content.remove("id");content.remove("created");content.remove("fingerprint");snapshot.fingerprint=s(ix::digest(ix::bytes(content)));
    Transaction tx(st,error);if(!tx.active||stopped(cancelled,error)||!saveSnapshot(st,snapshot,error)||stopped(cancelled,error)||!tx.commit(error))return false;
    result=std::move(snapshot);return true;
}

bool DataService::readHorizonRegion(long long id,double xMin,double yMin,double xMax,double yMax,int maxPoints,SeismicHorizon& result,std::string& error)const{
    result={};error.clear();if(!std::isfinite(xMin)||!std::isfinite(yMin)||!std::isfinite(xMax)||!std::isfinite(yMax)||xMin>xMax||yMin>yMax||maxPoints<1||maxPoints>2000000){error="层位区域范围或点数预算无效";return false;}
    SeismicHorizon surface;if(!readHorizon(id,0,1,surface,error))return false;surface.points.clear();QSqlQuery r;
    if(!impl_->store.exec("SELECT inline_no,crossline_no,x,y,z FROM horizon_points WHERE horizon_id=? AND x>=? AND x<=? AND y>=? AND y<=? ORDER BY seq,id LIMIT ?",{id,xMin,xMax,yMin,yMax,maxPoints+1},error,&r))return false;
    while(r.next()){if(static_cast<int>(surface.points.size())==maxPoints){error="层位局部范围超过点数预算，请缩小搜索区域；未返回抽稀结果";return false;}surface.points.push_back({r.value(0).toInt(),r.value(1).toInt(),value(r.value(2)),value(r.value(3)),value(r.value(4))});}
    result=std::move(surface);return true;
}

bool DataService::readCorrelationSnapshot(const std::string& id,CorrelationInputSnapshot& result,std::string& error)const{
    result={};error.clear();QSqlQuery r;if(!impl_->store.exec("SELECT payload,digest FROM correlation_input_snapshots WHERE id=?",{q(id)},error,&r))return false;if(!r.next()){error="输入快照不存在";return false;}QJsonObject o;CorrelationInputSnapshot tmp;if(!ix::parse(r.value(0).toByteArray(),r.value(1).toString(),o,error)||!ix::decode(o,tmp,error))return false;if(tmp.id!=id){error="输入快照身份不匹配";return false;}result=std::move(tmp);return true;
}

bool DataService::commitInterpretation(const InterpretationDocument& document,const InterpretationCommitOptions& options,InterpretationCommitResult& result,std::string& error){
    result={};error.clear();if(options.idempotencyKey.empty()){error="保存请求必须有幂等键";return false;}if(!ix::validate(document,error))return false;
    auto& st=impl_->store;std::string project;if(!ix::projectId(st,project,error))return false;
    if((!document.projectId.empty()&&document.projectId!=project)||(!document.snapshot.projectId.empty()&&document.snapshot.projectId!=project)){error="解释或输入快照属于其他工区";return false;}
    QJsonObject request{{"document",ix::encode(document)},{"expectedHeadRevision",q(options.expectedHeadRevision)}};const auto requestHash=ix::digest(ix::bytes(request));
    Transaction tx(st,error);if(!tx.active)return false;QSqlQuery existing;
    if(!st.exec("SELECT request_hash,interpretation_id,revision FROM interpretation_commits WHERE idempotency_key=?",{q(options.idempotencyKey)},error,&existing))return false;
    if(existing.next()){if(existing.value(0).toString()!=requestHash){error="幂等键已用于不同保存内容";return false;}InterpretationCommitResult reused{s(existing.value(1).toString()),s(existing.value(2).toString()),true};existing.finish();if(!tx.commit(error))return false;result=std::move(reused);return true;}existing.finish();
    InterpretationDocument saved=document;saved.id=document.id.empty()?s(ix::newId()):document.id;saved.projectId=project;saved.parentRevision=options.expectedHeadRevision;saved.revision=s(ix::newId());saved.created=s(ix::now());
    QSqlQuery head;if(!st.exec("SELECT head_revision FROM interpretations WHERE id=?",{q(saved.id)},error,&head))return false;const bool found=head.next();const std::string current=found?s(head.value(0).toString()):std::string();head.finish();
    if((found&&current!=options.expectedHeadRevision)||(!found&&!options.expectedHeadRevision.empty())){error="解释版本冲突，请刷新或另存方案";return false;}
    if(!saved.snapshot.id.empty()&&!saveSnapshot(st,saved.snapshot,error))return false;
    for(const auto& t:saved.templates)if(!saveNamed(st,"interpretation_templates",t,error))return false;
    for(const auto& c:saved.clusterings)if(!saveNamed(st,"interpretation_clusterings",c,error))return false;
    if(!found&&!st.exec("INSERT INTO interpretations(id,name,head_revision,created,well_count) VALUES(?,?,?,?,?)",{q(saved.id),q(saved.name),q(saved.revision),q(saved.created),int(saved.wells.size())},error))return false;
    QJsonObject manifest;if(!ix::encodeStored(st,saved,manifest,error))return false;const auto payload=ix::bytes(manifest);
    if(!st.exec("INSERT INTO interpretation_revisions(revision,interpretation_id,parent_revision,created,author,note,payload,digest) VALUES(?,?,?,?,?,?,?,?)",{q(saved.revision),q(saved.id),q(saved.parentRevision),q(saved.created),q(saved.author),q(saved.note),payload,ix::digest(payload)},error))return false;
    if(found){QSqlQuery updated;if(!st.exec("UPDATE interpretations SET name=?,head_revision=?,well_count=? WHERE id=? AND head_revision=?",{q(saved.name),q(saved.revision),int(saved.wells.size()),q(saved.id),q(options.expectedHeadRevision)},error,&updated))return false;if(updated.numRowsAffected()!=1){error="解释版本冲突";return false;}}
    if(!st.exec("INSERT INTO interpretation_commits(idempotency_key,request_hash,interpretation_id,revision) VALUES(?,?,?,?)",{q(options.idempotencyKey),requestHash,q(saved.id),q(saved.revision)},error)||!tx.commit(error))return false;
    result={saved.id,saved.revision,false};return true;
}
bool DataService::listInterpretations(std::vector<InterpretationSummary>& result,std::string& error)const{
    result.clear();error.clear();QSqlQuery r;if(!impl_->store.exec("SELECT id,name,head_revision,created,well_count FROM interpretations ORDER BY created,id",{},error,&r))return false;while(r.next())result.push_back({s(r.value(0).toString()),s(r.value(1).toString()),s(r.value(2).toString()),s(r.value(3).toString()),r.value(4).toInt()});return true;
}
bool DataService::listInterpretationRevisions(const std::string& id,std::vector<InterpretationRevisionSummary>& result,std::string& error)const{
    result.clear();error.clear();QSqlQuery r;if(!impl_->store.exec("SELECT revision,parent_revision,created,author,note FROM interpretation_revisions WHERE interpretation_id=? ORDER BY rowid",{q(id)},error,&r))return false;while(r.next())result.push_back({s(r.value(0).toString()),s(r.value(1).toString()),s(r.value(2).toString()),s(r.value(3).toString()),s(r.value(4).toString())});return true;
}
bool DataService::readInterpretationRevision(const std::string& id,const std::string& revision,InterpretationDocument& result,std::string& error)const{
    result={};error.clear();QSqlQuery r;const QString query=revision.empty()?"SELECT r.payload,r.digest,r.revision FROM interpretation_revisions r JOIN interpretations i ON r.revision=i.head_revision WHERE i.id=?":"SELECT payload,digest,revision FROM interpretation_revisions WHERE interpretation_id=? AND revision=?";QVariantList args{q(id)};if(!revision.empty())args<<q(revision);
    if(!impl_->store.exec(query,args,error,&r))return false;if(!r.next()){error="解释方案或修订不存在";return false;}QJsonObject o;InterpretationDocument tmp;if(!ix::parse(r.value(0).toByteArray(),r.value(1).toString(),o,error)||!ix::decodeStored(impl_->store,o,tmp,error))return false;if(tmp.id!=id||tmp.revision!=s(r.value(2).toString())){error="解释版本身份不匹配";return false;}result=std::move(tmp);return true;
}
bool DataService::saveReferenceTemplate(const InterpretationTemplate& value,std::string& error){error.clear();auto& st=impl_->store;Transaction tx(st,error);return tx.active&&saveNamed(st,"interpretation_templates",value,error)&&tx.commit(error);}
bool DataService::listReferenceTemplates(std::vector<InterpretationTemplate>& result,std::string& error)const{return listNamed(impl_->store,"interpretation_templates",result,error);}
bool DataService::readReferenceTemplate(const std::string& id,const std::string& version,InterpretationTemplate& result,std::string& error)const{return readNamed(impl_->store,"interpretation_templates",id,version,result,error);}
bool DataService::saveClustering(const InterpretationClustering& value,std::string& error){error.clear();auto& st=impl_->store;Transaction tx(st,error);return tx.active&&saveNamed(st,"interpretation_clusterings",value,error)&&tx.commit(error);}
bool DataService::listClusterings(std::vector<InterpretationClustering>& result,std::string& error)const{return listNamed(impl_->store,"interpretation_clusterings",result,error);}
bool DataService::readClustering(const std::string& id,const std::string& version,InterpretationClustering& result,std::string& error)const{return readNamed(impl_->store,"interpretation_clusterings",id,version,result,error);}
}
