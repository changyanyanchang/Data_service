// 层：数据（文件解析）；不依赖数据库或界面。
#include "paleo_data/parser.h"
#include "adapters/segy_reader.h"
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QStringList>
#include <QtEndian>

namespace paleo::data {
ParsedFile parseTimeDepthFile(const std::string&,const ParseOptions&,const CancelCheck&);
ParsedFile parseSpatialCompanion(const std::string&,const CancelCheck&);
ParsedFile parseSpreadsheetXmlFile(const std::string&,const ParseOptions&,const CancelCheck&);
void associateCoreAsset(ParsedFile&,const ParseOptions&,const CancelCheck&);
namespace {
std::string utf8(const QString& text) { return text.toUtf8().toStdString(); }
ParsedFile problem(const std::string& path, const std::string& kind, const std::string& message) {
    ParsedFile out; out.path=path; out.kind=kind; out.status="failed"; out.message=message;
    out.issues.push_back({"FILE_ERROR",message,"","",0,true}); return out;
}
ParsedFile image(const std::string& path, const ParseOptions& options) {
    const QString file=QString::fromUtf8(path);
    QImageReader reader(file);
    if (!reader.canRead()) return problem(path,"image",utf8(reader.errorString()));
    const QSize size=reader.size();
    if (!size.isValid()) return problem(path,"image","图片尺寸无效，无法登记为可预览图件");
    ParsedFile out; out.path=path; out.kind="map"; out.status="review";
    GeologicalMap map;
    map.mapName=utf8(QFileInfo(file).completeBaseName()); map.imagePath=path; map.sourceFile=path;
    map.coordinateSystem=options.coordinateSystem; map.georeferenceStatus=-1;
    map.description=utf8(QStringLiteral("原始图片 %1 × %2 像素；图名取自文件名，地层与配准资料待核对。").arg(size.width()).arg(size.height()));
    if (QString::fromUtf8(map.mapName).contains(QStringLiteral("沉积相"))) map.mapType="沉积相图";
    // Classification is only a filename hint. Do not fabricate strata, CRS or pixel georeferencing.
    out.maps.push_back(std::move(map));
    out.issues.push_back({"MAP_METADATA_REVIEW","图名和类型来自文件名；地层名称、坐标系及配准信息需核对。","","",0,false});
    out.message="图片已验证并登记，图件语义与配准信息待核对";
    return out;
}
ParsedFile segy(const std::string& path, const ParseOptions& options, const CancelCheck& cancelled) {
    // 与业务读取共用segyio布局解释，避免登记时和实际访问时采用不同偏移/字节序。
    // 检查是有界的：大体只抽查道头，不复制文件、不建立逐道振幅数据库。
    SeismicFileInfo info; std::string error;
    if (!segy_detail::inspectSegyFile(path,info,error,cancelled)) return problem(path,"volume",error);
    ParsedFile out; out.path=path;out.kind="volume";out.status="review";
    SeismicVolume volume; volume.volumeName=utf8(QFileInfo(QString::fromUtf8(path)).completeBaseName()); volume.dataFile=path;volume.fileFormat="SEG-Y";
    volume.geometry.coordinateSystem=options.coordinateSystem;volume.geometry.coordinateUnit=options.coordinateUnit;
    volume.geometry.traceCount=info.traceCount;
    volume.geometry.inlineMin=info.inlineMin;volume.geometry.inlineMax=info.inlineMax;
    volume.geometry.crosslineMin=info.crosslineMin;volume.geometry.crosslineMax=info.crosslineMax;
    volume.sampling.sampleCount=info.sampleCount;volume.sampling.sampleInterval=info.sampleInterval;
    volume.sampling.firstTime=info.firstTime;volume.sampling.timeUnit=info.timeUnit;
    out.documentText=info.textHeader;
    out.volumes.push_back(std::move(volume));
    out.issues.push_back({"SEGY_CONVENTIONS_REVIEW","已用segyio核验固定道长布局；时间轴来自道头。坐标系、OWT/TWT和地质时间基准需对照文本头确认。抽样网格不是逐道完整校验，每次读取会再核对实际道头。",info.geometryStatus,"",0,false});
    for (const auto& warning:info.warnings) out.issues.push_back({"SEGY_INSPECTION_NOTE",warning,"","",0,false});
    out.message="segyio已读取文件信息与有界网格检查；原始地震体仅保留外部路径，按需读取振幅";
    return out;
}
}

ParsedFile parseFile(const std::string& path, const ParseOptions& options, const CancelCheck& cancelled) {
    if (cancelled && cancelled()) return problem(path,"unknown","解析已取消");
    const QFileInfo info(QString::fromUtf8(path));
    const QString name=info.fileName(), ext=info.suffix().toLower();
    ParsedFile ignored;ignored.path=path;ignored.kind="auxiliary";ignored.status="ignored";
    if (name.startsWith("._") || name==".DS_Store" || name.compare("Thumbs.db",Qt::CaseInsensitive)==0 || name.startsWith("~$") || name.startsWith(".~") || ext=="tmp" || ext=="downloading" || info.absoluteFilePath().replace('\\','/').contains("/resources/background_clothing_conf/")) {
        ignored.message="伴随元数据、临时下载或资源图标文件，不作为地学资料导入";return ignored;
    }
    if (!info.isFile()) return problem(path,"unknown","路径不存在或不是普通文件");
    if(name.endsWith(".crsmeta.xml",Qt::CaseInsensitive))return parseSpatialCompanion(path,cancelled);
    if(ext=="xml")return parseSpreadsheetXmlFile(path,options,cancelled);
    // Petrel checkshots also use their dedicated extension.
    if(ext.isEmpty()||ext=="txt"||ext=="prn"||ext=="vsp"||ext=="txt-uniq1"||ext=="checkshots"){
        QFile probe(info.absoluteFilePath());if(probe.open(QIODevice::ReadOnly)){
            const auto head=probe.read(4096);
            if(head.contains("Petrel checkshots format"))return parseTimeDepthFile(path,options,cancelled);
            if(head.contains("Petrel well head")||head.contains("Petrel well heads"))return parseTextFile(path,options,cancelled);
            // Exported Petrel horizons commonly have only the horizon name, no extension.
            const QRegularExpression horizonHeader("^\\s*INLINE\\s*:\\s*[+-]?[0-9]+\\s+XLINE\\s*:",QRegularExpression::CaseInsensitiveOption);
            if(horizonHeader.match(QString::fromUtf8(head)).hasMatch())return parseTextFile(path,options,cancelled);
        }
    }
    if (ext=="csv" || ext=="txt" || ext=="dat" || ext=="las" || (ext.isEmpty() && info.absolutePath().contains(QStringLiteral("井坐标")))) return parseTextFile(path,options,cancelled);
    if (ext=="sgy" || ext=="segy") return segy(path,options,cancelled);
    if (ext=="png" || ext=="jpg" || ext=="jpeg" || ext=="tif" || ext=="tiff" || ext=="bmp" || ext=="gif") {
        const QString hint=options.sourceContextPath.empty()?info.absoluteFilePath():QString::fromUtf8(options.sourceContextPath);
        if(hint.contains(QStringLiteral("沉积相图"))||hint.contains(QStringLiteral("构造图"))||hint.contains(QStringLiteral("地质图")))return image(path,options);
        // Core photos, fossil plates and report illustrations are attachments, not inferred map objects.
        auto out=parseAuxiliaryFile(path,options,cancelled);associateCoreAsset(out,options,cancelled);return out;
    }
    auto out=parseAuxiliaryFile(path,options,cancelled);associateCoreAsset(out,options,cancelled);return out;
}
}
