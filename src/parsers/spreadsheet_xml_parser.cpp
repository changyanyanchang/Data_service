// Excel 2003 SpreadsheetML：逐行解析一个文件；不把 XML DOM 或整个工区装入内存。
// 公共结果仍使用 LogSet / IntervalTrack / CorePhotoTrack，不另建一套数据模型。
#include "paleo_data/parser.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>
#include <QStringConverter>
#include <QRegularExpression>
#include <QLocale>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace paleo::data {
namespace {
const QString spreadsheetNs=QStringLiteral("urn:schemas-microsoft-com:office:spreadsheet");
std::string s(const QString& v){return v.toUtf8().toStdString();}
QString q(const std::string& v){return QString::fromUtf8(v);}

// QXmlStreamReader 不保证支持 Windows 的 GB2312/GBK。先把 64 KiB 块转成 UTF-8。
// UTF 编码使用 Qt 的有状态解码器；Windows CP936 保留跨块的半个汉字。
class XmlUtf8Input : public QIODevice {
public:
    explicit XmlUtf8Input(QFile& file):file_(file){
        const QByteArray head=file.peek(512);QString encoding="UTF-8";
        if(head.startsWith("\xff\xfe")||head.startsWith("\xfe\xff"))encoding="UTF-16";
        else {
            const auto m=QRegularExpression("encoding\\s*=\\s*['\"]([^'\"]+)['\"]",QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(head));
            if(m.hasMatch())encoding=m.captured(1);
        }
        const QString normalized=encoding.toUpper().remove('-');
        legacy_=normalized=="GB2312"||normalized=="GBK"||normalized=="CP936"||normalized=="WINDOWS936";
        decoder_=std::make_unique<QStringDecoder>(encoding.toLatin1().constData());
        if(!decoder_->isValid()&&!legacy_)error_=QStringLiteral("XML 编码不受支持：%1").arg(encoding);
#ifndef _WIN32
        if(legacy_&&!decoder_->isValid())error_="当前 Qt 未提供 GB2312/GBK 编码支持；请提供 UTF-8 XML";
#endif
        open(QIODevice::ReadOnly);
    }
    bool isSequential() const override{return true;}
    QString error()const{return error_;}
protected:
    qint64 writeData(const char*,qint64)override{return -1;}
    qint64 readData(char* destination,qint64 size)override{
        if(!error_.isEmpty())return -1;
        if(buffer_.isEmpty()){
            QByteArray bytes=tail_+file_.read(65536);tail_.clear();
            if(file_.error()!=QFileDevice::NoError){error_=file_.errorString();return -1;}
            if(bytes.isEmpty())return 0;
            QString decoded;
#ifdef _WIN32
            if(legacy_){
                int i=0;while(i<bytes.size())i+=IsDBCSLeadByteEx(936,static_cast<BYTE>(bytes[i]))?2:1;
                if(i>bytes.size()){
                    if(file_.atEnd()){error_="GB2312/GBK 文件末尾有不完整字符";return -1;}
                    tail_=bytes.right(1);bytes.chop(1);
                }
                const int count=MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,bytes.constData(),int(bytes.size()),nullptr,0);
                if(count<=0){error_="GB2312/GBK XML 中有无法解码的字节";return -1;}
                decoded.resize(count);MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,bytes.constData(),int(bytes.size()),reinterpret_cast<wchar_t*>(decoded.data()),count);
            }else
#endif
            {
                decoded=(*decoder_)(bytes);
                if(decoder_->hasError()){error_="XML 文本编码与声明不一致";return -1;}
            }
            if(first_){
                first_=false;
                decoded.replace(QRegularExpression("(<\\?xml[^?]*encoding\\s*=\\s*['\"])[^'\"]+(['\"])",QRegularExpression::CaseInsensitiveOption),"\\1UTF-8\\2");
            }
            buffer_=decoded.toUtf8();
        }
        const qint64 copied=std::min<qint64>(size,buffer_.size());
        std::memcpy(destination,buffer_.constData(),size_t(copied));buffer_.remove(0,copied);return copied;
    }
private:
    QFile& file_;QByteArray buffer_,tail_;QString error_;std::unique_ptr<QStringDecoder> decoder_;bool legacy_=false,first_=true;
};

void issue(ParsedFile& out,const char* code,const QString& message,const QString& sheet={},int row=0,bool blocking=false,const QString& original={}){
    if(out.issues.size()<250)out.issues.push_back({code,s(message),s(sheet),s(original),row,blocking});
    else if(out.issues.size()==250)out.issues.push_back({"MORE_ISSUES","其余问题请核对原始文件；未删减业务数据",s(sheet),"",row,false});
}
ParsedFile fail(ParsedFile out,const QString& message){out.wells.clear();out.status="failed";out.message=s(message);issue(out,"XML_PARSE_ERROR",message,{},0,true);return out;}
QString attr(const QXmlStreamAttributes& attrs,const char* name){return attrs.value(spreadsheetNs,QLatin1String(name)).toString();}
QString cell(const QStringList& cells,int index){return index>=0&&index<cells.size()?cells[index]:QString();}
bool empty(const QStringList& cells){for(const auto& c:cells)if(!c.trimmed().isEmpty())return false;return true;}
QString headerName(QString value){value=value.trimmed().toUpper();value.remove(QRegularExpression("\\s+"));return value;}
int field(const QStringList& header,std::initializer_list<const char*> names){
    for(int i=0;i<header.size();++i)for(const char* name:names)if(headerName(header[i])==QString::fromUtf8(name))return i;return -1;
}
QString wellToken(const QString& value){
    static const QRegularExpression pattern("[A-Z]{1,8}[0-9]{1,4}(?:-[0-9]{1,4}[A-Z0-9]*){1,5}");
    QString v=value.toUpper();v.replace('_','-');v.replace(QChar(0x2013),'-');v.replace(QChar(0x2014),'-');return pattern.match(v).captured();
}
double number(const QString& value,bool* invalid=nullptr){
    if(invalid)*invalid=false;const QString text=value.trimmed();
    if(text.isEmpty()||text=="-"||text.compare("nan",Qt::CaseInsensitive)==0||text.compare("null",Qt::CaseInsensitive)==0)return Missing;
    bool ok=false;const double n=QLocale::c().toDouble(text,&ok);if(!ok||!std::isfinite(n)){if(invalid)*invalid=true;return Missing;}return n;
}
int flag(const QString& value){const QString t=value.trimmed().toLower();if(t=="true"||t=="1"||t==QStringLiteral("是"))return 1;if(t=="false"||t=="0"||t==QStringLiteral("否"))return 0;return -1;}
struct Sheet {
    QString name;QStringList header;SourceTable source;QString kind;int row=0,dataRows=0;
    int well=-1,depth=-1,tvd=-1,tvdss=-1,track=-1,layer=-1,top=-1,bottom=-1,topTvd=-1,bottomTvd=-1,topSs=-1,bottomSs=-1,nameCol=-1;
    int facies=-1,topUnc=-1,bottomUnc=-1,notPen=-1,image=-1,imageDir=-1;
    std::vector<int> curves;std::map<int,int> logSets;std::map<std::pair<int,QString>,int> tracks,photoTracks;
    long long badCurve=0,badDepth=0,suspectNulls=0;bool formula=false;
};
void identify(Sheet& sh){
    const auto& h=sh.header;
    sh.well=field(h,{"井号","井名","WELL","WELLNAME"});sh.depth=field(h,{"深度","DEPTH","DEPT","MD"});sh.tvd=field(h,{"TVD"});sh.tvdss=field(h,{"TVDSS"});
    sh.track=field(h,{"道名","TRACK"});sh.layer=field(h,{"层号","层名","地层","FORMATION"});
    sh.top=field(h,{"顶深","TOP","TOPDEPTH"});sh.bottom=field(h,{"底深","BOTTOM","BASE","BOTTOMDEPTH"});
    sh.topTvd=field(h,{"顶TVD"});sh.bottomTvd=field(h,{"底TVD"});sh.topSs=field(h,{"顶TVDSS"});sh.bottomSs=field(h,{"底TVDSS"});
    sh.facies=field(h,{"相类型"});sh.topUnc=field(h,{"顶部不整合"});sh.bottomUnc=field(h,{"底部不整合"});sh.notPen=field(h,{"底部未钻穿"});
    sh.image=field(h,{"图片名"});sh.imageDir=field(h,{"图片目录"});
    if(sh.depth>=0){sh.kind="logs";for(int i=0;i<h.size();++i)if(i!=sh.well&&i!=sh.depth&&i!=sh.tvd&&i!=sh.tvdss&&!h[i].isEmpty())sh.curves.push_back(i);}
    else if(sh.top>=0&&sh.bottom>=0){
        if(sh.image>=0)sh.kind="photos";
        else if((sh.nameCol=field(h,{"岩性"}))>=0)sh.kind="岩性";
        else if((sh.nameCol=field(h,{"文本"}))>=0)sh.kind="文本";
        else if((sh.nameCol=field(h,{"符号"}))>=0)sh.kind="符号";
        else if(field(h,{"筒次"})>=0){sh.kind="取心";sh.nameCol=field(h,{"筒次"});}
        else if(sh.layer>=0){sh.kind=sh.name.contains(QStringLiteral("解释"))?QStringLiteral("解释分层"):QStringLiteral("地层");sh.nameCol=sh.layer;}
    }
}
void sourceRow(Sheet& sh,const QStringList& row){
    const bool preview=sh.kind=="logs"||sh.kind.isEmpty();
    // 原文预览只声明实际保存的物理行范围，不能让分页把未保存的后续行显示为空值。
    // 完整测井/离散数值保存在 LogSet，共享深度轴及真实 sourceSheet 不受影响。
    sh.source.rowCount=preview?std::min(sh.row,33):sh.row;sh.source.columnCount=std::max(sh.source.columnCount,int(row.size()));
    if(preview&&sh.row>33)return;
    for(int c=0;c<row.size();++c)if(!row[c].isEmpty())sh.source.cells.push_back({sh.row,c+1,s(row[c])});
}
}

ParsedFile parseSpreadsheetXmlFile(const std::string& path,const ParseOptions& options,const CancelCheck& cancelled){
    ParsedFile out;out.path=path;out.kind="workbook";
    if(cancelled&&cancelled())return fail(std::move(out),"解析已取消");
    QFile file(q(path));if(!file.open(QIODevice::ReadOnly))return fail(std::move(out),file.errorString());
    XmlUtf8Input input(file);if(!input.error().isEmpty())return fail(std::move(out),input.error());
    QXmlStreamReader xml(&input);Sheet sh;QStringList row;QString value;int column=0,mergeAcross=0,dataDepth=0;
    bool inSheet=false,inTable=false,inRow=false,inCell=false,rootSeen=false,conflict=false;long long rows=0,logRows=0,intervalRows=0,photos=0;
    const QString fileWell=wellToken(QFileInfo(file).completeBaseName());const QString overrideName=q(canonicalWellName(options.wellNameOverride));
    QHash<QString,int> wells;
    auto processRow=[&](){
        if(empty(row)){sourceRow(sh,row);return;}
        if(sh.header.empty()){sh.header=row;identify(sh);sourceRow(sh,row);return;}
        ++sh.dataRows;sourceRow(sh,row);++rows;
        if(sh.kind.isEmpty())return;
        for(int c=sh.header.size();c<row.size();++c)if(!row[c].trimmed().isEmpty())issue(out,"XML_EXTRA_COLUMN","数据列超出表头；原文件保留，请补全字段映射",sh.name,sh.row,false,row[c]);
        const QString originalWell=cell(row,sh.well).trimmed();QString name=overrideName.isEmpty()?q(canonicalWellName(s(originalWell))):overrideName;
        const QString sheetWell=wellToken(sh.name);if(name.isEmpty())name=!fileWell.isEmpty()?fileWell:sheetWell;
        if(name.isEmpty()||(overrideName.isEmpty()&&((!fileWell.isEmpty()&&name!=fileWell)||(!sheetWell.isEmpty()&&name!=sheetWell)))){
            if(!conflict)issue(out,"WELL_NAME_CONFLICT","文件名、工作表或数据行的井号不一致；请指定正确归属后重新导入",sh.name,sh.row,true,originalWell+" / "+fileWell+" / "+sheetWell);
            conflict=true;return;
        }
        if(!wells.contains(name)){WellDataset w;w.wellInfo.wellName=s(name);w.defaultDepthInfo.depthUnit=options.depthUnit;w.sourceFiles.push_back(path);wells[name]=int(out.wells.size());out.wells.push_back(std::move(w));}
        const int wi=wells[name];auto& w=out.wells[wi];
        if(sh.kind=="logs"){
            if(!sh.logSets.contains(wi)){LogSet set;set.logSetName=s(sh.name);set.sourceFile=path;set.sourceSheet=s(sh.name);set.depthInfo.depthUnit=options.depthUnit;
                if(headerName(sh.header[sh.depth])=="MD")set.depthInfo.depthType="MD";
                // Confirmed comprehensive-well export contract, shared with the host:
                // exact leading headers + named curve worksheet declare MD in metres.
                // Other generic DEPTH/深度 tables retain unknown conventions.
                const bool comprehensive=options.comprehensiveWellMetreDepth&&(sh.name.startsWith(QStringLiteral("测井曲线"))||sh.name.startsWith(QStringLiteral("离散曲线")))
                    &&sh.header.size()>=4&&headerName(sh.header[0])==QStringLiteral("井号")
                    &&headerName(sh.header[1])==QStringLiteral("深度")&&headerName(sh.header[2])=="TVD"&&headerName(sh.header[3])=="TVDSS";
                if(comprehensive){set.depthInfo.depthType="MD";if(set.depthInfo.depthUnit.empty())set.depthInfo.depthUnit="m";}
                for(int c:sh.curves){LogCurve curve;curve.curveName=s(sh.header[c]);curve.description=s(sh.header[c]);set.logCurves.push_back(std::move(curve));}
                sh.logSets[wi]=int(w.wellbore.logSets.size());w.wellbore.logSets.push_back(std::move(set));}
            auto& set=w.wellbore.logSets[sh.logSets[wi]];auto& axis=set.samplingAxis;const double depth=number(cell(row,sh.depth));
            axis.depthValues.push_back(depth);if(!std::isfinite(depth))++sh.badDepth;
            if(sh.tvd>=0)axis.tvdValues.push_back(number(cell(row,sh.tvd)));if(sh.tvdss>=0)axis.tvdssValues.push_back(number(cell(row,sh.tvdss)));
            for(size_t i=0;i<sh.curves.size();++i){bool invalid=false;const double n=number(cell(row,sh.curves[i]),&invalid);set.logCurves[i].sampleValues.push_back(n);if(invalid)++sh.badCurve;if(n==-999900||n==-99925)++sh.suspectNulls;}
            ++logRows;return;
        }
        QString trackName=cell(row,sh.track).trimmed();if(trackName.isEmpty())trackName=sh.name;
        const auto key=std::make_pair(wi,trackName);
        if(sh.kind=="photos"){
            if(!sh.photoTracks.contains(key)){sh.photoTracks[key]=int(w.wellbore.corePhotoTracks.size());CorePhotoTrack t;t.trackName=s(trackName);w.wellbore.corePhotoTracks.push_back(std::move(t));}
            CorePhoto p;p.topDepth=number(cell(row,sh.top));p.bottomDepth=number(cell(row,sh.bottom));
            const QString relative=QDir(cell(row,sh.imageDir)).filePath(cell(row,sh.image));const QString resolved=QDir(QFileInfo(options.sourceContextPath.empty()?file.fileName():q(options.sourceContextPath)).absolutePath()).absoluteFilePath(relative);p.imagePath=s(resolved);
            QStringList description;description<<QStringLiteral("来源工作表 %1，第 %2 行").arg(sh.name).arg(sh.row);for(int c=0;c<sh.header.size();++c)if(!cell(row,c).isEmpty())description<<sh.header[c]+"="+cell(row,c);p.description=s(description.join("；"));
            if(cell(row,sh.image).isEmpty()||!QFileInfo(resolved).isFile())issue(out,"PHOTO_FILE_MISSING","图片道引用的文件未在原相对路径找到；保留文件名与深度，等待重新关联",sh.name,sh.row,false,relative);
            w.wellbore.corePhotoTracks[sh.photoTracks[key]].photos.push_back(std::move(p));++photos;return;
        }
        if(!sh.tracks.contains(key)){IntervalTrack t;t.trackName=s(trackName);t.trackType=s(sh.kind);if(sh.kind==QStringLiteral("文本")&&trackName.contains(QStringLiteral("相")))t.trackType="沉积相";if(sh.kind==QStringLiteral("符号")&&cell(row,sh.nameCol).contains(QStringLiteral("旋回")))t.trackType="旋回";t.sourceFile=path;t.sourceSheet=s(sh.name);t.depthInfo.depthUnit=options.depthUnit;sh.tracks[key]=int(w.wellbore.intervalTracks.size());w.wellbore.intervalTracks.push_back(std::move(t));}
        auto& track=w.wellbore.intervalTracks[sh.tracks[key]];
        // The host explicitly confirms this comprehensive export's MD/metre
        // convention; generic interval spreadsheets retain unknown conventions.
        if(options.comprehensiveWellMetreDepth && sh.header.size()>=3 &&
           headerName(sh.header[0])==QStringLiteral("井号") &&
           headerName(sh.header[1])==QStringLiteral("顶深") && headerName(sh.header[2])==QStringLiteral("底深")) {
            track.depthInfo.depthType="MD";
            if(track.depthInfo.depthUnit.empty())track.depthInfo.depthUnit="m";
        }
        GeologicalInterval interval;interval.intervalName=s(cell(row,sh.nameCol));interval.displayOrder=int(track.intervals.size());interval.sourceRow=sh.row;
        interval.topDepth=number(cell(row,sh.top));interval.bottomDepth=number(cell(row,sh.bottom));interval.topTvd=number(cell(row,sh.topTvd));interval.bottomTvd=number(cell(row,sh.bottomTvd));interval.topTvdss=number(cell(row,sh.topSs));interval.bottomTvdss=number(cell(row,sh.bottomSs));
        interval.faciesType=s(cell(row,sh.facies));interval.topUnconformity=flag(cell(row,sh.topUnc));interval.bottomUnconformity=flag(cell(row,sh.bottomUnc));interval.bottomNotPenetrated=flag(cell(row,sh.notPen));
        if(!std::isfinite(interval.topDepth)||!std::isfinite(interval.bottomDepth)||interval.bottomDepth<interval.topDepth)issue(out,"INTERVAL_DEPTH_INVALID","层段顶底深缺失或顺序异常；原值保留",sh.name,sh.row,false,row.join(','));
        const std::set<int> mapped={sh.well,sh.track,sh.nameCol,sh.top,sh.bottom,sh.topTvd,sh.bottomTvd,sh.topSs,sh.bottomSs,sh.facies,sh.topUnc,sh.bottomUnc,sh.notPen};
        for(int c=0;c<sh.header.size();++c)if(!mapped.contains(c)&&!cell(row,c).isEmpty()){Measurement m;m.measurementName=s(sh.header[c]);m.originalText=s(cell(row,c));m.numericValue=number(cell(row,c));interval.properties.push_back(std::move(m));}
        track.intervals.push_back(std::move(interval));++intervalRows;
    };
    while(!xml.atEnd()){
        const auto token=xml.readNext();if(token==QXmlStreamReader::Invalid)break;
        if(token==QXmlStreamReader::DTD)return fail(std::move(out),"不支持包含 DTD 的 SpreadsheetML");
        if(token==QXmlStreamReader::StartElement){
            const QString name=xml.name().toString();const bool ss=xml.namespaceUri()==spreadsheetNs;
            if(!rootSeen){rootSeen=true;if(name!="Workbook"||!ss)return fail(std::move(out),"不是 Excel 2003 SpreadsheetML Workbook");}
            if(inCell&&dataDepth>0){++dataDepth;continue;}
            if(ss&&name=="Worksheet"){sh={};sh.name=attr(xml.attributes(),"Name");sh.source.sheetName=s(sh.name);inSheet=true;}
            else if(ss&&inSheet&&name=="Table")inTable=true;
            else if(ss&&inTable&&name=="Row"){
                const QString index=attr(xml.attributes(),"Index");bool ok=true;const int next=index.isEmpty()?sh.row+1:index.toInt(&ok);if(!ok||next<=sh.row)return fail(std::move(out),"工作表行号必须严格递增且在整数范围内");sh.row=next;row.clear();column=0;inRow=true;
                if(cancelled&&cancelled())return fail(std::move(out),"解析已取消");
            }else if(ss&&inRow&&name=="Cell"){
                const QString index=attr(xml.attributes(),"Index");bool ok=true;const int next=index.isEmpty()?column+1:index.toInt(&ok);if(!ok||next<=column||next>16384)return fail(std::move(out),"单元格列号无效或超出 16384 列");column=next;mergeAcross=attr(xml.attributes(),"MergeAcross").toInt();if(mergeAcross<0||column+mergeAcross>16384)return fail(std::move(out),"合并单元格范围无效");
                if(!attr(xml.attributes(),"Formula").isEmpty())sh.formula=true;inCell=true;value.clear();
            }else if(ss&&inCell&&name=="Data")dataDepth=1;
        }else if(token==QXmlStreamReader::Characters&&dataDepth>0)value+=xml.text();
        else if(token==QXmlStreamReader::EndElement){
            const QString name=xml.name().toString();const bool ss=xml.namespaceUri()==spreadsheetNs;
            if(dataDepth>0){--dataDepth;continue;}
            if(ss&&name=="Cell"&&inCell){while(row.size()<column+mergeAcross)row.append(QString());row[column-1]=value;column+=mergeAcross;inCell=false;}
            else if(ss&&name=="Row"&&inRow){processRow();inRow=false;}
            else if(ss&&name=="Table")inTable=false;
            else if(ss&&name=="Worksheet"&&inSheet){
                if((sh.kind=="logs"||sh.kind.isEmpty())&&sh.row>33)
                    sh.source.sheetName=s(sh.name+QStringLiteral("（原文前33行预览；完整内容见原文件/测井曲线）"));
                if(sh.kind.isEmpty()&&sh.dataRows)issue(out,"XML_SHEET_MAPPING_REQUIRED","工作表表头未匹配业务模型；保留原文件及前33行预览",sh.name,0,true);
                if(sh.badCurve)issue(out,"NON_NUMERIC_CURVE",QStringLiteral("%1 个曲线单元格不是有限数值；保留缺失样点和原始文件").arg(sh.badCurve),sh.name);
                if(sh.badDepth)issue(out,"INVALID_LOG_DEPTH",QStringLiteral("%1 个采样深度无效；保留位置供核对").arg(sh.badDepth),sh.name);
                if(sh.suspectNulls)issue(out,"CURVE_NULL_REVIEW",QStringLiteral("发现 %1 个 -999900 / -99925 疑似缩放空值；来源未声明 NULL，保留原数值，需按曲线确认清理规则").arg(sh.suspectNulls),sh.name);
                if(sh.formula)issue(out,"XML_FORMULA_CACHED_VALUE","公式单元格读取文件保存的计算结果；未执行公式",sh.name);
                for(const auto& [wi,si]:sh.logSets){auto& axis=out.wells[wi].wellbore.logSets[si].samplingAxis;const auto& depths=axis.depthValues;bool regular=depths.size()>1;const double step=regular?depths[1]-depths[0]:Missing;regular=regular&&std::isfinite(step)&&step>0;for(size_t i=2;regular&&i<depths.size();++i)regular=std::isfinite(depths[i])&&std::abs((depths[i]-depths[i-1])-step)<1e-7*std::max(1.0,std::abs(step));if(regular)axis.sampleStep=step;else if(depths.size()>1&&!sh.name.startsWith(QStringLiteral("离散")))issue(out,"IRREGULAR_DEPTH_AXIS","采样深度非等距递增；保留完整深度数组",sh.name);}
                out.sourceTables.push_back(std::move(sh.source));inSheet=false;
            }
        }
    }
    if(!input.error().isEmpty())return fail(std::move(out),input.error());
    if(xml.hasError()||!rootSeen)return fail(std::move(out),QStringLiteral("XML 第 %1 行：%2").arg(xml.lineNumber()).arg(xml.errorString()));
    if(cancelled&&cancelled())return fail(std::move(out),"解析已取消");
    if(conflict){out.wells.clear();out.status="review";out.message="井归属冲突，保留来源预览；请指定正确井名后重新导入";return out;}
    if(options.depthUnit.empty()&&!out.wells.empty())issue(out,"DEPTH_UNIT_UNKNOWN","XML 表头未声明深度单位与基准；不默认套用米或 MD");
    out.status=out.issues.empty()?"parsed":"review";out.message=s(QStringLiteral("读取 %1 个工作表、%2 行：%3 测井/离散样点、%4 层段、%5 照片引用；原始值与工作表来源保留").arg(out.sourceTables.size()).arg(rows).arg(logRows).arg(intervalRows).arg(photos));return out;
}
}
