#include "store/project_store.h"
#include "file_digest.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QDateTime>
#include <QSqlError>
#include <QElapsedTimer>
#include <algorithm>
#include <QThreadPool>
#include <QRunnable>
#include <QHash>
#include <atomic>
#include <deque>
#include <future>

namespace paleo::data {
using namespace detail;
namespace {
struct MeasureTime {
    long long& total; QElapsedTimer timer;
    explicit MeasureTime(long long& target):total(target){timer.start();}
    ~MeasureTime(){total+=timer.elapsed();}
};
// A bounded change detector, not a replacement for the full source SHA-256.
// Old databases have no cache entries: their first scan still verifies the whole source.
static bool scanSignature(const QString& path,QString& signature,const CancelCheck& cancelled,std::string& e){
    QFileInfo before(path);
    if(!before.isFile()){e="来源文件不存在或不是普通文件";return false;}
    auto stamp=[](const QFileInfo& info){return QByteArray::number(info.size())+":"+
        QByteArray::number(info.lastModified().toMSecsSinceEpoch())+":"+
        QByteArray::number(info.metadataChangeTime().toMSecsSinceEpoch())+":"+
        QByteArray::number(info.birthTime().toMSecsSinceEpoch());};
    const auto originalStamp=stamp(before);
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){e=s(file.errorString());return false;}
    QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(originalStamp);
    constexpr qint64 window=65536;
    std::vector<qint64> offsets{0,std::max<qint64>(0,(before.size()-window)/2),std::max<qint64>(0,before.size()-window)};
    offsets.erase(std::unique(offsets.begin(),offsets.end()),offsets.end());
    for(auto offset:offsets){
        if(cancelled&&cancelled()){e="已取消";return false;}
        if(!file.seek(offset)){e=s(file.errorString());return false;}
        auto bytes=file.read(std::min(window,before.size()-offset));
        if(bytes.size()!=std::min(window,before.size()-offset)||file.error()!=QFile::NoError){e="来源文件读取失败或在检查时发生变化";return false;}
        hash.addData(QByteArray::number(offset));hash.addData(bytes);
    }
    if(originalStamp!=stamp(QFileInfo(path))){e="来源文件在检查时发生变化，请文件写入完成后重试";return false;}
    signature="scan-v1:"+QString::fromLatin1(hash.result().toHex());return true;
}
}
static QString optionsJson(const ParseOptions& o){
    QJsonArray rules;for(auto& r:o.cleaningRules){QJsonArray nulls;for(double n:r.nullValues)nulls.append(n);rules.append(QJsonObject{{"curve",q(r.curveType)},{"unit",q(r.unit)},{"nulls",nulls},{"checkMin",r.checkMinimum},{"min",r.minimum},{"checkMax",r.checkMaximum},{"max",r.maximum},{"replace",r.replaceInvalidWithMissing}});}
    QJsonObject config{{"parser_version",2},{"well",q(o.wellNameOverride)},{"crs",q(o.coordinateSystem)},{"unit",q(o.coordinateUnit)},{"depthUnit",q(o.depthUnit)},{"order",q(o.coordinateOrder)},{"rules",rules}};
    // Missing means false, preserving the signature of existing default imports
    // and their edits. Opting into MD/metres must invalidate both duplicate checks.
    if(o.comprehensiveWellMetreDepth)config["comprehensiveWellMetreDepth"]=true;
    return json(config);
}
static bool digestFile(const QString& path,QString& digest,const CancelCheck& cancelled,std::string& e,long long* bytesRead=nullptr){
    bool largeSegy=QStringList{"sgy","segy"}.contains(QFileInfo(path).suffix().toLower());
    if(largeSegy){
        if(cancelled&&cancelled()){e="已取消";return false;}
        QFile f(path);if(!f.open(QIODevice::ReadOnly)){e=s(f.errorString());return false;}
        QCryptographicHash hash(QCryptographicHash::Sha256);auto head=f.read(65536);
        if(f.error()!=QFile::NoError){e=s(f.errorString());return false;}
        if(bytesRead)*bytesRead+=head.size();hash.addData(head);hash.addData(QByteArray::number(f.size()));hash.addData(QByteArray::number(QFileInfo(path).lastModified().toMSecsSinceEpoch()));digest="quick:"+QString::fromLatin1(hash.result().toHex());return true;
    }
    long long read=0;const bool ok=fullFileSha256(path,digest,e,cancelled,&read);if(bytesRead)*bytesRead+=read;return ok;
}
// Parser workers own no database connections and never publish business objects.
// The snapshot only avoids unnecessary parsing; the writer repeats duplicate checks.
struct KnownSource {QString hash,signature;};
struct PreparedImport {
    QString signature,hash;
    ParsedFile parsed;
    bool ready=false,hasParsed=false;
    std::string error;
    long long hashMs=0,parseMs=0,hashedBytes=0;
};
static PreparedImport prepareImport(const QString& path,const ImportOptions& options,
    const std::vector<KnownSource>& known,const CancelCheck& cancelled,
    const std::function<void(const std::string&)>& phase){
    PreparedImport out;
    phase("校验");
    {
        MeasureTime measure(out.hashMs);
        if(!scanSignature(path,out.signature,cancelled,out.error))return out;
        if(!options.forceNewBatch&&!options.verifyUnchangedFiles)
            for(const auto& previous:known)if(previous.signature==out.signature){out.ready=true;return out;}
        if(!digestFile(path,out.hash,cancelled,out.error,&out.hashedBytes))return out;
    }
    if(!options.forceNewBatch)for(const auto& previous:known)if(previous.hash==out.hash){out.ready=true;return out;}
    phase("解析");
    {
        MeasureTime measure(out.parseMs);
        try{auto parsing=options.parsing;parsing.sourceContextPath=s(path);out.parsed=parseFile(s(path),parsing,cancelled);}
        catch(const std::exception& ex){out.parsed.path=s(path);out.parsed.status="failed";out.parsed.message=ex.what();}
        out.hasParsed=true;
    }
    if(cancelled&&cancelled()){out.error="已取消";return out;}
    out.ready=true;return out;
}
static bool copySource(const QString& src,const QString& target,const CancelCheck& cancelled,std::string& e){
    if(QFileInfo::exists(target))return true;QDir().mkpath(QFileInfo(target).absolutePath());QFile in(src);QSaveFile out(target);
    if(!in.open(QIODevice::ReadOnly)||!out.open(QIODevice::WriteOnly)){e="无法保存原始资料副本";return false;}
    while(!in.atEnd()){if(cancelled&&cancelled()){e="已取消";return false;}auto b=in.read(1024*1024);if((b.isEmpty()&&in.error()!=QFile::NoError)||out.write(b)!=b.size()){e="原始资料副本写入失败";return false;}}if(!out.commit()){e=s(out.errorString());return false;}return true;
}
static void countStatus(const std::string& status,ImportReport& report){if(status=="parsed")++report.imported;else if(status=="review")++report.review;else if(status=="registered")++report.registered;else if(status=="ignored")++report.ignored;else ++report.failed;}
static bool doImport(ProjectStore& store,const QString& input,const QString& identity,const ImportOptions& options,ImportReport& report,std::string& e,const CancelCheck& cancelled,long long replaceId=0,const std::function<void(const std::string&)>& phase={},PreparedImport* prepared=nullptr){
    if(!store.db.isOpen()){e="请先打开工区";return false;}++report.seen;
    if(cancelled&&cancelled()){report.cancelled=true;e="已取消";return false;}
    if(phase)phase("校验");
    QString signature,hash,config=optionsJson(options.parsing);
    if(prepared){
        report.hashMs+=prepared->hashMs;report.parseMs+=prepared->parseMs;report.hashedBytes+=prepared->hashedBytes;
        if(!prepared->ready){e=prepared->error;return false;}
        signature=prepared->signature;hash=prepared->hash;
    }
    auto stable=[&](){QString now;if(!scanSignature(input,now,cancelled,e))return false;if(now!=signature){e="文件在导入过程中发生变化，未发布本次结果，请写入完成后重试";return false;}return true;};
    auto saveCache=[&](long long id){return store.exec("INSERT INTO source_scan_cache(source_id,signature) VALUES(?,?) ON CONFLICT(source_id) DO UPDATE SET signature=excluded.signature",{id,signature},e);};
    {
        MeasureTime measure(report.hashMs);
        if(prepared?!stable():!scanSignature(input,signature,cancelled,e)){if(cancelled&&cancelled())report.cancelled=true;else ++report.failed;return false;}
        if(!options.verifyUnchangedFiles&&!options.forceNewBatch&&!replaceId){
            QSqlQuery cached;
            if(!store.exec("SELECT s.id FROM sources s JOIN source_scan_cache c ON c.source_id=s.id WHERE s.original_path=? AND s.options=? AND s.active=1 AND s.status<>'failed' AND c.signature=? LIMIT 1",{identity,config,signature},e,&cached))return false;
            if(cached.next()){++report.skipped;++report.cached;return true;}
        }
        if(hash.isEmpty()&&!digestFile(input,hash,cancelled,e,&report.hashedBytes)){if(cancelled&&cancelled())report.cancelled=true;else ++report.failed;return false;}
        if(!options.forceNewBatch&&!replaceId){
            QSqlQuery old;
            if(!store.exec("SELECT id FROM sources WHERE original_path=? AND hash=? AND options=? AND active=1 AND status<>'failed'",{identity,hash,config},e,&old))return false;
            if(old.next()){auto id=old.value(0).toLongLong();old.finish();if(!stable()||!saveCache(id))return false;++report.skipped;return true;}
        }
    }
    if(!options.forceNewBatch&&!replaceId){
        // Automatically upgrade registrations which had no business objects. Existing structured
        // data may contain user edits: preserve it until the explicit reimport action is used.
        QSqlQuery previous;if(!store.exec("SELECT id,options FROM sources WHERE original_path=? AND hash=? AND active=1 ORDER BY id DESC",{identity,hash},e,&previous))return false;
        auto current=object(config);current.remove("parser_version");
        long long olderId=0;while(previous.next()){auto saved=object(previous.value(1).toString());const int version=saved.take("parser_version").toInt();if(version<2&&saved==current){olderId=previous.value(0).toLongLong();break;}}previous.finish();
        if(olderId){QSqlQuery objects;
            if(!store.exec("SELECT (SELECT count(*) FROM log_sets WHERE source_id=?)+(SELECT count(*) FROM tracks WHERE source_id=?)+(SELECT count(*) FROM sample_tables WHERE source_id=?)+(SELECT count(*) FROM well_objects WHERE source_id=?)+(SELECT count(*) FROM horizons WHERE source_id=?)+(SELECT count(*) FROM volumes WHERE source_id=?)+(SELECT count(*) FROM maps WHERE source_id=?)",{olderId,olderId,olderId,olderId,olderId,olderId,olderId},e,&objects)||!objects.next())return false;
            const bool hasObjects=objects.value(0).toLongLong()>0;objects.finish();
            if(hasObjects){
                if(!store.exec("INSERT INTO issues(source_id,code,message,blocking) SELECT ?,'PARSER_UPDATE_AVAILABLE','解析器已更新；现有结构化数据和人工修改保持不变。如需按新版解析，请在来源清单明确重新导入。',0 WHERE NOT EXISTS(SELECT 1 FROM issues WHERE source_id=? AND code='PARSER_UPDATE_AVAILABLE')",{olderId,olderId},e))return false;
                ++report.skipped;return true;
            }
            replaceId=olderId;
        }
    }
    ParsedFile parsed;
    if(prepared&&prepared->hasParsed)parsed=std::move(prepared->parsed);
    else{
    if(phase)phase("解析");
    {MeasureTime measure(report.parseMs);
    try{auto parsing=options.parsing;parsing.sourceContextPath=s(identity);parsed=parseFile(s(input),parsing,cancelled);}catch(const std::exception& ex){parsed.status="failed";parsed.message=ex.what();parsed.path=s(input);}}
    }
    if(cancelled&&cancelled()){report.cancelled=true;e="已取消";return false;}
    if(parsed.status.empty()){parsed.status="failed";parsed.message="解析器未返回明确状态";}
    if(parsed.kind.empty())parsed.kind=s(QFileInfo(input).suffix().toLower());
    QString stored=input;
    bool external=QStringList{"sgy","segy"}.contains(QFileInfo(input).suffix().toLower());
    if(options.copySources&&!external&&parsed.status!="ignored"){
        if(phase)phase("复制");MeasureTime measure(report.copyMs);
        stored="sources/"+hash.left(20)+"/"+QFileInfo(input).fileName();
        if(!copySource(input,QDir(store.root).filePath(stored),cancelled,e)){if(cancelled&&cancelled())report.cancelled=true;else ++report.failed;return false;}
    }
    for(auto& map:parsed.maps){if(q(map.imagePath)==input)map.imagePath=s(stored);if(q(map.sourceFile)==input)map.sourceFile=s(stored);}
    for(auto& well:parsed.wells){
        for(auto& track:well.wellbore.corePhotoTracks)for(auto& photo:track.photos)
            if(QFileInfo(q(photo.imagePath)).absoluteFilePath()==input)photo.imagePath=s(stored);
        for(auto& attachment:well.attachments)
            if(QFileInfo(q(attachment.filePath)).absoluteFilePath()==input)attachment.filePath=s(stored);
    }
    {MeasureTime measure(report.hashMs);if(!stable()){if(cancelled&&cancelled())report.cancelled=true;return false;}}
    if(phase)phase("入库");MeasureTime storeMeasure(report.storeMs);
    if(!store.begin(e))return false;
    if(!options.forceNewBatch){QSqlQuery duplicate;if(!store.exec("SELECT id FROM sources WHERE original_path=? AND hash=? AND options=? AND active=1 AND status<>'failed'",{identity,hash,config},e,&duplicate)){store.rollback();return false;}if(duplicate.next()){store.rollback();++report.skipped;return true;}}
    QSqlQuery created;
    if(!store.exec("INSERT INTO sources(original_path,stored_path,hash,options,kind,status,message,bytes,document) VALUES(?,?,?,?,?,?,?,?,?)",{identity,stored,hash,config,q(parsed.kind),q(parsed.status),q(parsed.message),QFileInfo(input).size(),q(parsed.documentText)},e,&created)){store.rollback();return false;}
    auto sourceId=created.lastInsertId().toLongLong();
    if(!store.exec("SAVEPOINT parsed_objects",{},e)){store.rollback();return false;}
    bool good=parsed.status!="failed"&&parsed.status!="ignored";
    if(good&&!store.saveParsed(sourceId,parsed,options.parsing,e,cancelled)){store.rollback();if(cancelled&&cancelled()){report.cancelled=true;e="已取消";return false;}++report.failed;std::string sqlError=e,recordError;store.exec("INSERT INTO sources(original_path,stored_path,hash,options,kind,status,message,bytes) VALUES(?,?,?,?,?,'failed',?,?)",{identity,stored,hash,config,q(parsed.kind),q(sqlError),QFileInfo(input).size()},recordError);return false;}
    if(!good){for(auto& issue:parsed.issues)if(!store.exec("INSERT INTO issues(source_id,code,message,original_text,sheet,row_num,blocking) VALUES(?,?,?,?,?,?,?)",{sourceId,q(issue.code),q(issue.message),q(issue.originalText),q(issue.sheet),issue.row,issue.blocking},e)){store.rollback();return false;}}
    if(cancelled&&cancelled()){store.rollback();report.cancelled=true;e="已取消";return false;}
    QSqlQuery issues;if(!store.exec("SELECT count(*),coalesce(sum(blocking),0) FROM issues WHERE source_id=? AND resolved=0",{sourceId},e,&issues)){store.rollback();return false;}issues.next();bool blocked=issues.value(1).toInt()>0;
    if(good&&issues.value(0).toInt()>0){parsed.status="review";if(!store.exec("UPDATE sources SET status='review' WHERE id=?",{sourceId},e)){store.rollback();return false;}}
    if(replaceId&&blocked){
        QSqlQuery details;if(!store.exec("SELECT code,message,original_text,sheet,row_num,blocking FROM issues WHERE source_id=?",{sourceId},e,&details)){store.rollback();return false;}std::vector<QVariantList> savedIssues;while(details.next()){QVariantList issue{sourceId};for(int column=0;column<6;++column)issue<<details.value(column);savedIssues.push_back(std::move(issue));}details.finish();
        if(!store.exec("ROLLBACK TO parsed_objects",{},e)){store.rollback();return false;}
        for(const auto& issue:savedIssues)if(!store.exec("INSERT INTO issues(source_id,code,message,original_text,sheet,row_num,blocking) VALUES(?,?,?,?,?,?,?)",issue,e)){store.rollback();return false;}
        if(!store.exec("UPDATE sources SET active=0,status='review',message=message || '；核对未通过，旧批次保持有效，新批次仅留问题记录' WHERE id=?",{sourceId},e)){store.rollback();return false;}
    }
    if(!store.exec("RELEASE parsed_objects",{},e)){store.rollback();return false;}
    if(replaceId&&good&&!blocked){if(!store.removeSourceObjects(replaceId,e)||!store.exec("UPDATE issues SET resolved=1,resolution='已按明确映射重新导入；请查看新批次问题' WHERE source_id=?",{replaceId},e)){store.rollback();return false;}}
    if(parsed.status!="failed"&&!saveCache(sourceId)){store.rollback();return false;}
    if(!store.commit(e)){store.rollback();return false;}countStatus(parsed.status,report);
    if(parsed.status=="failed"){e=parsed.message.empty()?"文件解析失败":parsed.message;return false;}return true;
}
bool DataService::importFile(const std::string& path,const ImportOptions& options,ImportReport& report,std::string& e,const CancelCheck& cancelled){
    if(impl_->store.readOnly){report={};e="工区以只读方式打开，不能导入或重导入";return false;}
    report={};e.clear();MeasureTime measure(report.elapsedMs);bool ok=doImport(impl_->store,QFileInfo(q(path)).absoluteFilePath(),QFileInfo(q(path)).absoluteFilePath(),options,report,e,cancelled);if(!ok&&cancelled&&cancelled())report.cancelled=true;if(!ok&&!report.cancelled&&!report.failed)++report.failed;return ok;
}
static bool importPreparedDirectory(ProjectStore& store,const QStringList& files,const ImportOptions& options,
    ImportReport& report,std::string& error,const ProgressCallback& progress,const CancelCheck& cancelled){
    if(progress)progress({0,int(files.size()),{},"准备来源索引与解析线程"});
    QHash<QString,std::vector<KnownSource>> known;
    QSqlQuery rows;
    if(!store.exec("SELECT s.original_path,s.hash,coalesce(c.signature,'') FROM sources s LEFT JOIN source_scan_cache c ON c.source_id=s.id WHERE s.active=1 AND s.status<>'failed' AND s.options=?",{optionsJson(options.parsing)},error,&rows))return false;
    while(rows.next())known[rows.value(0).toString()].push_back({rows.value(1).toString(),rows.value(2).toString()});rows.finish();

    std::atomic<bool> stop{false};
    QThreadPool pool;pool.setMaxThreadCount(report.workerCount);
    struct JoinWorkers {std::atomic<bool>& stop;QThreadPool& pool;~JoinWorkers(){stop=true;pool.waitForDone();}} join{stop,pool};
    struct Pending {int index;long long weight;std::future<PreparedImport> result;std::shared_ptr<std::atomic<int>> stage;};
    std::deque<Pending> pending;
    const auto budget=std::clamp(options.maxPendingBytes,8LL*1024*1024,1024LL*1024*1024);
    auto estimate=[&](const QString& path){
        const QFileInfo info(path);const auto size=info.size();
        const auto ext=info.suffix().toLower();
        // These formats only yield bounded headers/attachment registrations, not full decoded media.
        // Hashing them uses a 1MiB buffer; a large archive/photo need not block all other workers.
        if(QStringList{"cdr","zip","rar","7z","mp4","mov","jpg","jpeg","png","bmp","gif","tif","tiff","sgy","segy","pdf"}.contains(ext))return 2LL*1024*1024;
        if(size>=32LL*1024*1024&&ext!="xml"&&ext!="las")return budget; // Compressed/unknown expansion stays isolated.
        const bool table=QStringList{"xml","xls","xlsx","las","csv","dat","txt",""}.contains(ext);
        // Count both running jobs AND completed results waiting for their turn.
        // SpreadsheetML is uncompressed and streamed row-by-row; the XML tags already
        // occupy most input bytes. XLSX/other tables need a larger expansion allowance.
        const auto expansion=(ext=="xml"||ext=="las")?4LL:32LL;
        return std::min(budget,std::max(2LL*1024*1024,table?size*expansion:2LL*1024*1024));
    };
    long long reserved=0;int scheduled=0,completed=0;
    auto checkCancel=[&](){if(cancelled&&cancelled())stop=true;if(stop.load())report.cancelled=true;return stop.load();};
    while(completed<files.size()){
        if(checkCancel())break;
        while(scheduled<files.size()&&pending.size()<size_t(report.workerCount)){
            const QString file=files[scheduled];const auto weight=estimate(file);
            if(!pending.empty()&&reserved+weight>budget)break;
            auto stage=std::make_shared<std::atomic<int>>(0);
            const auto previous=known.value(file);
            auto task=std::make_shared<std::packaged_task<PreparedImport()>>([file,options,previous,stage,&stop]{
                return prepareImport(file,options,previous,[&stop]{return stop.load();},
                    [stage](const std::string& name){*stage=name=="解析"?2:1;});
            });
            auto future=task->get_future();
            pool.start(QRunnable::create([task]{(*task)();}));
            pending.push_back({scheduled,weight,std::move(future),stage});reserved+=weight;++scheduled;
        }
        auto& head=pending.front();const QString file=files[head.index];
        while(head.result.wait_for(std::chrono::milliseconds(50))!=std::future_status::ready){
            if(checkCancel())break;
            if(progress)progress({completed,int(files.size()),s(file),head.stage->load()==2?"并行解析":"并行校验"});
        }
        if(checkCancel())break;
        PreparedImport prepared;
        try{prepared=head.result.get();}catch(const std::exception& ex){prepared.error=ex.what();}catch(...){prepared.error="后台文件准备发生未知错误";}
        reserved-=head.weight;pending.pop_front();
        auto phase=[&](const std::string& name){if(progress)progress({completed,int(files.size()),s(file),name});};
        std::string local;const auto skipped=report.skipped,beforeFailed=report.failed;
        const bool ok=doImport(store,file,file,options,report,local,checkCancel,0,phase,&prepared);
        if(!ok&&!report.cancelled){if(report.failed==beforeFailed)++report.failed;report.errors.push_back(s(file)+": "+local);}
        ++completed;
        if(progress)progress({completed,int(files.size()),s(file),report.cancelled?"已取消":(!ok?"失败":report.skipped>skipped?"已跳过":"已处理")});
        if(report.cancelled)break;
    }
    if(report.cancelled){error="导入已取消，当前文件未发布，已完成文件保留";return false;}
    if(report.failed){error="导入完成，部分文件失败；请查看来源和问题列表";return false;}return true;
}
bool DataService::importDirectory(const std::string& directory,const ImportOptions& options,ImportReport& report,std::string& e,const ProgressCallback& progress,const CancelCheck& cancelled){
    if(impl_->store.readOnly){report={};e="工区以只读方式打开，不能导入或重导入";return false;}
    report={};e.clear();MeasureTime measure(report.elapsedMs);QDir input(q(directory));if(!input.exists()){e="输入目录不存在";return false;}if(!impl_->store.db.isOpen()){e="请先打开工区";return false;}
    // Walk one directory at a time so we can prune the project's copied sources entirely.
    // The old recursive iterator entered the whole project before discarding its files.
    QStringList files,pending{input.absolutePath()};const auto own=QDir::fromNativeSeparators(QDir(impl_->store.root).absolutePath());
    QStringList excluded;
    for(const auto& path:options.excludedDirectories)excluded<<QDir::fromNativeSeparators(QDir(q(path)).absolutePath());
    if(progress)progress({0,0,s(input.absolutePath()),"扫描目录"});
    int enumerated=0;
    while(!pending.isEmpty()){
        if(cancelled&&cancelled()){report.cancelled=true;break;}
        const auto folder=QDir::fromNativeSeparators(pending.takeLast());
        if(folder.compare(own,Qt::CaseInsensitive)==0||folder.startsWith(own+"/",Qt::CaseInsensitive))continue;
        bool pruned=false;
        for(const auto& path:excluded)if(folder.compare(path,Qt::CaseInsensitive)==0||folder.startsWith(path+"/",Qt::CaseInsensitive)){pruned=true;break;}
        if(pruned)continue;
        QDirIterator it(folder,QDir::Files|QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot,QDirIterator::NoIteratorFlags);
        while(it.hasNext()){
            if(cancelled&&cancelled()){report.cancelled=true;break;}
            auto path=QDir::fromNativeSeparators(it.next());const auto info=it.fileInfo();
            if(info.isDir()){if(!info.isSymLink())pending<<path;}
            else if(info.isFile())files<<path;
            if(++enumerated%100==0&&progress)progress({0,0,s(path),"扫描目录"});
        }
        if(report.cancelled)break;
    }
    if(report.cancelled){e="目录扫描已取消，尚未导入文件";return false;}files.sort();
    report.workerCount=std::clamp(options.parserThreads,1,4);
    if(report.workerCount>1)return importPreparedDirectory(impl_->store,files,options,report,e,progress,cancelled);
    int completed=0;for(auto& file:files){if(cancelled&&cancelled()){report.cancelled=true;break;}std::string local;int skipped=report.skipped,beforeFailed=report.failed;
        auto phase=[&](const std::string& status){if(progress)progress({completed,int(files.size()),s(file),status});};
        bool ok=doImport(impl_->store,file,file,options,report,local,cancelled,0,phase);if(!ok&&cancelled&&cancelled())report.cancelled=true;if(!ok&&!report.cancelled){if(report.failed==beforeFailed)++report.failed;report.errors.push_back(s(file)+": "+local);}
        ++completed;if(progress)progress({completed,int(files.size()),s(file),report.cancelled?"已取消":(!ok?"失败":(report.skipped>skipped?"已跳过":"已处理"))});if(report.cancelled)break;
    }if(report.cancelled){e="导入已取消，当前文件未发布，已完成文件保留";return false;}if(report.failed){e="导入完成，部分文件失败；请查看来源和问题列表";return false;}return true;
}
bool DataService::reimportSource(long long id,const ParseOptions& mapping,ImportReport& report,std::string& e,const CancelCheck& cancelled){
    if(impl_->store.readOnly){report={};e="工区以只读方式打开，不能导入或重导入";return false;}
    report={};e.clear();MeasureTime measure(report.elapsedMs);QSqlQuery r;if(!impl_->store.exec("SELECT original_path,stored_path FROM sources WHERE id=?",{id},e,&r))return false;if(!r.next()){e="来源不存在";return false;}
    auto path=impl_->store.absolutePath(r.value(1).toString());ImportOptions options;options.parsing=mapping;options.forceNewBatch=true;return doImport(impl_->store,path,r.value(0).toString(),options,report,e,cancelled,id);
}
bool DataService::relocateSource(long long id,const std::string& path,std::string& e){
    e.clear();
    QSqlQuery r;auto& st=impl_->store;if(!st.exec("SELECT hash,stored_path FROM sources WHERE id=?",{id},e,&r))return false;if(!r.next()){e="来源不存在";return false;}
    const auto expected=r.value(0).toString();
    auto replacement=QFileInfo(q(path)).absoluteFilePath();QString digest;
    // Prepared sources use full SHA-256 even for SEG-Y. Match the stored identity
    // scheme, just as verifySource does, instead of guessing from the extension.
    if(expected.startsWith("quick:")?!digestFile(replacement,digest,{},e):!fullFileSha256(replacement,digest,e))return false;
    if(digest!=expected){e="新文件与记录身份不符，不能静默替换；请作为新资料导入。";if(expected.startsWith("quick:"))e+="地震体快速身份包含文件大小、修改时间和文件头。";return false;}
    const QString previous=r.value(1).toString();r.finish();
    if(!st.begin(e))return false;
    QSqlQuery objects;if(!st.exec("SELECT id,kind,payload FROM well_objects WHERE source_id=? AND kind IN ('core_photos','attachments')",{id},e,&objects)){st.rollback();return false;}
    std::vector<std::pair<qlonglong,QString>> updates;
    while(objects.next()){
        auto doc=QJsonDocument::fromJson(objects.value(2).toString().toUtf8());
        auto replacePath=[&](QJsonObject o){const QString p=o["path"].toString();if(!p.isEmpty()&&(p==previous||st.absolutePath(p)==st.absolutePath(previous)))o["path"]=replacement;return o;};
        if(doc.isArray()){QJsonArray images;for(auto v:doc.array())images.append(replacePath(v.toObject()));updates.push_back({objects.value(0).toLongLong(),json(images)});}
        else updates.push_back({objects.value(0).toLongLong(),json(replacePath(doc.object()))});
    }objects.finish();
    for(const auto& [objectId,payload]:updates)if(!st.exec("UPDATE well_objects SET payload=? WHERE id=?",{payload,objectId},e)){st.rollback();return false;}
    bool ok=st.exec("UPDATE sources SET stored_path=? WHERE id=?",{replacement,id},e)&&st.exec("UPDATE volumes SET path=? WHERE source_id=?",{replacement,id},e)&&st.exec("UPDATE maps SET image_path=CASE WHEN image_path=? THEN ? ELSE image_path END,source_file=CASE WHEN source_file=? THEN ? ELSE source_file END WHERE source_id=?",{previous,replacement,previous,replacement,id},e)&&st.exec("INSERT INTO changes(kind,entity_id,description) VALUES('source',?,'核验文件身份后重新定位来源')",{id},e)&&st.commit(e);if(!ok)st.rollback();return ok;
}
bool DataService::verifySource(long long id,std::string& e)const{
    QSqlQuery r;const auto& st=impl_->store;if(!st.exec("SELECT hash,stored_path FROM sources WHERE id=?",{id},e,&r))return false;if(!r.next()){e="来源不存在";return false;}QString digest;const auto expected=r.value(0).toString();const auto path=st.absolutePath(r.value(1).toString());if(expected.startsWith("quick:")?!digestFile(path,digest,{},e):!fullFileSha256(path,digest,e))return false;if(digest!=expected){e="来源文件已改变，请核对后重新导入，不能继续使用旧对象读取";return false;}return true;
}
}
