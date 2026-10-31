// 层：文件解析；无数据库和界面依赖。
#include "paleo_data/parser.h"
#include "xls_formula_cache.h"
#include "../../vendor/freexl/freexl-2.0.0/headers/freexl.h"
#include "../../vendor/paleo_io/ziparchive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace paleo::data {
namespace {
QString qs(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
std::string ss(const QString& s) { return s.toUtf8().toStdString(); }
bool stopped(const CancelCheck& c) { return c && c(); }
void issue(ParsedFile& out, const char* code, const QString& text, const QString& sheet={}, int row=0, bool blocking=false, const QString& original={}) {
    out.issues.push_back({code,ss(text),ss(sheet),ss(original),row,blocking});
}
QString key(QString v) { v.remove(QRegularExpression(QStringLiteral("[\\s\\r\\n]"))); return v.toLower(); }
QString excelColumn(int c) {
    QString out;
    while(c>0) { --c; out.prepend(QChar(u'A'+c%26)); c/=26; }
    return out;
}
QString rangeAddress(int row, int firstColumn, int lastColumn) {
    return excelColumn(firstColumn)+QString::number(row)+":"+excelColumn(lastColumn)+QString::number(row);
}
bool numeric(QString text, double& result) {
    text=text.trimmed();
    if(text.endsWith('%')) text.chop(1);
    static const QRegularExpression valid(QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$"));
    if(!valid.match(text).hasMatch()) return false;
    bool ok=false; result=text.toDouble(&ok);
    return ok && std::isfinite(result);
}
bool depthRange(QString text, double& top, double& bottom) {
    text=text.trimmed();
    static const QRegularExpression re(QStringLiteral("^([0-9]+(?:\\.[0-9]+)?)\\s*(?:[-~～—–至]\\s*([0-9]+(?:\\.[0-9]+)?))?\\s*(?:m|米)?$"),QRegularExpression::CaseInsensitiveOption);
    const auto m=re.match(text);
    if(!m.hasMatch()) return false;
    top=m.captured(1).toDouble(); bottom=m.captured(2).isEmpty()?top:m.captured(2).toDouble();
    return std::isfinite(top) && std::isfinite(bottom) && bottom>=top;
}
QSet<QString> wellNames(const QString& value) {
    // Development-well suffixes such as B1, A5P1 and A8ST3P are part of the ID.
    static const QRegularExpression re(QStringLiteral("(?<![A-Z0-9])(?:HZ|XJ|LF|PY|LH|WC|EP|BY)[-_ ]?[0-9]+[-_][0-9]+[A-Z]*(?:[-_][A-Z]*[0-9]+[A-Z0-9]*| +[A-Z]+[0-9]+[A-Z0-9]*)"),QRegularExpression::CaseInsensitiveOption);
    QSet<QString> names;
    auto matches=re.globalMatch(value);
    while(matches.hasNext()) {auto name=matches.next().captured().toUpper().replace('_','-');name.replace(QRegularExpression("(?<=[0-9]) +(?=[A-Z])"),"-");names.insert(name.remove(' '));}
    return names;
}
QString pathWell(const QString& path) {
    QFileInfo f(path);
    auto names=wellNames(f.completeBaseName());
    if(names.size()==1) return *names.begin();
    if(names.size()>1) return {};
    auto dir=f.dir();
    for(int i=0;i<6;++i) {
        names=wellNames(dir.dirName());
        if(names.size()==1) return *names.begin();
        if(names.size()>1) return {};
        if(!dir.cdUp()) break;
    }
    return {};
}

// Public ABI is taken from the bundled, unmodified FreeXL 2.0.0 header.
// The library remains dynamic so the standalone application can ship it alongside the executable.
struct FreeXl {
    QLibrary library;
    using Open=int(*)(const char*,const void**);
    using Close=int(*)(const void*);
    using Count=int(*)(const void*,unsigned int*);
    using Name=int(*)(const void*,unsigned short,const char**);
    using Select=int(*)(const void*,unsigned short);
    using Dimensions=int(*)(const void*,unsigned int*,unsigned short*);
    using Cell=int(*)(const void*,unsigned int,unsigned short,FreeXL_CellValue*);
    Open openXls=nullptr;
    Close close=nullptr;
    Count count=nullptr;
    Name name=nullptr;
    Select select=nullptr;
    Dimensions dimensions=nullptr;
    Cell cell=nullptr;
    bool load(QString& error) {
        QStringList candidates;
        const auto configured=qEnvironmentVariable("PALEO_FREEXL_LIBRARY");
        if(!configured.isEmpty()) candidates << configured;
        candidates << QDir(QCoreApplication::applicationDirPath()).filePath("paleo_freexl")
                   << QDir(QCoreApplication::applicationDirPath()).filePath("freexl") << "freexl";
        for(const auto& candidate:candidates) {
            library.setFileName(candidate);
            if(library.load()) break;
        }
        if(!library.isLoaded()) {error="FreeXL 2.x 未加载："+library.errorString();return false;}
        openXls=reinterpret_cast<Open>(library.resolve("freexl_open"));
        close=reinterpret_cast<Close>(library.resolve("freexl_close"));
        count=reinterpret_cast<Count>(library.resolve("freexl_get_worksheets_count"));
        name=reinterpret_cast<Name>(library.resolve("freexl_get_worksheet_name"));
        select=reinterpret_cast<Select>(library.resolve("freexl_select_active_worksheet"));
        dimensions=reinterpret_cast<Dimensions>(library.resolve("freexl_worksheet_dimensions"));
        cell=reinterpret_cast<Cell>(library.resolve("freexl_get_cell_value"));
        if(!openXls||!close||!count||!name||!select||!dimensions||!cell) {error="FreeXL 接口不完整，需要 2.x 版本";return false;}
        return true;
    }
};

bool xmlOkay(QXmlStreamReader& xml,QString& error) {
    if(xml.hasError()) {error="XML 读取失败："+xml.errorString();return false;}
    return true;
}
bool address(const QString& value,int& row,int& col) {
    static const QRegularExpression re("^([A-Za-z]+)([1-9][0-9]*)$");
    const auto m=re.match(value);if(!m.hasMatch())return false;
    col=0;for(auto c:m.captured(1).toUpper()) {col=col*26+c.unicode()-u'A'+1;if(col>16384)return false;}
    bool ok=false;row=m.captured(2).toInt(&ok);return ok&&row<=1048576;
}
// XLSX is XML inside ZIP. Reading physical references avoids FreeXL limitations
// with some OOXML producers and preserves blank but declared worksheet extents.
bool officeTextEntries(QFile& file,QVector<paleo::data::zip::ZipEntry>& entries,const CancelCheck& cancel,QString& error,bool workbook=false);
bool extractFileEntry(QFile& file,const paleo::data::zip::ZipEntry& entry,QByteArray& xml,QString& error);
bool readXlsx(QFile& file,const paleo::data::zip::ZipListResult& list,ParsedFile& out,const CancelCheck& cancel,QString& error) {
    auto extract=[&](const QString& path,QByteArray& xml)->bool {
        const auto* entry=list.entry(path);
        if(!entry) {error="工作簿缺少条目："+path;return false;}
        return extractFileEntry(file,*entry,xml,error);
    };
    QByteArray data;
    QVector<QString> strings;
    if(list.contains("xl/sharedStrings.xml")) {
        if(!extract("xl/sharedStrings.xml",data))return false;
        QXmlStreamReader xml(data);QString current;bool inString=false;quint64 tokens=0;
        while(!xml.atEnd()) {
            const auto t=xml.readNext();
            if((++tokens%256)==0&&stopped(cancel)) {error="已取消";return false;}
            if(t==QXmlStreamReader::DTD) {error="不支持工作簿中的 DTD";return false;}
            if(t==QXmlStreamReader::StartElement&&xml.name()==u"si") {current.clear();inString=true;}
            else if(inString&&t==QXmlStreamReader::StartElement&&xml.name()==u"t") current+=xml.readElementText();
            else if(t==QXmlStreamReader::EndElement&&xml.name()==u"si") {strings.push_back(current);inString=false;if(strings.size()>1000000){error="共享文本超过解析预算";return false;}}
        }
        if(!xmlOkay(xml,error))return false;
    }
    if(!extract("xl/_rels/workbook.xml.rels",data))return false;
    QMap<QString,QString> targets;
    {
        QXmlStreamReader xml(data);
        while(!xml.atEnd()) {
            const auto t=xml.readNext();
            if(t==QXmlStreamReader::DTD) {error="不支持工作簿中的 DTD";return false;}
            if(t==QXmlStreamReader::StartElement&&xml.name()==u"Relationship") {
                const auto a=xml.attributes();
                if(a.value("TargetMode")==u"External") continue;
                auto target=a.value("Target").toString().replace('\\','/');
                target=target.startsWith('/')?target.mid(1):QDir::cleanPath("xl/"+target);
                if(target.startsWith("../")||target.contains("/../")||target.contains(':')) {error="工作表路径超出容器目录";return false;}
                targets[a.value("Id").toString()]=target;
            }
        }
        if(!xmlOkay(xml,error))return false;
    }
    if(!extract("xl/workbook.xml",data))return false;
    QVector<QPair<QString,QString>> sheets;
    {
        QXmlStreamReader xml(data);
        while(!xml.atEnd()) {
            const auto t=xml.readNext();
            if(t==QXmlStreamReader::DTD) {error="不支持工作簿中的 DTD";return false;}
            if(t==QXmlStreamReader::StartElement&&xml.name()==u"sheet") {
                QString id;
                for(const auto& a:xml.attributes())if(a.name()==u"id")id=a.value().toString();
                if(!targets.contains(id)) {error="工作表关联不存在";return false;}
                sheets.push_back({xml.attributes().value("name").toString(),targets[id]});
            }
        }
        if(!xmlOkay(xml,error))return false;
    }
    if(sheets.isEmpty()||sheets.size()>2048) {error="工作表数量无效或超出预算";return false;}
    quint64 cellBudget=0;
    for(const auto& sheet:sheets) {
        if(stopped(cancel)) {error="已取消";return false;}
        if(!extract(sheet.second,data))return false;
        SourceTable result;result.sheetName=ss(sheet.first);
        QXmlStreamReader xml(data);int physicalRow=0,nextColumn=1;quint64 tokens=0;bool formulaMissing=false;
        while(!xml.atEnd()) {
            const auto t=xml.readNext();
            if((++tokens%256)==0&&stopped(cancel)) {error="已取消";return false;}
            if(t==QXmlStreamReader::DTD) {error="不支持工作簿中的 DTD";return false;}
            if(t!=QXmlStreamReader::StartElement) continue;
            if(xml.name()==u"dimension") {
                auto ref=xml.attributes().value("ref").toString().section(':',-1);int r,c;
                if(address(ref,r,c)) {result.rowCount=r;result.columnCount=c;}
            } else if(xml.name()==u"row") {
                const auto ref=xml.attributes().value("r").toString();bool ok=false;
                const int r=ref.toInt(&ok);physicalRow=ok?r:physicalRow+1;nextColumn=1;
                if(physicalRow<=0||physicalRow>1048576) {error="工作表行号无效";return false;}
            } else if(xml.name()==u"c") {
                const auto a=xml.attributes();int row=physicalRow,col=nextColumn;
                const auto reference=a.value("r").toString();
                if(!reference.isEmpty()&&!address(reference,row,col)) {error="单元格位置无效："+reference;return false;}
                if(row<=0||col<=0||col>16384) {error="单元格位置无效";return false;}
                nextColumn=col+1;
                QString value,inlineText,formula;const auto type=a.value("t").toString();
                while(!xml.atEnd()) {
                    const auto child=xml.readNext();
                    if(child==QXmlStreamReader::DTD) {error="不支持工作簿中的 DTD";return false;}
                    if(child==QXmlStreamReader::StartElement&&xml.name()==u"v") value=xml.readElementText();
                    else if(child==QXmlStreamReader::StartElement&&xml.name()==u"t") inlineText+=xml.readElementText();
                    else if(child==QXmlStreamReader::StartElement&&xml.name()==u"f") formula=xml.readElementText();
                    else if(child==QXmlStreamReader::EndElement&&xml.name()==u"c")break;
                }
                if(type=="s"&&!value.isEmpty()) {bool ok=false;const auto index=value.toInt(&ok);if(!ok||index<0||index>=strings.size()){error="共享文本索引无效";return false;}value=strings[index];}
                else if(type=="inlineStr") value=inlineText;
                else if(type=="b"&&!value.isEmpty()) value=value=="1"?"TRUE":"FALSE";
                if(value.isEmpty()&&!formula.isEmpty()) {value="="+formula;formulaMissing=true;}
                result.rowCount=std::max(result.rowCount,row);result.columnCount=std::max(result.columnCount,col);
                if(!value.isEmpty()) {
                    if(++cellBudget>4000000) {error="工作簿超过 400 万有效单元格读取预算";return false;}
                    result.cells.push_back({row,col,ss(value)});
                }
            }
        }
        if(!xmlOkay(xml,error))return false;
        if(formulaMissing)issue(out,"formula_without_cached_value","部分公式没有缓存计算结果，保留公式文字，未计算",sheet.first);
        out.sourceTables.push_back(std::move(result));
    }
    return true;
}

bool readWorkbook(const QString& path, ParsedFile& out, const CancelCheck& cancel, QString& error) {
    constexpr qint64 maxBytes=64LL*1024*1024;
    constexpr quint64 maxCells=4000000;
    QFileInfo info(path);
    if(info.suffix().compare("xlsx",Qt::CaseInsensitive)==0) {
        QFile file(path);if(!file.open(QIODevice::ReadOnly)) {error=file.errorString();return false;}
        paleo::data::zip::ZipListResult list;
        if(!officeTextEntries(file,list.entries,cancel,error,true))return false;
        return readXlsx(file,list,out,cancel,error);
    }
    if(info.size()>maxBytes) {error="XLS 工作簿超过 64 MiB 读取预算；请分表导入";return false;}
    FreeXl api;
    if(!api.load(error)) return false;
    // C-runtime fopen used by FreeXL is not reliably UTF-8 on Windows. Copy through
    // QFile to a temporary ASCII basename without changing or renaming the source.
    QTemporaryDir temp(QDir::tempPath()+"/paleo-sheet-XXXXXX");
    if(!temp.isValid()) {error="无法创建工作簿读取临时目录";return false;}
    const auto suffix=info.suffix().toLower();
    const auto local=temp.filePath("input."+suffix);
    if(!QFile::copy(path,local)) {error="无法准备工作簿读取副本";return false;}
    const void* handle=nullptr;
    const auto bytes=QFile::encodeName(local);
    int rc=api.openXls(bytes.constData(),&handle);
    auto close=[&](){ if(handle) {api.close(handle);handle=nullptr;} };
    if(rc!=FREEXL_OK) {close();error=QString("工作簿解析失败，FreeXL 错误码 %1").arg(rc);return false;}
    unsigned int sheets=0;
    if(api.count(handle,&sheets)!=FREEXL_OK||sheets>2048) {close();error="工作表数量无效或超出预算";return false;}
    quint64 scanned=0;
    for(unsigned int s=0;s<sheets;++s) {
        if(stopped(cancel)) {close();error="已取消";return false;}
        const char* rawName=nullptr;
        if(api.name(handle,static_cast<unsigned short>(s),&rawName)!=FREEXL_OK||api.select(handle,static_cast<unsigned short>(s))!=FREEXL_OK) {close();error="读取工作表失败";return false;}
        unsigned int rows=0; unsigned short columns=0;
        if(api.dimensions(handle,&rows,&columns)!=FREEXL_OK) {close();error="工作表尺寸无效";return false;}
        // FreeXL implementation reports counts (its header's historical zero-based note is stale).
        const quint64 size=quint64(rows)*columns;
        if(rows>1048576||columns>16384||size>maxCells||scanned+size>maxCells) {close();error="工作簿超过 400 万单元格读取预算";return false;}
        scanned+=size;
        SourceTable table;
        table.sheetName=rawName?rawName:std::to_string(s+1);
        table.rowCount=int(rows);table.columnCount=int(columns);
        for(unsigned int r=0;r<rows;++r) {
            if((r%64)==0 && stopped(cancel)) {close();error="已取消";return false;}
            for(unsigned int c=0;c<columns;++c) {
                FreeXL_CellValue value{};
                if(api.cell(handle,r,static_cast<unsigned short>(c),&value)!=FREEXL_OK) {close();error="读取单元格失败";return false;}
                QString text;
                switch(value.type) {
                case FREEXL_CELL_NULL: continue;
                case FREEXL_CELL_INT:text=QString::number(value.value.int_value);break;
                case FREEXL_CELL_DOUBLE:text=QString::number(value.value.double_value,'g',17);break;
                case FREEXL_CELL_TEXT:case FREEXL_CELL_SST_TEXT:case FREEXL_CELL_DATE:case FREEXL_CELL_DATETIME:case FREEXL_CELL_TIME:
                    if(value.value.text_value) text=QString::fromUtf8(value.value.text_value);break;
                default: continue;
                }
                if(!text.isEmpty()) table.cells.push_back({int(r)+1,int(c)+1,ss(text)});
            }
        }
        out.sourceTables.push_back(std::move(table));
    }
    close();
    return supplementXlsFormulaCells(path,out,cancel,error);
}

using Rows=QMap<int,QMap<int,QString>>;
Rows rowsOf(const SourceTable& table) {
    Rows result;
    for(const auto& cell:table.cells) result[cell.row][cell.column]=qs(cell.text);
    return result;
}
QString at(const Rows& rows,int row,int col) {
    const auto it=rows.constFind(row);
    return it==rows.cend()?QString():it->value(col);
}
QString header(const Rows& rows,int first,int last,int col) {
    QStringList parts;
    for(int r=first;r<=last;++r) {
        const QString v=at(rows,r,col).trimmed();
        if(v.isEmpty()) continue;
        double n; if(numeric(v,n)) continue;
        if(!parts.contains(v)) parts << v;
    }
    return parts.join(" / ");
}
QString measurementUnit(const QString& label,const QString& sourceText) {
    if(label.contains("ppm",Qt::CaseInsensitive)||label.contains("μg/g")||label.contains("ug/g")) return "ppm";
    if(label.contains('%')||sourceText.trimmed().endsWith('%')) return "%";
    if(label.contains(QStringLiteral("粒/g"))) return "粒/g";
    if(label.contains(QStringLiteral("粒"))) return "粒";
    if(label.contains("mg/g",Qt::CaseInsensitive)) return "mg/g";
    for(const auto& unit:QStringList{"mD/cP","psi/min","g/cm3","gm/cc","g/cc","MPa","psi","mD","md","degC"})
        if(label.contains(unit)) return unit;
    return {};
}
QString depthUnit(const QString& label,const ParseOptions& options) {
    if(!options.depthUnit.empty()) return qs(options.depthUnit);
    if(label.contains(QStringLiteral("米"))||QRegularExpression("(?:[(（]\\s*m\\s*[)）]|[深度]\\s*m$|(?:^| / |/)m$)",QRegularExpression::CaseInsensitiveOption).match(label).hasMatch()) return "m";
    if(QRegularExpression("(?:[(（]\\s*ft\\s*[)）])",QRegularExpression::CaseInsensitiveOption).match(label).hasMatch()) return "ft";
    return {};
}
WellDataset& wellFor(ParsedFile& out,const QString& name) {
    const auto normalized=ss(name);
    for(auto& w:out.wells) if(w.wellInfo.wellName==normalized) return w;
    out.wells.emplace_back();
    out.wells.back().wellInfo.wellName=normalized;
    out.wells.back().sourceFiles.push_back(out.path);
    return out.wells.back();
}
bool isDepthHeader(const QString& t) {
    const auto k=key(t);
    // A horizontal form label is not a column header: its grain-size bins or
    // pressure steps below must never become fictitious sample depths.
    if(k.endsWith(':')||k.endsWith(QStringLiteral("："))) return false;
    return k=="depth"||k.startsWith("depth(")||k.startsWith(QStringLiteral("depth（"))||k=="md"||k.startsWith("md(")||k==QStringLiteral("井深")||k==QStringLiteral("井深m")||k.startsWith(QStringLiteral("井深("))||k.startsWith(QStringLiteral("井深（"))||k==QStringLiteral("深度")||k==QStringLiteral("深度m")||k==QStringLiteral("深度/m")||k==QStringLiteral("深度点")||k==QStringLiteral("岩样深度")||k.startsWith(QStringLiteral("深度("))||k.startsWith(QStringLiteral("深度（"))||k.startsWith(QStringLiteral("取样深度"))||k==QStringLiteral("取样井段");
}
bool matches(const QString& t,const QStringList& keys) {const auto k=key(t);for(const auto& word:keys) if(k==word||k.startsWith(word+"(")||k.startsWith(word+QStringLiteral("（"))) return true;return false;}
struct Block {int row=0,depth=0,bottom=0,separator=0,name=0,well=0,type=0,end=0,start=0; bool interval=false;};

int wellheadCoordinateAxis(QString label) {
    label=key(label);
    if(label.startsWith(QStringLiteral("井口")))label.remove(0,2);
    else if(label.startsWith("wellhead"))label.remove(0,8);
    const auto parts=label.split(QRegularExpression(QStringLiteral("[()（）/,，]")),Qt::SkipEmptyParts);
    int literalAxis=0,semanticAxis=0;
    for(int i=0;i<parts.size();++i) {
        const auto& part=parts[i];
        if(QStringList{"m","ft","米","英尺","degree","degrees","度"}.contains(part)) {
            if(i==0)return 0;
            continue; // A unit label alone does not confirm the coordinate reference.
        }
        const int literal=part=="x"?1:part=="y"?2:0;
        if(literal) {if(literalAxis)return 0;literalAxis=literal;continue;}
        int semantic=0;
        if(QStringList{"横坐标","东坐标","easting","east"}.contains(part))semantic=1;
        else if(QStringList{"纵坐标","北坐标","northing","north"}.contains(part))semantic=2;
        if(!semantic||semanticAxis)return 0;
        semanticAxis=semantic;
    }
    // Match complete labels, not substrings: 井底横坐标/横坐标位移/X(轨迹)
    // describe other positions. Preserve X(纵坐标)/Y(横坐标) axis conventions.
    return semanticAxis?semanticAxis:literalAxis;
}

int mapCoordinates(ParsedFile& out,const ParseOptions& options,const CancelCheck& cancel,QSet<QString>& handled,QSet<QString>* declaredWells=nullptr) {
    int mapped=0;QSet<QString> blocked;
    for(const auto& table:out.sourceTables) {
        Rows headers;for(const auto& cell:table.cells)if(cell.row<=60)headers[cell.row][cell.column]=qs(cell.text);
        const auto sheet=qs(table.sheetName);
        for(auto header=headers.cbegin();header!=headers.cend();++header) {
            int name=0,x=0,y=0;bool ambiguous=false,depth=false;
            for(auto c=header->cbegin();c!=header->cend();++c) {
                const auto label=key(c.value());
                if(isDepthHeader(label)||matches(label,{"tvd","tvdss","顶深","底深"}))depth=true;
                if(matches(label,{"井号","井名","well","wellname","well_name"})) {if(name)ambiguous=true;name=c.key();}
                const int axis=wellheadCoordinateAxis(label);
                if(axis==1){if(x)ambiguous=true;x=c.key();}
                if(axis==2){if(y)ambiguous=true;y=c.key();}
            }
            if(!name||!x||!y||depth)continue; // A trajectory/sample position is not a wellhead.
            handled.insert(sheet);
            if(ambiguous){issue(out,"coordinate_columns_ambiguous","井位表存在重复坐标或井名列，保留原表待核对",sheet,header.key(),true);break;}
            QSet<QString> seen;
            const auto rows=rowsOf(table);
            for(auto r=rows.upperBound(header.key());r!=rows.cend();++r) {
                if((r.key()%128)==0&&stopped(cancel))return -1;
                const auto wellName=qs(canonicalWellName(ss(r->value(name))));
                if(wellName.isEmpty())continue;
                if(declaredWells)declaredWells->insert(wellName);
                double vx=Missing,vy=Missing;
                const bool duplicate=seen.contains(wellName);seen.insert(wellName);
                if(duplicate||!numeric(r->value(x),vx)||!numeric(r->value(y),vy)) {
                    blocked.insert(wellName);
                    for(auto& well:out.wells)if(qs(well.wellInfo.wellName)==wellName)well.wellInfo.wellheadX=well.wellInfo.wellheadY=Missing;
                    issue(out,"coordinate_row_invalid","井位重复或坐标无效，未生成可定位坐标",sheet,r.key(),true,wellName);
                    continue;
                }
                auto& info=wellFor(out,wellName).wellInfo;
                if(blocked.contains(wellName))continue;
                if(std::isfinite(info.wellheadX)&&(info.wellheadX!=vx||info.wellheadY!=vy)) {
                    info.wellheadX=info.wellheadY=Missing;blocked.insert(wellName);
                    issue(out,"coordinate_conflict","多个工作表的井位不一致，保留原表待核对",sheet,r.key(),true,wellName);continue;
                }
                info.wellheadX=vx;info.wellheadY=vy;info.coordinateSystem=options.coordinateSystem;info.coordinateUnit=options.coordinateUnit;++mapped;
            }
            if(options.coordinateSystem.empty()||options.coordinateUnit.empty())issue(out,"coordinate_reference_unknown","井口坐标已按明确表头读取；坐标系或单位未声明，使用前需确认",sheet,header.key());
            break;
        }
    }return mapped;
}

QString workbookWell(ParsedFile& out,const ParseOptions& options,const QSet<QString>& ignored,const QSet<QString>& coordinateWells) {
    if(!options.wellNameOverride.empty()) return qs(options.wellNameOverride).trimmed().toUpper();
    const auto fromPath=pathWell(qs(options.sourceContextPath.empty()?out.path:options.sourceContextPath));QSet<QString> declared;
    for(const auto& table:out.sourceTables) {
        if(ignored.contains(qs(table.sheetName)))continue;
        const auto rows=rowsOf(table);
        for(auto row=rows.cbegin();row!=rows.cend()&&row.key()<=60;++row) for(auto c=row->cbegin();c!=row->cend();++c) {
            const auto label=key(c.value());
            if(!label.startsWith(QStringLiteral("井号："))&&!label.startsWith(QStringLiteral("井号:"))&&label!=QStringLiteral("井号")) continue;
            auto names=wellNames(c.value());
            if(names.isEmpty()) for(int step=1;step<=3;++step) {names=wellNames(at(rows,row.key(),c.key()+step));if(!names.isEmpty())break;}
            // Also audit an explicit well-name column even when the rest of
            // this table has no supported business mapping (e.g. NMR spectra).
            if(names.isEmpty()&&label==QStringLiteral("井号")) {
                for(auto data=rows.upperBound(row.key());data!=rows.cend();++data) {
                    if(key(data->value(c.key()))==QStringLiteral("井号"))break;
                    names.unite(wellNames(data->value(c.key())));
                }
            }
            declared.unite(names);
        }
    }
    // A single-well coordinate page can identify the other pages. Keep every
    // declared row name (even invalid XY), so a multi-well table never becomes
    // a single-well fallback just because only one location parsed successfully.
    if(coordinateWells.size()==1)declared.unite(coordinateWells);
    if(!fromPath.isEmpty()) {
        for(const auto& name:declared) if(name!=fromPath) issue(out,"well_conflict","工作簿井号与文件或目录井号不一致；请核对原资料后指定井名重新导入",{},0,true,name+" / "+fromPath);
        return fromPath;
    }
    return declared.size()==1?*declared.begin():QString();
}

// Only recognized series headers become curves/surveys. A number-only sheet
// or a second, ambiguous depth axis remains available as an original table.
int mapWellSeries(ParsedFile& out,const ParseOptions& options,const CancelCheck& cancel,const QString& well,QSet<QString>& handledSheets) {
    int mapped=0;
    for(const auto& table:out.sourceTables) {
        if(stopped(cancel)) return -1;
        const auto rows=rowsOf(table);const auto sheet=qs(table.sheetName);
        for(auto r=rows.cbegin();r!=rows.cend()&&r.key()<=60;++r) {
            int depth=0,depthCount=0,inclination=0,azimuth=0;bool gamma=false;
            QMap<int,std::pair<QString,QString>> curves;
            for(auto c=r->cbegin();c!=r->cend();++c) {
                const auto name=key(c.value());
                if(isDepthHeader(c.value())) {depth=c.key();++depthCount;}
                if(name.startsWith("incl(")||name.startsWith(QStringLiteral("井斜角("))||name.startsWith(QStringLiteral("井斜角（"))) inclination=c.key();
                if(name.startsWith("azimgrid(")||name.startsWith("azim(")||name.startsWith(QStringLiteral("方位角("))||name.startsWith(QStringLiteral("方位角（"))) azimuth=c.key();
                QString unit;
                if(name=="totalgamma"||name=="totalgamapi"||name=="gr"||name==QStringLiteral("伽马")) {gamma=true;if(name.endsWith("api"))unit="API";}
                else if(name=="totalgamma-uranium"||name=="uranfreegamapi") {if(name.endsWith("api"))unit="API";}
                else if(name=="k"||name=="kfraction") {if(name=="kfraction")unit="Fraction";}
                else if(name=="u"||name=="uppm"||name=="th"||name=="thppm") {if(name.endsWith("ppm"))unit="ppm";}
                else continue;
                const auto below=at(rows,r.key()+1,c.key()).trimmed();
                if(QStringList{"api","ppm","fraction","%"}.contains(below.toLower()))unit=below;
                curves[c.key()]={c.value().trimmed(),unit};
            }
            const bool survey=inclination&&azimuth&&depth;
            if(!depth||(!gamma&&!survey)) continue;
            handledSheets.insert(sheet);
            if(depthCount!=1) {issue(out,"multiple_depth_axes","曲线表包含多个深度轴（含归位前后深度）；原表已保留，请确认采用哪一列",sheet,r.key(),true);break;}
            if(well.isEmpty()) {issue(out,"well_mapping_needed","识别到曲线或测斜表，但井归属不明确；请指定井名后导入",sheet,r.key(),true);break;}
            const auto unit=depthUnit(header(rows,r.key(),r.key()+1,depth),options);
            if(survey) {
                const auto inclLabel=at(rows,r.key(),inclination),azimLabel=at(rows,r.key(),azimuth);
                if((!inclLabel.contains(QChar(0x00b0))&&!inclLabel.contains(QStringLiteral("度")))||(!azimLabel.contains(QChar(0x00b0))&&!azimLabel.contains(QStringLiteral("度")))) {issue(out,"angle_unit_unknown","测斜角度单位未确认，保留原表；未假定为度",sheet,r.key(),true);break;}
                TrajectorySurvey data;data.surveyName=table.sheetName;
                if(key(azimLabel).contains("grid"))data.azimuthReference="grid north";
                for(auto row=rows.upperBound(r.key());row!=rows.cend();++row) {
                    if((row.key()%128)==0&&stopped(cancel))return -1;
                    SurveyPoint point;
                    if(!numeric(row->value(depth),point.measuredDepth))continue;
                    if(!numeric(row->value(inclination),point.inclinationDegrees)||!numeric(row->value(azimuth),point.azimuthDegrees)||point.measuredDepth<0||point.inclinationDegrees<0||point.inclinationDegrees>180||point.azimuthDegrees<0||point.azimuthDegrees>=360) {issue(out,"invalid_survey_point","测斜点角度或井深无效，原单元格保留待核对",sheet,row.key());continue;}
                    data.surveyPoints.push_back(point);
                }
                if(!data.surveyPoints.empty()) {
                    auto& w=wellFor(out,well);
                    if(!w.defaultDepthInfo.depthUnit.empty()&&!unit.isEmpty()&&w.defaultDepthInfo.depthUnit!=ss(unit)) {issue(out,"depth_unit_conflict","测斜表深度单位与井内其他表不一致，保留原表待核对",sheet,r.key(),true);break;}
                    if(w.defaultDepthInfo.depthUnit.empty())w.defaultDepthInfo.depthUnit=ss(unit);
                    mapped+=int(data.surveyPoints.size());w.wellbore.trajectories.push_back(std::move(data));
                }
            } else {
                LogSet data;data.logSetName=table.sheetName+" / 岩心伽马";data.sourceFile=out.path;data.sourceSheet=table.sheetName;data.depthInfo.depthUnit=ss(unit);
                for(auto c=curves.cbegin();c!=curves.cend();++c) {LogCurve curve;curve.curveName=ss(c->first);curve.curveUnit=ss(c->second);curve.description="岩心伽马扫描；原始列 "+ss(excelColumn(c.key()));data.logCurves.push_back(std::move(curve));}
                for(auto row=rows.upperBound(r.key());row!=rows.cend();++row) {
                    if((row.key()%128)==0&&stopped(cancel))return -1;
                    double d;
                    if(!numeric(row->value(depth),d))continue;
                    data.samplingAxis.depthValues.push_back(d);int cindex=0;
                    for(auto c=curves.cbegin();c!=curves.cend();++c,++cindex) {double v=Missing;numeric(row->value(c.key()),v);data.logCurves[cindex].sampleValues.push_back(v);}
                }
                if(data.samplingAxis.depthValues.size()>=2) {
                    const auto& depths=data.samplingAxis.depthValues;const double delta=depths[1]-depths[0];bool regular=std::isfinite(delta)&&delta!=0;
                    for(size_t i=2;i<depths.size()&&regular;++i)regular=std::abs(depths[i]-depths[i-1]-delta)<=1e-6*std::max(1.0,std::abs(delta));
                    if(regular)data.samplingAxis.sampleStep=std::abs(delta);
                    mapped+=int(data.logCurves.size());wellFor(out,well).wellbore.logSets.push_back(std::move(data));
                } else issue(out,"curve_table_incomplete","曲线表不足两个有效深度点，已保留原表",sheet,r.key());
            }
            if(unit.isEmpty())issue(out,"depth_unit_unknown","曲线或测斜表未明确深度单位，保留原值；使用前需核对",sheet,r.key());
            break;
        }
    }
    return mapped;
}

// The current grain-size workbook is one report card per sample, repeated
// vertically. Its depth is a label/value pair, not a depth column. Map only
// explicitly labelled results; keep the whole original card in SourceTable.
int mapGrainCards(ParsedFile& out,const ParseOptions& options,const CancelCheck& cancel,const QString& well,QSet<QString>& handledSheets) {
    int mapped=0;
    for(const auto& table:out.sourceTables) {
        const auto rows=rowsOf(table);QVector<int> starts;
        for(auto row=rows.cbegin();row!=rows.cend();++row)for(const auto& value:row.value())
            if(key(value).contains(QStringLiteral("粒度图像分析测定报告"))) {starts.push_back(row.key());break;}
        if(starts.isEmpty())continue;
        handledSheets.insert(qs(table.sheetName));
        if(well.isEmpty()) {issue(out,"well_mapping_needed","粒度报告已读取，但井归属不明确",qs(table.sheetName),0,true);continue;}
        SampleTable target;target.tableName=table.sheetName;target.sourceFile=out.path;target.sourceSheet=table.sheetName;
        QString unit;
        for(qsizetype i=0;i<starts.size();++i) {
            if(stopped(cancel))return -1;
            const int first=starts[i],last=i+1<starts.size()?starts[i+1]-1:table.rowCount;
            const auto right=[&](int r,int c) {for(int n=c+1;n<=std::min(c+4,table.columnCount);++n) {const auto v=at(rows,r,n).trimmed();if(!v.isEmpty())return v;}return QString();};
            SampleRecord sample;sample.sourceRange=ss("A"+QString::number(first)+":"+excelColumn(table.columnCount)+QString::number(last));
            auto add=[&](const QString& name,const QString& value,const QString& u=QString()) {
                if(value.isEmpty())return;
                Measurement item;item.measurementName=ss(name);item.measurementUnit=ss(u);item.originalText=ss(value);numeric(value,item.numericValue);sample.measurements.push_back(std::move(item));
            };
            for(auto row=rows.lowerBound(first);row!=rows.cend()&&row.key()<=last;++row)for(auto cell=row->cbegin();cell!=row->cend();++cell) {
                auto label=key(cell.value());label.remove(':');label.remove(QStringLiteral("："));
                if(label==QStringLiteral("样品号"))sample.sampleNumber=ss(right(row.key(),cell.key()));
                else if(label==QStringLiteral("样品类型"))sample.sampleType=ss(right(row.key(),cell.key()));
                else if(label.startsWith(QStringLiteral("井深"))) {numeric(right(row.key(),cell.key()),sample.topDepth);unit=depthUnit(cell.value(),options);}
                else if(label==QStringLiteral("颗粒总数")||label==QStringLiteral("岩石定名")) add(cell.value().trimmed(),right(row.key(),cell.key()));
                else if(label.startsWith(QStringLiteral("c值"))||label.startsWith(QStringLiteral("m值"))) add(cell.value().trimmed(),right(row.key(),cell.key()),label.contains("mm")?"mm":"");
                else if(label==QStringLiteral("粒径区间")&&key(at(rows,row.key()+1,cell.key()))=="mm") {
                    const int c=cell.key();
                    for(int r=row.key()+2;r<=last;++r) {
                        double ordinal;if(!numeric(at(rows,r,c-1),ordinal))break;
                        const auto interval=at(rows,r,c).trimmed(),phi=at(rows,r,c+1).trimmed();
                        const auto prefix=QStringLiteral("粒径区间 ")+interval+QStringLiteral(" mm / ")+phi+QStringLiteral(" φ / ");
                        for(int offset:{2,3}) {const auto name=at(rows,row.key()+1,c+offset).trimmed();if(!name.isEmpty())add(prefix+name,at(rows,r,c+offset).trimmed(),name.contains('%')?"%":"");}
                    }
                } else if(label==QStringLiteral("粒级分布")) {
                    for(int c=cell.key()+1;c<=table.columnCount;++c) {const auto name=at(rows,row.key(),c).trimmed();if(!name.isEmpty())add(QStringLiteral("粒级分布 / ")+name+" / "+at(rows,row.key()+1,c).trimmed(),at(rows,row.key()+2,c).trimmed());}
                } else if(label==QStringLiteral("矩法")||label==QStringLiteral("图解法")) {
                    for(int c=cell.key()+1;c<table.columnCount;c+=2) {const auto name=at(rows,row.key(),c).trimmed();if(!name.isEmpty())add(cell.value().trimmed()+" / "+name,at(rows,row.key(),c+1).trimmed());}
                }
            }
            if(!std::isfinite(sample.topDepth)||sample.sampleNumber.empty()||sample.measurements.empty()) {issue(out,"grain_card_incomplete","粒度卡片缺少可靠的样号、深度或结果，保留原表待核对",qs(table.sheetName),first);continue;}
            sample.bottomDepth=sample.topDepth;target.samples.push_back(std::move(sample));
        }
        if(!target.samples.empty()) {
            auto& w=wellFor(out,well);
            if(!unit.isEmpty()&&!w.defaultDepthInfo.depthUnit.empty()&&w.defaultDepthInfo.depthUnit!=ss(unit)) {issue(out,"depth_unit_conflict","粒度报告深度单位冲突，保留原表待核对",qs(table.sheetName),0,true);continue;}
            if(w.defaultDepthInfo.depthUnit.empty())w.defaultDepthInfo.depthUnit=ss(unit);
            mapped+=int(target.samples.size());w.wellbore.sampleTables.push_back(std::move(target));
        }
    }
    return mapped;
}

int mapSamples(ParsedFile& out,const ParseOptions& options,const CancelCheck& cancel,const QSet<QString>& handledSheets,const QString& fileWell) {
    int mapped=0;
    QSet<QString> reported;
    for(const auto& table:out.sourceTables) {
        if(stopped(cancel)) return -1;
        if(handledSheets.contains(qs(table.sheetName))) continue;
        const auto rows=rowsOf(table);
        if(rows.isEmpty()) {issue(out,"worksheet_no_cells","未读出单元格；工作表可能仅包含图表或绘图对象",qs(table.sheetName));continue;}
        const bool sampleContext=QRegularExpression(QStringLiteral("元素|孢粉|古生物|分析|样品|岩屑|统计|岩心|岩芯|薄片|物性|孔渗|核磁|测压")).match(qs(options.sourceContextPath.empty()?out.path:options.sourceContextPath)+qs(table.sheetName)).hasMatch();
        std::vector<Block> blocks;
        QSet<int> seenDepthColumns;
        for(auto ri=rows.cbegin();ri!=rows.cend()&&ri.key()<=60;++ri) {
            for(auto ci=ri->cbegin();ci!=ri->cend();++ci) {
                const bool sample=isDepthHeader(ci.value());
                const bool interval=matches(ci.value(),{"顶深","顶界深度","顶界井深"});
                if((!sample&&!interval)||seenDepthColumns.contains(ci.key())) continue;
                if(sample&&!sampleContext) continue;
                Block b; b.row=ri.key();b.depth=ci.key();b.interval=interval;b.end=table.columnCount;b.start=1;
                for(auto hi=ri->cbegin();hi!=ri->cend();++hi) {
                    if(matches(hi.value(),{"井号","井名","well"})) b.well=hi.key();
                    if(matches(hi.value(),interval?QStringList{"层号","层名","层位","岩性"}:QStringList{"样品编号","样品号","样号","岩样号","编号","序号","no"})) b.name=hi.key();
                    if(matches(hi.value(),{"样品类型","样品性质"})) b.type=hi.key();
                    if(interval && hi.key()>b.depth && !b.bottom && matches(hi.value(),{"底深","底界深度","底界井深"})) b.bottom=hi.key();
                }
                if(interval&&!b.bottom) continue;
                // A recognizable physical-depth column must be followed by at least two valid rows.
                int good=0;
                for(int r=b.row+1;r<=std::min(table.rowCount,b.row+18);++r) {double top,bottom;if(depthRange(at(rows,r,b.depth),top,bottom))++good;}
                if(good<2) continue;
                seenDepthColumns.insert(b.depth);blocks.push_back(b);
            }
        }
        if(blocks.empty()) {issue(out,"table_mapping_needed","已保留全部单元格；未识别到可靠的深度表头，需核对表块和字段映射",qs(table.sheetName));continue;}
        std::sort(blocks.begin(),blocks.end(),[](const Block&a,const Block&b){return a.depth<b.depth;});
        for(size_t i=0;i<blocks.size();++i) {
            if(i) blocks[i].start=blocks[i].name>blocks[i-1].depth&&blocks[i].name<blocks[i].depth?blocks[i].name:blocks[i].depth;
            if(i+1<blocks.size()) blocks[i].end=blocks[i+1].name>blocks[i].depth&&blocks[i+1].name<blocks[i+1].depth?blocks[i+1].name-1:blocks[i+1].depth-1;
        }
        for(auto b:blocks) {
            int firstData=0;
            for(int r=b.row+1;r<=std::min(table.rowCount,b.row+18);++r) {double t,z;if(depthRange(at(rows,r,b.depth),t,z)) {firstData=r;break;}}
            if(!firstData) continue;
            b.name=0;b.well=0;b.type=0;
            // Scope identities to this physical table block, never to an adjacent table.
            for(int r=b.row;r<firstData;++r) for(int c=b.start;c<=b.end;++c) {
                const auto v=at(rows,r,c);
                if(matches(v,{"井号","井名","well"})) b.well=c;
                if(matches(v,b.interval?QStringList{"层号","层名","层位","岩性"}:QStringList{"样品编号","样品号","样号","岩样号","编号","序号","no"})) b.name=c;
                if(matches(v,{"样品类型","样品性质"})) b.type=c;
            }
            const QString depLabel=header(rows,b.row,firstData-1,b.depth);
            const auto unit=depthUnit(depLabel,options);
            int separator=0,splitBottom=b.bottom;
            // Some pollen reports physically split a range into B=top, C=～, D=bottom.
            // Require an explicit separator and two agreeing rows before recognizing it.
            if(!b.interval && at(rows,b.row,b.depth+1).isEmpty() && at(rows,b.row,b.depth+2).isEmpty()) {
                int validRanges=0;
                for(int r=firstData;r<=std::min(table.rowCount,firstData+17);++r) {
                    const auto sep=at(rows,r,b.depth+1).trimmed(); double t,z;
                    if(QStringList{"-","~","～","—","–","至"}.contains(sep)&&numeric(at(rows,r,b.depth),t)&&numeric(at(rows,r,b.depth+2),z)&&z>=t) ++validRanges;
                }
                if(validRanges>=2) {separator=b.depth+1;splitBottom=b.depth+2;}
            }
            bool unassignedCells=false,continuationRows=false,alternateHeader=false,reportedAlternate=false;
            QMap<QString,SampleTable> samples;
            QMap<QString,IntervalTrack> intervals;
            for(auto ri=rows.lowerBound(firstData);ri!=rows.cend();++ri) {
                if((ri.key()%64)==0 && stopped(cancel)) return -1;
                double top=Missing,bottom=Missing;
                if(isDepthHeader(at(rows,ri.key(),b.depth))||matches(at(rows,ri.key(),b.depth),{"顶深","顶界深度","顶界井深"})) {
                    bool same=false;
                    for(int headerRow=b.row;headerRow<firstData;++headerRow) {
                        bool equal=true;
                        for(int c=b.start;c<=b.end;++c) if(key(at(rows,headerRow,c))!=key(at(rows,ri.key(),c))) {equal=false;break;}
                        if(equal) {same=true;break;}
                    }
                    if(same) {alternateHeader=false;continue;} // repeated print-page header with unchanged columns
                    alternateHeader=true;
                    if(!reportedAlternate) {issue(out,"repeated_table_header","出现字段不同的表头，该表块保留原单元格；再次遇到原表头后继续识别",qs(table.sheetName),ri.key());reportedAlternate=true;}
                    continue;
                }
                // Thin-section reports alternate quantitative and description
                // blocks for the same samples. A new "样品号 / 定名 / 特征描述"
                // header must not reuse the previous grain-size/unit labels.
                if(b.name&&matches(at(rows,ri.key(),b.name),{"样品编号","样品号","样号","岩样号","编号","序号","no"})) {
                    alternateHeader=true;
                    if(!reportedAlternate) {issue(out,"alternate_table_header","同页存在字段不同的附表；附表保留原单元格，未套用前一个表块的指标名称",qs(table.sheetName),ri.key());reportedAlternate=true;}
                    continue;
                }
                if(alternateHeader)continue;
                if(!depthRange(at(rows,ri.key(),b.depth),top,bottom)) {
                    for(auto c=ri->lowerBound(b.start);c!=ri->cend()&&c.key()<=b.end;++c) {double n;if(numeric(c.value(),n)) continuationRows=true;}
                    continue;
                }
                if(splitBottom && !numeric(at(rows,ri.key(),splitBottom),bottom)) {issue(out,"invalid_depth_range","底深无法识别，保留原单元格待核对",qs(table.sheetName),ri.key());continue;}
                if(bottom<top) {issue(out,"invalid_interval","底深小于顶深，保留原单元格待核对",qs(table.sheetName),ri.key());continue;}
                auto well=fileWell;
                if(b.well>=b.start&&b.well<=b.end && options.wellNameOverride.empty()) {
                    auto names=wellNames(at(rows,ri.key(),b.well));
                    if(names.size()==1) {
                        const auto rowWell=*names.begin();
                        if(!well.isEmpty()&&rowWell!=well&&!reported.contains(rowWell+well)) {reported.insert(rowWell+well);issue(out,"well_conflict","表内井名与文件或目录井名不一致，请确认后重新导入",qs(table.sheetName),ri.key(),true,rowWell+" / "+well);}
                        well=rowWell;
                    }
                }
                if(well.isEmpty()) continue;
                std::vector<Measurement> values;
                for(int col=b.start;col<=b.end;++col) {
                    if(col==b.depth||col==splitBottom||col==separator||col==b.name||col==b.type||col==b.well) continue;
                    const auto text=at(rows,ri.key(),col).trimmed();
                    if(text.isEmpty()) continue;
                    const auto name=header(rows,b.row,std::min(firstData-1,b.row+5),col);
                    if(name.isEmpty()) {unassignedCells=true;continue;} // sourceTables retains the cell
                    Measurement m;
                    m.measurementName=ss(name);m.measurementUnit=ss(measurementUnit(name,text));m.originalText=ss(text);
                    double n;
                    if(numeric(text,n)) m.numericValue=n;
                    values.push_back(std::move(m));
                }
                if(values.empty()) continue;
                if(b.interval) {
                    auto& track=intervals[well];track.trackName=table.sheetName+" / 层段解释";track.trackType="层段解释";track.sourceFile=out.path;track.sourceSheet=table.sheetName;track.depthInfo.depthUnit=ss(unit);
                    GeologicalInterval item;item.intervalName=ss(at(rows,ri.key(),b.name));item.topDepth=top;item.bottomDepth=bottom;item.sourceRow=ri.key();item.properties=std::move(values);track.intervals.push_back(std::move(item));
                } else {
                    auto& target=samples[well];target.tableName=table.sheetName+(blocks.size()>1?" / "+ss(excelColumn(b.start)+":"+excelColumn(b.end)):"");target.sourceFile=out.path;target.sourceSheet=table.sheetName;
                    SampleRecord record;record.sampleNumber=ss(at(rows,ri.key(),b.name));record.sampleType=ss(at(rows,ri.key(),b.type));record.sourceRange=ss(rangeAddress(ri.key(),b.start,b.end));record.topDepth=top;record.bottomDepth=bottom;record.measurements=std::move(values);target.samples.push_back(std::move(record));
                }
            }
            for(auto it=samples.begin();it!=samples.end();++it) {auto& w=wellFor(out,it.key());if(w.defaultDepthInfo.depthUnit.empty()) w.defaultDepthInfo.depthUnit=ss(unit);else if(!unit.isEmpty()&&w.defaultDepthInfo.depthUnit!=ss(unit)) {issue(out,"depth_unit_conflict","同井分析表深度单位不一致，保留原表待核对",qs(table.sheetName),b.row,true);continue;}mapped+=int(it->samples.size());w.wellbore.sampleTables.push_back(std::move(it.value()));}
            for(auto it=intervals.begin();it!=intervals.end();++it) {auto& w=wellFor(out,it.key());mapped+=int(it->intervals.size());w.wellbore.intervalTracks.push_back(std::move(it.value()));}
            if(unit.isEmpty()&&(!samples.isEmpty()||!intervals.isEmpty())) issue(out,"depth_unit_unknown","取样或层段深度单位未明确；原值保留，计算前需核对",qs(table.sheetName),b.row);
            if(unassignedCells) issue(out,"unmapped_columns","部分数据列缺少可靠表头，仅保留原单元格，需核对列映射",qs(table.sheetName),b.row);
            if(continuationRows) issue(out,"continuation_rows","存在无独立深度的数值行，可能是跨行样品或汇总；仅保留原单元格，未自动补填深度",qs(table.sheetName),b.row);
            if(fileWell.isEmpty()&&samples.isEmpty()&&intervals.isEmpty()) issue(out,"well_mapping_needed","找到深度表块，但井归属不明确；请指定井名后重新导入",qs(table.sheetName),b.row,true);
        }
    }
    return mapped;
}

// Rock-core reports can contain hundreds of MiB of photographs. Read only the
// ZIP directory and selected text entries: image/video payloads stay on disk.
// Reuse the shared decompressor and CRC checks for each bounded entry.
bool officeTextEntries(QFile& file,QVector<paleo::data::zip::ZipEntry>& entries,const CancelCheck& cancel,QString& error,bool workbook) {
    const auto fail=[&](const QString& message) {error=message;return false;};
    const auto u16=[](const QByteArray& b,int p) {return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData()+p));};
    const auto u32=[](const QByteArray& b,int p) {return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData()+p));};
    const auto u64=[](const QByteArray& b,int p) {return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData()+p));};
    const quint64 size=quint64(file.size());
    if(size<22) return fail("不是 Office ZIP 容器，或文件被截断");
    const qint64 tailOffset=file.size()-std::min<qint64>(file.size(),65557);
    if(!file.seek(tailOffset)) return fail("无法定位文档目录");
    const QByteArray tail=file.read(65557);
    int end=-1;
    for(int p=int(tail.size())-22;p>=0;--p) {
        if(u32(tail,p)==0x06054b50u&&p+22+u16(tail,p+20)==tail.size()) {end=p;break;}
    }
    if(end<0) return fail("不是 ZIP 容器（没有完整的中央目录结束记录）");
    if(u16(tail,end+4)||u16(tail,end+6)||u16(tail,end+8)!=u16(tail,end+10)) return fail("不支持分卷 Office 文档");
    quint64 count=u16(tail,end+10),directorySize=u32(tail,end+12),directoryOffset=u32(tail,end+16);
    if(count==0xffff||directorySize==0xffffffffu||directoryOffset==0xffffffffu) {
        if(end<20||u32(tail,end-20)!=0x07064b50u||u32(tail,end-16)!=0||u32(tail,end-4)!=1) return fail("ZIP64 文档目录定位失败");
        const auto offset=u64(tail,end-12);
        if(offset>size||size-offset<56||!file.seek(qint64(offset))) return fail("ZIP64 文档目录越界");
        const auto record=file.read(56);
        if(record.size()!=56||u32(record,0)!=0x06064b50u||u32(record,16)||u32(record,20)||u64(record,24)!=u64(record,32)) return fail("ZIP64 文档目录无效");
        count=u64(record,32);directorySize=u64(record,40);directoryOffset=u64(record,48);
    }
    if(count>100000||directorySize>16*1024*1024||directoryOffset>size||directorySize>size-directoryOffset) return fail("文档目录越界或超过读取预算");
    if(!file.seek(qint64(directoryOffset))) return fail("无法读取文档目录");
    const auto directory=file.read(qint64(directorySize));
    if(quint64(directory.size())!=directorySize) return fail("文档目录被截断");
    QSet<QString> names;int offset=0;quint64 selectedBytes=0;
    for(quint64 i=0;i<count;++i) {
        if(stopped(cancel)) return fail("已取消");
        if(directory.size()-offset<46||u32(directory,offset)!=0x02014b50u) return fail("文档目录条目无效");
        const int nameLength=u16(directory,offset+28),extraLength=u16(directory,offset+30),commentLength=u16(directory,offset+32);
        const int length=46+nameLength+extraLength+commentLength;
        if(length>directory.size()-offset) return fail("文档目录条目被截断");
        paleo::data::zip::ZipEntry entry;
        entry.name=QString::fromUtf8(directory.constData()+offset+46,nameLength);
        entry.method=u16(directory,offset+10);entry.crc32=u32(directory,offset+16);
        entry.compressedSize=u32(directory,offset+20);entry.uncompressedSize=u32(directory,offset+24);entry.localHeaderOffset=u32(directory,offset+42);
        entry.encrypted=(u16(directory,offset+8)&1)!=0;
        if(u16(directory,offset+34)) return fail("不支持分卷 Office 条目");
        if(entry.compressedSize==0xffffffffu||entry.uncompressedSize==0xffffffffu||entry.localHeaderOffset==0xffffffffu) {
            int at=offset+46+nameLength;const int limit=at+extraLength;bool resolved=false;
            while(at+4<=limit) {
                const int tag=u16(directory,at),n=u16(directory,at+2);at+=4;
                if(n>limit-at) return fail("ZIP64 条目附加字段被截断");
                if(tag==1) {
                    int field=at;
                    for(auto* value:{&entry.uncompressedSize,&entry.compressedSize,&entry.localHeaderOffset}) {
                        if(*value!=0xffffffffu) continue;
                        if(at+n-field<8) return fail("ZIP64 条目尺寸字段被截断");
                        *value=u64(directory,field);field+=8;
                    }
                    resolved=true;break;
                }
                at+=n;
            }
            if(!resolved) return fail("ZIP64 条目缺少尺寸字段");
        }
        if(names.contains(entry.name)) return fail("文档存在重名 ZIP 条目："+entry.name);
        names.insert(entry.name);
        if(entry.localHeaderOffset>size||size-entry.localHeaderOffset<30) return fail("文档条目偏移越界");
        const bool wanted=workbook?(entry.name=="xl/workbook.xml"||entry.name=="xl/_rels/workbook.xml.rels"||entry.name=="xl/sharedStrings.xml"||QRegularExpression("^xl/worksheets/[^/]+\\.xml$").match(entry.name).hasMatch()):(entry.name=="word/document.xml"||QRegularExpression("^ppt/slides/slide[0-9]+\\.xml$").match(entry.name).hasMatch());
        if(wanted) {
            const quint64 partLimit=workbook?128ULL*1024*1024:16ULL*1024*1024;
            if(entry.compressedSize>partLimit||entry.uncompressedSize>partLimit||entries.size()>=(workbook?2051:1000)) return fail("工作表或文档正文超过解析预算");
            selectedBytes+=entry.uncompressedSize;
            if(selectedBytes>256ULL*1024*1024) return fail("工作表或文档正文总量超过 256 MiB 解析预算");
            entries.push_back(std::move(entry));
        }
        offset+=length;
    }
    return true;
}

bool extractFileEntry(QFile& file,const paleo::data::zip::ZipEntry& entry,QByteArray& xml,QString& error) {
    if(!file.seek(qint64(entry.localHeaderOffset))) {error="无法定位 ZIP 条目";return false;}
    QByteArray local=file.read(30);
    if(local.size()!=30||qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(local.constData()))!=0x04034b50u) {error="ZIP 条目本地头无效";return false;}
    const auto nameLength=qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(local.constData()+26));
    const auto extraLength=qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(local.constData()+28));
    const auto remaining=qint64(nameLength)+extraLength+qint64(entry.compressedSize);
    local+=file.read(remaining);
    if(local.size()!=30+remaining) {error="ZIP 条目被截断";return false;}
    if(QString::fromUtf8(local.constData()+30,nameLength)!=entry.name) {error="ZIP 条目名称与目录不符";return false;}
    auto boundedEntry=entry;boundedEntry.localHeaderOffset=0;
    return paleo::data::zip::zipExtractEntry(local,boundedEntry,&xml,&error);
}

bool readOfficeText(const QString& path,ParsedFile& out,const CancelCheck& cancel,QString& error) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) {error="无法打开文档："+file.errorString();return false;}
    QVector<paleo::data::zip::ZipEntry> entries;
    if(!officeTextEntries(file,entries,cancel,error)) return false;
    QString text;
    bool found=false;
    int parts=0;
    for(const auto& entry:entries) {
        if(stopped(cancel)) {error="已取消";return false;}
        const bool doc=entry.name=="word/document.xml";
        const bool slide=QRegularExpression("^ppt/slides/slide[0-9]+\\.xml$").match(entry.name).hasMatch();
        if(!doc&&!slide) continue;
        if(entry.uncompressedSize>16*1024*1024||++parts>1000) {error="文档正文超过解析预算";return false;}
        QByteArray xml;
        if(!extractFileEntry(file,entry,xml,error)) return false;
        QXmlStreamReader reader(xml);quint64 tokens=0;
        if(slide) text += "\n["+entry.name+"]\n";
        while(!reader.atEnd()) {
            const auto token=reader.readNext();
            if((++tokens%256)==0&&stopped(cancel)) {error="已取消";return false;}
            if(token==QXmlStreamReader::DTD) {error="不支持文档中的 DTD";return false;}
            if(token==QXmlStreamReader::StartElement && reader.name()==u"t") text+=reader.readElementText();
            else if(token==QXmlStreamReader::EndElement && reader.name()==u"p") text+='\n';
            if(text.size()>4000000) {error="文档文字超过 400 万字符预算";return false;}
        }
        if(reader.hasError()) {error=reader.errorString();return false;}
        found=true;
    }
    if(!found) {error="未找到可解析的文档正文";return false;}
    out.documentText=ss(text);
    return true;
}
}

bool mapWellCoordinateTables(ParsedFile& out,const ParseOptions& options,const CancelCheck& cancelled) {
    QSet<QString> handled;const int count=mapCoordinates(out,options,cancelled,handled);
    if(handled.empty())return false;
    if(count<0){out.status="failed";out.message="已取消";out.wells.clear();return true;}
    out.kind="coordinates";out.status=out.issues.empty()?"parsed":"review";
    out.message="按明确表头读取 "+std::to_string(count)+" 条井位；原始单元格保留";
    return true;
}

ParsedFile parseAuxiliaryFile(const std::string& path,const ParseOptions& options,const CancelCheck& cancelled) {
    ParsedFile out;out.path=path;
    const QFileInfo info(qs(path));const auto ext=info.suffix().toLower();
    if(stopped(cancelled)) {out.status="failed";out.message="已取消";return out;}
    if(!info.exists()||!info.isFile()) {out.status="failed";out.message="文件不存在";return out;}
    if(ext=="xls"||ext=="xlsx") {
        out.kind="workbook";QString error;
        if(!readWorkbook(info.filePath(),out,cancelled,error)) {out.status="failed";out.message=ss(error);out.sourceTables.clear();return out;}
        if(stopped(cancelled)) {out.status="failed";out.message="已取消";out.sourceTables.clear();return out;}
        QSet<QString> handledSheets,coordinateWells;
        const int coordinates=mapCoordinates(out,options,cancelled,handledSheets,&coordinateWells);
        const auto well=workbookWell(out,options,handledSheets,coordinateWells);
        const int cards=mapGrainCards(out,options,cancelled,well,handledSheets);
        const int series=cards<0?-1:mapWellSeries(out,options,cancelled,well,handledSheets);
        const int rows=series<0?-1:mapSamples(out,options,cancelled,handledSheets,well);
        const int count=rows<0||coordinates<0?-1:cards+rows+coordinates;
        if(count<0||stopped(cancelled)) {out.status="failed";out.message="已取消";out.sourceTables.clear();out.wells.clear();out.issues.clear();return out;}
        // A warning alone does not stop the store from persisting well objects.
        // Conflicting identities must therefore expose original tables only,
        // until the caller explicitly confirms a well and reparses the file.
        if(options.wellNameOverride.empty()&&std::any_of(out.issues.begin(),out.issues.end(),[](const ParseIssue& i){return i.code=="well_conflict";})) {
            out.wells.clear();out.status="review";
            out.message="已保存 "+std::to_string(out.sourceTables.size())+" 个工作表原单元格；井号相互矛盾，本文件业务数据暂未归井，请确认井名后重新导入";
            return out;
        }
        out.status=out.issues.empty()?"parsed":"review";
        out.message="已保存 "+std::to_string(out.sourceTables.size())+" 个工作表原单元格，识别 "+std::to_string(count)+" 条样品或层段记录、"+std::to_string(series)+" 条曲线或测斜点；公式仅读取保存值，未重新计算";
        if(!count&&!series&&out.issues.empty()) {issue(out,"table_mapping_needed","工作簿单元格已解析，业务字段映射待核对");out.status="review";}
        return out;
    }
    if(ext=="docx"||ext=="pptx") {
        out.kind="document";QString error;
        if(!readOfficeText(info.filePath(),out,cancelled,error)) {out.status="failed";out.message=ss(error);return out;}
        out.status="parsed";out.message="已按需提取正文文字供检索；图片、图版及原排版保留在原文件中";
        if(qs(out.documentText).trimmed().isEmpty()) {out.status="review";out.message="文档正文未发现可检索文字；图像、图表等内容保留在原文件中";issue(out,"document_no_text","未提取到正文文字；未对图片执行 OCR");}
        return out;
    }
    out.kind="attachment";out.status="registered";
    out.message="仅登记原文件；此格式尚未结构化解析，可打开原资料核对";
    return out;
}
}
