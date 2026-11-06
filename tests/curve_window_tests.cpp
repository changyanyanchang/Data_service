#include "paleo_data/service.h"
#include "store/project_store.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
std::string u(const QString& text){return text.toUtf8().toStdString();}
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
bool bits(double a,double b){return std::memcmp(&a,&b,sizeof(double))==0;}
QVariant sql(const QString& project,const QString& command,const QVariantList& values={}) {
    const auto name="curve-window-test-"+QUuid::createUuid().toString();QVariant result;std::string error;
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(project+"/data.sqlite");
        if(!db.open())error=u(db.lastError().text());else{
            QSqlQuery query(db);if(!query.prepare(command))error=u(query.lastError().text());else{
                for(const auto& value:values)query.addBindValue(value);
                if(!query.exec())error=u(query.lastError().text());else if(query.next())result=query.value(0);
            }
        }db.close();
    }QSqlDatabase::removeDatabase(name);require(error.empty(),error);return result;
}
void legacy(const QString& project) {
    for(const auto& suffix:QStringList{"curve_invalidate","curve_delete","curve_meta_delete","axis_invalidate","set_delete","set_meta_delete","axis_block_delete","axis_block_update","axis_block_insert","value_block_delete","value_block_update","value_block_insert"})sql(project,"DROP TRIGGER curve_window_"+suffix+"_v1");
    for(const auto& table:QStringList{"values","curves","axis","sets"})sql(project,"DROP TABLE curve_window_"+table+"_v1");
}
struct Fixture {
    ParsedFile model;SamplingAxis axis;std::vector<double> values;
    explicit Fixture(size_t count) {
        model.kind="logs";model.status="parsed";WellDataset well;well.wellInfo.wellName="SYNTH-CURVE";
        LogSet log;log.logSetName="explicit axes";log.depthInfo={"MD","ft","fixture KB","fixture TVD","fixture MSL","up"};
        LogCurve curve;curve.curveName="SYNTH-RAW";curve.curveUnit="test-unit";
        for(size_t i=0;i<count;++i){axis.depthValues.push_back(100+double(i)*0.125);axis.tvdValues.push_back(80+double(i)*0.123456789);axis.tvdssValues.push_back(-800+double(i)*0.123456789);values.push_back(100000000000.00002+double(i)*0.0625);}
        if(count>53003){axis.depthValues[5]=axis.depthValues[53001];axis.depthValues[1]=-7.5;axis.depthValues[512]=Missing;axis.tvdValues[53000]=Missing;values[53002]=Missing;}
        log.samplingAxis=axis;curve.sampleValues=values;log.logCurves.push_back(curve);well.wellbore.logSets.push_back(log);model.wells.push_back(well);
    }
};
long long publish(DataService& service,const QString& path,const Fixture& fixture) {
    const QByteArray content="synthetic prepared curve\n";QFile file(path);require(file.open(QIODevice::WriteOnly),"Open synthetic source");require(file.write(content)==content.size(),"Write synthetic source");file.close();
    const auto hash=QCryptographicHash::hash(content,QCryptographicHash::Sha256).toHex();long long source=0;std::string error;
    require(service.importPreparedFile(u(path),u(path),u(path),hash.toStdString(),fixture.model,source,error),error);
    std::vector<DataEntry> entries;require(service.listData("curves",0,entries,error)&&entries.size()==1,error);return entries.front().id;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temporary;require(temporary.isValid(),"Temporary directory");const auto project=temporary.path()+"/project";
        DataService service;std::string error;require(service.createProject(u(project),"Synthetic windows",error),error);
        Fixture fixture(65536);const auto id=publish(service,temporary.path()+"/curve.data",fixture);
        CurveReadOptions options;options.maxLegacyBytes=0;CurveReadStats stats;CurveSlice slice;
        const double depth=fixture.axis.depthValues[53001];
        require(service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats),error);
        require(stats.chunked&&stats.legacyBytesMaterialized==0&&stats.axisBlocksRead==2&&stats.valueBlocksRead==2,"Read only two matching chunks for duplicate, unsorted depth");
        require(stats.payloadBytesRead==2*4096*17&&stats.payloadBytesRead<65536*8,"Narrow read bytes exclude the complete legacy axis/raw blobs");
        require(slice.values.size()==2&&bits(slice.values[0],fixture.values[5])&&bits(slice.values[1],fixture.values[53001]),"Preserve source order and float64 bits for duplicate depths");
        require(slice.depthUnit=="ft"&&slice.unit=="test-unit","No unit guessing/conversion");
        CurveWindowResult depthWindow;
        require(service.readCurveDepthWindow(id,depth,depth,true,depthWindow,error,options,&stats),error);
        require(depthWindow.firstSample==-1&&depthWindow.curve.values.size()==2&&stats.axisBlocksRead==2&&
                bits(depthWindow.tvdssValues[1],fixture.axis.tvdssValues[53001]),"Depth windows carry only aligned auxiliary axes");
        const auto setId=sql(project,"SELECT set_id FROM curves WHERE id=?",{id}).toLongLong();
        sql(project,"INSERT INTO curves(set_id,name,unit,raw_values,quality) VALUES(?,'UNUSED-BROKEN','?',X'00',X'00')",{setId});
        const auto unusedId=sql(project,"SELECT id FROM curves WHERE name='UNUSED-BROKEN'").toLongLong();
        std::vector<DataEntry> selected;require(service.listData("curves",0,selected,error),error);
        CorrelationInputRequest capture;CorrelationWellSelection selection;selection.curveId=id;
        for(const auto& entry:selected)if(entry.id==id)selection.wellId=entry.wellId;
        capture.wells={selection};capture.firstDepth=depth;capture.lastDepth=depth;CorrelationInputSnapshot captured;
        require(service.captureCorrelationInputs(capture,captured,error),error);
        require(captured.wells.size()==1&&captured.wells.front().log.values.size()==2&&
                bits(captured.wells.front().logTvdssValues[1],fixture.axis.tvdssValues[53001]),
                "Correlation captures the selected bounded curve, without reading an unrelated broken curve");
        require(service.deleteData("curves",unusedId,error),error);
        CurveWindowQuery query;query.curveId=id;query.firstSample=53000;query.sampleCount=3;CurveWindowResult window;
        require(service.readCurveWindow(query,window,error,options,&stats),error);
        require(window.totalSamples==65536&&window.firstSample==53000&&window.curve.depths.size()==3,"Index window metadata");
        require(window.depthInfo.mdReference=="fixture KB"&&window.depthInfo.tvdssDirection=="up","Preserve complete axis declarations");
        require(std::isnan(window.tvdValues[0])&&bits(window.tvdssValues[1],fixture.axis.tvdssValues[53001]),"Auxiliary axes preserve missing values and precision");
        require(std::isnan(window.curve.rawValues[2])&&std::isnan(window.curve.values[2])&&window.curve.quality[2]==1,"NULL processed column falls back to raw, missing quality retained");
        query.firstSample=512;query.sampleCount=1;require(service.readCurveWindow(query,window,error,options,&stats)&&std::isnan(window.curve.depths.front()),"Index paging retains missing primary depth");
        DataTable table;require(service.readTable("curves",id,53000,3,table,error),error);
        require(table.rows.size()==3&&table.rows[0][0]=="53000"&&std::stod(table.rows[1][3])==fixture.values[53001],"Legacy table API uses true paging and double precision text");
        require(service.readCurveTableWindow(id,53000,3,table,error,options,&stats)&&stats.axisBlocksRead==1&&stats.payloadBytesRead==4096*17,"Table paging fetches one bounded axis/value chunk");
        const double edited=3.1415926535897931;require(service.updateCurveValue(id,53001,edited,error),error);
        require(service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&bits(slice.values[1],edited)&&bits(slice.rawValues[1],fixture.values[53001])&&slice.quality[1]==4,"Edits atomically synchronize chunks, preserving raw and quality");
        require(service.readCurveSlice(id,depth,depth,false,slice,error,options,&stats)&&bits(slice.values[1],fixture.values[53001]),"Raw read never returns the edited processed value");
        sql(project,"CREATE TRIGGER reject_curve_window BEFORE INSERT ON curve_window_values_v1 BEGIN SELECT RAISE(ABORT,'injected chunk failure'); END");
        require(!service.updateCurveValue(id,53001,9,error),"Chunk failure rejects the entire edit");
        sql(project,"DROP TRIGGER reject_curve_window");
        require(service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&bits(slice.values[1],edited),"Chunk failure rolls back canonical and window values");
        require(service.resetCurve(id,error),error);require(service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&bits(slice.values[1],fixture.values[53001])&&slice.quality[1]==0,"Reset synchronizes processed chunks");
        CurveReadOptions tiny=options;tiny.maxReadBytes=1;
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,tiny,&stats)&&slice.values.empty()&&stats.valueBlocksRead==0,"Reject byte budget before value payload decode");
        tiny=options;tiny.maxSamples=1;
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,tiny,&stats)&&slice.values.empty(),"Sample budget rejects and clears output");
        int cancelled=0;tiny=options;tiny.cancelled=[&]{return ++cancelled==4;};
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,tiny,&stats)&&slice.values.empty(),"Mid-read cancellation clears all partial output");
        tiny=options;tiny.cancelled=[]{return true;};
        require(!service.readCurveTableWindow(id,53000,3,table,error,tiny,&stats)&&table.rows.empty(),"Table cancellation propagates");
        require(service.readCurveTableWindow(id,std::numeric_limits<long long>::max(),3,table,error,options,&stats)&&table.rows.empty()&&stats.payloadBytesRead==0,"Overflow-safe late page without numeric reads");
        sql(project,"UPDATE curve_window_values_v1 SET raw_values=X'00' WHERE curve_id=? AND first_sample=49152",{id});
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&slice.values.empty(),"Corrupt selected chunk length rejects atomically");
        require(service.upgradeCurveWindowStorage(error),error);
        // Removing the matching axis chunk used to make a depth-filter query
        // silently return only the other duplicate (or an empty successful slice).
        sql(project,"DELETE FROM curve_window_axis_v1 WHERE set_id=? AND first_sample=49152",{setId});
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&slice.values.empty()&&stats.payloadBytesRead==0&&error.find("MigrationRequired")!=std::string::npos,"Missing axis chunk revokes readiness before a bounded depth read");
        require(!service.readCurveTableWindow(id,53000,3,table,error,options,&stats)&&table.rows.empty()&&stats.payloadBytesRead==0,"Lost axis chunks also reject index paging without scanning the axis");
        require(service.upgradeCurveWindowStorage(error),error);
        require(service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&slice.values.size()==2&&stats.axisBlocksRead==2,"Explicit repair restores all occurrences and bounded reads");
        sql(project,"UPDATE curve_window_axis_v1 SET min_depth=0,max_depth=0 WHERE set_id=? AND first_sample=49152",{setId});
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&slice.values.empty()&&stats.payloadBytesRead==0,"Modified block bounds cannot hide existing samples");
        require(service.upgradeCurveWindowStorage(error),error);
        sql(project,"DELETE FROM curve_window_values_v1 WHERE curve_id=? AND first_sample=49152",{id});
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&slice.values.empty()&&stats.payloadBytesRead==0,"Missing value block revokes readiness in constant-time metadata lookup");
        require(service.upgradeCurveWindowStorage(error),error);
        service.closeProject();require(service.openProjectReadOnly(u(project),error),error);
        require(service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&stats.chunked,"Chunk windows persist and read on a read-only reopen");
        service.closeProject();legacy(project);require(service.openProjectReadOnly(u(project),error),error);
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&error.find("MigrationRequired")!=std::string::npos&&stats.legacyBytesMaterialized==0,"Old large blobs do not silently materialize under a narrow limit");
        require(!service.upgradeCurveWindowStorage(error),"Read-only sessions never migrate");
        CurveReadOptions compatible;compatible.maxLegacyBytes=8*1024*1024;
        require(service.readCurveSlice(id,depth,depth,true,slice,error,compatible,&stats)&&!stats.chunked&&stats.legacyBytesMaterialized==65536*25,"Explicit finite compatibility counts entire old payload");
        service.closeProject();require(service.openProject(u(project),error),error);
        require(!service.upgradeCurveWindowStorage(error,{},1024),"Legacy upgrade enforces aggregate payload budget");
        require(sql(project,"SELECT count(*) FROM sqlite_master WHERE name='curve_window_sets_v1'").toInt()==0,"Failed migration rolls back schema and blocks");
        cancelled=0;require(!service.upgradeCurveWindowStorage(error,[&]{return ++cancelled==8;}),"Cancel migration after schema/axis work");
        require(sql(project,"SELECT count(*) FROM sqlite_master WHERE name='curve_window_sets_v1'").toInt()==0,"Cancelled migration rolls back all new storage");
        require(service.upgradeCurveWindowStorage(error),error);require(service.upgradeCurveWindowStorage(error,{},0),"Repeated complete migration is idempotent and reads no legacy payload");
        require(service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&stats.chunked&&bits(slice.values[1],fixture.values[53001]),"Migrated old values are bit-preserved and bounded");
        sql(project,"UPDATE log_sets SET depths=X'00' WHERE id=(SELECT set_id FROM curves WHERE id=?)",{id});
        require(!service.readCurveSlice(id,depth,depth,true,slice,error,options,&stats)&&slice.values.empty()&&stats.payloadBytesRead==0,"Malformed canonical axis invalidates chunks and fails before materialization");
        // A separate small legacy database exercises NULL processed_values and no chunk tables.
        const auto small=temporary.path()+"/small";DataService smallService;require(smallService.createProject(u(small),"Small legacy",error),error);
        sql(small,"INSERT INTO log_sets(id,name,depth_json,depths,sample_count) VALUES(1,'small','{}',?,3)",{detail::pack({-1,0,1})});
        sql(small,"INSERT INTO curves(id,set_id,name,raw_values,quality) VALUES(1,1,'signed',?,?)",{detail::pack({-999.25,-30,Missing}),QByteArray::fromHex("010001")});
        require(smallService.readCurveSlice(1,Missing,Missing,true,slice,error,compatible,&stats)&&slice.rawValues[0]==-999.25&&slice.values[1]==-30&&slice.quality[0]==1&&std::isnan(slice.values[2]),"Small old storage preserves source null sentinels, valid negative values and SQL NULL fallback");
        require(smallService.upgradeCurveWindowStorage(error),error);require(smallService.readCurveSlice(1,Missing,Missing,true,slice,error,options,&stats)&&stats.chunked&&slice.rawValues[0]==-999.25&&slice.quality[0]==1,"Legacy upgrade preserves finite source sentinel and quality");
        std::cout<<"Curve window checks passed\n";return 0;
    }catch(const std::exception& exception){std::cerr<<exception.what()<<'\n';return 1;}
}
