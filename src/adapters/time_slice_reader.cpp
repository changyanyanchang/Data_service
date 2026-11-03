// Tiled time-slice coordinator. SQL/UI remain outside this layer.
// Workers own their readers; only this calling thread invokes user callbacks.
#include "time_slice_reader.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <list>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

namespace paleo::data::segy_detail {
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
bool fail(std::string& error,const std::string& message){error=message;return false;}
bool stopped(const CancelCheck& cancel,std::string& error){return cancel&&cancel()?(error="时间切片读取已取消",true):false;}

struct BandTile {
    // Time-major cache: each plane contains this tile's row-major cells.
    std::vector<float> amplitudes;
    std::vector<double> x,y;
    size_t bytes()const{return amplitudes.size()*sizeof(float)+(x.size()+y.size())*sizeof(double);}
};
class TileCache {
    struct Entry {std::shared_ptr<const BandTile> data;std::list<std::string>::iterator order;};
    std::mutex mutex_;
    std::map<std::string,Entry> items_;
    std::list<std::string> order_;
    size_t bytes_=0,budget_=512ULL*1024*1024;
    void trim(){while(bytes_>budget_&&!order_.empty()){const auto at=items_.find(order_.back());bytes_-=at->second.data->bytes();items_.erase(at);order_.pop_back();}}
public:
    void budget(size_t bytes){std::lock_guard lock(mutex_);budget_=bytes;trim();}
    std::shared_ptr<const BandTile> get(const std::string& key){std::lock_guard lock(mutex_);auto at=items_.find(key);if(at==items_.end())return {};order_.splice(order_.begin(),order_,at->second.order);return at->second.data;}
    void put(const std::string& key,std::shared_ptr<const BandTile> value){
        std::lock_guard lock(mutex_);if(value->bytes()>budget_)return;
        auto at=items_.find(key);if(at!=items_.end()){bytes_-=at->second.data->bytes();order_.erase(at->second.order);items_.erase(at);}
        const size_t added=value->bytes();
        order_.push_front(key);
        // Keep the LRU and lookup table consistent if allocating the map node fails.
        try{items_.emplace(key,Entry{std::move(value),order_.begin()});}
        catch(...){order_.pop_front();throw;}
        bytes_+=added;trim();
    }
    void clear(){std::lock_guard lock(mutex_);items_.clear();order_.clear();bytes_=0;}
};
TileCache& cache(){static TileCache value;return value;}

// Bounded identity check, not a full content hash. An in-place change preserving
// size/mtime and both sampled ends is not detectable; callers must reimport it.
bool identity(const std::string& source,std::string& key,std::string& error){
    QFileInfo before(QString::fromUtf8(source));QFile file(before.absoluteFilePath());
    if(!file.open(QIODevice::ReadOnly))return fail(error,"无法核验切片来源："+file.errorString().toStdString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 block=std::min<qint64>(65536,before.size());
    auto head=file.read(block);if(head.size()!=block)return fail(error,"读取切片来源指纹失败");hash.addData(head);
    if(before.size()>block){if(!file.seek(before.size()-block))return fail(error,"定位切片来源尾部失败");auto tail=file.read(block);if(tail.size()!=block)return fail(error,"读取切片来源尾部失败");hash.addData(tail);}
    QFileInfo after(file.fileName());
    if(before.size()!=after.size()||before.lastModified()!=after.lastModified())return fail(error,"地震来源在读取期间发生变化");
    key=(after.canonicalFilePath()+"|"+QString::number(after.size())+"|"+QString::number(after.lastModified().toMSecsSinceEpoch())+"|"+hash.result().toHex()).toUtf8().toStdString();
    return true;
}
std::vector<int> range(const std::vector<int>& axis,int minimum,int maximum){
    return {std::lower_bound(axis.begin(),axis.end(),minimum),std::upper_bound(axis.begin(),axis.end(),maximum)};
}
std::vector<int> select(const std::vector<int>& axis,int maximum){
    const int count=std::min<int>(static_cast<int>(axis.size()),maximum);std::vector<int> out;out.reserve(count);
    for(int n=0;n<count;++n)out.push_back(axis[count==1?axis.size()/2:size_t(static_cast<long long>(n)*(axis.size()-1)/(count-1))]);return out;
}
void accumulate(SeismicReadStatistics& total,const SeismicReadStatistics& part){
    total.positionMs+=part.positionMs;total.readMs+=part.readMs;total.validationMs+=part.validationMs;total.conversionMs+=part.conversionMs;
    total.readCalls+=part.readCalls;total.seekCalls+=part.seekCalls;total.bytesRead+=part.bytesRead;total.mappedBytes+=part.mappedBytes;total.tracesRead+=part.tracesRead;
    if(!part.ioMode.empty())total.ioMode=total.ioMode.empty()?part.ioMode:(total.ioMode==part.ioMode?total.ioMode:"mixed");
}
struct Job {int row=0,column=0,rows=0,columns=0;std::vector<int> il,xl;std::string key;};
struct TileLayout {
    int rows=0,columns=0;
    bool inlineFast=false,reverseInline=false,reverseCrossline=false;
};
TileLayout tileLayout(const Reader& reader,int rows,int columns,int tileSize){
    TileLayout layout{tileSize,tileSize};
    const auto& il=reader.inlines();const auto& xl=reader.crosslines();const auto& facts=reader.info();
    // Only a complete candidate rule grid can supply a physical scan direction.
    // Missing/irregular grids and one-axis selections keep ordinary square tiles.
    if(facts.geometryStatus!="sampled-regular"||rows<2||columns<2||il.size()<2||xl.size()<2||
       static_cast<long long>(il.size())*static_cast<long long>(xl.size())!=facts.traceCount)return layout;
    const auto first=reader.traceIndex(il.front(),xl.front());
    const auto nextIl=reader.traceIndex(il[1],xl.front()),nextXl=reader.traceIndex(il.front(),xl[1]);
    if(first<0||nextIl<0||nextXl<0)return layout;
    // Use neighbours in the FULL axes: preview-selected neighbours may skip traces.
    const auto inlineStride=nextIl-first,crosslineStride=nextXl-first;
    const auto magnitude=[](long long value){return value<0?-value:value;};
    const bool inlineFast=magnitude(inlineStride)==1&&magnitude(crosslineStride)==static_cast<long long>(il.size());
    const bool crosslineFast=magnitude(crosslineStride)==1&&magnitude(inlineStride)==static_cast<long long>(xl.size());
    if(inlineFast==crosslineFast)return layout;
    // Check the inferred index arithmetic at corners, neighbouring edges and the
    // middle. This verifies the layout API, not unvisited source headers: every
    // actual trace is still checked by readTraceBatch before publishing a tile.
    for(size_t i:{size_t(0),size_t(1),il.size()/2,il.size()-2,il.size()-1})
        for(size_t j:{size_t(0),size_t(1),xl.size()/2,xl.size()-2,xl.size()-1}){
            const auto expected=first+static_cast<long long>(i)*inlineStride+static_cast<long long>(j)*crosslineStride;
            if(expected<0||expected>=facts.traceCount||reader.traceIndex(il[i],xl[j])!=expected)return layout;
        }
    const auto lastIl=first+static_cast<long long>(il.size()-1)*inlineStride;
    const auto lastXl=first+static_cast<long long>(xl.size()-1)*crosslineStride;
    const auto lastBoth=lastIl+static_cast<long long>(xl.size()-1)*crosslineStride;
    if(std::min({first,lastIl,lastXl,lastBoth})!=0||std::max({first,lastIl,lastXl,lastBoth})!=facts.traceCount-1)return layout;

    // A square tile jumps between many lines, then returns to scan the next
    // crossline segment. Long strips finish the physical fast axis first, so
    // adjacent jobs read nearby traces instead of repeatedly crossing lines.
    // The shape changes; the sample count and maximum points per job do not.
    const int budget=tileSize*tileSize;
    const int fastExtent=std::min(inlineFast?rows:columns,budget);
    const int slowExtent=std::min(inlineFast?columns:rows,std::max(1,budget/fastExtent));
    layout.rows=inlineFast?fastExtent:slowExtent;layout.columns=inlineFast?slowExtent:fastExtent;
    layout.inlineFast=inlineFast;layout.reverseInline=inlineStride<0;layout.reverseCrossline=crosslineStride<0;
    return layout;
}
std::string tileKey(const std::string& source,const Job& job,int first,int count,bool strict,double residual){
    std::ostringstream out;out<<source<<"|slice-v1|"<<first<<'|'<<count<<'|'<<strict<<'|'<<residual<<"|IL:";
    for(int n:job.il)out<<n<<',';out<<"|XL:";for(int n:job.xl)out<<n<<',';return out.str();
}
class ReaderPool {
public:
    struct Slot {Reader reader;std::mutex mutex;bool opened=false;};
    explicit ReaderPool(int workers){for(int n=0;n<workers;++n)slots_.push_back(std::make_unique<Slot>());}
    Slot& next(){return *slots_[next_++%slots_.size()];}
private:
    std::vector<std::unique_ptr<Slot>> slots_;
    std::atomic_size_t next_=0;
};
struct WorkResult {bool ok=false;std::string error;std::shared_ptr<const BandTile> tile;SeismicReadStatistics stats;};
WorkResult loadTile(const std::string& path,const Job& job,int first,int count,const TimeSliceReadOptions& options,
                    bool strict,double residual,const std::shared_ptr<std::atomic_bool>& abort,
                    const std::shared_ptr<ReaderPool>& readers){
    WorkResult result;
    try{
        const Cancel cancel=[abort]{return abort->load();};
        auto& slot=readers->next();std::lock_guard lock(slot.mutex);Reader& worker=slot.reader;
        if(!slot.opened){if(!worker.open(path,result.error,cancel))return result;slot.opened=true;}
        const size_t cells=size_t(job.rows)*job.columns;
        auto tile=std::make_shared<BandTile>();tile->amplitudes.assign(cells*count,float(Missing));tile->x.resize(cells,Missing);tile->y.resize(cells,Missing);
        std::vector<long long> ordinals;std::vector<size_t> positions;ordinals.reserve(cells);positions.reserve(cells);
        const auto planning=Clock::now();
        for(int i=0;i<job.rows;++i)for(int j=0;j<job.columns;++j){
            const size_t cell=size_t(i)*job.columns+j;
            worker.mapLine(job.il[i],job.xl[j],tile->x[cell],tile->y[cell]);
            const auto ordinal=worker.traceIndex(job.il[i],job.xl[j]);if(ordinal>=0){ordinals.push_back(ordinal);positions.push_back(cell);}
        }
        if(options.collectTimings)result.stats.positionMs+=ms(planning);
        BatchReadOptions batchOptions;batchOptions.ioMode=options.ioMode;batchOptions.collectTimings=options.collectTimings;
        TraceWindowBatch batch;SeismicReadStatistics reads;
        if(!ordinals.empty()&&!worker.readTraceBatch(ordinals,first,count,batchOptions,batch,reads,result.error,cancel))return result;
        accumulate(result.stats,reads);
        const auto validation=Clock::now();
        for(size_t n=0;n<positions.size();++n){
            if((n%4096)==0&&cancel()){result.error="时间切片读取已取消";return result;}
            const size_t cell=positions[n];const int il=job.il[cell/job.columns],xl=job.xl[cell%job.columns];
            if(batch.inlineNumbers[n]!=il||batch.crosslineNumbers[n]!=xl){result.error="实际道编号与请求网格不一致，停止读取";return result;}
            if(strict&&(!std::isfinite(batch.x[n])||!std::isfinite(batch.y[n])||!std::isfinite(tile->x[cell])||!std::isfinite(tile->y[cell])||std::hypot(batch.x[n]-tile->x[cell],batch.y[n]-tile->y[cell])>residual)){
                result.error="所选道的实际XY坐标偏离抽样网格，停止读取";return result;
            }
            tile->x[cell]=batch.x[n];tile->y[cell]=batch.y[n];
        }
        if(options.collectTimings)result.stats.validationMs+=ms(validation);
        const auto conversion=Clock::now();
        for(size_t n=0;n<positions.size();++n)for(int sample=0;sample<count;++sample)
            tile->amplitudes[size_t(sample)*cells+positions[n]]=batch.amplitudes[n*count+sample];
        if(options.collectTimings)result.stats.conversionMs+=ms(conversion);
        result.tile=std::move(tile);result.ok=true;
    }catch(const std::exception& e){result.error=std::string("分块读取失败：")+e.what();}
    return result;
}
}
void clearSliceCache(){cache().clear();}

bool readSliceTiled(Reader& reader,double time,const TimeSliceReadOptions& options,SeismicTimeSliceResult& result,
                    std::string& error,const CancelCheck& cancel,const TimeSliceTileCallback& tileReady,bool strict,double residual){
    result={};error.clear();const auto started=Clock::now();
    if(stopped(cancel,error))return false;
    if(!std::isfinite(time)||options.inlineMin>options.inlineMax||options.crosslineMin>options.crosslineMax||options.maxValues<1||options.maxValues>64000000||
       (!options.fullResolution&&(options.maxInlineCount<1||options.maxInlineCount>4096||options.maxCrosslineCount<1||options.maxCrosslineCount>4096))||
       (options.workerCount!=1&&options.workerCount!=2&&options.workerCount!=4)||options.tileSize<32||options.tileSize>512||options.adjacentSamples<1||options.adjacentSamples>16||options.cacheBudgetMiB<0||options.cacheBudgetMiB>1024||
       (options.ioMode!="auto"&&options.ioMode!="buffered"&&options.ioMode!="mapped"&&options.ioMode!="legacy"))
        return fail(error,"切片参数无效：核对测线范围、读取模式、线程1/2/4、分块32–512、相邻样点1–16和内存预算");
    const auto& facts=reader.info();const double sample=(time-facts.firstTime)/facts.sampleInterval;
    if(!std::isfinite(sample)||sample<0||sample>facts.sampleCount-1)return fail(error,"请求时间超出地震体采样范围");
    const int sampleIndex=static_cast<int>(std::round(sample));
    const auto nativeIl=range(reader.inlines(),options.inlineMin,options.inlineMax),nativeXl=range(reader.crosslines(),options.crosslineMin,options.crosslineMax);
    if(nativeIl.empty()||nativeXl.empty())return fail(error,"请求范围内没有可用测线网格");
    SeismicTimeSliceResult output;output.nativeInlineCount=static_cast<int>(nativeIl.size());output.nativeCrosslineCount=static_cast<int>(nativeXl.size());
    output.inlineNumbers=options.fullResolution?nativeIl:select(nativeIl,options.maxInlineCount);output.crosslineNumbers=options.fullResolution?nativeXl:select(nativeXl,options.maxCrosslineCount);
    output.decimated=output.inlineNumbers.size()!=nativeIl.size()||output.crosslineNumbers.size()!=nativeXl.size();
    const int rows=static_cast<int>(output.inlineNumbers.size()),columns=static_cast<int>(output.crosslineNumbers.size());const long long cells=static_cast<long long>(rows)*columns;
    if(cells>options.maxValues)return fail(error,"切片需要"+std::to_string(cells)+"个振幅，超过maxValues预算；请缩小测线范围或提高预算，高清模式不会自动降低分辨率");
    const bool caching=options.useCache&&options.cacheBudgetMiB>0;
    const int bandFirst=caching?(sampleIndex/options.adjacentSamples)*options.adjacentSamples:sampleIndex;
    const int bandCount=caching?std::min(options.adjacentSamples,facts.sampleCount-bandFirst):1;
    if(caching)cache().budget(size_t(options.cacheBudgetMiB)*1024*1024);
    std::string sourceIdentity;if(!identity(facts.sourceFile,sourceIdentity,error))return false;
    output.actualTime=facts.firstTime+sampleIndex*facts.sampleInterval;output.timeUnit="ms";
    output.amplitudes.resize(size_t(cells),float(Missing));output.x.resize(size_t(cells),Missing);output.y.resize(size_t(cells),Missing);
    auto& stats=output.statistics;stats.workerCount=0;
    const auto layout=tileLayout(reader,rows,columns,options.tileSize);
    const int rowBlocks=(rows+layout.rows-1)/layout.rows,columnBlocks=(columns+layout.columns-1)/layout.columns;
    stats.totalTiles=rowBlocks*columnBlocks;
    auto abort=std::make_shared<std::atomic_bool>(false);
    auto readers=std::make_shared<ReaderPool>(options.workerCount);
    struct Pending {Job job;std::future<WorkResult> future;};
    std::vector<Pending> pending;pending.reserve(options.workerCount);
    // Declared after futures: cancels workers before future destructors join on any exit.
    struct AbortGuard {std::shared_ptr<std::atomic_bool> flag;~AbortGuard(){flag->store(true);}} guard{abort};
    bool any=false;int next=0;
    auto publish=[&](const Job& job,const BandTile& tile,bool hit){
        const size_t tileCells=size_t(job.rows)*job.columns,base=size_t(sampleIndex-bandFirst)*tileCells;
        for(int i=0;i<job.rows;++i)for(int j=0;j<job.columns;++j){const size_t local=size_t(i)*job.columns+j,global=size_t(job.row+i)*columns+job.column+j;
            output.x[global]=tile.x[local];output.y[global]=tile.y[local];output.amplitudes[global]=tile.amplitudes[base+local];any|=std::isfinite(output.amplitudes[global]);}
        ++stats.completedTiles;
        if(tileReady){SeismicTimeSliceTile tileOutput;tileOutput.inlineOffset=job.row;tileOutput.crosslineOffset=job.column;tileOutput.totalInlineCount=rows;tileOutput.totalCrosslineCount=columns;
            tileOutput.inlineCount=job.rows;tileOutput.crosslineCount=job.columns;tileOutput.completedTiles=stats.completedTiles;tileOutput.totalTiles=stats.totalTiles;
            tileOutput.actualTime=output.actualTime;tileOutput.timeUnit="ms";tileOutput.fromCache=hit;tileOutput.amplitudes.assign(tile.amplitudes.begin()+base,tile.amplitudes.begin()+base+tileCells);tileReady(tileOutput);}
    };
    while(next<stats.totalTiles||!pending.empty()){
        if(stopped(cancel,error))return false;
        while(next<stats.totalTiles&&pending.size()<size_t(options.workerCount)){
            if(stopped(cancel,error))return false;
            // Generate only the next job. Blocks follow physical file order,
            // while each block's public offsets and axes remain IL/XL ascending.
            int rowBlock=layout.inlineFast?next%rowBlocks:next/columnBlocks;
            int columnBlock=layout.inlineFast?next/rowBlocks:next%columnBlocks;
            if(layout.reverseInline)rowBlock=rowBlocks-1-rowBlock;
            if(layout.reverseCrossline)columnBlock=columnBlocks-1-columnBlock;
            Job job;job.row=rowBlock*layout.rows;job.column=columnBlock*layout.columns;
            job.rows=std::min(layout.rows,rows-job.row);job.columns=std::min(layout.columns,columns-job.column);
            job.il.assign(output.inlineNumbers.begin()+job.row,output.inlineNumbers.begin()+job.row+job.rows);job.xl.assign(output.crosslineNumbers.begin()+job.column,output.crosslineNumbers.begin()+job.column+job.columns);
            ++next;job.key=tileKey(sourceIdentity,job,bandFirst,bandCount,strict,residual);
            if(caching){auto cached=cache().get(job.key);if(cached){++stats.cacheHits;publish(job,*cached,true);continue;}}
            ++stats.cacheMisses;
            auto future=std::async(std::launch::async,[path=facts.sourceFile,job,bandFirst,bandCount,options,strict,residual,abort,readers]{return loadTile(path,job,bandFirst,bandCount,options,strict,residual,abort,readers);});
            pending.push_back({std::move(job),std::move(future)});stats.workerCount=std::max(stats.workerCount,static_cast<int>(pending.size()));
        }
        bool completed=false;
        for(size_t n=0;n<pending.size();){
            auto& task=pending[n];if(task.future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){++n;continue;}
            auto loaded=task.future.get();if(!loaded.ok)return fail(error,loaded.error);
            if(stopped(cancel,error))return false;
            accumulate(stats,loaded.stats);if(caching)cache().put(task.job.key,loaded.tile);publish(task.job,*loaded.tile,false);
            pending.erase(pending.begin()+n);completed=true;
        }
        if(!completed&&!pending.empty())pending.front().future.wait_for(std::chrono::milliseconds(5));
    }
    std::string afterIdentity;if(!identity(facts.sourceFile,afterIdentity,error)||afterIdentity!=sourceIdentity){cache().clear();return fail(error,"地震来源在读取期间变化，已清除切片缓存，请重新导入");}
    if(stopped(cancel,error))return false;
    if(!any)return fail(error,"时间切片没有有限振幅");
    stats.elapsedMs=ms(started);if(stats.ioMode.empty())stats.ioMode="cache";
    result=std::move(output);return true;
}
}
