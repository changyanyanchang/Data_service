// Synthetic, temporary fixtures only. No private sources or amplitude cubes are bundled.
#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
void check(bool ok,const std::string& message) {if(!ok)throw std::runtime_error(message);}
std::string u(const QString& value) {return value.toUtf8().toStdString();}
bool near(double a,double b) {return std::isfinite(a)&&std::abs(a-b)<1e-7;}
std::string depthDiagnostic(const SeismicHorizonWindowResult& result) {
    std::string message="status="+result.depthStatus+", axis="+result.depthType+"/"+result.depthReference;
    for(size_t i=0;i<std::min<size_t>(result.depthWarnings.size(),3);++i)message+="; "+result.depthWarnings[i];
    return message;
}
void fixtureSegy(const QString& path,int firstTime,int crosslineCount) {
    QByteArray bytes(3600,'\0');for(int i=0;i<3200;++i)bytes[i]=' ';
    qToBigEndian<quint16>(2000,bytes.data()+3216);qToBigEndian<quint16>(8,bytes.data()+3220);
    qToBigEndian<quint16>(5,bytes.data()+3224);qToBigEndian<quint16>(0x100,bytes.data()+3500);qToBigEndian<quint16>(1,bytes.data()+3502);
    for(int i=0;i<2;++i)for(int j=0;j<crosslineCount;++j) {
        QByteArray trace(272,'\0');qToBigEndian<quint32>(i*crosslineCount+j+1,trace.data());
        qToBigEndian<quint16>(1,trace.data()+70);qToBigEndian<quint16>(1,trace.data()+88);
        qToBigEndian<quint16>(static_cast<quint16>(static_cast<qint16>(firstTime)),trace.data()+108);
        qToBigEndian<quint16>(8,trace.data()+114);qToBigEndian<quint16>(2000,trace.data()+116);
        qToBigEndian<quint32>(100+i*2,trace.data()+188);qToBigEndian<quint32>(200+j*2,trace.data()+192);
        qToBigEndian<quint32>(500000+j*100,trace.data()+180);qToBigEndian<quint32>(4000000+i*100,trace.data()+184);
        for(int k=0;k<8;++k) {const float value=float(i*20+j*10+k);quint32 bits;std::memcpy(&bits,&value,4);qToBigEndian(bits,trace.data()+240+k*4);}
        bytes.append(trace);
    }
    QFile file(path);check(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"write synthetic SEG-Y");
}
struct Fixture {
    QTemporaryDir temp;
    DataService service;
    std::string error;
    long long wellId=0,volumeId=0;
    std::vector<DataEntry> relations,horizons;
    int firstTime=100;
    int extraSources=0;
    explicit Fixture(const std::function<void(TimeDepthTable&)>& changeSecond={},int first=100,int horizonOffset=0,int crosslineCount=3):firstTime(first) {
        check(temp.isValid(),"temporary fixture");check(service.createProject(u(temp.filePath("model")),"window semantics",error),error);
        const auto segy=temp.filePath("cube.sgy");fixtureSegy(segy,firstTime,crosslineCount);
        ImportOptions options;options.copySources=false;ImportReport report;check(service.importFile(u(segy),options,report,error),error);
        std::vector<DataEntry> volumes;check(service.listData("volumes",0,volumes,error)&&volumes.size()==1,"one volume");volumeId=volumes.front().id;
        SeismicVolume volume;check(service.readVolume(volumeId,volume,error),error);
        volume.geometry.coordinateSystem="EPSG:32650";volume.geometry.coordinateUnit="m";
        volume.sampling.timeUnit="ms";volume.sampling.timeType="TWT";volume.sampling.timeReference="fixture-datum";
        check(service.saveVolume(volumeId,volume,error),error);
        ParsedFile parsed;parsed.kind="test";parsed.status="parsed";WellDataset well;
        well.wellInfo.wellName="A";well.wellInfo.wellheadX=500000;well.wellInfo.wellheadY=4000000;
        well.wellInfo.coordinateSystem="EPSG:32650";well.wellInfo.coordinateUnit="m";
        well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit="m";well.defaultDepthInfo.mdReference="KB";
        for(int i=0;i<2;++i) {
            TimeDepthTable table;table.relationName="same-display-name";table.depthInfo=well.defaultDepthInfo;
            table.timeUnit="ms";table.timeType="TWT";table.timeReference="fixture-datum";
            table.depthTimePairs={{double(i?300:100),double(firstTime)},{double(i?500:200),double(firstTime+20)}};
            if(i==1&&changeSecond)changeSecond(table);well.timeDepthRelations.push_back(table);
        }
        parsed.wells.push_back(well);
        for(int k=0;k<2;++k) {
            SeismicHorizon horizon;horizon.horizonName=k==0?"TOP":"BASE";horizon.parentVolumeId=int(volumeId);
            horizon.coordinateSystem="EPSG:32650";horizon.coordinateUnit="m";horizon.timeUnit="ms";horizon.timeType="TWT";horizon.timeReference="fixture-datum";
            for(int i=0;i<2;++i)for(int j=0;j<2;++j)
                horizon.points.push_back({100+i*2,200+j*2,double(500000+j*100),double(4000000+i*100),double(firstTime+horizonOffset+(k==0?2:10))});
            parsed.horizons.push_back(horizon);
        }
        const auto input=temp.filePath("tables.txt");const QByteArray bytes("synthetic table source");QFile file(input);
        check(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"write synthetic tables");file.close();long long sourceId=0;
        check(service.importPreparedFile(u(input),u(input),u(input),QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().toStdString(),parsed,sourceId,error),error);
        std::vector<WellSummary> wells;check(service.listWells(wells,error)&&wells.size()==1,"one well");wellId=wells.front().id;
        check(service.listData("time_depth",wellId,relations,error)&&relations.size()==2,"two independent same-name TDRs");
        check(service.listData("horizons",0,horizons,error)&&horizons.size()==2,"two horizons");
    }
    void importExtra(const ParsedFile& parsed) {
        const QByteArray bytes="synthetic extra source "+QByteArray::number(++extraSources);
        const auto input=temp.filePath(QStringLiteral("extra-%1.txt").arg(extraSources));QFile file(input);
        check(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"write extra synthetic source");file.close();long long sourceId=0;
        check(service.importPreparedFile(u(input),u(input),u(input),QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().toStdString(),parsed,sourceId,error),error);
    }
    void duplicateTop() {
        ParsedFile parsed;parsed.kind="test";parsed.status="parsed";SeismicHorizon top;
        check(service.readHorizon(horizons[0].id,0,1000,top,error),error);parsed.horizons.push_back(top);importExtra(parsed);
    }
    SeismicTimeDepthBinding secondWell() {
        ParsedFile parsed;parsed.kind="test";parsed.status="parsed";WellDataset well;
        well.wellInfo.wellName="B";well.wellInfo.wellheadX=500200;well.wellInfo.wellheadY=4000000;
        well.wellInfo.coordinateSystem="EPSG:32650";well.wellInfo.coordinateUnit="m";
        well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit="m";well.defaultDepthInfo.mdReference="KB";
        TimeDepthTable table;table.relationName="B selected";table.depthInfo=well.defaultDepthInfo;
        table.timeUnit="ms";table.timeType="TWT";table.timeReference="fixture-datum";
        table.depthTimePairs={{500,double(firstTime)},{700,double(firstTime+20)}};well.timeDepthRelations.push_back(table);
        parsed.wells.push_back(well);importExtra(parsed);std::vector<WellSummary> wells;
        check(service.listWells(wells,error),error);
        for(const auto& candidate:wells)if(candidate.name=="B") {
            std::vector<DataEntry> tables;check(service.listData("time_depth",candidate.id,tables,error)&&tables.size()==1,"B relation");return {candidate.id,tables[0].id};
        }
        throw std::runtime_error("B well missing");
    }
    SeismicHorizonWindowQuery query() const {
        SeismicHorizonWindowQuery value;value.volumeId=volumeId;value.target.topHorizonId=horizons[0].id;value.target.bottomHorizonId=horizons[1].id;
        value.domainKind=HorizonSpatialDomainKind::ArbitraryTraceList;value.explicitTraces={{100,200},{100,202},{102,200},{102,202}};
        value.sampleMode=HorizonWindowSampleMode::SummaryAttributesOnly;value.timeDepthBindings={{wellId,relations[1].id}};return value;
    }
    SeismicHorizonWindowResult read(const SeismicHorizonWindowQuery& query) {
        SeismicHorizonWindowResult result;check(service.readSeismicHorizonWindow(query,result,error),error);check(result.validTraceCount==4,"four valid amplitude windows");return result;
    }
};
void selectedRelationsAndCoverage() {
    Fixture fixture;auto query=fixture.query();auto selected=fixture.read(query);
    check(selected.depthStatus=="available"&&selected.depthType=="MD"&&selected.depthReference=="KB","declared depth axis: "+depthDiagnostic(selected));
    check(selected.usedTimeDepthBindings.size()==1&&selected.usedTimeDepthBindings[0].timeDepthId==fixture.relations[1].id,"selected second same-name relation");
    check(near(selected.traces.front().windowTopDepthM,320)&&near(selected.traces.front().windowBottomDepthM,400),"second relation controls actual converted values");
    query.timeDepthBindings.clear();auto absent=fixture.read(query);
    check(absent.depthStatus=="unavailable"&&!absent.depthWarnings.empty()&&std::isnan(absent.traces.front().windowTopDepthM),"ambiguous/unselected TDR never chooses first");
    check(fixture.service.saveCorrelationTimeDepthPreferences({{fixture.wellId,fixture.relations[1].id,"{\"basis\":\"explicit test choice\"}"}},fixture.error),fixture.error);
    check(near(fixture.read(query).traces.front().windowTopDepthM,320),"persisted preference used");
    query.timeDepthBindings={{fixture.wellId,fixture.relations[0].id}};
    check(near(fixture.read(query).traces.front().windowTopDepthM,110),"explicit context selection overrides preference");
    query.timeDepthBindings={{fixture.wellId,fixture.relations[0].id},{fixture.wellId,fixture.relations[1].id}};
    SeismicHorizonWindowResult rejected;check(!fixture.service.readSeismicHorizonWindow(query,rejected,fixture.error)&&rejected.traces.empty(),"duplicate well selections rejected atomically");
    query.computeDepthRange=false;query.maxDepthEvaluations=0;auto disabled=fixture.read(query);
    check(disabled.depthStatus=="not_requested"&&disabled.depthWarnings.empty()&&disabled.usedTimeDepthBindings.empty(),"disabled depth conversion ignores TDR selection and does no conversion work");
    query=fixture.query();query.timeDepthBindings={{fixture.wellId+10000,fixture.relations[1].id}};
    check(fixture.read(query).depthStatus=="unavailable","foreign relation ownership does not produce depths");
    Fixture outside([](auto& table){table.depthTimePairs[1].timeValue=105;});
    auto result=outside.read(outside.query());check(result.depthStatus=="unavailable"&&std::isnan(result.traces.front().windowBottomDepthM),"no extrapolation beyond selected relationship coverage");
}
void unknownDeclarationsDoNotBecomeDepths() {
    const std::vector<std::function<void(TimeDepthTable&)>> cases{
        [](auto& t){t.timeUnit.clear();},[](auto& t){t.timeType.clear();},[](auto& t){t.timeReference.clear();},
        [](auto& t){t.timeReference="other-datum";},[](auto& t){t.depthInfo.depthType="TVD";},
        [](auto& t){t.depthInfo.depthUnit.clear();},[](auto& t){t.depthInfo.mdReference.clear();},
        [](auto& t){t.timeSign="unconfirmed-sign";},[](auto& t){t.timeSign="NEGATED";},
        [](auto& t){t.timeSign=" negated";},[](auto& t){t.timeSign="negated ";},[](auto& t){t.timeSign=" ";},
        [](auto& t){for(auto& p:t.depthTimePairs)p.timeValue=-p.timeValue;}
    };
    for(const auto& change:cases) {
        Fixture fixture(change);const auto result=fixture.read(fixture.query());
        check(result.depthStatus=="unavailable"&&!result.depthWarnings.empty(),"missing/conflicting physical declaration is reported");
        for(const auto& trace:result.traces)check(trace.depthStatus==2&&std::isnan(trace.windowTopDepthM)&&std::isnan(trace.windowBottomDepthM),"amplitudes remain usable while depths stay unknown");
    }
    Fixture coordinates;WellInfo well;check(coordinates.service.readWellInfo(coordinates.wellId,well,coordinates.error),coordinates.error);
    well.coordinateSystem="EPSG:32649";check(coordinates.service.saveWellInfo(coordinates.wellId,well,coordinates.error),coordinates.error);
    check(coordinates.read(coordinates.query()).depthStatus=="unavailable","different planar CRS is not silently mixed");
}
void explicitUnitAndSignConversions() {
    Fixture units([](auto& table){table.depthInfo.depthUnit="ft";table.timeUnit="s";table.timeType="OWT";
        for(auto& point:table.depthTimePairs){point.depthValue/=0.3048;point.timeValue/=2000.;}});
    check(near(units.read(units.query()).traces.front().windowTopDepthM,320),"explicit feet/seconds/OWT conversion");
    Fixture sign([](auto& table){table.timeSign="negated";for(auto& point:table.depthTimePairs)point.timeValue=-point.timeValue;});
    check(near(sign.read(sign.query()).traces.front().windowTopDepthM,320),"confirmed negated sign applied exactly once by readTimeDepth");
    TimeDepthTable table;check(sign.service.readTimeDepth(sign.relations[1].id,table,sign.error),sign.error);
    check(table.depthTimePairs[0].timeValue==100,"repeat read does not negate twice");
    Fixture negative({},-120);auto result=negative.read(negative.query());
    check(near(result.traces.front().windowTopDepthM,320)&&result.traces.front().windowTopTimeMs==-118,"valid signed time axis is preserved without abs");
}
void depthBudgetAndMetadataCancellation() {
    Fixture fixture;auto query=fixture.query();query.timeDepthBindings.push_back(fixture.secondWell());query.maxDepthEvaluations=16;
    auto both=fixture.read(query);check(both.usedTimeDepthBindings.size()==2,"all selected IDW relationships retained");
    bool checkedMidpoint=false;
    for(const auto& trace:both.traces)if(trace.inlineNumber==100&&trace.crosslineNumber==202) {
        check(near(trace.windowTopDepthM,420),"IDW uses both equidistant well relationships");checkedMidpoint=true;
    }
    check(checkedMidpoint,"midpoint trace present");query.maxDepthEvaluations=15;
    check(!fixture.service.readSeismicHorizonWindow(query,both,fixture.error)&&both.traces.empty()&&both.usedTimeDepthBindings.empty()&&
          fixture.error.find("maxDepthEvaluations")!=std::string::npos,"depth budget failure publishes no partial relation or trace results");
    query.computeDepthRange=false;query.maxDepthEvaluations=0;
    query.target.topHorizonId=fixture.horizons[1].id;query.target.bottomHorizonId=fixture.horizons[0].id;
    query.domainKind=HorizonSpatialDomainKind::ArbitraryTraceList;query.explicitTraces={{100,200}};
    int oneChecks=0;check(fixture.service.readSeismicHorizonWindow(query,both,fixture.error,[&]{++oneChecks;return false;}),fixture.error);
    query.explicitTraces={{100,200},{100,202},{102,200},{102,202}};
    int fourChecks=0;check(fixture.service.readSeismicHorizonWindow(query,both,fixture.error,[&]{++fourChecks;return false;}),fixture.error);
    check(both.validTraceCount==0&&fourChecks==oneChecks+3,"each metadata trace checks cancellation even when no waveform spans are valid");
    int checks=0;check(!fixture.service.readSeismicHorizonWindow(query,both,fixture.error,[&]{return ++checks==fourChecks-2;})&&
        both.traces.empty()&&fixture.error.find("取消")!=std::string::npos,"metadata cancellation clears prior results");
    Fixture outside([](auto& table){for(auto& point:table.depthTimePairs)point.timeValue+=100;},100,100);
    SeismicHorizonWindowResult empty;check(outside.service.readSeismicHorizonWindow(outside.query(),empty,outside.error),outside.error);
    check(empty.validTraceCount==0&&empty.statistics.readCalls==0&&empty.depthStatus=="available"&&empty.traces.front().depthStatus==1,
          "empty waveform spans still finalize available depth metadata");
    query=outside.query();std::swap(query.target.topHorizonId,query.target.bottomHorizonId);
    check(outside.service.readSeismicHorizonWindow(query,empty,outside.error),outside.error);
    check(empty.validTraceCount==0&&empty.depthStatus=="unavailable"&&!empty.depthWarnings.empty(),"empty invalid surfaces retain depth diagnostics");
    Fixture headers({},100,0,2);auto fromHeaders=headers.query();fromHeaders.maxDepthEvaluations=8;
    auto actual=headers.read(fromHeaders);
    check(actual.depthStatus=="available"&&near(actual.traces.front().windowTopDepthM,320),
          "four-trace survey uses existing batch header positions without an affine map: "+depthDiagnostic(actual));
    fromHeaders.maxDepthEvaluations=7;
    check(!headers.service.readSeismicHorizonWindow(fromHeaders,actual,headers.error)&&actual.traces.empty()&&
          headers.error.find("maxDepthEvaluations")!=std::string::npos,"deferred header-based conversion charges each trace once and fails atomically");
    fromHeaders=headers.query();std::swap(fromHeaders.target.topHorizonId,fromHeaders.target.bottomHorizonId);
    check(headers.service.readSeismicHorizonWindow(fromHeaders,actual,headers.error),headers.error);
    check(actual.depthStatus=="unavailable"&&!actual.depthWarnings.empty()&&actual.statistics.readCalls==0,
          "without a map or valid waveform spans, no trace position or depth is fabricated");
}
StratigraphicScheme publishScheme(Fixture& fixture,StratigraphicScheme scheme,const std::string& expected={}) {
    StratigraphicScheme committed;std::vector<FrameworkIssue> issues;
    check(fixture.service.commitStratigraphicScheme(scheme,expected,committed,issues,fixture.error),fixture.error);return committed;
}
void invalidSpansStillObeyFixedTensorBudget() {
    const auto checkBudget=[](Fixture& fixture,SeismicHorizonWindowQuery query,const std::string& context) {
        query.computeDepthRange=false;query.sampleMode=HorizonWindowSampleMode::ResampledFixedCount;
        query.resampledSampleCount=32;query.domainKind=HorizonSpatialDomainKind::ArbitraryTraceList;
        query.explicitTraces={{100,200}};query.maxValues=1;
        SeismicHorizonWindowResult result;
        check(!fixture.service.readSeismicHorizonWindow(query,result,fixture.error)&&result.traces.empty()&&result.amplitudes.empty()&&
              fixture.error.find("maxValues")!=std::string::npos,context+": missing spans cannot bypass fixed tensor budget");
        query.maxValues=32;check(fixture.service.readSeismicHorizonWindow(query,result,fixture.error),fixture.error);
        check(result.validTraceCount==0&&result.amplitudes.size()==32,context+": exact fixed tensor budget allows review-only output");
        for(float value:result.amplitudes)check(std::isnan(value),context+": invalid spans preserve missing amplitudes");
    };
    Fixture missingSurface;auto missing=missingSurface.query();std::swap(missing.target.topHorizonId,missing.target.bottomHorizonId);
    checkBudget(missingSurface,missing,"inverted/missing surface");
    Fixture outside({},100,100);checkBudget(outside,outside.query(),"window outside seismic sampling");
    auto missingTrace=missingSurface.query();missingTrace.computeDepthRange=false;
    missingTrace.sampleMode=HorizonWindowSampleMode::ResampledFixedCount;missingTrace.resampledSampleCount=32;
    missingTrace.domainKind=HorizonSpatialDomainKind::ArbitraryTraceList;missingTrace.explicitTraces={{999,999}};missingTrace.maxValues=1;
    SeismicHorizonWindowResult result;check(!missingSurface.service.readSeismicHorizonWindow(missingTrace,result,missingSurface.error)&&
        result.amplitudes.empty()&&result.traces.empty()&&missingSurface.error.find("maxValues")!=std::string::npos,"missing explicit trace is budgeted before metadata early exits");
}
void fixedSchemeAndStableTargets() {
    Fixture fixture;StratigraphicScheme scheme;scheme.id="default";scheme.name="synthetic linked scheme";
    for(int i=0;i<2;++i) {
        StratigraphicBoundary boundary;boundary.name=i?"BASE":"TOP";boundary.order=i+1;
        boundary.seismicHorizonResourceId=fixture.horizons[size_t(i)].id;scheme.boundaries.push_back(boundary);
    }
    framework::deriveUnits(scheme);scheme.divisions[0].fractions={0.1,0.4,0.8};auto original=publishScheme(fixture,scheme);
    auto query=fixture.query();query.target.schemeId="default";query.target.revision=original.revision.value;
    query.target.subIntervalId=original.subIntervals[1].id;
    auto pinned=fixture.read(query);check(pinned.schemeRevision==original.revision&&near(pinned.fractionTop,0.1)&&near(pinned.fractionBottom,0.4),"typed target honors fixed revision fractions");
    auto edited=original;edited.divisions[0].fractions={0.2,0.6,0.9};auto head=publishScheme(fixture,edited,original.revision.value);
    check(near(fixture.read(query).fractionTop,0.1),"new head never changes fixed revision query");
    auto latest=query;latest.target.revision.clear();check(near(fixture.read(latest).fractionTop,0.2),"empty revision explicitly follows selected scheme head");
    const auto reject=[&](const SeismicHorizonWindowQuery& invalid,const std::string& message) {
        SeismicHorizonWindowResult result=pinned;
        check(!fixture.service.readSeismicHorizonWindow(invalid,result,fixture.error)&&result.traces.empty()&&result.schemeId.empty()&&!fixture.error.empty(),message);
    };
    auto invalid=query;invalid.target.schemeId="missing-scheme";reject(invalid,"explicit missing scheme never falls back to supplied horizon resources");
    invalid=query;invalid.target.revision="missing-revision";reject(invalid,"missing fixed revision never falls back to head or names");
    invalid=query;invalid.target.subIntervalId={"TOP-BASE-S2"};invalid.target.intervalId=original.intervals[0].id;invalid.target.unitName="TOP-BASE-S2";
    reject(invalid,"typed subinterval ID never reinterprets a display label or falls back to parent");
    invalid=query;invalid.target.subIntervalId={};invalid.target.intervalId={"TOP-BASE"};invalid.target.unitName="TOP-BASE";
    reject(invalid,"typed interval ID never reinterprets a display label");
    invalid=query;invalid.target.intervalId={"other-parent"};reject(invalid,"conflicting typed parent and subinterval fail");
    invalid=query;invalid.target.subIndex=3;reject(invalid,"typed subinterval and explicit subindex must agree");
    invalid=query;invalid.target.topHorizonId=fixture.horizons[1].id;reject(invalid,"resource override cannot mix with fixed scheme boundary");
    invalid=query;invalid.target.subIntervalId={};invalid.target.unitName="missing-unit";reject(invalid,"fixed scheme with unresolved unit never uses direct resources as fallback");
    edited=head;for(auto& boundary:edited.boundaries)boundary.seismicHorizonResourceId=0;
    auto unlinked=publishScheme(fixture,edited,head.revision.value);invalid=query;invalid.target.revision=unlinked.revision.value;
    reject(invalid,"unlinked fixed boundaries never use current names or supplied resource fallback");
    check(fixture.read(query).schemeRevision==original.revision,"earlier linked snapshot remains readable");
    StratigraphicScheme ambiguous;ambiguous.id="ambiguous";int order=0;
    for(const auto& name:{"A","B-C","A-B","C"}) {
        StratigraphicBoundary boundary;boundary.name=name;boundary.order=++order;
        boundary.seismicHorizonResourceId=fixture.horizons[size_t((order-1)%2)].id;ambiguous.boundaries.push_back(boundary);
    }
    auto collision=publishScheme(fixture,ambiguous);invalid=fixture.query();invalid.target.schemeId=collision.id;invalid.target.unitName="A-B-C";
    reject(invalid,"same display label within a scheme cannot choose first interval");
    Fixture names;auto named=names.query();named.target.topHorizonId=0;named.target.bottomHorizonId=0;named.target.unitName="TOP-BASE";
    check(names.read(named).validTraceCount==4,"unique name-only query remains supported without a scheme");names.duplicateTop();
    SeismicHorizonWindowResult result;check(!names.service.readSeismicHorizonWindow(named,result,names.error)&&result.traces.empty()&&
        names.error.find("歧义")!=std::string::npos,"same-name horizons cannot select minimum ID or largest source");
    check(names.read(names.query()).validTraceCount==4,"explicit horizon resource IDs remain valid after duplicate name import");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {selectedRelationsAndCoverage();unknownDeclarationsDoNotBecomeDepths();explicitUnitAndSignConversions();depthBudgetAndMetadataCancellation();invalidSpansStillObeyFixedTensorBudget();fixedSchemeAndStableTargets();
        std::cout<<"horizon window semantic selection contract passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
