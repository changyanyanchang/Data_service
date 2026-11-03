// Synthetic fixtures only: byte order, numeric formats, ROI, callbacks and cache correctness.
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace paleo::data;
namespace {
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
std::string utf8(const QString& value){return value.toUtf8().toStdString();}
template<class T>void put(QByteArray& bytes,int offset,T value,bool little){if(little)qToLittleEndian<T>(value,bytes.data()+offset);else qToBigEndian<T>(value,bytes.data()+offset);}
void write(QFile& file,const QByteArray& bytes){check(file.write(bytes)==bytes.size(),"fixture write");}
int width(int format){switch(format){case 3:case 11:return 2;case 6:case 9:case 12:return 8;case 8:case 16:return 1;default:return 4;}}
bool unsignedFormat(int format){return format==10||format==11||format==12||format==16;}
float expected(int il,int xl,int sample,int format=5){
    if((format==5||format==6)&&il==1&&xl==1&&sample==4)return float(Missing);
    return float((il*3+xl*5+sample)%41-(unsignedFormat(format)?0:20));
}
quint32 ibm(float value){if(value==0)return 0;const quint32 sign=value<0?0x80000000u:0;double magnitude=std::abs(double(value));int exponent=64;while(magnitude>=1){magnitude/=16;++exponent;}while(magnitude<1.0/16){magnitude*=16;--exponent;}return sign|(quint32(exponent)<<24)|quint32(magnitude*16777216.0);}
struct Fixture {
    int rows=5,columns=7,samples=16,format=5;
    bool little=false,crosslineMajor=false,missing=false;
    // Reverse the physical order only. Returned IL/XL axes must still be ascending.
    bool reverseInline=false,reverseCrossline=false;
};
void fixture(const QString& path,const Fixture& spec){
    QFile file(path);check(file.open(QIODevice::WriteOnly|QIODevice::NewOnly),"new generated file");
    write(file,QByteArray(3200,' '));QByteArray binary(400,'\0');
    put(binary,16,quint16(2000),spec.little);put(binary,20,quint16(spec.samples),spec.little);put(binary,24,quint16(spec.format),spec.little);
    put(binary,300,quint16(0x100),spec.little);put(binary,302,quint16(1),spec.little);write(file,binary);
    for(int n=0;n<spec.rows*spec.columns;++n){
        const int physicalInline=spec.crosslineMajor?n%spec.rows:n/spec.columns;
        const int physicalCrossline=spec.crosslineMajor?n/spec.rows:n%spec.columns;
        const int il=spec.reverseInline?spec.rows-1-physicalInline:physicalInline;
        const int xl=spec.reverseCrossline?spec.columns-1-physicalCrossline:physicalCrossline;
        if(spec.missing&&il==2&&xl==3)continue;
        QByteArray header(240,'\0');put(header,0,qint32(n+1),spec.little);put(header,4,qint32(n+1),spec.little);
        put(header,70,qint16(-10),spec.little);put(header,88,qint16(1),spec.little);put(header,108,qint16(100),spec.little);
        put(header,114,quint16(spec.samples),spec.little);put(header,116,quint16(2000),spec.little);
        put(header,188,qint32(100+il*2),spec.little);put(header,192,qint32(200+xl*3),spec.little);
        put(header,180,qint32(10000+xl*250),spec.little);put(header,184,qint32(20000+il*500),spec.little);write(file,header);
        QByteArray samples(spec.samples*width(spec.format),'\0');
        for(int sample=0;sample<spec.samples;++sample){
            const int offset=sample*width(spec.format);const float value=expected(il,xl,sample,spec.format);
            switch(spec.format){
            case 1:put(samples,offset,ibm(value),spec.little);break;
            case 2:put(samples,offset,qint32(value),spec.little);break;
            case 3:put(samples,offset,qint16(value),spec.little);break;
            case 5:{quint32 bits=std::bit_cast<quint32>(value);put(samples,offset,bits,spec.little);break;}
            case 6:{quint64 bits=std::bit_cast<quint64>(double(value));put(samples,offset,bits,spec.little);break;}
            case 8:samples[offset]=char(qint8(value));break;
            case 9:put(samples,offset,qint64(value),spec.little);break;
            case 10:put(samples,offset,quint32(value),spec.little);break;
            case 11:put(samples,offset,quint16(value),spec.little);break;
            case 12:put(samples,offset,quint64(value),spec.little);break;
            case 16:samples[offset]=char(quint8(value));break;
            }
        }write(file,samples);
    }
}
DataEntry findVolume(DataService& service,const QString& path,std::string& error){
    std::vector<DataEntry> entries;check(service.listData("volumes",0,entries,error),error);
    for(auto it=entries.rbegin();it!=entries.rend();++it){SeismicVolume volume;check(service.readVolume(it->id,volume,error),error);if(volume.dataFile==utf8(path))return *it;}
    throw std::runtime_error("generated volume missing");
}
DataEntry importVolume(DataService& service,const QString& path,std::string& error){ImportOptions options;options.copySources=false;ImportReport report;check(service.importFile(utf8(path),options,report,error)&&report.failed==0,error);return findVolume(service,path,error);}
bool equalNumber(float a,float b){return (std::isnan(a)&&std::isnan(b))||std::bit_cast<quint32>(a)==std::bit_cast<quint32>(b);}
bool equalNumber(double a,double b){return (std::isnan(a)&&std::isnan(b))||a==b;}
void compare(const SeismicTimeSliceResult& a,const SeismicTimeSliceResult& b){
    check(a.inlineNumbers==b.inlineNumbers&&a.crosslineNumbers==b.crosslineNumbers&&a.actualTime==b.actualTime&&a.timeUnit==b.timeUnit,"axes identical");
    check(a.amplitudes.size()==b.amplitudes.size()&&a.x.size()==b.x.size()&&a.y.size()==b.y.size(),"array sizes identical");
    for(size_t i=0;i<a.amplitudes.size();++i)check(equalNumber(a.amplitudes[i],b.amplitudes[i]),"every amplitude bit matches legacy (or both NaN)");
    for(size_t i=0;i<a.x.size();++i)check(equalNumber(a.x[i],b.x[i])&&equalNumber(a.y[i],b.y[i]),"every actual coordinate matches legacy");
}
TimeSliceReadOptions options(){TimeSliceReadOptions value;value.fullResolution=true;value.maxValues=100000;value.tileSize=32;value.adjacentSamples=1;value.useCache=false;value.collectTimings=true;return value;}
std::vector<std::string> modes(){
#ifdef _WIN32
    return {"buffered","mapped"};
#else
    return {"buffered"};
#endif
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);QCoreApplication::setApplicationName("timeslice-batch-tests");
    try{
        QTemporaryDir temp;check(temp.isValid(),"temporary directory");DataService service;std::string error;
        check(service.createProject(utf8(temp.path()+"/project"),"Synthetic time slice tests",error),error);
        // Numeric conversion must match segyio for every supported fixed-length format and both byte orders.
        for(int format:{1,2,3,5,6,8,9,10,11,12,16})for(bool little:{false,true}){
            Fixture spec;spec.format=format;spec.little=little;spec.crosslineMajor=little;const auto path=temp.path()+QString("/f%1-%2.sgy").arg(format).arg(little);
            fixture(path,spec);const auto entry=importVolume(service,path,error);auto opt=options();opt.ioMode="legacy";opt.workerCount=1;SeismicTimeSliceResult baseline;
            check(service.readSeismicTimeSlice(entry.id,108,opt,baseline,error),error);
            for(int il=0;il<spec.rows;++il)for(int xl=0;xl<spec.columns;++xl)check(equalNumber(baseline.amplitudes[size_t(il)*spec.columns+xl],expected(il,xl,4,format)),"source format expected amplitude");
            for(const auto& mode:modes()){opt.ioMode=mode;SeismicTimeSliceResult value;check(service.readSeismicTimeSlice(entry.id,108,opt,value,error),error);compare(baseline,value);}
        }
        // A storage-friendly tile planner must work when either physical axis runs
        // backwards, and when a volume has only one IL or only one XL. The long
        // single-axis fixtures also cross the 32*32-cell tile budget boundary.
        std::vector<Fixture> orderedFixtures;
        for(bool crosslineMajor:{false,true})for(bool reverseFast:{false,true}){
            Fixture ordered;ordered.rows=37;ordered.columns=65;ordered.crosslineMajor=crosslineMajor;
            ordered.reverseInline=crosslineMajor?reverseFast:!reverseFast;
            ordered.reverseCrossline=crosslineMajor?!reverseFast:reverseFast;
            orderedFixtures.push_back(ordered);
        }
        Fixture singleInline;singleInline.rows=1;singleInline.columns=2053;singleInline.reverseCrossline=true;
        orderedFixtures.push_back(singleInline);
        Fixture singleCrossline;singleCrossline.rows=2053;singleCrossline.columns=1;
        singleCrossline.crosslineMajor=true;singleCrossline.reverseInline=true;orderedFixtures.push_back(singleCrossline);
        Fixture singleCell;singleCell.rows=1;singleCell.columns=1;orderedFixtures.push_back(singleCell);
        for(size_t fixtureIndex=0;fixtureIndex<orderedFixtures.size();++fixtureIndex){
            const auto& ordered=orderedFixtures[fixtureIndex];
            const auto orderedPath=temp.path()+QString("/physical-order-%1.sgy").arg(fixtureIndex);
            fixture(orderedPath,ordered);const auto orderedEntry=importVolume(service,orderedPath,error);
            auto orderedOptions=options();orderedOptions.ioMode="legacy";orderedOptions.workerCount=1;
            SeismicTimeSliceResult reference;check(service.readSeismicTimeSlice(orderedEntry.id,108,orderedOptions,reference,error),error);
            check(reference.inlineNumbers.size()==size_t(ordered.rows)&&reference.crosslineNumbers.size()==size_t(ordered.columns),"physical-order fixture axis sizes");
            for(int row=0;row<ordered.rows;++row)check(reference.inlineNumbers[size_t(row)]==100+row*2,"IL axis remains ascending regardless of physical direction");
            for(int column=0;column<ordered.columns;++column)check(reference.crosslineNumbers[size_t(column)]==200+column*3,"XL axis remains ascending regardless of physical direction");
            for(int row=0;row<ordered.rows;++row)for(int column=0;column<ordered.columns;++column){
                const size_t cell=size_t(row)*ordered.columns+column;
                check(equalNumber(reference.amplitudes[cell],expected(row,column,4)),"physical-order fixture matches independent expected amplitude");
                check(reference.x[cell]==1000.0+column*25&&reference.y[cell]==2000.0+row*50,"physical-order fixture matches independently known coordinates");
            }
            for(const auto& mode:modes())for(int workers:{1,2,4}){
                orderedOptions.ioMode=mode;orderedOptions.workerCount=workers;SeismicTimeSliceResult value;
                std::vector<float> tiled(reference.amplitudes.size(),float(Missing));std::vector<int> visits(tiled.size());
                int callbackCount=0;const auto coordinator=std::this_thread::get_id();
                check(service.readSeismicTimeSlice(orderedEntry.id,108,orderedOptions,value,error,{},[&](const SeismicTimeSliceTile& tile){
                    check(std::this_thread::get_id()==coordinator,"physical-order tile callback stays on coordinator");++callbackCount;
                    check(tile.totalInlineCount==ordered.rows&&tile.totalCrosslineCount==ordered.columns,"physical-order tile overall dimensions");
                    check(tile.inlineCount>0&&tile.crosslineCount>0&&tile.amplitudes.size()==size_t(tile.inlineCount)*tile.crosslineCount,"physical-order tile rectangular dimensions");
                    check(tile.inlineOffset>=0&&tile.crosslineOffset>=0&&tile.inlineOffset+tile.inlineCount<=ordered.rows&&tile.crosslineOffset+tile.crosslineCount<=ordered.columns,"physical-order tile bounds");
                    for(int row=0;row<tile.inlineCount;++row)for(int column=0;column<tile.crosslineCount;++column){
                        const size_t destination=size_t(tile.inlineOffset+row)*ordered.columns+tile.crosslineOffset+column;
                        tiled[destination]=tile.amplitudes[size_t(row)*tile.crosslineCount+column];++visits[destination];
                    }
                }),error);
                compare(reference,value);check(callbackCount>0&&value.statistics.completedTiles==callbackCount,"physical-order tile completion accounting");
                for(size_t cell=0;cell<visits.size();++cell)check(visits[cell]==1&&equalNumber(tiled[cell],reference.amplitudes[cell]),"physical-order rectangular tiles visit every cell exactly once");
            }
        }
        Fixture spec;spec.rows=70;spec.columns=73;spec.crosslineMajor=true;spec.missing=true;
        const auto path=temp.path()+"/crossline-major-missing.sgy";fixture(path,spec);auto entry=importVolume(service,path,error);
        auto opt=options();opt.ioMode="legacy";opt.workerCount=1;SeismicTimeSliceResult baseline;
        check(service.readSeismicTimeSlice(entry.id,108,opt,baseline,error),error);
        check(std::isnan(baseline.amplitudes[2*73+3]),"missing source cell remains NaN");
        auto fileOptions=options();fileOptions.workerCount=2;SeismicTimeSliceResult fileResult;int fileCallbacks=0;
        check(DataService::readSeismicFileTimeSlice(utf8(path),108,fileOptions,fileResult,error,{},[&](const SeismicTimeSliceTile&){++fileCallbacks;}),error);
        compare(baseline,fileResult);check(fileCallbacks>1&&fileResult.statistics.completedTiles==fileCallbacks,"detached file forwards tile callbacks");
        const auto caller=std::this_thread::get_id();
        for(const auto& mode:modes())for(int workers:{1,2,4}){
            opt.ioMode=mode;opt.workerCount=workers;SeismicTimeSliceResult result;
            std::vector<float> tiled(baseline.amplitudes.size(),float(Missing));std::vector<int> visits(tiled.size());int callbackCount=0;
            check(service.readSeismicTimeSlice(entry.id,108,opt,result,error,[&]{check(std::this_thread::get_id()==caller,"cancel callback uses coordinator");return false;},
                [&](const SeismicTimeSliceTile& tile){
                    check(std::this_thread::get_id()==caller,"tile callback uses coordinator");++callbackCount;
                    check(tile.totalInlineCount==70&&tile.totalCrosslineCount==73,"tile overall dimensions");
                    check(tile.amplitudes.size()==size_t(tile.inlineCount)*tile.crosslineCount,"tile dimensions");
                    for(int i=0;i<tile.inlineCount;++i)for(int j=0;j<tile.crosslineCount;++j){
                        const size_t to=size_t(tile.inlineOffset+i)*73+tile.crosslineOffset+j;
                        check(to<tiled.size(),"tile within matrix");tiled[to]=tile.amplitudes[size_t(i)*tile.crosslineCount+j];++visits[to];}
                }),error);
            compare(baseline,result);check(callbackCount>1&&result.statistics.completedTiles==callbackCount,"progress reflects complete tiles");
            for(size_t i=0;i<tiled.size();++i)check(visits[i]==1&&equalNumber(tiled[i],baseline.amplitudes[i]),"tiles assemble exactly with no duplicates or holes");
            check(result.statistics.tracesRead>0&&result.statistics.positionMs>=0&&result.statistics.readMs>=0&&result.statistics.validationMs>=0&&result.statistics.conversionMs>=0,"diagnostics are populated and nonnegative");
        }
        // Range boundaries are actual line numbers and inclusive, including non-axis values between keys.
        opt=options();opt.inlineMin=103;opt.inlineMax=112;opt.crosslineMin=206;opt.crosslineMax=218;
        SeismicTimeSliceResult roi;check(service.readSeismicTimeSlice(entry.id,108,opt,roi,error),error);
        check(roi.inlineNumbers==std::vector<int>({104,106,108,110,112})&&roi.crosslineNumbers==std::vector<int>({206,209,212,215,218}),"inclusive ROI on step2/step3 axes");
        check(!roi.decimated&&roi.nativeInlineCount==5&&roi.nativeCrosslineCount==5,"range crop is not decimation");
        for(size_t i=0;i<roi.inlineNumbers.size();++i)for(size_t j=0;j<roi.crosslineNumbers.size();++j){const int il=(roi.inlineNumbers[i]-100)/2,xl=(roi.crosslineNumbers[j]-200)/3;
            check(equalNumber(roi.amplitudes[i*roi.crosslineNumbers.size()+j],baseline.amplitudes[size_t(il)*73+xl]),"ROI is exact subset of full plane");}
        SeismicTimeSliceResult strict;
        check(!service.readTimeSlice(entry.id,108,opt,strict,error),"strict XY slice still requires declared conventions");
        SeismicVolume volume;check(service.readVolume(entry.id,volume,error),error);volume.geometry.coordinateSystem="SYNTHETIC_GRID";volume.geometry.coordinateUnit="m";
        volume.sampling={16,100,2,"ms","TWT","generated time origin"};check(service.saveVolume(entry.id,volume,error),error);
        check(service.readTimeSlice(entry.id,108,opt,strict,error),error);compare(roi,strict);
        opt.maxValues=24;check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error)&&roi.amplitudes.empty(),"ROI budget enforced before output");
        opt=options();opt.inlineMin=5000;opt.inlineMax=6000;check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error)&&roi.amplitudes.empty(),"outside ROI rejected");
        opt=options();opt.inlineMin=110;opt.inlineMax=100;check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error),"reversed ROI rejected");
        for(const auto& mode:modes()){
            opt=options();opt.ioMode=mode;opt.workerCount=4;bool cancel=false;int tiles=0;
            check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error,[&]{return cancel;},[&](const SeismicTimeSliceTile&){++tiles;cancel=true;})&&roi.amplitudes.empty()&&tiles==1,"cancel after first tile gives no final partial result");
        }
        opt=options();opt.ioMode="not-an-io-mode";check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error),"bad mode rejected");
        opt=options();opt.workerCount=3;check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error),"unsupported worker count rejected");
        opt=options();opt.adjacentSamples=0;check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error),"empty time band rejected");
        opt=options();opt.tileSize=16;check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error),"unsupported tile size rejected");
        // Cache contains source floats, so neighbouring sample values must remain different and exact.
        DataService::clearSeismicSliceCache();opt=options();opt.ioMode="buffered";opt.useCache=true;opt.cacheBudgetMiB=16;opt.adjacentSamples=4;
        check(service.readSeismicTimeSlice(entry.id,108,opt,roi,error)&&roi.statistics.cacheMisses>0,error);compare(baseline,roi);
        check(service.readSeismicTimeSlice(entry.id,108,opt,roi,error)&&roi.statistics.cacheHits>0&&roi.statistics.tracesRead==0,"same-time cache avoids source payload reads");compare(baseline,roi);
        check(service.readSeismicTimeSlice(entry.id,110,opt,roi,error)&&roi.statistics.cacheHits>0&&roi.statistics.tracesRead==0,"adjacent-time cache avoids source payload reads");
        auto uncached=opt;uncached.useCache=false;uncached.ioMode="legacy";SeismicTimeSliceResult adjacent;
        check(service.readSeismicTimeSlice(entry.id,110,uncached,adjacent,error),error);compare(adjacent,roi);
        auto noBudget=opt;noBudget.cacheBudgetMiB=0;
        check(service.readSeismicTimeSlice(entry.id,110,noBudget,roi,error)&&roi.statistics.cacheHits==0&&roi.statistics.tracesRead>0,"zero cache budget bypasses cache");compare(adjacent,roi);
        service.closeProject();check(service.openProject(utf8(temp.path()+"/project"),error),error);
        check(service.readSeismicTimeSlice(entry.id,110,opt,roi,error),error);compare(adjacent,roi);
        DataService::clearSeismicSliceCache();check(service.readSeismicTimeSlice(entry.id,108,opt,roi,error)&&roi.statistics.cacheHits==0&&roi.statistics.cacheMisses>0,"explicit cache clear");
        // Change only this generated source. Cached values cannot bypass source verification.
        QFile changed(path);check(changed.open(QIODevice::ReadWrite),"open generated source mutation");
        check(changed.seek(3600+240+4*4),"seek generated first trace sample");QByteArray bytes(4,'\0');put(bytes,0,std::bit_cast<quint32>(123.0f),false);write(changed,bytes);
        check(changed.flush(),"flush generated mutation");check(changed.setFileTime(QDateTime::currentDateTimeUtc().addSecs(5),QFileDevice::FileModificationTime),"change generated source time");changed.close();
        check(!service.readSeismicTimeSlice(entry.id,108,opt,roi,error)&&roi.amplitudes.empty(),"changed source rejects stale cache");
        ImportReport report;check(service.reimportSource(entry.sourceId,{},report,error)&&report.failed==0,error);entry=findVolume(service,path,error);
        check(service.readSeismicTimeSlice(entry.id,108,opt,roi,error),error);check(roi.amplitudes.front()==123&&roi.statistics.cacheMisses>0,"reimport uses changed source identity, not old cache");
        DataService::clearSeismicSliceCache();
        std::cout<<"Time-slice batch tests passed: formats/endians, ROI, 1/2/4 workers, exact tiles, cancellation, cache and source invalidation\n";
        return 0;
    }catch(const std::exception& exception){std::cerr<<exception.what()<<'\n';return 1;}
}
