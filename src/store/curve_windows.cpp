#include "curve_windows.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <QFileInfo>

namespace paleo::data::detail {
namespace {
bool stopped(const std::function<bool()>& cancel,std::string& error) {
    if(cancel&&cancel()){error="CurveWindowCancelled: 已取消曲线读取";return true;}return false;
}
bool charge(long long bytes,const CurveReadOptions& options,CurveReadStats& stats,std::string& error) {
    if(bytes<0||bytes>options.maxReadBytes-stats.payloadBytesRead){error="CurveWindowBudgetExceeded: 曲线读取超出字节预算";return false;}
    return true;
}
QByteArray part(const std::vector<double>& values,size_t first,size_t count) {
    return pack(std::vector<double>(values.begin()+first,values.begin()+first+count));
}
class Snapshot {
    ProjectStore& store_;QString name_;bool active_=false;
public:
    Snapshot(ProjectStore& store,std::string& error):store_(store) {
        static std::atomic<unsigned long long> serial{0};
        name_="curve_window_read_"+QString::number(++serial);active_=store_.exec("SAVEPOINT "+name_,{},error);
    }
    ~Snapshot(){if(active_){std::string ignored;store_.exec("ROLLBACK TO "+name_,{},ignored);store_.exec("RELEASE "+name_,{},ignored);}}
    bool active()const{return active_;}
    bool finish(std::string& error){if(!store_.exec("RELEASE "+name_,{},error))return false;active_=false;return true;}
};
struct Metadata {
    long long setId=0,count=0,depthBytes=0,tvdBytes=0,ssBytes=0,rawBytes=0,processedBytes=0,qualityBytes=0;
    bool hasProcessed=false;
};
bool metadata(ProjectStore& store,long long id,Metadata& m,std::string& error) {
    QSqlQuery r;
    if(!store.exec("SELECT l.id,l.sample_count,coalesce(length(l.depths),0),coalesce(length(l.tvd),0),coalesce(length(l.tvdss),0),coalesce(length(c.raw_values),0),coalesce(length(c.processed_values),0),coalesce(length(c.quality),0),c.processed_values IS NOT NULL,l.encoding,coalesce(length(c.name),0),coalesce(length(c.unit),0),coalesce(length(l.depth_json),0) FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE c.id=?",{id},error,&r))return false;
    if(!r.next()){error="曲线不存在";return false;}
    m.setId=r.value(0).toLongLong();m.count=r.value(1).toLongLong();m.depthBytes=r.value(2).toLongLong();m.tvdBytes=r.value(3).toLongLong();m.ssBytes=r.value(4).toLongLong();m.rawBytes=r.value(5).toLongLong();m.processedBytes=r.value(6).toLongLong();m.qualityBytes=r.value(7).toLongLong();m.hasProcessed=r.value(8).toBool();
    if(r.value(9).toString()!="float64-le"){error="CurveWindowUnsupportedEncoding: 曲线编码不是 float64-le";return false;}
    if(m.count<0||m.count>std::numeric_limits<long long>::max()/8||m.depthBytes!=m.count*8||m.rawBytes!=m.count*8||m.qualityBytes!=m.count||(m.hasProcessed&&m.processedBytes!=m.count*8)||(m.tvdBytes&&m.tvdBytes!=m.count*8)||(m.ssBytes&&m.ssBytes!=m.count*8)){
        error="曲线数组长度损坏";return false;
    }
    for(int i=10;i<=12;++i)if(r.value(i).toLongLong()>65536){error="CurveWindowBudgetExceeded: 曲线元数据超出预算";return false;}
    return true;
}
bool hasChunks(ProjectStore& store,long long id,const Metadata& m,bool& available,std::string& error) {
    available=false;QSqlQuery r;
    if(!store.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='curve_window_curves_v1'",{},error,&r))return false;
    if(!r.next())return true;r.finish();
    if(!store.exec("SELECT c.set_id,c.sample_count,c.processed_present,a.sample_count,a.chunk_samples FROM curve_window_curves_v1 c JOIN curve_window_sets_v1 a ON a.set_id=c.set_id WHERE c.curve_id=?",{id},error,&r))return false;
    if(!r.next())return true;
    if(r.value(0).toLongLong()!=m.setId||r.value(1).toLongLong()!=m.count||r.value(2).toBool()!=m.hasProcessed||r.value(3).toLongLong()!=m.count||r.value(4).toLongLong()!=CurveChunkSamples){error="曲线分块元数据损坏";return false;}
    available=true;return true;
}
bool selected(double depth,double top,double bottom) {
    return (!std::isfinite(top)||(std::isfinite(depth)&&depth>=top))&&(!std::isfinite(bottom)||(std::isfinite(depth)&&depth<=bottom));
}
bool append(CurveSlice& out,std::vector<double>& tvd,std::vector<double>& ss,const std::vector<double>& depths,
            const std::vector<double>& raw,const std::vector<double>& used,const QByteArray& quality,
            const std::vector<double>& allTvd,const std::vector<double>& allSs,long long first,long long start,long long end,
            double top,double bottom,const CurveReadOptions& options,std::string& error) {
    for(size_t i=0;i<depths.size();++i){
        if(i%1024==0&&stopped(options.cancelled,error))return false;
        const auto index=first+static_cast<long long>(i);
        if(index<start||index>=end||!selected(depths[i],top,bottom))continue;
        if(out.depths.size()>=static_cast<size_t>(options.maxSamples)){error="CurveWindowBudgetExceeded: 曲线样点超出预算";return false;}
        out.depths.push_back(depths[i]);out.rawValues.push_back(raw[i]);out.values.push_back(used[i]);out.quality.push_back(static_cast<unsigned char>(quality[qsizetype(i)]));
        if(!allTvd.empty())tvd.push_back(allTvd[i]);if(!allSs.empty())ss.push_back(allSs[i]);
    }return true;
}
}

bool ensureCurveWindowSchema(ProjectStore& store,std::string& error) {
    if(store.readOnly){error="只读工区不能升级曲线存储";return false;}
    const QStringList sql{
        "CREATE TABLE IF NOT EXISTS curve_window_sets_v1(set_id INTEGER PRIMARY KEY REFERENCES log_sets(id) ON DELETE CASCADE,sample_count INTEGER NOT NULL,chunk_samples INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS curve_window_axis_v1(set_id INTEGER NOT NULL,first_sample INTEGER NOT NULL,sample_count INTEGER NOT NULL,min_depth REAL,max_depth REAL,depths BLOB NOT NULL,tvd BLOB,tvdss BLOB,PRIMARY KEY(set_id,first_sample)) WITHOUT ROWID",
        "CREATE INDEX IF NOT EXISTS curve_window_axis_range_v1 ON curve_window_axis_v1(set_id,max_depth,min_depth)",
        "CREATE TABLE IF NOT EXISTS curve_window_curves_v1(curve_id INTEGER PRIMARY KEY REFERENCES curves(id) ON DELETE CASCADE,set_id INTEGER NOT NULL,sample_count INTEGER NOT NULL,processed_present INTEGER NOT NULL)",
        "CREATE INDEX IF NOT EXISTS curve_window_curves_set_v1 ON curve_window_curves_v1(set_id)",
        "CREATE TABLE IF NOT EXISTS curve_window_values_v1(curve_id INTEGER NOT NULL,first_sample INTEGER NOT NULL,sample_count INTEGER NOT NULL,raw_values BLOB NOT NULL,processed_values BLOB,quality BLOB NOT NULL,PRIMARY KEY(curve_id,first_sample)) WITHOUT ROWID",
        "CREATE TRIGGER IF NOT EXISTS curve_window_curve_invalidate_v1 AFTER UPDATE OF set_id,raw_values,processed_values,quality ON curves BEGIN DELETE FROM curve_window_curves_v1 WHERE curve_id=OLD.id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_curve_delete_v1 AFTER DELETE ON curves BEGIN DELETE FROM curve_window_curves_v1 WHERE curve_id=OLD.id; DELETE FROM curve_window_values_v1 WHERE curve_id=OLD.id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_curve_meta_delete_v1 AFTER DELETE ON curve_window_curves_v1 BEGIN DELETE FROM curve_window_values_v1 WHERE curve_id=OLD.curve_id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_axis_invalidate_v1 AFTER UPDATE OF depths,tvd,tvdss,sample_count,encoding ON log_sets BEGIN DELETE FROM curve_window_sets_v1 WHERE set_id=OLD.id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_set_delete_v1 AFTER DELETE ON log_sets BEGIN DELETE FROM curve_window_sets_v1 WHERE set_id=OLD.id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_set_meta_delete_v1 AFTER DELETE ON curve_window_sets_v1 BEGIN DELETE FROM curve_window_axis_v1 WHERE set_id=OLD.set_id; DELETE FROM curve_window_curves_v1 WHERE set_id=OLD.set_id; END",
        // Writers create blocks first and publish ready metadata last. Any change to
        // blocks of an already-ready representation revokes readiness in the same
        // transaction, including lost blocks that a min/max query would never see.
        "CREATE TRIGGER IF NOT EXISTS curve_window_axis_block_delete_v1 AFTER DELETE ON curve_window_axis_v1 BEGIN DELETE FROM curve_window_sets_v1 WHERE set_id=OLD.set_id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_axis_block_update_v1 AFTER UPDATE ON curve_window_axis_v1 BEGIN DELETE FROM curve_window_sets_v1 WHERE set_id=OLD.set_id OR set_id=NEW.set_id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_axis_block_insert_v1 AFTER INSERT ON curve_window_axis_v1 WHEN EXISTS(SELECT 1 FROM curve_window_sets_v1 WHERE set_id=NEW.set_id) BEGIN DELETE FROM curve_window_sets_v1 WHERE set_id=NEW.set_id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_value_block_delete_v1 AFTER DELETE ON curve_window_values_v1 BEGIN DELETE FROM curve_window_curves_v1 WHERE curve_id=OLD.curve_id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_value_block_update_v1 AFTER UPDATE ON curve_window_values_v1 BEGIN DELETE FROM curve_window_curves_v1 WHERE curve_id=OLD.curve_id OR curve_id=NEW.curve_id; END",
        "CREATE TRIGGER IF NOT EXISTS curve_window_value_block_insert_v1 AFTER INSERT ON curve_window_values_v1 WHEN EXISTS(SELECT 1 FROM curve_window_curves_v1 WHERE curve_id=NEW.curve_id) BEGIN DELETE FROM curve_window_curves_v1 WHERE curve_id=NEW.curve_id; END"
    };
    for(const auto& statement:sql)if(!store.exec(statement,{},error))return false;return true;
}
bool writeCurveAxisWindows(ProjectStore& store,long long setId,const SamplingAxis& axis,std::string& error,const CancelCheck& cancelled) {
    if((!axis.tvdValues.empty()&&axis.tvdValues.size()!=axis.depthValues.size())||(!axis.tvdssValues.empty()&&axis.tvdssValues.size()!=axis.depthValues.size())){error="附加深度轴长度损坏";return false;}
    if(!store.exec("DELETE FROM curve_window_sets_v1 WHERE set_id=?",{setId},error))return false;
    for(size_t first=0;first<axis.depthValues.size();first+=CurveChunkSamples){
        if(stopped(cancelled,error))return false;const auto count=std::min<size_t>(CurveChunkSamples,axis.depthValues.size()-first);
        double low=Missing,high=Missing;for(size_t i=first;i<first+count;++i)if(std::isfinite(axis.depthValues[i])){low=std::isfinite(low)?std::min(low,axis.depthValues[i]):axis.depthValues[i];high=std::isfinite(high)?std::max(high,axis.depthValues[i]):axis.depthValues[i];}
        if(!store.exec("INSERT INTO curve_window_axis_v1 VALUES(?,?,?,?,?,?,?,?)",{setId,qlonglong(first),qlonglong(count),number(low),number(high),part(axis.depthValues,first,count),axis.tvdValues.empty()?QVariant():QVariant(part(axis.tvdValues,first,count)),axis.tvdssValues.empty()?QVariant():QVariant(part(axis.tvdssValues,first,count))},error))return false;
    }
    return store.exec("INSERT INTO curve_window_sets_v1 VALUES(?,?,?)",{setId,qlonglong(axis.depthValues.size()),CurveChunkSamples},error);
}
bool writeCurveValueWindows(ProjectStore& store,long long curveId,long long setId,const std::vector<double>& raw,
                           const std::vector<double>* processed,const QByteArray& quality,std::string& error,const CancelCheck& cancelled) {
    if(quality.size()!=qsizetype(raw.size())||(processed&&processed->size()!=raw.size())){error="曲线数组长度损坏";return false;}
    if(!store.exec("DELETE FROM curve_window_curves_v1 WHERE curve_id=?",{curveId},error))return false;
    for(size_t first=0;first<raw.size();first+=CurveChunkSamples){
        if(stopped(cancelled,error))return false;const auto count=std::min<size_t>(CurveChunkSamples,raw.size()-first);
        if(!store.exec("INSERT INTO curve_window_values_v1 VALUES(?,?,?,?,?,?)",{curveId,qlonglong(first),qlonglong(count),part(raw,first,count),processed?QVariant(part(*processed,first,count)):QVariant(),quality.mid(qsizetype(first),qsizetype(count))},error))return false;
    }
    return store.exec("INSERT INTO curve_window_curves_v1 VALUES(?,?,?,?)",{curveId,setId,qlonglong(raw.size()),processed!=nullptr},error);
}
bool refreshCurveValueWindows(ProjectStore& store,long long id,std::string& error) {
    QSqlQuery exists;if(!store.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='curve_window_sets_v1'",{},error,&exists))return false;if(!exists.next())return true;exists.finish();
    QSqlQuery r;if(!store.exec("SELECT c.set_id,c.raw_values,c.processed_values,c.quality FROM curves c JOIN curve_window_sets_v1 a ON a.set_id=c.set_id WHERE c.id=?",{id},error,&r))return false;
    if(!r.next())return true;
    const auto setId=r.value(0).toLongLong();std::vector<double> raw,processed;
    if(!unpack(r.value(1).toByteArray(),raw,error)||(!r.value(2).isNull()&&!unpack(r.value(2).toByteArray(),processed,error)))return false;
    const bool hasProcessed=!r.value(2).isNull();const auto quality=r.value(3).toByteArray();r.finish();
    return writeCurveValueWindows(store,id,setId,raw,hasProcessed?&processed:nullptr,quality,error);
}
bool upgradeCurveWindows(ProjectStore& store,std::string& error,const CancelCheck& cancelled,long long maxBytes) {
    error.clear();if(maxBytes<0){error="曲线升级预算无效";return false;}
    if(store.readOnly){error="只读工区不能升级曲线存储";return false;}
    if(stopped(cancelled,error))return false;
    Transaction tx(store,error);if(!tx.active||!ensureCurveWindowSchema(store,error))return false;
    long long charged=0;QSqlQuery ids;if(!store.exec("SELECT id FROM curves ORDER BY set_id,id",{},error,&ids))return false;
    while(ids.next()){
        if(stopped(cancelled,error))return false;const auto id=ids.value(0).toLongLong();Metadata m;
        if(!metadata(store,id,m,error))return false;bool complete=false;if(!hasChunks(store,id,m,complete,error))return false;if(complete)continue;
        QSqlQuery existing;if(!store.exec("SELECT sample_count FROM curve_window_sets_v1 WHERE set_id=?",{m.setId},error,&existing))return false;
        const bool needAxis=!existing.next();existing.finish();
        const auto bytes=m.rawBytes+m.processedBytes+m.qualityBytes+(needAxis?m.depthBytes+m.tvdBytes+m.ssBytes:0);
        if(bytes>maxBytes-charged){error="CurveWindowMigrationBudgetExceeded: 显式曲线升级超过旧载荷预算";return false;}charged+=bytes;
        if(needAxis){
            QSqlQuery axis;if(!store.exec("SELECT depths,tvd,tvdss FROM log_sets WHERE id=?",{m.setId},error,&axis)||!axis.next())return false;
            SamplingAxis values;if(!unpack(axis.value(0).toByteArray(),values.depthValues,error)||!unpack(axis.value(1).toByteArray(),values.tvdValues,error)||!unpack(axis.value(2).toByteArray(),values.tvdssValues,error))return false;axis.finish();
            if(!writeCurveAxisWindows(store,m.setId,values,error,cancelled))return false;
        }
        QSqlQuery curve;if(!store.exec("SELECT raw_values,processed_values,quality FROM curves WHERE id=?",{id},error,&curve)||!curve.next())return false;
        std::vector<double> raw,processed;if(!unpack(curve.value(0).toByteArray(),raw,error)||(m.hasProcessed&&!unpack(curve.value(1).toByteArray(),processed,error)))return false;
        const auto quality=curve.value(2).toByteArray();curve.finish();
        if(!writeCurveValueWindows(store,id,m.setId,raw,m.hasProcessed?&processed:nullptr,quality,error,cancelled))return false;
    }
    ids.finish();return !stopped(cancelled,error)&&tx.commit(error);
}

bool readCurveWindows(ProjectStore& store,long long id,long long start,long long count,double top,double bottom,bool processed,bool auxiliary,
                      CurveSlice& result,DepthInfo& depthInfo,std::vector<double>& resultTvd,std::vector<double>& resultSs,
                      long long& total,const CurveReadOptions& options,CurveReadStats& stats,std::string& error,CurveDepthOrigins* origins) {
    result={};depthInfo={};resultTvd.clear();resultSs.clear();total=0;stats={};error.clear();
    if(origins)*origins={};CurveDepthOrigins observed;
    if(id<=0||start<0||count<0||options.maxSamples<0||options.maxReadBytes<0||options.maxLegacyBytes<0||std::isinf(top)||std::isinf(bottom)||(std::isfinite(top)&&std::isfinite(bottom)&&top>bottom)){error="曲线窗口或预算无效";return false;}
    if(stopped(options.cancelled,error))return false;
    Snapshot snapshot(store,error);if(!snapshot.active())return false;Metadata m;if(!metadata(store,id,m,error))return false;
    const auto end=start>=m.count?m.count:start+std::min(count,m.count-start);
    bool chunked=false;if(!hasChunks(store,id,m,chunked,error))return false;stats.chunked=chunked;
    CurveSlice out;DepthInfo info;std::vector<double> tvd,ss;QSqlQuery meta;
    if(!store.exec("SELECT CASE WHEN length(CAST(c.name AS BLOB))<=65536 THEN c.name END,"
        "CASE WHEN length(CAST(c.unit AS BLOB))<=65536 THEN c.unit END,"
        "CASE WHEN length(CAST(l.depth_json AS BLOB))<=65536 THEN l.depth_json END,"
        "CASE WHEN length(CAST(s.kind AS BLOB))<=65536 THEN s.kind END,CASE WHEN length(CAST(s.options AS BLOB))<=65536 THEN s.options END,"
        "CASE WHEN length(CAST(s.original_path AS BLOB))<=65536 THEN s.original_path END,"
        "coalesce(length(CAST(s.options AS BLOB)),0),coalesce(length(CAST(s.original_path AS BLOB)),0),"
        "coalesce(length(CAST(c.name AS BLOB)),0),coalesce(length(CAST(c.unit AS BLOB)),0),"
        "coalesce(length(CAST(l.depth_json AS BLOB)),0),coalesce(length(CAST(s.kind AS BLOB)),0) "
        "FROM curves c JOIN log_sets l ON l.id=c.set_id LEFT JOIN sources s ON s.id=l.source_id WHERE c.id=?",{id},error,&meta)||!meta.next())return false;
    for(int column=6;column<12;++column)if(meta.value(column).toLongLong()>65536){error="CurveWindowBudgetExceeded: 曲线元数据声明超过预算";return false;}
    out.curveId=id;out.name=s(meta.value(0).toString());out.unit=s(meta.value(1).toString());info=readDepth(object(meta.value(2).toString()));
    if(!info.depthType.empty())observed.depthType=CurveDepthOrigin::Declared;
    if(!info.depthUnit.empty())observed.depthUnit=CurveDepthOrigin::Declared;
    if(!info.mdReference.empty())observed.mdReference=CurveDepthOrigin::Declared;
    if(meta.value(3).toString()=="workbook" && object(meta.value(4).toString())["comprehensiveWellMetreDepth"].toBool()) {
        if(info.depthType.empty()){info.depthType="MD";observed.depthType=CurveDepthOrigin::Inferred;}
        if(info.depthUnit.empty()){info.depthUnit="m";observed.depthUnit=CurveDepthOrigin::Inferred;}
    } else if(info.depthType.empty() && meta.value(3).toString()=="logs" &&
        QFileInfo(meta.value(5).toString()).suffix().compare("las",Qt::CaseInsensitive)==0) {info.depthType="MD";observed.depthType=CurveDepthOrigin::Inferred;}
    out.depthUnit=info.depthUnit;meta.finish();
    if(start<end){
        if(!chunked){
            const long long bytes=m.depthBytes+m.rawBytes+m.processedBytes+m.qualityBytes+(auxiliary?m.tvdBytes+m.ssBytes:0);
            if(bytes>options.maxLegacyBytes){error="CurveWindowMigrationRequired: 旧曲线 BLOB 超出有限兼容预算，请显式升级分块存储";return false;}
            if(!charge(bytes,options,stats,error)||stopped(options.cancelled,error))return false;
            QSqlQuery r;if(!store.exec(auxiliary?"SELECT l.depths,c.raw_values,c.processed_values,c.quality,l.tvd,l.tvdss FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE c.id=?":"SELECT l.depths,c.raw_values,c.processed_values,c.quality FROM curves c JOIN log_sets l ON l.id=c.set_id WHERE c.id=?",{id},error,&r)||!r.next())return false;
            stats.payloadBytesRead+=bytes;stats.legacyBytesMaterialized=bytes;
            std::vector<double> depths,raw,used,atvd,ass;
            if(!unpack(r.value(0).toByteArray(),depths,error)||!unpack(r.value(1).toByteArray(),raw,error))return false;
            if(processed&&m.hasProcessed){if(!unpack(r.value(2).toByteArray(),used,error))return false;}else used=raw;
            if(auxiliary&&(!unpack(r.value(4).toByteArray(),atvd,error)||!unpack(r.value(5).toByteArray(),ass,error)))return false;
            if(!append(out,tvd,ss,depths,raw,used,r.value(3).toByteArray(),atvd,ass,0,start,end,top,bottom,options,error))return false;
        }else{
            // Bounds are per chunk, so unsorted/duplicate/missing axes retain source order.
            QSqlQuery blocks;
            if(!store.exec("SELECT first_sample,sample_count,coalesce(length(depths),0),coalesce(length(tvd),0),coalesce(length(tvdss),0) FROM curve_window_axis_v1 WHERE set_id=? AND first_sample>=? AND first_sample<? AND (? IS NULL OR max_depth>=?) AND (? IS NULL OR min_depth<=?) ORDER BY first_sample",{m.setId,(start/CurveChunkSamples)*CurveChunkSamples,end,number(top),number(top),number(bottom),number(bottom)},error,&blocks))return false;
            long long visited=0;const bool bounded=std::isfinite(top)||std::isfinite(bottom);
            while(blocks.next()){
                if(stopped(options.cancelled,error))return false;
                const auto first=blocks.value(0).toLongLong(),n=blocks.value(1).toLongLong();
                const auto expected=std::min(CurveChunkSamples,m.count-first);
                if(first<0||first%CurveChunkSamples||n<=0||n!=expected||blocks.value(2).toLongLong()!=n*8||blocks.value(3).toLongLong()!=(m.tvdBytes?n*8:0)||blocks.value(4).toLongLong()!=(m.ssBytes?n*8:0)){error="曲线深度分块长度损坏";return false;}
                ++visited;const auto axisBytes=n*8+(auxiliary?(m.tvdBytes?n*8:0)+(m.ssBytes?n*8:0):0);
                if(!charge(axisBytes,options,stats,error))return false;
                QSqlQuery axis;if(!store.exec(auxiliary?"SELECT depths,tvd,tvdss FROM curve_window_axis_v1 WHERE set_id=? AND first_sample=?":"SELECT depths FROM curve_window_axis_v1 WHERE set_id=? AND first_sample=?",{m.setId,first},error,&axis)||!axis.next())return false;
                stats.payloadBytesRead+=axisBytes;++stats.axisBlocksRead;
                std::vector<double> depths,atvd,ass;if(!unpack(axis.value(0).toByteArray(),depths,error)||(auxiliary&&(!unpack(axis.value(1).toByteArray(),atvd,error)||!unpack(axis.value(2).toByteArray(),ass,error))))return false;axis.finish();
                bool hit=false;for(size_t i=0;i<depths.size();++i)if(first+static_cast<long long>(i)>=start&&first+static_cast<long long>(i)<end&&selected(depths[i],top,bottom)){hit=true;break;}
                if(!hit)continue;
                QSqlQuery lengths;if(!store.exec("SELECT sample_count,coalesce(length(raw_values),0),coalesce(length(processed_values),0),coalesce(length(quality),0),processed_values IS NOT NULL FROM curve_window_values_v1 WHERE curve_id=? AND first_sample=?",{id,first},error,&lengths))return false;
                if(!lengths.next()||lengths.value(0).toLongLong()!=n||lengths.value(1).toLongLong()!=n*8||lengths.value(2).toLongLong()!=(m.hasProcessed?n*8:0)||lengths.value(3).toLongLong()!=n||lengths.value(4).toBool()!=m.hasProcessed){error="曲线数值分块长度损坏";return false;}lengths.finish();
                // Read only selected value channels; raw/quality are always part of CurveSlice.
                if(!charge(n*9+(processed&&m.hasProcessed?n*8:0),options,stats,error)||stopped(options.cancelled,error))return false;
                QSqlQuery values;if(!store.exec(processed&&m.hasProcessed?"SELECT raw_values,quality,processed_values FROM curve_window_values_v1 WHERE curve_id=? AND first_sample=?":"SELECT raw_values,quality FROM curve_window_values_v1 WHERE curve_id=? AND first_sample=?",{id,first},error,&values)||!values.next())return false;
                stats.payloadBytesRead+=n*9+(processed&&m.hasProcessed?n*8:0);++stats.valueBlocksRead;
                std::vector<double> raw,used;if(!unpack(values.value(0).toByteArray(),raw,error))return false;
                if(processed&&m.hasProcessed){if(!unpack(values.value(2).toByteArray(),used,error))return false;}else used=raw;
                if(!append(out,tvd,ss,depths,raw,used,values.value(1).toByteArray(),atvd,ass,first,start,end,top,bottom,options,error))return false;
            }
            if(!bounded&&visited!=(end-1)/CurveChunkSamples-start/CurveChunkSamples+1){error="曲线深度分块缺失";return false;}
        }
    }
    if(stopped(options.cancelled,error)||!snapshot.finish(error))return false;
    if(origins)*origins=observed;
    total=m.count;result=std::move(out);depthInfo=std::move(info);resultTvd=std::move(tvd);resultSs=std::move(ss);return true;
}
}
