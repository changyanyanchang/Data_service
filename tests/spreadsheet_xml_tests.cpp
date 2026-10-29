// Repository fixtures are generated synthetic examples; no private source data is bundled.
#include "paleo_data/parser.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QtEndian>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace paleo::data { ParsedFile parseSpreadsheetXmlFile(const std::string&,const ParseOptions&,const CancelCheck&); }
using namespace paleo::data;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string str(const QString& v){return v.toUtf8().toStdString();}
QString cell(const QString& value){return "<s:Cell><s:Data s:Type=\"String\">"+value.toHtmlEscaped()+"</s:Data></s:Cell>";}
QString row(std::initializer_list<const char*> values){QString text="<s:Row>";for(auto* v:values)text+=cell(QString::fromUtf8(v));return text+"</s:Row>";}
QString sheet(const QString& name,const QString& rows){return "<s:Worksheet s:Name=\""+name+"\"><s:Table>"+rows+"</s:Table></s:Worksheet>";}
QString book(const QString& sheets,const QString& encoding="UTF-8"){return "<?xml version=\"1.0\" encoding=\""+encoding+"\"?><s:Workbook xmlns:s=\"urn:schemas-microsoft-com:office:spreadsheet\">"+sheets+"</s:Workbook>";}
std::string write(const QTemporaryDir& temp,const QString& name,const QByteArray& bytes){QFile file(temp.filePath(name));check(file.open(QIODevice::WriteOnly),"write fixture");check(file.write(bytes)==bytes.size(),"write complete");return str(file.fileName());}
bool has(const ParsedFile& p,const char* code){for(const auto& i:p.issues)if(i.code==code)return true;return false;}
ParsedFile parse(const std::string& path,const ParseOptions& options={},const CancelCheck& cancelled={}){return parseSpreadsheetXmlFile(path,options,cancelled);}
QString digest(const std::vector<double>& values){QCryptographicHash hash(QCryptographicHash::Sha256);for(double value:values){quint64 bits=0;std::memcpy(&bits,&value,8);if(!std::isfinite(value))bits=Q_UINT64_C(0x7ff8000000000000);const quint64 little=qToLittleEndian(bits);hash.addData(QByteArrayView(reinterpret_cast<const char*>(&little),8));}return QString::fromLatin1(hash.result().toHex());}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temp;check(temp.isValid(),"temporary directory");
        const QString header=row({"井号","深度","TVD","TVDSS","GR","SP","CNL"});
        const QString logs=sheet("测井曲线-HZ1-1-1井",header+row({"HZ1-1-1","100","99","-20","0","-30","-0.1"})+"<s:Row s:Index=\"4\">"+cell("HZ1-1-1")+cell("100.125")+"<s:Cell s:Index=\"5\"><s:Data s:Type=\"Number\">-9999</s:Data></s:Cell>"+cell("-31")+cell("bad")+"</s:Row>");
        const QString intervals=sheet("文本道",row({"井号","道名","层号","顶深","顶TVD","顶TVDSS","底深","底TVD","底TVDSS","文本"})+row({"HZ1-1-1","微相","","100","99","-20","103","102","-17","分流河道"}));
        const QString formations=sheet("地层单位道",row({"井号","道名","层号","顶深","底深","顶部不整合","底部不整合","底部未钻穿"})+row({"HZ1-1-1","组","文昌组","0","110","false","true","true"}));
        const QString cores=sheet("取心数据道",row({"井号","道名","顶深","底深","筒次","心长","进尺","收获率"})+row({"HZ1-1-1","","100","103","2","2.5","3","83.33%"}));
        const QString photos=sheet("图片道",row({"井号","道名","顶深","底深","图片名","图片目录"})+row({"HZ1-1-1","岩心","100","103","photo.png","资源"}));
        auto path=write(temp,"HZ1-1-1.xml",book(logs+intervals+formations+cores+photos).toUtf8());
        auto p=parse(path);check(p.status!="failed"&&p.wells.size()==1,"UTF8 workbook and well");check(p.sourceTables.size()==5,"all worksheets accounted");
        const auto& w=p.wells[0];const auto& set=w.wellbore.logSets[0];check(set.logCurves.size()==3&&set.samplingAxis.depthValues.size()==2,"log array shape");
        check(set.logCurves[0].sampleValues[0]==0&&set.logCurves[0].sampleValues[1]==-9999,"zero and raw sentinel preserved");
        check(set.logCurves[1].sampleValues[0]==-30&&set.logCurves[2].sampleValues[0]==-0.1,"valid signed values preserved");
        check(std::isnan(set.samplingAxis.tvdValues[1])&&std::isnan(set.logCurves[2].sampleValues[1]),"sparse cells and invalid numeric");
        check(set.samplingAxis.tvdssValues[0]==-20&&set.samplingAxis.sampleStep==0.125,"signed TVDSS and regular step");
        check(p.sourceTables[0].rowCount==4&&p.sourceTables[0].columnCount==7,"physical sparse row/column");
        check(has(p,"NON_NUMERIC_CURVE")&&has(p,"DEPTH_UNIT_UNKNOWN"),"data review issues");check(p.declaredNullValues.empty(),"no invented NULL declaration");
        check(set.depthInfo.depthUnit.empty()&&set.depthInfo.depthType.empty()&&set.logCurves[0].curveUnit.empty(),"unknown conventions stay empty");
        ParseOptions confirmed;confirmed.comprehensiveWellMetreDepth=true;
        const auto declared=parse(path,confirmed);
        check(declared.wells[0].wellbore.logSets[0].depthInfo.depthUnit=="m"&&declared.wells[0].wellbore.logSets[0].depthInfo.depthType=="MD","explicit host comprehensive-export contract");
        check(w.wellbore.intervalTracks.size()==3&&w.wellbore.intervalTracks[0].trackType=="沉积相","text facies maps to interval model");
        check(w.wellbore.intervalTracks[0].intervals[0].intervalName=="分流河道"&&w.wellbore.intervalTracks[0].sourceSheet=="文本道","Chinese value and provenance");
        check(w.wellbore.intervalTracks[1].intervals[0].bottomNotPenetrated==1&&w.wellbore.intervalTracks[1].intervals[0].topUnconformity==0,"formation flags");
        check(w.wellbore.intervalTracks[2].intervals[0].properties.back().originalText=="83.33%","core measurements preserve original percent");
        check(w.wellbore.corePhotoTracks.size()==1&&w.wellbore.corePhotoTracks[0].photos.size()==1&&has(p,"PHOTO_FILE_MISSING"),"photo array with missing file visible");
        QDir().mkpath(temp.filePath("original/资源"));write(temp,"original/资源/photo.png",QByteArray("photo placeholder"));
        ParseOptions sourceContext;sourceContext.sourceContextPath=str(temp.filePath("original/HZ1-1-1.xml"));
        p=parse(path,sourceContext);check(!has(p,"PHOTO_FILE_MISSING")&&p.wells[0].wellbore.corePhotoTracks[0].photos[0].imagePath==str(temp.filePath("original/资源/photo.png")),"reimport resolves photos against original source context");
        const auto conflict=write(temp,"HZ9-9-9.xml",book(logs).toUtf8());p=parse(conflict);check(p.status=="review"&&p.wells.empty()&&has(p,"WELL_NAME_CONFLICT"),"conflicting names not silently merged");
        ParseOptions options;options.wellNameOverride="HZ9-9-9";options.depthUnit="m";p=parse(conflict,options);check(p.wells.size()==1&&p.wells[0].wellInfo.wellName=="HZ9-9-9","explicit correction applies");
        auto bad=write(temp,"truncated.xml",book(logs).toUtf8().chopped(8));p=parse(bad);check(p.status=="failed"&&p.wells.empty(),"malformed file no partial business data");
        bad=write(temp,"bad-index.xml",book(sheet("logs",header+"<s:Row>"+cell("HZ1-1-1")+"<s:Cell s:Index=\"0\"/></s:Row>")).toUtf8());check(parse(bad).status=="failed","invalid sparse index rejected");
        p=parse(path,{},[]{return true;});check(p.status=="failed"&&p.wells.empty(),"early cancellation");int calls=0;p=parse(path,{},[&]{return ++calls>5;});check(p.status=="failed"&&p.wells.empty(),"mid-file cancellation rollback");
        const auto empty=write(temp,"empty-sheet.xml",book(sheet("测井曲线-HZ1-1-1井",header)).toUtf8());p=parse(empty);check(p.wells.empty()&&p.sourceTables.size()==1,"header-only sheet does not invent curves");
        QString manyRows=header;for(int i=0;i<40;++i)manyRows+=row({"HZ1-1-1","100","99","-20","0","-30","-0.1"});
        const auto preview=write(temp,"HZ1-1-1-preview.xml",book(sheet("测井曲线-HZ1-1-1井",manyRows)).toUtf8());p=parse(preview);
        check(p.wells[0].wellbore.logSets[0].samplingAxis.depthValues.size()==40&&p.wells[0].wellbore.logSets[0].sourceSheet=="测井曲线-HZ1-1-1井","preview limit does not truncate samples or alter source sheet");
        check(p.sourceTables[0].rowCount==33&&p.sourceTables[0].sheetName.find("前33行预览")!=std::string::npos,"source preview has explicit label and bounded pagination");
        for(const auto& c:p.sourceTables[0].cells)check(c.row<=33,"preview preserves only declared physical rows");
#ifdef _WIN32
        // 让一个汉字正好跨过 64 KiB 边界，验证 GBK 流式解码没有乱码或丢字。
        const QString large=book(logs+intervals,"gb2312");
        const int count=WideCharToMultiByte(936,0,reinterpret_cast<LPCWCH>(large.utf16()),int(large.size()),nullptr,0,nullptr,nullptr);QByteArray gbk(count,Qt::Uninitialized);WideCharToMultiByte(936,0,reinterpret_cast<LPCWCH>(large.utf16()),int(large.size()),gbk.data(),count,nullptr,nullptr);
        int firstChinese=0;while(firstChinese<gbk.size()&&static_cast<unsigned char>(gbk[firstChinese])<128)++firstChinese;
        const int padding=65535-firstChinese;check(padding>7,"GBK boundary fixture");gbk.insert(gbk.indexOf("<s:Workbook"),QByteArray("<!--")+QByteArray(padding-7,'x')+QByteArray("-->"));
        path=write(temp,"HZ1-1-1-gbk.xml",gbk);p=parse(path);check(p.wells.size()==1&&p.wells[0].wellbore.intervalTracks[0].intervals[0].intervalName=="分流河道","GB2312 Chinese decoding");
#endif
        std::cout<<"SpreadsheetML synthetic checks passed\n";
        if(argc>1){
            QDir directory(app.arguments()[1]);if(directory.exists("单井数据"))directory.cd("单井数据");
            long long logRows=0,continuousRows=0,discreteRows=0,layerRows=0,photoRows=0;int sheets=0,wells=0,curves=0,files=0;QJsonArray actual;
            for(const auto& name:directory.entryList({"*.xml"},QDir::Files,QDir::Name)){
                auto result=parse(str(directory.filePath(name)));if(result.status=="failed"||result.wells.empty())std::cerr<<str(name)<<": "<<result.message<<'\n';check(result.status!="failed"&&result.wells.size()==1,"real XML parsed without name conflict");
                ++files;wells+=int(result.wells.size());sheets+=int(result.sourceTables.size());QJsonObject fileResult{{"file",name}};QJsonArray objects;
                for(const auto& well:result.wells){
                    for(const auto& log:well.wellbore.logSets){const auto n=log.samplingAxis.depthValues.size();logRows+=n;curves+=int(log.logCurves.size());if(QString::fromUtf8(log.sourceSheet).startsWith("离散"))discreteRows+=n;else continuousRows+=n;for(const auto& c:log.logCurves)check(c.sampleValues.size()==n,"real shared-axis curve length");QJsonObject hashes{{"2",digest(log.samplingAxis.depthValues)},{"3",digest(log.samplingAxis.tvdValues)},{"4",digest(log.samplingAxis.tvdssValues)}};for(int c=0;c<log.logCurves.size();++c)hashes[QString::number(c+5)]=digest(log.logCurves[c].sampleValues);objects.append(QJsonObject{{"sheet",QString::fromUtf8(log.sourceSheet)},{"rows",qint64(n)},{"curves",int(log.logCurves.size())},{"columnSha256",hashes}});}
                    for(const auto& track:well.wellbore.intervalTracks){layerRows+=track.intervals.size();objects.append(QJsonObject{{"sheet",QString::fromUtf8(track.sourceSheet)},{"track",QString::fromUtf8(track.trackName)},{"rows",qint64(track.intervals.size())}});}
                    for(const auto& track:well.wellbore.corePhotoTracks){photoRows+=track.photos.size();objects.append(QJsonObject{{"sheet","图片道"},{"rows",qint64(track.photos.size())}});}
                }
                fileResult["objects"]=objects;fileResult["message"]=QString::fromUtf8(result.message);actual.append(fileResult);
            }
            std::cout<<"files="<<files<<" wells="<<wells<<" sheets="<<sheets<<" curves="<<curves<<" continuous_rows="<<continuousRows<<" discrete_rows="<<discreteRows<<" interval_rows="<<layerRows<<" photos="<<photoRows<<'\n';
            check(logRows==continuousRows+discreteRows,"external curve row accounting is internally consistent");
            if(argc>2){QFile report(app.arguments()[2]);check(report.open(QIODevice::WriteOnly),"write real acceptance report");report.write(QJsonDocument(actual).toJson());}
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
