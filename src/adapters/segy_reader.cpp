#include "segy_reader.h"
#include "segy_window_io.h"
#include "Data/Sgy/SgyCoordinateMapper.h"
#include "Data/Sgy/SgyIndex.h"
#include "Data/Sgy/SgyRuleLayout.h"
#include <segyio/segy.h>
#include <QFileInfo>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace paleo::data::segy_detail {
namespace {
constexpr int SmallIndexLimit = 100000;
constexpr int ProbeLimit = 64;
using Clock=std::chrono::steady_clock;
double elapsedMs(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
bool fail(std::string& error, const std::string& message) { error = message; return false; }
bool cancelled(const Cancel& cancel, std::string& error) {
    return cancel && cancel() ? (error = "地震读取已取消", true) : false;
}
bool ok(int code, const char* operation, std::string& error) {
    return code == SEGY_OK || fail(error, std::string(operation) + " failed, segyio error=" + std::to_string(code));
}
using Header = std::array<char, 240>;
int i32(const Header& h, int at) { return qFromBigEndian<qint32>(h.data() + at); }
int i16(const Header& h, int at) { return qFromBigEndian<qint16>(h.data() + at); }
int u16(const Header& h, int at) { return qFromBigEndian<quint16>(h.data() + at); }
double scale(int code) { return code < 0 ? 1.0 / -double(code) : code > 0 ? double(code) : 1.0; }
template<class T> float valueAt(const std::vector<unsigned char>& raw, int n) {
    T value{}; std::memcpy(&value, raw.data() + size_t(n) * sizeof(T), sizeof(T));
    const double converted = static_cast<double>(value);
    return std::isfinite(converted) && std::abs(converted) <= std::numeric_limits<float>::max()
        ? static_cast<float>(value) : std::numeric_limits<float>::quiet_NaN();
}
bool supportedFormat(int format){return format==1||format==2||format==3||format==5||format==6||format==8||format==9||format==10||format==11||format==12||format==16;}
bool convertSamples(int format,int count,std::vector<unsigned char>& raw,float* output,std::string& error,const Cancel& cancel){
    if(!ok(segy_to_native(format,count,raw.data()),"segy_to_native",error))return false;
    for(int n=0;n<count;++n){
        if((n%4096)==0&&cancelled(cancel,error))return false;
        switch(format){
        case 1:case 5:output[n]=valueAt<float>(raw,n);break;
        case 2:output[n]=valueAt<std::int32_t>(raw,n);break;
        case 3:output[n]=valueAt<std::int16_t>(raw,n);break;
        case 6:output[n]=valueAt<double>(raw,n);break;
        case 8:output[n]=valueAt<std::int8_t>(raw,n);break;
        case 9:output[n]=valueAt<std::int64_t>(raw,n);break;
        case 10:output[n]=valueAt<std::uint32_t>(raw,n);break;
        case 11:output[n]=valueAt<std::uint16_t>(raw,n);break;
        case 12:output[n]=valueAt<std::uint64_t>(raw,n);break;
        case 16:output[n]=valueAt<std::uint8_t>(raw,n);break;
        default:return fail(error,"不支持的SEG-Y振幅格式");
        }
    }
    return true;
}
// Instrument the existing segyio seek/read operations without changing their behavior.
// The hook is thread-local, and every reader still owns its own datasource/FILE cursor.
struct ProfileHook;
thread_local ProfileHook* activeProfile=nullptr;
struct ProfileHook {
    segy_datasource* file;
    SeismicReadStatistics* stats;
    bool timings;
    decltype(segy_datasource::read) originalRead;
    decltype(segy_datasource::seek) originalSeek;
    ProfileHook* previous;
    ProfileHook(segy_datasource* source,SeismicReadStatistics* output,bool enabled)
        :file(source),stats(output),timings(enabled),originalRead(source->read),originalSeek(source->seek),previous(activeProfile){
        if(stats){activeProfile=this;file->read=read;file->seek=seek;}
    }
    ~ProfileHook(){if(stats){file->read=originalRead;file->seek=originalSeek;activeProfile=previous;}}
    static int read(segy_datasource* source,void* buffer,size_t size){
        auto& profile=*activeProfile;const auto start=profile.timings?Clock::now():Clock::time_point{};
        const int status=profile.originalRead(source,buffer,size);++profile.stats->readCalls;
        if(status==0)profile.stats->bytesRead+=static_cast<long long>(size);
        if(profile.timings)profile.stats->readMs+=elapsedMs(start);return status;
    }
    static int seek(segy_datasource* source,long long offset,int whence){
        auto& profile=*activeProfile;const auto start=profile.timings?Clock::now():Clock::time_point{};
        const int status=profile.originalSeek(source,offset,whence);++profile.stats->seekCalls;
        if(profile.timings)profile.stats->positionMs+=elapsedMs(start);return status;
    }
};
int axisStep(const std::vector<int>& axis) {
    if (axis.size() < 2) return 0;
    const long long step = static_cast<long long>(axis[1]) - axis[0];
    if (step <= 0 || step > std::numeric_limits<int>::max()) return 0;
    for (size_t i = 2; i < axis.size(); ++i) if (static_cast<long long>(axis[i]) - axis[i-1] != step) return 0;
    return static_cast<int>(step);
}
bool expectedKey(const seismic::SgyRuleLayout& r,int ordinal,int& il,int& xl) {
    if(!r.valid||ordinal<0||ordinal>=r.traceCount||r.inlineCount<1||r.xlineCount<1)return false;
    const long long i=r.inlineMajor?ordinal/r.xlineCount:ordinal%r.inlineCount;
    const long long j=r.inlineMajor?ordinal%r.xlineCount:ordinal/r.inlineCount;
    const long long inlineNumber=r.firstInline+i*r.inlineStep,crosslineNumber=r.firstXline+j*r.xlineStep;
    if(inlineNumber<std::numeric_limits<int>::min()||inlineNumber>std::numeric_limits<int>::max()||crosslineNumber<std::numeric_limits<int>::min()||crosslineNumber>std::numeric_limits<int>::max())return false;
    il=static_cast<int>(inlineNumber);xl=static_cast<int>(crosslineNumber);return true;
}
}

struct Reader::Impl {
    segy_datasource* file = nullptr;
    SeismicFileInfo facts;
    int inlineAt = 188, crosslineAt = 192;
    int xAt = 180, yAt = 184;
    int intervalUs = 0;
    double delayMs = 0;
    bool coordinatesPresent = false;
    bool unitsConsistent = true;
    bool geometryKnown = false;
    seismic::SgyRuleLayout rule;
    std::map<std::pair<int,int>, int> locations;
    std::vector<int> ilValues, xlValues;
    seismic::SgyIndex coordinateIndex;
    seismic::SgyCoordinateMapper mapper;
    std::unique_ptr<WindowAccess> batchIo;
    std::string batchMode;
    int batchWindowMiB=0;
    ~Impl() { if (file) segy_close(file); }

    bool header(int trace, Header& out, std::string& error, const Cancel& cancel) {
        if (cancelled(cancel,error)) return false;
        if (!ok(segy_read_standard_traceheader(file,trace,out.data()), "segy_read_standard_traceheader",error)) return false;
        ++facts.checkedHeaders;
        return true;
    }
    std::pair<int,int> key(const Header& h) const { return {i32(h,inlineAt),i32(h,crosslineAt)}; }
    double delay(const Header& h) const { return i16(h,108) * scale(i16(h,214)); }
    bool sampling(const Header& h, std::string& error) const {
        const int ns = u16(h,114), dt = u16(h,116);
        if ((ns && ns != facts.sampleCount) || (dt && dt != intervalUs))
            return fail(error,"所选地震道的采样数或步长与文件共同采样轴不一致；不能按固定道长继续读取");
        if (std::abs(delay(h)-delayMs) > 1e-8)
            return fail(error,"所选地震道延迟时间与文件共同采样轴不一致");
        return true;
    }
    bool validate(long long ordinal,const Header& h,std::string& error)const{
        if(!sampling(h,error))return false;
        const auto [il,xl]=key(h);
        if(rule.valid){int expectedIl=0,expectedXl=0;
            if(!expectedKey(rule,static_cast<int>(ordinal),expectedIl,expectedXl)||il!=expectedIl||xl!=expectedXl)
                return fail(error,"实际道头不符合抽样规则网格；已停止读取，需建立明确索引，不能显示错位振幅");}
        if(geometryKnown&&!rule.valid){const auto it=locations.find({il,xl});
            if(it==locations.end()||it->second!=ordinal)return fail(error,"道头与已建立索引不一致");}
        return true;
    }
    void normalizeRawHeader(Header& h)const{
        if(file->metadata.endianness!=SEGY_LSB)return;
        // The public segyio header reader normalizes these fields to big-endian.
        // Batch reads apply the same normalization only to fields consumed below.
        for(int at:{4,8,20,72,76,180,184,188,192})qToBigEndian(qFromLittleEndian<quint32>(h.data()+at),h.data()+at);
        for(int at:{70,88,108,114,116,214})qToBigEndian(qFromLittleEndian<quint16>(h.data()+at),h.data()+at);
    }
    void coordinates(const Header& h, double& x, double& y) const {
        x = y = Missing;
        // 单个道若声明不同坐标单位，XY留空；原生振幅仍可读，严格XY提取会拒绝。
        if (!coordinatesPresent || u16(h,88) != facts.coordinateUnitsCode) return;
        const double factor = scale(i16(h,70));
        x = i32(h,xAt) * factor; y = i32(h,yAt) * factor;
    }
    void addCoordinate(int ordinal, const Header& h) {
        const int units = u16(h,88);
        if (units != facts.coordinateUnitsCode) unitsConsistent = false;
        if (!coordinatesPresent) return;
        double x, y; coordinates(h,x,y);
        const auto [il,xl] = key(h);
        coordinateIndex.coordinateSamples.push_back({ordinal,il,xl,x,y});
    }
    void summarizeGeometry() {
        if (!geometryKnown || ilValues.empty() || xlValues.empty()) return;
        facts.inlineMin = ilValues.front(); facts.inlineMax = ilValues.back();
        facts.crosslineMin = xlValues.front(); facts.crosslineMax = xlValues.back();
        facts.inlineCount = static_cast<int>(ilValues.size()); facts.crosslineCount = static_cast<int>(xlValues.size());
        facts.inlineStep = axisStep(ilValues); facts.crosslineStep = axisStep(xlValues);
        coordinateIndex.inlineMin = facts.inlineMin; coordinateIndex.inlineMax = facts.inlineMax;
        coordinateIndex.xlineMin = facts.crosslineMin; coordinateIndex.xlineMax = facts.crosslineMax;
        coordinateIndex.coordinateFieldsPresent = coordinatesPresent && unitsConsistent;
        coordinateIndex.coordinateUnits = facts.coordinateUnitsCode;
        if (unitsConsistent && (facts.coordinateUnitsCode==0 || facts.coordinateUnitsCode==1))
            mapper = seismic::SgyCoordinateMapper::Fit(coordinateIndex,25.0);
    }
};

Reader::Reader() : impl_(std::make_unique<Impl>()) {}
Reader::~Reader() = default;
const SeismicFileInfo& Reader::info() const { return impl_->facts; }
const std::vector<int>& Reader::inlines() const { return impl_->ilValues; }
const std::vector<int>& Reader::crosslines() const { return impl_->xlValues; }

bool Reader::open(const std::string& path, std::string& error, const Cancel& cancel, bool geometry) {
    error.clear(); impl_ = std::make_unique<Impl>(); auto& p = *impl_;
    if (cancelled(cancel,error)) return false;
    const QFileInfo source(QString::fromUtf8(path));
    if (!source.isFile() || source.size() < 3840) return fail(error,"SEG-Y 文件不存在、为空或缺少完整道头");
    p.file = segy_open(path.c_str(),"rb");
    if (!p.file) return fail(error,"segy_open 无法只读打开 SEG-Y 文件");
    // segyio 2 alpha defaults to big-endian when the Rev2 endian marker is absent.
    // Classic little-endian exports need a bounded, unambiguous header check first.
    std::array<char,400> rawBinary{};
    if(p.file->seek(p.file,3200,SEEK_SET)!=0||p.file->read(p.file,rawBinary.data(),rawBinary.size())!=0)
        return fail(error,"无法读取SEG-Y二进制头以确认字节序");
    const auto marker=qFromBigEndian<quint32>(rawBinary.data()+96);
    int endian=-1;bool inferredEndian=false;
    if(marker==0x01020304u)endian=SEGY_MSB;
    else if(marker==0x04030201u)endian=SEGY_LSB;
    else if(marker!=0)return fail(error,"SEG-Y字节序标记无效或使用暂不支持的成对字节交换");
    else {
        const auto plausible=[&](bool little){
            const auto word=[&](int at){return little?qFromLittleEndian<quint16>(rawBinary.data()+at):qFromBigEndian<quint16>(rawBinary.data()+at);};
            const int format=word(24), samples=word(20), interval=word(16);
            const int extendedSamples=little?qFromLittleEndian<qint32>(rawBinary.data()+68):qFromBigEndian<qint32>(rawBinary.data()+68);
            const bool extended=static_cast<unsigned char>(rawBinary[300])>=2&&extendedSamples>0;
            return format>=1&&format<=16&&segy_formatsize(format)>0&&(samples>0||extended)&&interval>0;
        };
        const bool big=plausible(false),little=plausible(true);
        if(big==little)return fail(error,"缺少明确字节序标记，且格式码/采样数/步长不能唯一确认字节序");
        endian=little?SEGY_LSB:SEGY_MSB;inferredEndian=true;
    }
    // -1 honors the declared extended textual headers. A zero would incorrectly force trace0=3600.
    if (!ok(segy_collect_metadata(p.file,endian,-1,-1),"segy_collect_metadata",error)) return false;
    const auto& meta = p.file->metadata;
    if (meta.samplecount <= 0 || meta.elemsize <= 0 || meta.traceheader_count != 1)
        return fail(error,"当前按需服务要求正采样数、固定道长和每道一个240字节标准道头");
    const auto fileBytes = static_cast<unsigned long long>(source.size());
    const auto recordBytes = 240ULL + static_cast<unsigned long long>(meta.trace_bsize);
    if (meta.trace0 > fileBytes || (fileBytes-meta.trace0) % recordBytes != 0)
        return fail(error,"SEG-Y 文件长度与固定道长布局不一致；可能有变长道、截断或附加尾记录");
    const auto count = (fileBytes-meta.trace0) / recordBytes;
    if (count == 0 || count > static_cast<unsigned long long>(std::numeric_limits<int>::max()) || count != static_cast<unsigned long long>(meta.tracecount))
        return fail(error,"SEG-Y 道数无效或超过当前 segyio 道编号范围");
    auto& facts = p.facts;
    facts.engine="equinor/segyio v2.0.0-alpha.1 + paleo overflow patch";
    facts.sourceFile=source.absoluteFilePath().toUtf8().toStdString();
    facts.fileBytes=source.size(); facts.traceCount=static_cast<long long>(count);
    facts.sampleCount=meta.samplecount; facts.sampleFormat=meta.format;
    facts.byteOrder=meta.endianness==SEGY_LSB?"little-endian":"big-endian";
    facts.firstTraceOffset=static_cast<long long>(meta.trace0); facts.extendedTextHeaders=meta.ext_textheader_count;
    facts.timeUnit="ms"; facts.geometryStatus="unavailable";
    if(inferredEndian)facts.warnings.push_back("文件未提供Rev2字节序标记；已由格式码、采样数与采样步长唯一判定字节序，并核对固定道长布局。");
    facts.warnings.push_back("返回的时间轴按道头延迟加正采样步长排列；文本头中的负Z范围/地质时间约定需人工核对，不自动翻转样点，不推断TWT或OWT。");
    std::array<char,400> binary{};
    if (!ok(segy_binheader(p.file,binary.data()),"segy_binheader",error)) return false;
    const int revision=static_cast<unsigned char>(binary[300]);
    const int fixedLengthFlag=qFromBigEndian<quint16>(binary.data()+302);
    if(revision==0||fixedLengthFlag!=1)
        facts.warnings.push_back("文件未可靠声明固定道长；当前按固定道长核对文件长度与抽查采样轴，并在每次读道时复核，未证明全文件均为共同采样轴。");
    p.intervalUs=qFromBigEndian<quint16>(binary.data()+16);
    // Extended rev2 sample interval is not silently treated as the classic unsigned16 field.
    if (p.intervalUs<=0) return fail(error,"二进制头缺少可用的经典采样步长；需明确映射扩展采样轴");
    std::array<char,3201> text{};
    if (segy_read_textheader(p.file,text.data())==SEGY_OK) {
        for (int row=0;row<40;++row) { facts.textHeader.append(text.data()+row*80,80); facts.textHeader+='\n'; }
    }
    const int traceCount=static_cast<int>(count);
    std::map<int,Header> seeds;
    for (int ordinal:std::set<int>{0,std::min(1,traceCount-1),std::min(2,traceCount-1),traceCount-1}) {
        Header h{}; if (!p.header(ordinal,h,error,cancel)) return false; seeds.emplace(ordinal,h);
    }
    const Header& first=seeds.begin()->second;
    p.delayMs=p.delay(first); facts.firstTime=p.delayMs; facts.sampleInterval=p.intervalUs/1000.0;
    facts.coordinateUnitsCode=u16(first,88); facts.coordinateScalar=i16(first,70);
    const int measurement=p.file->metadata.endianness==SEGY_LSB?qFromLittleEndian<quint16>(binary.data()+54):qFromBigEndian<quint16>(binary.data()+54);
    if(facts.coordinateUnitsCode==1)facts.coordinateLengthUnit=measurement==1?"m":measurement==2?"ft":"";
    bool anyStandardIl=false,anyStandardXl=false,anyCdpXY=false,anySourceXY=false;
    for (const auto& [ordinal,h]:seeds) {
        (void)ordinal; anyStandardIl|=i32(h,188)!=0; anyStandardXl|=i32(h,192)!=0;
        anyCdpXY|=i32(h,180)!=0||i32(h,184)!=0; anySourceXY|=i32(h,72)!=0||i32(h,76)!=0;
        if (!p.sampling(h,error)) return false;
    }
    // Select one header dialect once. Do not switch word interpretation at each trace.
    if (!anyStandardXl) { p.crosslineAt=20; if(!anyStandardIl) p.inlineAt=8; }
    p.coordinatesPresent=anyCdpXY||anySourceXY;
    if (!anyCdpXY && anySourceXY) { p.xAt=72; p.yAt=76; }
    facts.warnings.push_back("测线字段采用1起始字节 INLINE="+std::to_string(p.inlineAt+1)+"、XLINE="+std::to_string(p.crosslineAt+1)+"；每次定位后复核实际道头。");
    if (!p.coordinatesPresent) facts.warnings.push_back("抽查道头中未发现有效XY坐标；仍可按文件道序号读取。");
    if (!geometry) return true;

    // Derive only a candidate regular rectangle from head/tail, then validate bounded probes.
    if (traceCount>=4) {
        const auto a=p.key(seeds.at(0)), b=p.key(seeds.at(1)), c=p.key(seeds.at(2)), z=p.key(seeds.at(traceCount-1));
        long long di=static_cast<long long>(b.first)-a.first, dj=static_cast<long long>(b.second)-a.second;
        if (!((di==0)!=(dj==0))) { di=static_cast<long long>(c.first)-b.first; dj=static_cast<long long>(c.second)-b.second; }
        const bool inlineMajor=di==0&&dj!=0;
        const bool crosslineMajor=dj==0&&di!=0;
        if (inlineMajor||crosslineMajor) {
            const long long step=inlineMajor?dj:di;
            const long long delta=inlineMajor?static_cast<long long>(z.second)-a.second:static_cast<long long>(z.first)-a.first;
            if (delta%step==0 && delta/step>=0) {
                const long long fastCount=delta/step+1;
                if (fastCount>0&&fastCount<=traceCount&&traceCount%fastCount==0) {
                    const long long slowCount=traceCount/fastCount;
                    const long long slowDelta=inlineMajor?static_cast<long long>(z.first)-a.first:static_cast<long long>(z.second)-a.second;
                    if(slowCount>1&&slowDelta%(slowCount-1)==0) {
                        const long long slowStep=slowDelta/(slowCount-1);
                        if(slowStep!=0&&std::abs(step)<=std::numeric_limits<int>::max()&&std::abs(slowStep)<=std::numeric_limits<int>::max()) {
                            auto& r=p.rule; r.valid=true;r.inlineMajor=inlineMajor;r.traceCount=traceCount;
                            r.firstInline=a.first;r.lastInline=z.first;r.firstXline=a.second;r.lastXline=z.second;
                            r.inlineCount=static_cast<int>(inlineMajor?slowCount:fastCount);r.xlineCount=static_cast<int>(inlineMajor?fastCount:slowCount);
                            r.inlineStep=static_cast<int>(inlineMajor?slowStep:step);r.xlineStep=static_cast<int>(inlineMajor?step:slowStep);
                        }
                    }
                }
            }
        }
    }
    std::set<int> probes;
    for(int n=0;n<std::min(ProbeLimit,traceCount);++n) probes.insert(static_cast<int>(static_cast<long long>(n)*(traceCount-1)/std::max(1,std::min(ProbeLimit,traceCount)-1)));
    // Row corners prevent a diagonal-only coordinate fit and verify wrap boundaries.
    if(p.rule.valid) { const int fast=p.rule.inlineMajor?p.rule.xlineCount:p.rule.inlineCount; for(int at:{fast-1,fast,traceCount-fast}) if(at>=0&&at<traceCount) probes.insert(at); }
    for(int ordinal:probes) {
        Header h{};const auto it=seeds.find(ordinal);
        if(it!=seeds.end()) h=it->second; else if(!p.header(ordinal,h,error,cancel))return false;
        if(!p.sampling(h,error))return false;
        p.addCoordinate(ordinal,h);
        if(p.rule.valid) { int il=0,xl=0;const auto actual=p.key(h); if(!expectedKey(p.rule,ordinal,il,xl)||actual!=std::pair<int,int>{il,xl})p.rule.valid=false; }
    }
    if(p.rule.valid) {
        // Axis size also has a strict bound, independent of the file's total bytes.
        if(static_cast<long long>(p.rule.inlineCount)+p.rule.xlineCount<=2000000) {
            for(int n=0;n<p.rule.inlineCount;++n)p.ilValues.push_back(static_cast<int>(p.rule.firstInline+static_cast<long long>(n)*p.rule.inlineStep));
            for(int n=0;n<p.rule.xlineCount;++n)p.xlValues.push_back(static_cast<int>(p.rule.firstXline+static_cast<long long>(n)*p.rule.xlineStep));
            std::sort(p.ilValues.begin(),p.ilValues.end());std::sort(p.xlValues.begin(),p.xlValues.end());
            p.geometryKnown=true;facts.geometryStatus="sampled-regular";
            facts.warnings.push_back("规则网格仅抽查道头；尚未全文件扫描，每条实际读取的道都再次核对测线编号和采样轴。");
        } else p.rule.valid=false;
    }
    if(!p.geometryKnown&&traceCount<=SmallIndexLimit) {
        std::set<int> ilSet,xlSet;bool duplicates=false;
        for(int ordinal=0;ordinal<traceCount;++ordinal) {
            Header h{};if(!p.header(ordinal,h,error,cancel)||!p.sampling(h,error))return false;
            const auto key=p.key(h);if(!p.locations.emplace(key,ordinal).second)duplicates=true;
            ilSet.insert(key.first);xlSet.insert(key.second);
        }
        if(!duplicates && !(ilSet.size()==1&&xlSet.size()==1&&traceCount>1)) {
            p.ilValues.assign(ilSet.begin(),ilSet.end());p.xlValues.assign(xlSet.begin(),xlSet.end());
            p.geometryKnown=true;facts.geometryStatus="full-header-index";
        } else { p.locations.clear();facts.warnings.push_back("测线编号重复，不能当作唯一三维网格；仍可按文件道序号读取。"); }
    }
    if(!p.geometryKnown)facts.warnings.push_back("无法在有界抽查中确认规则网格；未扫描大型文件。请按道序读取或提供明确索引，测线/切片提取暂不可用。");
    if(!p.unitsConsistent)facts.warnings.push_back("抽查道头坐标单位码不一致，禁用XY映射。");
    p.summarizeGeometry();
    return !cancelled(cancel,error);
}

long long Reader::traceIndex(int il,int xl) const {
    if(!impl_->geometryKnown)return -1;
    if(impl_->rule.valid){const auto& r=impl_->rule;const long long di=static_cast<long long>(il)-r.firstInline,dx=static_cast<long long>(xl)-r.firstXline;
        if(di%r.inlineStep||dx%r.xlineStep)return -1;const long long i=di/r.inlineStep,j=dx/r.xlineStep;
        if(i<0||i>=r.inlineCount||j<0||j>=r.xlineCount)return -1;return r.inlineMajor?i*r.xlineCount+j:j*r.inlineCount+i;}
    const auto it=impl_->locations.find({il,xl});return it==impl_->locations.end()?-1:it->second;
}
bool Reader::readTrace(long long ordinal,int first,int count,SeismicTraceResult& result,std::string& error,const Cancel& cancel,
                       SeismicReadStatistics* statistics,bool collectTimings) {
    result={};error.clear();auto& p=*impl_;
    if(cancelled(cancel,error))return false;
    if(!p.file||ordinal<0||ordinal>=p.facts.traceCount||first<0||count<1||first>=p.facts.sampleCount||count>p.facts.sampleCount-first||count>8000000)
        return fail(error,"地震道序号或采样范围无效/超过8000000样点预算");
    ProfileHook profile(p.file,statistics,collectTimings);
    const bool profileTime=statistics&&collectTimings;
    auto operationStart=profileTime?Clock::now():Clock::time_point{};
    double ioBefore=profileTime?statistics->positionMs+statistics->readMs:0;
    Header h{};if(!p.header(static_cast<int>(ordinal),h,error,cancel))return false;
    // segyio normalizes header byte order outside its seek/read callbacks.
    if(profileTime)statistics->validationMs+=std::max(0.0,elapsedMs(operationStart)-(statistics->positionMs+statistics->readMs-ioBefore));
    auto started=statistics&&collectTimings?Clock::now():Clock::time_point{};
    if(!p.validate(ordinal,h,error))return false;
    const auto [il,xl]=p.key(h);
    const int format=p.facts.sampleFormat;
    if(!supportedFormat(format))return fail(error,"当前振幅读取暂不支持 SEG-Y 样点格式码 "+std::to_string(format));
    if(statistics&&collectTimings)statistics->validationMs+=elapsedMs(started);
    std::vector<unsigned char> raw(size_t(count)*p.file->metadata.elemsize);
    operationStart=profileTime?Clock::now():Clock::time_point{};
    ioBefore=profileTime?statistics->positionMs+statistics->readMs:0;
    if(!ok(segy_readsubtr(p.file,static_cast<int>(ordinal),first,first+count,1,raw.data(),nullptr),"segy_readsubtr",error))return false;
    // The residual includes segyio's sample-offset arithmetic and LE→BE swap.
    if(profileTime)statistics->conversionMs+=std::max(0.0,elapsedMs(operationStart)-(statistics->positionMs+statistics->readMs-ioBefore));
    SeismicTraceResult output;output.traceIndex=ordinal;output.traceNumber=i32(h,4);
    output.inlineNumber=il;output.crosslineNumber=xl;p.coordinates(h,output.x,output.y);
    output.firstTime=p.delayMs+first*p.facts.sampleInterval;output.sampleInterval=p.facts.sampleInterval;output.timeUnit="ms";
    output.amplitudes.resize(count);
    started=statistics&&collectTimings?Clock::now():Clock::time_point{};
    if(!convertSamples(format,count,raw,output.amplitudes.data(),error,cancel))return false;
    if(statistics){if(collectTimings)statistics->conversionMs+=elapsedMs(started);++statistics->tracesRead;}
    if(cancelled(cancel,error))return false;result=std::move(output);return true;
}

bool Reader::readTraceBatch(const std::vector<long long>& ordinals,int first,int count,const BatchReadOptions& options,
                            TraceWindowBatch& result,SeismicReadStatistics& statistics,std::string& error,const Cancel& cancel){
    result={};statistics={};error.clear();const auto overall=Clock::now();auto& p=*impl_;
    if(cancelled(cancel,error))return false;
    if(!p.file||first<0||count<1||first>=p.facts.sampleCount||count>p.facts.sampleCount-first||count>8000000||
       ordinals.size()>1000000||ordinals.size()>64000000ULL/static_cast<unsigned>(count)||options.bufferMiB<1||options.bufferMiB>32)
        return fail(error,"批量道范围无效，或超过100万道/6400万振幅/1–32 MiB窗口预算");
    if(options.ioMode!="legacy"&&options.ioMode!="auto"&&options.ioMode!="buffered"&&options.ioMode!="mapped")
        return fail(error,"批量模式应为auto、buffered、mapped或legacy");
    if(!supportedFormat(p.facts.sampleFormat))return fail(error,"当前批量读取暂不支持SEG-Y格式码 "+std::to_string(p.facts.sampleFormat));
    for(auto ordinal:ordinals)if(ordinal<0||ordinal>=p.facts.traceCount)return fail(error,"批量请求包含无效物理道序号");
    TraceWindowBatch output;output.sampleCount=count;output.traceIndexes=ordinals;
    output.inlineNumbers.resize(ordinals.size());output.crosslineNumbers.resize(ordinals.size());
    output.x.resize(ordinals.size());output.y.resize(ordinals.size());output.amplitudes.resize(ordinals.size()*size_t(count));
    statistics.ioMode=options.ioMode;
    if(options.ioMode=="legacy"){
        for(size_t at=0;at<ordinals.size();++at){
            SeismicTraceResult trace;
            if(!readTrace(ordinals[at],first,count,trace,error,cancel,&statistics,options.collectTimings))return false;
            output.inlineNumbers[at]=trace.inlineNumber;output.crosslineNumbers[at]=trace.crosslineNumber;
            output.x[at]=trace.x;output.y[at]=trace.y;
            std::copy(trace.amplitudes.begin(),trace.amplitudes.end(),output.amplitudes.begin()+at*size_t(count));
        }
    }else if(!ordinals.empty()){
        auto started=options.collectTimings?Clock::now():Clock::time_point{};
        if(!p.batchIo||p.batchMode!=options.ioMode||p.batchWindowMiB!=options.bufferMiB){
            p.batchIo=std::make_unique<WindowAccess>();
            if(!p.batchIo->open(QString::fromUtf8(p.facts.sourceFile),p.facts.fileBytes,options.ioMode,options.bufferMiB,error)){
                p.batchIo.reset();return false;
            }
            p.batchMode=options.ioMode;p.batchWindowMiB=options.bufferMiB;
        }
        statistics.ioMode=p.batchIo->mode();
        std::vector<size_t> order(ordinals.size());std::iota(order.begin(),order.end(),0);
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return ordinals[a]<ordinals[b];});
        const auto recordBytes=240LL+static_cast<long long>(p.file->metadata.trace_bsize);
        const auto sampleBytes=static_cast<long long>(count)*p.file->metadata.elemsize;
        const auto sampleOffset=240LL+static_cast<long long>(first)*p.file->metadata.elemsize;
        const auto requestEnd=p.facts.firstTraceOffset+ordinals[order.back()]*recordBytes+sampleOffset+sampleBytes;
        if(requestEnd>p.facts.fileBytes)return fail(error,"批量样点位置越过文件边界");
        if(options.collectTimings)statistics.positionMs+=elapsedMs(started);
        std::vector<unsigned char> raw(static_cast<size_t>(sampleBytes));
        for(size_t at:order){
            if(cancelled(cancel,error))return false;
            started=options.collectTimings?Clock::now():Clock::time_point{};
            const auto ordinal=ordinals[at];
            const auto offset=p.facts.firstTraceOffset+ordinal*recordBytes;
            if(options.collectTimings)statistics.positionMs+=elapsedMs(started);
            Header h{};
            if(!p.batchIo->read(offset,h.data(),h.size(),requestEnd,statistics,options.collectTimings,error))return false;
            started=options.collectTimings?Clock::now():Clock::time_point{};
            p.normalizeRawHeader(h);++p.facts.checkedHeaders;
            if(!p.validate(ordinal,h,error))return false;
            const auto [il,xl]=p.key(h);output.inlineNumbers[at]=il;output.crosslineNumbers[at]=xl;
            p.coordinates(h,output.x[at],output.y[at]);
            if(options.collectTimings)statistics.validationMs+=elapsedMs(started);
            if(!p.batchIo->read(offset+sampleOffset,raw.data(),raw.size(),requestEnd,statistics,options.collectTimings,error))return false;
            started=options.collectTimings?Clock::now():Clock::time_point{};
            // segy_to_native expects the same canonical big-endian buffer as segy_readsubtr returns.
            if(p.file->metadata.endianness==SEGY_LSB&&p.file->metadata.elemsize>1){
                const auto width=static_cast<size_t>(p.file->metadata.elemsize);
                for(size_t sample=0;sample<raw.size();sample+=width)std::reverse(raw.begin()+sample,raw.begin()+sample+width);
            }
            if(!convertSamples(p.facts.sampleFormat,count,raw,output.amplitudes.data()+at*size_t(count),error,cancel))return false;
            if(options.collectTimings)statistics.conversionMs+=elapsedMs(started);
            ++statistics.tracesRead;
        }
    }
    if(cancelled(cancel,error))return false;
    statistics.elapsedMs=elapsedMs(overall);result=std::move(output);return true;
}
bool Reader::hasCoordinateMap() const {return impl_->mapper.valid();}
double Reader::maxCoordinateResidual() const {return impl_->mapper.fit().maxResidual;}
int Reader::coordinateSampleCount() const {return impl_->mapper.fit().sampleCount;}
double Reader::coordinateRmsResidual() const {return impl_->mapper.fit().rmsResidual;}
bool Reader::mapXY(double x,double y,double& il,double& xl)const{return impl_->mapper.MapXY(x,y,il,xl);}
bool Reader::mapLine(double il,double xl,double& x,double& y)const{return impl_->mapper.MapInlineXline(il,xl,x,y);}
bool Reader::covers(double x,double y) const {
    const auto& p=*impl_;if(!std::isfinite(x)||!std::isfinite(y)||!p.mapper.valid())return false;
    if(p.mapper.InCoverage(x,y))return true;
    double il=0,xl=0;if(!p.mapper.MapXY(x,y,il,xl))return false;
    il=std::clamp(il,double(p.facts.inlineMin),double(p.facts.inlineMax));xl=std::clamp(xl,double(p.facts.crosslineMin),double(p.facts.crosslineMax));
    double edgeX=0,edgeY=0;if(!p.mapper.MapInlineXline(il,xl,edgeX,edgeY))return false;
    return std::hypot(x-edgeX,y-edgeY)<=p.mapper.fit().maxResidual+1e-6;
}
bool inspectSegyFile(const std::string& path,SeismicFileInfo& result,std::string& error,const Cancel& cancel) {
    result={};error.clear();try {
        Reader reader;if(!reader.open(path,error,cancel))return false;
        auto info=reader.info();
        if(reader.hasCoordinateMap()&&!reader.inlines().empty()&&!reader.crosslines().empty()) {
            const int il[]={info.inlineMin,info.inlineMin,info.inlineMax,info.inlineMax};
            const int xl[]={info.crosslineMin,info.crosslineMax,info.crosslineMax,info.crosslineMin};
            for(int i=0;i<4;++i) {
                const auto ordinal=reader.traceIndex(il[i],xl[i]);SeismicTraceResult trace;std::string detail;
                if(ordinal<0||!reader.readTrace(ordinal,0,1,trace,detail,cancel)||!std::isfinite(trace.x)||!std::isfinite(trace.y)) {
                    if(cancel&&cancel()){error="地震读取已取消";return false;}
                    info.cornerX.clear();info.cornerY.clear();break;
                }
                info.cornerX.push_back(trace.x);info.cornerY.push_back(trace.y);
            }
        }
        result=std::move(info);return true;
    }
    catch(const std::exception& e){return fail(error,std::string("SEG-Y 检查失败：")+e.what());}
}
}
