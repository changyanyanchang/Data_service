// 层：数据（文件解析）；原始文件只读，字段与数值不依赖 SQL/UI。
#include "paleo_data/parser.h"
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QStringConverter>
#include <QRegularExpression>
#include <QLocale>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <map>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace paleo::data {
bool mapWellCoordinateTables(ParsedFile&,const ParseOptions&,const CancelCheck&);
namespace {
std::string s(const QString& value) { return value.toUtf8().toStdString(); }
QString q(const std::string& value) { return QString::fromUtf8(value); }
const QRegularExpression WellPattern(QStringLiteral("[A-Z]{1,8}[0-9]{1,4}(?:-[A-Z]{0,3}[0-9]{1,4}[A-Z0-9]*){1,5}"));
QString wellToken(QString text) {
    text=text.trimmed().toUpper();text.replace('_','-');text.replace(QChar(0x2013),'-');text.replace(QChar(0x2014),'-');
    const auto match=WellPattern.match(text); return match.hasMatch()?match.captured():QString();
}
bool cancelledNow(const CancelCheck& cancelled, int) { return cancelled && cancelled(); }
void addIssue(ParsedFile& out, const char* code, const QString& message, int row=0, bool blocking=false, const QString& original={}) {
    if (out.issues.size()<250) out.issues.push_back({code,s(message),"",s(original),row,blocking});
    else if (out.issues.size()==250) out.issues.push_back({"MORE_ISSUES","更多异常未逐条展开，原始文件已保留；请核对来源。","","",0,false});
}
ParsedFile fail(ParsedFile out, const QString& message) {
    out.status="failed";out.message=s(message);out.wells.clear();out.horizons.clear();out.volumes.clear();
    addIssue(out,"PARSE_FAILED",message,0,true);return out;
}
bool isSentinel(double value) { return value==-99999 || value==-9999 || value==-999 || value==-999.25; }
double number(QString text, bool rawCurve, bool* bad=nullptr) {
    text=text.trimmed();
    if (bad) *bad=false;
    if (text.isEmpty() || text.compare("nan",Qt::CaseInsensitive)==0 || text.compare("null",Qt::CaseInsensitive)==0 || text=="-") return Missing;
    bool ok=false;double value=QLocale::c().toDouble(text,&ok);
    if (!ok || !std::isfinite(value)) { if(bad)*bad=true;return Missing; }
    return !rawCurve && isSentinel(value)?Missing:value;
}
int flag(QString value) {
    value=value.trimmed().toLower();if(value=="true"||value=="1"||value==QStringLiteral("是"))return 1;
    if(value=="false"||value=="0"||value==QStringLiteral("否"))return 0;return -1;
}
struct Record { QStringList cells; int row=0; bool bad=false; QString error; };
Record readRecord(QTextStream& input, int& physicalRow, QChar separator=',') {
    Record out;out.row=physicalRow+1;QString value;bool quoted=false, atStart=true, closed=false;
    while(!input.atEnd()) {
        QString line=input.readLine();++physicalRow;
        if(line.size()>4*1024*1024) {out.bad=true;out.error="单行超过解析上限";return out;}
        for(qsizetype i=0;i<line.size();++i) {
            QChar c=line.at(i);
            if(quoted) {if(c=='"'){if(i+1<line.size()&&line.at(i+1)=='"'){value+='"';++i;}else{quoted=false;closed=true;}}else value+=c;}
            else if(c==separator){out.cells<<value;value.clear();atStart=true;closed=false;}
            else if(c=='"'&&atStart){quoted=true;atStart=false;}
            else {if(closed&&!c.isSpace()){out.bad=true;out.error="CSV 引号后出现未分隔内容";}value+=c;atStart=false;}
        }
        if(!quoted) {out.cells<<value;return out;}
        value+='\n';if(value.size()>4*1024*1024){out.bad=true;out.error="CSV 多行字段过大";return out;}
    }
    out.bad=quoted;out.error=quoted?QStringLiteral("CSV 引号未闭合"):QString();out.cells<<value;return out;
}
bool blank(const QStringList& cells) {return std::all_of(cells.cbegin(),cells.cend(),[](const QString&v){return v.trimmed().isEmpty();});}
void previewRow(SourceTable& table,const Record& record,int maximum=33) {
    if(record.row>maximum)return;
    table.rowCount=std::max(table.rowCount,record.row);table.columnCount=std::max(table.columnCount,int(record.cells.size()));
    for(int col=0;col<record.cells.size();++col)if(!record.cells[col].isEmpty())table.cells.push_back({record.row,col+1,s(record.cells[col])});
}
QString col(const QStringList& values,int index) {return index>=0&&index<values.size()?values[index]:QString();}
int field(const QStringList& header,std::initializer_list<const char*> names) {
    for(int i=0;i<header.size();++i)for(const char* name:names)if(header[i].trimmed().compare(QString::fromUtf8(name),Qt::CaseInsensitive)==0)return i;return -1;
}
void finish(ParsedFile& out,const QString& message) {
    out.message=s(message);out.status=out.issues.empty()?"parsed":"review";
}
WellDataset makeWell(const QString& name,const std::string& path,const ParseOptions& options) {
    WellDataset well;well.wellInfo.wellName=s(name);well.defaultDepthInfo.depthUnit=options.depthUnit;
    well.sourceFiles.push_back(path);return well;
}
ParsedFile parsePetrelCoordinates(ParsedFile out,QFile& file,const ParseOptions& options,const CancelCheck& cancelled) {
    out.kind="coordinates";
    if(file.size()>16*1024*1024)return fail(std::move(out),"Petrel 井口表超过当前解析上限，请分批导入");
    file.seek(0);const QByteArray bytes=file.readAll();QStringDecoder decoder(QStringConverter::Utf8);QString text=decoder(bytes);
    if(decoder.hasError()) {
#ifdef _WIN32
        const int count=MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,bytes.constData(),int(bytes.size()),nullptr,0);
        if(count<=0)return fail(std::move(out),"井口表编码不能识别；请另存 UTF-8");
        std::wstring wide(count,L'\0');MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,bytes.constData(),int(bytes.size()),wide.data(),count);text=QString::fromStdWString(wide);
        addIssue(out,"TEXT_ENCODING_REVIEW","文件不是 UTF-8，按 GBK 解码；原文件保留，中文文本需核对");
#else
        return fail(std::move(out),"井口表编码不能识别；请另存 UTF-8");
#endif
    }
    QTextStream input(&text);QStringList header;bool inHeader=false,body=false;int row=0;std::string crs,unit,depthUnit;
    SourceTable table;table.sheetName="Petrel井口原文";QSet<QString> names;
    while(!input.atEnd()) {
        const QString line=input.readLine();++row;if(cancelled&&cancelled())return fail(std::move(out),"解析已取消");
        if(!line.isEmpty())table.cells.push_back({row,1,s(line)});table.rowCount=row;table.columnCount=1;
        if(line.startsWith("# Coordinate reference system X, Y:"))crs=s(line.section(':',1).trimmed());
        if(line.startsWith("# Unit in X and Y direction:"))unit=s(line.section(':',1).trimmed());
        if(line.startsWith("# Unit in depth:"))depthUnit=s(line.section(':',1).trimmed());
        if(line.trimmed()=="BEGIN HEADER"){inHeader=true;continue;}
        if(line.trimmed()=="END HEADER"){inHeader=false;body=true;continue;}
        if(inHeader){header<<line.trimmed();continue;}if(!body||line.trimmed().isEmpty())continue;
        // Petrel quotes string fields; quotes inside unquoted DMS coordinates are literal.
        QStringList cells;int pos=0;bool bad=false;
        while(pos<line.size()){while(pos<line.size()&&line[pos].isSpace())++pos;if(pos>=line.size())break;QString cell;
            if(line[pos]=='"'){++pos;while(pos<line.size()&&line[pos]!='"')cell+=line[pos++];if(pos>=line.size()){bad=true;break;}++pos;}
            else while(pos<line.size()&&!line[pos].isSpace())cell+=line[pos++];cells<<cell;}
        if(bad||cells.size()!=header.size()){addIssue(out,"COORDINATE_COLUMNS","Petrel 井口行与字段定义不符，未写入该井",row,true,line);continue;}
        const int ni=field(header,{"Name"}),ui=field(header,{"UWI"}),xi=field(header,{"Surface X"}),yi=field(header,{"Surface Y"}),td=field(header,{"TD (MD)"}),datum=field(header,{"Well datum name"});
        if(ni<0||xi<0||yi<0)return fail(std::move(out),"Petrel 表缺少 Name/Surface X/Surface Y 字段");
        const QString name=q(canonicalWellName(s(cells[ni])));if(name.isEmpty())continue;
        if(names.contains(name)){addIssue(out,"DUPLICATE_WELL_COORDINATES","井口表井名重复，全部同名坐标待核对",row,true,line);for(auto& w:out.wells)if(w.wellInfo.wellName==s(name)){w.wellInfo.wellheadX=Missing;w.wellInfo.wellheadY=Missing;}continue;}names.insert(name);
        WellDataset well=makeWell(name,out.path,options);well.wellInfo.wellheadX=number(cells[xi],false);well.wellInfo.wellheadY=number(cells[yi],false);
        well.wellInfo.coordinateSystem=options.coordinateSystem.empty()?crs:options.coordinateSystem;well.wellInfo.coordinateUnit=options.coordinateUnit.empty()?unit:options.coordinateUnit;
        well.defaultDepthInfo.depthType="MD";well.defaultDepthInfo.depthUnit=options.depthUnit.empty()?depthUnit:options.depthUnit;well.defaultDepthInfo.mdReference=s(col(cells,datum));well.wellInfo.totalDrilledDepth=number(col(cells,td),false);
        if(ui>=0&&q(canonicalWellName(s(cells[ui])))!=name)addIssue(out,"WELL_IDENTIFIER_DIFFERS","Name 与 UWI 不同；按 Name 登记，未将两个名称自动合并",row,false,cells[ni]+" / "+cells[ui]);
        well.wellInfo.remark=s(QStringLiteral("Petrel井口资料第 %1 行；其他属性见原始表").arg(row));out.wells.push_back(std::move(well));
    }
    out.sourceTables.push_back(std::move(table));if(out.wells.empty())return fail(std::move(out),"Petrel 井口表没有有效记录");
    finish(out,QStringLiteral("读取 %1 口井；坐标约定使用表头声明").arg(out.wells.size()));return out;
}
ParsedFile parseCoordinates(ParsedFile out,QTextStream& input,const ParseOptions& options,const CancelCheck& cancelled) {
    out.kind="coordinates";SourceTable table;table.sheetName="井坐标原文（无表头）";
    int physicalRow=0;QSet<QString> names;
    while(!input.atEnd()) {
        Record row=readRecord(input,physicalRow);if(row.bad)return fail(std::move(out),row.error);
        if(blank(row.cells))continue;
        if(cancelledNow(cancelled,physicalRow))return fail(std::move(out),"解析已取消");
        if(row.cells.size()==1)row.cells=row.cells[0].split(QRegularExpression("\\s+"),Qt::SkipEmptyParts);
        previewRow(table,row,std::numeric_limits<int>::max());
        if(row.cells.size()<3){addIssue(out,"COORDINATE_COLUMNS","井坐标记录不足三列",row.row,true,row.cells.join(','));continue;}
        const QString name=wellToken(row.cells[0]);bool bad1=false,bad2=false;
        const double a=number(row.cells[1],false,&bad1),b=number(row.cells[2],false,&bad2);
        if(name.isEmpty()){addIssue(out,"WELL_NAME_UNKNOWN","井坐标记录缺少可识别的井名",row.row,true,row.cells[0]);continue;}
        WellDataset well=makeWell(name,out.path,options);
        if(names.contains(name))addIssue(out,"DUPLICATE_WELL_COORDINATES","同一井出现多个坐标记录，不应自动覆盖",row.row,true,row.cells.join(','));
        names.insert(name);
        if(bad1||bad2||!std::isfinite(a)||!std::isfinite(b))addIssue(out,"INVALID_COORDINATES","坐标无效，原始记录保留",row.row,true,row.cells.join(','));
        else if(options.coordinateOrder=="xy"||options.coordinateOrder=="yx") {
            well.wellInfo.wellheadX=options.coordinateOrder=="xy"?a:b;well.wellInfo.wellheadY=options.coordinateOrder=="xy"?b:a;
            well.wellInfo.coordinateSystem=options.coordinateSystem;well.wellInfo.coordinateUnit=options.coordinateUnit;
        }
        well.wellInfo.remark=s(QStringLiteral("来源第 %1 行，原坐标列2=%2，列3=%3").arg(row.row).arg(row.cells[1],row.cells[2]));
        out.wells.push_back(std::move(well));
    }
    out.sourceTables.push_back(std::move(table));
    if(options.coordinateOrder!="xy"&&options.coordinateOrder!="yx")addIssue(out,"COORDINATE_ORDER_UNKNOWN","无表头坐标文件的轴序未确认；井已登记，坐标保留在原始表中，尚未写入 X/Y。",0,true);
    if(options.coordinateSystem.empty()||options.coordinateUnit.empty())addIssue(out,"COORDINATE_REFERENCE_UNKNOWN","坐标系或坐标单位未确认，不能直接开展空间叠加。",0,false);
    if(out.wells.empty())return fail(std::move(out),"没有可识别的井坐标记录");
    finish(out,QStringLiteral("读取 %1 条井坐标记录").arg(out.wells.size()));return out;
}
ParsedFile parseHorizon(ParsedFile out,QTextStream& input,const ParseOptions& options,const CancelCheck& cancelled) {
    out.kind="horizon";SeismicHorizon horizon;horizon.horizonName=s(QFileInfo(q(out.path)).completeBaseName());
    horizon.sourceFile=out.path;horizon.coordinateSystem=options.coordinateSystem;horizon.coordinateUnit=options.coordinateUnit;
    const QString num="([+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[Ee][+-]?[0-9]+)?)";
    const QRegularExpression pattern("^\\s*INLINE\\s*:\\s*([+-]?[0-9]+)\\s+XLINE\\s*:\\s*([+-]?[0-9]+)\\s+"+num+"\\s+"+num+"\\s+"+num+"\\s*$",QRegularExpression::CaseInsensitiveOption);
    int row=0,badRows=0;
    while(!input.atEnd()) {
        const QString line=input.readLine();++row;if(line.trimmed().isEmpty())continue;
        if(cancelledNow(cancelled,row))return fail(std::move(out),"解析已取消");
        const auto match=pattern.match(line);
        if(!match.hasMatch()){++badRows;addIssue(out,"HORIZON_BAD_ROW","层位行不符合 INLINE/XLINE/X/Y/Z 时间格式",row,false,line);continue;}
        bool ilOk=false,xlOk=false;SeismicHorizonPoint point;
        point.inlineNumber=match.captured(1).toInt(&ilOk);point.crosslineNumber=match.captured(2).toInt(&xlOk);
        point.x=number(match.captured(3),false);point.y=number(match.captured(4),false);point.zTime=number(match.captured(5),false);
        if(!ilOk||!xlOk||!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.zTime)){++badRows;addIssue(out,"HORIZON_INVALID_VALUE","层位编号或数值无效，原始文件保留",row,false,line);continue;}
        horizon.points.push_back(point);
    }
    if(horizon.points.empty())return fail(std::move(out),"没有有效的标记式层位点；需要对应格式解析器");
    addIssue(out,"HORIZON_REFERENCE_UNKNOWN","Z 列按本资料约定保存为时间；时间单位、单/双程含义、零点及所属地震体待核对。",0,false);
    if(options.coordinateSystem.empty()||options.coordinateUnit.empty())addIssue(out,"COORDINATE_REFERENCE_UNKNOWN","层位坐标系或坐标单位未确认",0,false);
    const auto count=horizon.points.size();out.horizons.push_back(std::move(horizon));
    finish(out,QStringLiteral("读取 %1 个层位点，%2 行需核对").arg(count).arg(badRows));return out;
}
ParsedFile parseCsv(ParsedFile out,QTextStream& input,const ParseOptions& options,const CancelCheck& cancelled,QChar separator=',') {
    int physicalRow=0;Record header;
    do {header=readRecord(input,physicalRow,separator);}while(!input.atEnd()&&blank(header.cells));
    if(header.bad)return fail(std::move(out),header.error);
    for(auto& name:header.cells)name=name.trimmed();
    const int wellCol=field(header.cells,{"井号","井名","WELL","WELLNAME"});
    const int depthCol=field(header.cells,{"深度","DEPTH","DEPT","MD"});
    const int topCol=field(header.cells,{"顶深","TOP","TOPDEPTH"}),bottomCol=field(header.cells,{"底深","BASE","BOTTOM","BOTTOMDEPTH"});
    const int lithCol=field(header.cells,{"岩性","LITHOLOGY"}),trackCol=field(header.cells,{"道名","TRACK"}),layerCol=field(header.cells,{"层号","层名","地层","FORMATION"});
    const bool logs=depthCol>=0, lith=!logs&&topCol>=0&&bottomCol>=0&&lithCol>=0, layers=!logs&&!lith&&topCol>=0&&bottomCol>=0&&layerCol>=0;
    out.kind=logs?"logs":lith?"lithology":layers?"formations":"table";
    if(!logs&&!lith&&!layers){
        SourceTable table;table.sheetName="原始文本表";previewRow(table,header,2147483647);
        while(!input.atEnd()){
            if(cancelled&&cancelled())return fail(std::move(out),"解析已取消");
            auto row=readRecord(input,physicalRow,separator);if(row.bad)return fail(std::move(out),row.error);
            if(!blank(row.cells))previewRow(table,row,2147483647);
            if(table.cells.size()>2000000)return fail(std::move(out),"未映射文本表超过 200 万非空格预算，请分表导入");
        }
        out.sourceTables.push_back(std::move(table));out.status="review";out.message="已保存文本表全部原始单元格，业务字段映射待核对";
        if(mapWellCoordinateTables(out,options,cancelled))return out;
        addIssue(out,"TEXT_MAPPING_REQUIRED","表头不属于已确认模板；MD/TVD/X/Y 轨迹坐标表不会伪造井斜角或误当测井曲线",header.row,false,header.cells.join(separator));return out;
    }
    const QString fileWell=wellToken(QFileInfo(q(out.path)).completeBaseName());
    const QString overrideName=options.wellNameOverride.empty()?QString():q(canonicalWellName(options.wellNameOverride));
    if(wellCol<0&&overrideName.isEmpty()&&fileWell.isEmpty())return fail(std::move(out),"找不到井名字段或明确的井名映射");
    QHash<QString,int> wellIndices;std::vector<std::map<QString,int>> trackIndices;
    SourceTable preview;preview.sheetName="原文预览（前33物理行）";previewRow(preview,header);
    bool identityConflict=false;int dataRows=0,invalidNumbers=0,invalidDepths=0,firstInvalidRow=0;
    int stepChanges=0;
    const int tvdCol=field(header.cells,{"TVD"}),tvdssCol=field(header.cells,{"TVDSS"});
    const int topTvd=field(header.cells,{"顶TVD"}),bottomTvd=field(header.cells,{"底TVD"}),topTvdss=field(header.cells,{"顶TVDSS"}),bottomTvdss=field(header.cells,{"底TVDSS"});
    const int faciesCol=field(header.cells,{"相类型"}),topUnc=field(header.cells,{"顶部不整合"}),bottomUnc=field(header.cells,{"底部不整合"}),notPen=field(header.cells,{"底部未钻穿"});
    std::vector<int> curveColumns;
    if(logs)for(int i=0;i<header.cells.size();++i)if(i!=wellCol&&i!=depthCol&&i!=tvdCol&&i!=tvdssCol)curveColumns.push_back(i);
    while(!input.atEnd()) {
        Record row=readRecord(input,physicalRow,separator);if(row.bad)return fail(std::move(out),QStringLiteral("第 %1 行：%2").arg(row.row).arg(row.error));
        if(blank(row.cells))continue;previewRow(preview,row);
        if(cancelledNow(cancelled,physicalRow))return fail(std::move(out),"解析已取消");
        if(row.cells.size()!=header.cells.size())return fail(std::move(out),QStringLiteral("第 %1 行列数 %2 与表头 %3 不符；未导入部分数据").arg(row.row).arg(row.cells.size()).arg(header.cells.size()));
        const QString originalName=col(row.cells,wellCol).trimmed();
        QString name=overrideName.isEmpty()?q(canonicalWellName(s(originalName))):overrideName;
        if(name.isEmpty())name=fileWell;
        if(name.isEmpty()){identityConflict=true;addIssue(out,"WELL_NAME_UNKNOWN","数据行没有可识别井名",row.row,true,originalName);continue;}
        if(overrideName.isEmpty()&&!fileWell.isEmpty()&&name!=fileWell){
            if(!identityConflict)addIssue(out,"WELL_NAME_CONFLICT",QStringLiteral("文件名井号 %1 与内容井号 %2 不一致；需指定正确归属后重新导入").arg(fileWell,name),row.row,true,originalName);
            identityConflict=true;
        }
        if(!wellIndices.contains(name)) {
            const int index=int(out.wells.size());wellIndices.insert(name,index);out.wells.push_back(makeWell(name,out.path,options));trackIndices.emplace_back();
            if(logs) {
                LogSet set;set.logSetName=s(QFileInfo(q(out.path)).completeBaseName());set.sourceFile=out.path;set.depthInfo.depthUnit=options.depthUnit;
                if(header.cells[depthCol].compare("MD",Qt::CaseInsensitive)==0)set.depthInfo.depthType="MD";
                for(int column:curveColumns){LogCurve curve;curve.curveName=s(header.cells[column]);curve.description=s(header.cells[column]);set.logCurves.push_back(std::move(curve));}
                out.wells.back().wellbore.logSets.push_back(std::move(set));
            }
        }
        const int index=wellIndices[name];auto& well=out.wells[index];++dataRows;
        if(logs) {
            auto& set=well.wellbore.logSets[0];auto& axis=set.samplingAxis;bool bad=false;
            const double depth=number(col(row.cells,depthCol),false,&bad);axis.depthValues.push_back(depth);
            if(bad||!std::isfinite(depth)){++invalidDepths;if(!firstInvalidRow)firstInvalidRow=row.row;}
            if(tvdCol>=0)axis.tvdValues.push_back(number(col(row.cells,tvdCol),false));
            if(tvdssCol>=0)axis.tvdssValues.push_back(number(col(row.cells,tvdssCol),false));
            for(int i=0;i<curveColumns.size();++i){const double value=number(col(row.cells,curveColumns[i]),true,&bad);set.logCurves[i].sampleValues.push_back(value);if(bad){++invalidNumbers;if(!firstInvalidRow)firstInvalidRow=row.row;}}
        } else {
            QString trackName=lith?QStringLiteral("岩性"):col(row.cells,trackCol).trimmed();if(trackName.isEmpty())trackName="地层";
            auto& byName=trackIndices[index];
            if(!byName.contains(trackName)){IntervalTrack track;track.trackName=s(trackName);track.trackType=lith?"岩性":"地层";track.sourceFile=out.path;track.depthInfo.depthUnit=options.depthUnit;byName[trackName]=int(well.wellbore.intervalTracks.size());well.wellbore.intervalTracks.push_back(std::move(track));}
            auto& track=well.wellbore.intervalTracks[byName[trackName]];GeologicalInterval interval;
            interval.intervalName=s(col(row.cells,lith?lithCol:layerCol));interval.displayOrder=int(track.intervals.size());interval.sourceRow=row.row;
            interval.topDepth=number(col(row.cells,topCol),false);interval.bottomDepth=number(col(row.cells,bottomCol),false);
            interval.topTvd=number(col(row.cells,topTvd),false);interval.bottomTvd=number(col(row.cells,bottomTvd),false);
            interval.topTvdss=number(col(row.cells,topTvdss),false);interval.bottomTvdss=number(col(row.cells,bottomTvdss),false);
            interval.faciesType=s(col(row.cells,faciesCol));interval.topUnconformity=flag(col(row.cells,topUnc));interval.bottomUnconformity=flag(col(row.cells,bottomUnc));interval.bottomNotPenetrated=flag(col(row.cells,notPen));
            if(!std::isfinite(interval.topDepth)||!std::isfinite(interval.bottomDepth)||interval.bottomDepth<interval.topDepth)addIssue(out,"INTERVAL_DEPTH_INVALID","层段顶底深缺失或次序异常；原值保留",row.row,false,row.cells.join(','));
            const std::set<int> mapped={wellCol,trackCol,lithCol,layerCol,topCol,bottomCol,topTvd,bottomTvd,topTvdss,bottomTvdss,faciesCol,topUnc,bottomUnc,notPen};
            for(int c=0;c<header.cells.size();++c)if(!mapped.contains(c)&&!row.cells[c].isEmpty()){Measurement m;m.measurementName=s(header.cells[c]);m.originalText=s(row.cells[c]);m.numericValue=number(row.cells[c],true);interval.properties.push_back(std::move(m));}
            track.intervals.push_back(std::move(interval));
        }
    }
    if(cancelled&&cancelled())return fail(std::move(out),"解析已取消");
    if(identityConflict){out.wells.clear();out.sourceTables.push_back(std::move(preview));finish(out,"井归属冲突，已保留来源与原文预览；明确井名后重新解析");return out;}
    if(!dataRows)return fail(std::move(out),"表中没有有效数据行");
    if(logs)for(auto& well:out.wells){auto& axis=well.wellbore.logSets[0].samplingAxis;const auto& depths=axis.depthValues;if(depths.size()>1){double step=depths[1]-depths[0];bool regular=std::isfinite(step)&&step>0;for(size_t i=2;i<depths.size();++i)if(!std::isfinite(depths[i])||std::abs((depths[i]-depths[i-1])-step)>1e-7*std::max(1.0,std::abs(step))){regular=false;break;}if(regular)axis.sampleStep=step;else ++stepChanges;}}
    if(invalidNumbers)addIssue(out,"NON_NUMERIC_CURVE",QStringLiteral("%1 个曲线值不是有限数值，标记为缺失；原文件保留，样点未删除").arg(invalidNumbers),firstInvalidRow,false);
    if(invalidDepths)addIssue(out,"INVALID_LOG_DEPTH",QStringLiteral("%1 个深度值无效，保留样点位置供核对，不能直接插值").arg(invalidDepths),firstInvalidRow,false);
    if(stepChanges)addIssue(out,"IRREGULAR_DEPTH_AXIS","采样深度不是严格等距递增序列；保留显式深度，不自动重采样",0,false);
    if(options.depthUnit.empty())addIssue(out,"DEPTH_UNIT_UNKNOWN","来源未明确深度单位；保留原数值，待确认后再开展需单位的业务",0,false);
    finish(out,QStringLiteral("读取 %1 行，%2 口井；原值与来源保留").arg(dataRows).arg(out.wells.size()));return out;
}
ParsedFile parseLas(ParsedFile out,QTextStream& input,const ParseOptions& options,const CancelCheck& cancelled) {
    out.kind="logs";QString section,wellName;QStringList mnemonics,units;std::vector<std::vector<double>> rows;
    bool wrapped=false;int physical=0;double nullValue=-999.25;bool declaredNull=false;
    QString headerDepthUnit;QStringList pending;std::vector<int> pendingRows;
    const QRegularExpression item("^\\s*([^\\s.]+)\\s*\\.([^\\s:]*)\\s*([^:]*)(?::(.*))?$");
    while(!input.atEnd()) {
        QString line=input.readLine();++physical;line=line.trimmed();if(line.isEmpty()||line.startsWith('#'))continue;
        if(cancelledNow(cancelled,physical))return fail(std::move(out),"解析已取消");
        if(line.startsWith('~')){section=line.mid(1,1).toUpper();continue;}
        if(section=="A") {
            if(mnemonics.empty())return fail(std::move(out),"LAS 缺少曲线定义");
            const auto cells=line.split(QRegularExpression("\\s+"),Qt::SkipEmptyParts);
            if(!wrapped&&cells.size()!=mnemonics.size())return fail(std::move(out),QStringLiteral("LAS 第 %1 行样点列数与曲线定义不一致").arg(physical));
            pending.append(cells);
            while(pending.size()>=mnemonics.size()){std::vector<double> values;values.reserve(mnemonics.size());for(int i=0;i<mnemonics.size();++i){bool bad=false;values.push_back(number(pending[i],true,&bad));if(bad)addIssue(out,"NON_NUMERIC_CURVE","LAS 数值无效，保留缺失样点",physical,false,pending[i]);}rows.push_back(std::move(values));pending=pending.mid(mnemonics.size());}
            continue;
        }
        const auto match=item.match(line);if(!match.hasMatch())continue;
        const QString mnemonic=match.captured(1).toUpper(),unit=match.captured(2),value=match.captured(3).trimmed();
        if(section=="V"&&mnemonic=="WRAP")wrapped=value.compare("YES",Qt::CaseInsensitive)==0;
        if(section=="W"&&mnemonic=="WELL"){
            wellName=value;
            // Some LAS 1.2 exports put a literal WELL label before ':' and the actual name after it.
            if(value.compare("WELL",Qt::CaseInsensitive)==0&&!wellToken(match.captured(4)).isEmpty())wellName=match.captured(4).trimmed();
        }
        if(section=="W"&&mnemonic=="NULL"){bool bad=false;nullValue=number(value,true,&bad);declaredNull=!bad&&std::isfinite(nullValue);if(!declaredNull)return fail(std::move(out),"LAS NULL 声明不是有限数值");}
        if(section=="C"){mnemonics<<match.captured(1);units<<unit;}
    }
    if(!pending.empty())return fail(std::move(out),"LAS 包装记录在文件末尾不完整");
    if(rows.empty()||mnemonics.size()<2)return fail(std::move(out),"LAS 没有曲线样点");
    QString fileStem=QFileInfo(q(out.path)).completeBaseName();
    fileStem.remove(QRegularExpression("(?<=\\d)MAIN$",QRegularExpression::CaseInsensitiveOption));
    fileStem.remove(QRegularExpression("_2DNMR$",QRegularExpression::CaseInsensitiveOption));
    const QString fileWell=wellToken(fileStem);
    QString actual=q(canonicalWellName(s(wellName)));if(actual.isEmpty())actual=fileWell;
    if(!options.wellNameOverride.empty())actual=q(canonicalWellName(options.wellNameOverride));
    else if(!fileWell.isEmpty()&&!actual.isEmpty()&&fileWell!=actual){addIssue(out,"WELL_NAME_CONFLICT","LAS WELL 与文件名井号冲突，需显式指定归属后导入",0,true,wellName);finish(out,"井归属待核对，尚未生成井数据");return out;}
    if(actual.isEmpty())return fail(std::move(out),"LAS 未提供井名，需要指定井名映射");
    int depth=-1;for(int i=0;i<mnemonics.size();++i)if(mnemonics[i].compare("DEPT",Qt::CaseInsensitive)==0||mnemonics[i].compare("DEPTH",Qt::CaseInsensitive)==0||mnemonics[i].compare("MD",Qt::CaseInsensitive)==0){depth=i;break;}
    if(depth<0)return fail(std::move(out),"LAS 未识别到深度曲线，不能默认使用第一列");
    WellDataset well=makeWell(actual,out.path,options);LogSet set;set.logSetName=s(QFileInfo(q(out.path)).completeBaseName());set.sourceFile=out.path;
    set.depthInfo.depthUnit=s(units[depth]);if(set.depthInfo.depthUnit.empty())set.depthInfo.depthUnit=options.depthUnit;
    if(mnemonics[depth].compare("MD",Qt::CaseInsensitive)==0)set.depthInfo.depthType="MD";
    std::vector<int> columns;for(int i=0;i<mnemonics.size();++i)if(i!=depth){LogCurve curve;curve.curveName=s(mnemonics[i]);curve.curveUnit=s(units[i]);set.logCurves.push_back(curve);columns.push_back(i);}
    for(const auto& values:rows){const double d=values[depth];set.samplingAxis.depthValues.push_back((declaredNull&&d==nullValue)?Missing:d);for(int c=0;c<columns.size();++c)set.logCurves[c].sampleValues.push_back(values[columns[c]]);}
    const auto& axis=set.samplingAxis.depthValues;
    if(axis.size()>1){const double step=axis[1]-axis[0];bool regular=std::isfinite(step)&&step>0;
        for(size_t i=2;regular&&i<axis.size();++i)regular=std::isfinite(axis[i])&&std::abs((axis[i]-axis[i-1])-step)<1e-6*std::max(1.0,std::abs(step));
        if(regular)set.samplingAxis.sampleStep=step;
    }
    if(declaredNull)out.declaredNullValues.push_back(nullValue);
    well.wellbore.logSets.push_back(std::move(set));out.wells.push_back(std::move(well));
    if(options.depthUnit.empty()&&units[depth].isEmpty())addIssue(out,"DEPTH_UNIT_UNKNOWN","LAS 深度单位未声明",0,false);
    finish(out,QStringLiteral("读取 LAS %1 个样点、%2 条曲线").arg(rows.size()).arg(columns.size()));return out;
}
}

std::string canonicalWellName(const std::string& text) {
    QString raw=q(text).trimmed().toUpper();const QString token=wellToken(raw);if(!token.isEmpty())return s(token);
    if(raw.endsWith(QStringLiteral("井")))raw.chop(1);return s(raw.trimmed());
}
ParsedFile parseTextFile(const std::string& path,const ParseOptions& options,const CancelCheck& cancelled) {
    ParsedFile out;out.path=path;out.kind="text";
    if(cancelled&&cancelled())return fail(std::move(out),"解析已取消");
    QFile file(q(path));if(!file.open(QIODevice::ReadOnly))return fail(std::move(out),file.errorString());
    if(file.size()==0)return fail(std::move(out),"文件为空");
    if(file.peek(64).startsWith("# Petrel well head"))return parsePetrelCoordinates(std::move(out),file,options,cancelled);
    QTextStream input(&file);input.setEncoding(QStringConverter::Utf8);input.setAutoDetectUnicode(true);
    const QString extension=QFileInfo(file).suffix().toLower();
    if(extension=="dat")return parseHorizon(std::move(out),input,options,cancelled);
    if(extension=="las")return parseLas(std::move(out),input,options,cancelled);
    if(extension=="txt"&&(QFileInfo(file).fileName().compare("wells2.txt",Qt::CaseInsensitive)==0||QFileInfo(file).absolutePath().contains(QStringLiteral("井坐标"))))return parseCoordinates(std::move(out),input,options,cancelled);
    const auto head=file.peek(4096);const QChar separator=head.contains('\t')&&!head.contains(',')?QChar('\t'):QChar(',');
    return parseCsv(std::move(out),input,options,cancelled,separator);
}
}
