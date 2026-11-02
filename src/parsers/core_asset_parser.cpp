// 岩心原图及附件的路径关联。只读图片头和文件名，不 OCR、不修改原图、不展开压缩包。
#include "paleo_data/parser.h"
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>

namespace paleo::data {
namespace {
QString q(const std::string& value) { return QString::fromUtf8(value); }
std::string s(const QString& value) { return value.toUtf8().toStdString(); }

// 只识别完整的“区块-构造-单井”三段名称；两段油田名、CORE2-1、NMR220 等不是井。
// 当前目录的支线后缀包含 D/SA/DSA/H/P/ST，其他未知后缀不猜；保留 B12H、A8ST3P、A5P1。
const QRegularExpression& wellNamePattern() {
    thread_local const QRegularExpression pattern(QStringLiteral("(?<![A-Z0-9])([A-Z]{1,8}[0-9]{1,4})-([0-9]{1,4})(?:[- ]+|(?=[A-Z]))([A-Z]{0,3}[0-9]{1,4}(?:(?:DSA|SA|ST|D|H|P)[0-9]*)*)(?![A-Z0-9])"));
    return pattern;
}
const QSet<QString> ImageExtensions={"jpg","jpeg","png","bmp","tif","tiff","gif"};

QString normalized(QString text) {
    text=text.toUpper(); text.replace('_','-');
    for (const QChar dash : {QChar(0x2013),QChar(0x2014),QChar(0xff0d)}) text.replace(dash,'-');
    // 这两个井名前缀在当前目录中有中英文两种写法；其他中文名称不猜缩写。
    text.replace(QStringLiteral("西江"),"XJ"); text.replace(QStringLiteral("惠州"),"HZ");
    return text;
}

void issue(ParsedFile& out,const char* code,const QString& message,const QString& original={}) {
    out.issues.push_back({code,s(message),"",s(original),0,false});
}

QStringList wellNames(const QStringList& components) {
    QSet<QString> names;
    for (const QString& component:components) {
        auto matches=wellNamePattern().globalMatch(normalized(component));
        while(matches.hasNext()) {
            const auto match=matches.next();
            // NMR110_220_250_240 是本批核磁测井处理通道组合，不是 NMR110-220-250 井。
            if(match.captured(1).startsWith("NMR")) continue;
            names.insert(match.captured(1)+"-"+match.captured(2)+"-"+match.captured(3));
        }
    }
    QStringList result(names.cbegin(),names.cend()); result.sort(); return result;
}

int coreDirectory(const QStringList& components) {
    for (int i=0;i+1<components.size();++i)
        if(components[i].contains(QStringLiteral("岩心")) || components[i].contains(QStringLiteral("岩芯"))) return i;
    return -1;
}

QString attachmentType(const QString& extension,const QString& name) {
    if(ImageExtensions.contains(extension)) {
        if(name.contains(QStringLiteral("柱状图")) || name.contains(QStringLiteral("岩芯分析")) || name.contains(QStringLiteral("岩心分析"))) return QStringLiteral("岩心图件");
        return QStringLiteral("图片（类型待核对）");
    }
    if(QSet<QString>{"mp4","mov","avi","mkv","wmv"}.contains(extension)) return QStringLiteral("视频（仅登记）");
    if(QSet<QString>{"zip","rar","7z","gz","tar"}.contains(extension)) return QStringLiteral("压缩包（未展开）");
    if(QSet<QString>{"cdr","cgm","emf","wmf","grf"}.contains(extension)) return QStringLiteral("矢量图件（仅登记）");
    if(QSet<QString>{"dlis","dls","xtf","aff","wlp","scg","pds"}.contains(extension)) return QStringLiteral("专有资料（仅登记）");
    if(extension=="pdf") return QStringLiteral("PDF 报告（仅登记）");
    if(QSet<QString>{"doc","docx","ppt","pptx"}.contains(extension)) return QStringLiteral("报告或演示文档");
    return QStringLiteral("原始附件");
}

bool looksLikeCorePhoto(const QStringList& relativeParts,const QString& fileName) {
    // 解释图、柱状图、报告插图没有照片的可靠语义，不能因其位于“岩心”目录就归入照片道。
    thread_local const QRegularExpression chart(QStringLiteral("柱状图|岩芯分析|岩心分析|取芯描述|取心描述|岩心描述|岩芯描述|扫描伽马|ELAN|综合图|附图"),QRegularExpression::CaseInsensitiveOption);
    if(chart.match(fileName).hasMatch()) return false;
    const QString parents=relativeParts.mid(0,relativeParts.size()-1).join('/');
    thread_local const QRegularExpression photoFolder(QStringLiteral("照片|照相|拍摄|扫描照片|(^|/)(ygtx[0-9]*|zqm[0-9]*)($|/)"),QRegularExpression::CaseInsensitiveOption);
    const bool explicitPhotoFolder=photoFolder.match(parents).hasMatch();
    if(!explicitPhotoFolder && (parents.contains(QStringLiteral("报告")) || parents.contains(QStringLiteral("描述")))) return false;
    return explicitPhotoFolder || fileName.contains(QStringLiteral("白光")) || fileName.contains(QStringLiteral("荧光"));
}

QString illumination(const QString& fileName) {
    const bool white=fileName.contains(QStringLiteral("白光")), fluorescence=fileName.contains(QStringLiteral("荧光"));
    if(white && fluorescence) return QStringLiteral("白光与荧光（待核对）");
    if(white) return QStringLiteral("白光");
    if(fluorescence) return QStringLiteral("荧光");
    return {};
}

QString depthUnit(QString text) {
    text=text.trimmed().toLower();
    if(text=="m" || text==QStringLiteral("米")) return "m";
    if(text=="ft" || text==QStringLiteral("英尺")) return "ft";
    return text;
}

// 有单位的区间/单点可以落值。无单位的区间仅保留候选，除非导入者明确给出单位。
// 盒号 1-4、相机时间戳及 1-3810.0 这类“序号-深度”都不会被转换为顶底深。
void photoDepth(CorePhoto& photo,DepthInfo& info,const QString& fileName,const ParseOptions& options,ParsedFile& out) {
    QString label=normalized(QFileInfo(fileName).completeBaseName()); label.remove(wellNamePattern()); label=label.trimmed();
    thread_local const QString number=QStringLiteral("([0-9]+(?:\\.[0-9]+)?)");
    thread_local const QRegularExpression range(QStringLiteral("^\\s*")+number+QStringLiteral("\\s*(M|FT|米|英尺)?\\s*[-~～至]\\s*")+number+QStringLiteral("\\s*(M|FT|米|英尺)?(?![0-9.])"));
    thread_local const QRegularExpression point(QStringLiteral("^\\s*")+number+QStringLiteral("\\s*(M|FT|米|英尺)(?![A-Z])"));
    const auto match=range.match(label);
    double top=Missing,bottom=Missing; QString unit,observed;
    bool validCandidate=false;
    if(match.hasMatch()) {
        top=match.captured(1).toDouble(); bottom=match.captured(3).toDouble();
        const QString leftUnit=depthUnit(match.captured(2)),rightUnit=depthUnit(match.captured(4));
        if(!leftUnit.isEmpty() && !rightUnit.isEmpty() && leftUnit!=rightUnit) {
            issue(out,"CORE_PHOTO_UNIT_CONFLICT",QStringLiteral("文件名区间使用了不同单位；顶底深保持未知。"),fileName);return;
        }
        unit=rightUnit.isEmpty()?leftUnit:rightUnit; observed=match.captured();
        // “4309.16m-1-裂缝”中的 1 是同一深度的照片序号；不能形成 4309.16→1 的区间。
        if(!leftUnit.isEmpty() && rightUnit.isEmpty() && top>=100 && bottom<100 && !match.captured(3).contains('.')) {
            bottom=top; observed=match.captured(1)+match.captured(2);
        }
        // 无单位的短整数区间很可能是箱号；保守限制不会把 1-4 或 1-3810 当成井深。
        validCandidate=!unit.isEmpty() || (top>=100 && bottom>=100 && bottom>=top);
        if(validCandidate && bottom<top) {
            issue(out,"CORE_PHOTO_REVERSED_RANGE",QStringLiteral("文件名底深小于顶深；保留原名，顶底深未自动交换。"),fileName);return;
        }
    } else {
        const auto one=point.match(label);
        if(one.hasMatch()) { top=bottom=one.captured(1).toDouble();unit=depthUnit(one.captured(2));observed=one.captured();validCandidate=true; }
    }
    if(!validCandidate) {
        issue(out,"CORE_PHOTO_DEPTH_UNKNOWN",QStringLiteral("照片名未给出可确认的深度；箱号、照片序号和日期不作为井深。"),fileName);return;
    }
    const QString requested=depthUnit(q(options.depthUnit));
    if(!unit.isEmpty() && !requested.isEmpty() && unit!=requested) {
        issue(out,"CORE_PHOTO_UNIT_CONFLICT",QStringLiteral("照片名单位与导入设置不同；没有自动换算或套用井默认单位，顶底深保持未知。"),fileName);return;
    }
    if(unit.isEmpty()) unit=requested;
    if(unit.isEmpty()) {
        photo.description+=s(QStringLiteral("；文件名数值区间候选：%1（单位未声明，未写入顶底深）").arg(observed));
        issue(out,"CORE_PHOTO_DEPTH_UNIT_UNKNOWN",QStringLiteral("文件名含数值区间，但没有深度单位；在导入设置确认单位后才写入顶底深。"),observed);return;
    }
    info.depthUnit=s(unit); photo.topDepth=top; photo.bottomDepth=bottom;
    photo.description+=s(QStringLiteral("；文件名深度：%1，单位 %2；深度类型和基准待核对").arg(observed,unit));
    issue(out,"CORE_PHOTO_DEPTH_REFERENCE_UNKNOWN",QStringLiteral("顶底深来自照片文件名；MD/TVD 类型和起算面未声明，保持未知。"),observed);
}
}

// 由 parseFile 在主格式解析后调用。保留主解析器已有的表格、文本和 typed 对象。
// 一个文件可补一个照片道（photos 数组）或一个井级附件；合并照片道由查询层按井/道名完成。
void associateCoreAsset(ParsedFile& out,const ParseOptions& options,const CancelCheck& cancelled) {
    if(out.status=="failed" || out.status=="ignored" || !out.wells.empty() || !out.maps.empty() || !out.volumes.empty() || !out.horizons.empty()) return;
    // A filename hint must never override a conflict established from workbook contents.
    if(std::any_of(out.issues.begin(),out.issues.end(),[](const ParseIssue& value){return value.blocking;}))return;
    if(cancelled && cancelled()) { out.status="failed";out.message="关联岩心资料时已取消";return; }
    const QFileInfo file(q(out.path)); // 实际可读文件；可能位于工区 sources 缓存中。
    const QFileInfo context(q(options.sourceContextPath.empty()?out.path:options.sourceContextPath));
    const QStringList parts=QDir::fromNativeSeparators(context.absoluteFilePath()).split('/',Qt::SkipEmptyParts);
    const int root=coreDirectory(parts); if(root<0) return;
    const QStringList relativeParts=parts.mid(root); const QString name=context.fileName(),extension=context.suffix().toLower();
    if(name.startsWith("._") || name==".DS_Store" || name.startsWith("~$") || name.startsWith(".~") || QSet<QString>{"tmp","bak","db"}.contains(extension)) {
        out.status="ignored";out.message="岩心目录中的系统索引、临时文件或备份文件，不作为地学资料导入";return;
    }
    QStringList names=wellNames(relativeParts);
    QString well;
    if(options.wellNameOverride.empty() && name.contains(QStringLiteral("连井"))) {
        out.status="review";out.message="连井资料已登记；不因父目录名称将多井资料强行归到一口井";
        issue(out,"CORE_MULTIWELL_ASSET",QStringLiteral("文件名明确为连井资料；作为外部公共资料保留，关联对象需核对。"),relativeParts.join('/'));return;
    }
    if(!options.wellNameOverride.empty()) {
        well=normalized(q(options.wellNameOverride)).trimmed();if(well.endsWith(QStringLiteral("井")))well.chop(1);
        issue(out,"CORE_WELL_OVERRIDE",QStringLiteral("使用导入时明确指定的井名；原目录与文件名保留供核对。"),relativeParts.join('/'));
    } else if(names.size()==1) well=names.front();
    else {
        out.status="review";
        if(names.isEmpty()) issue(out,"CORE_WELL_UNKNOWN",QStringLiteral("路径没有可确认的井号；登记原文件，不创建占位井。"),relativeParts.join('/'));
        else issue(out,"CORE_WELL_CONFLICT",QStringLiteral("路径出现不同井号 %1；不按多数、最近目录或文件名强行归井。").arg(names.join(QStringLiteral("、"))),relativeParts.join('/'));
        out.message=names.isEmpty()?"岩心原文件已登记，所属井待核对":"原文件已登记，路径井号冲突，未关联到单井";return;
    }
    if(!file.isFile()) {out.status="failed";out.message="岩心资料路径不存在或不是普通文件";return;}
    WellDataset record;record.wellInfo.wellName=s(well);record.sourceFiles.push_back(out.path);
    record.wellInfo.remark="资料关联来自目录/文件名，未经报告正文或图片内容核验";
    issue(out,"CORE_ASSET_PATH_ASSOCIATION",QStringLiteral("所属井来自资料路径；保留原路径，未通过图片 OCR 或报告正文确认。"),relativeParts.join('/'));
    if(ImageExtensions.contains(extension) && looksLikeCorePhoto(relativeParts,name)) {
        QImageReader reader(file.absoluteFilePath());
        if(!reader.canRead() || !reader.size().isValid()) {
            out.status="failed";out.message="岩心照片文件头或图片尺寸无效，未生成照片对象";
            issue(out,"CORE_PHOTO_INVALID_IMAGE",reader.errorString(),relativeParts.join('/'));return;
        }
        CorePhotoTrack track;
        QString group=relativeParts.mid(0,relativeParts.size()-1).join('/');
        const QString mode=illumination(name); if(!mode.isEmpty()) group+=" / "+mode;
        track.trackName=s(group);
        CorePhoto photo;photo.imagePath=out.path;
        photo.description=s(QStringLiteral("原文件：%1；图片头 %2 × %3 像素；分组保留原目录与光照标签，未识别图片内容").arg(relativeParts.join('/')).arg(reader.size().width()).arg(reader.size().height()));
        photoDepth(photo,record.defaultDepthInfo,name,options,out);
        track.photos.push_back(std::move(photo));record.wellbore.corePhotoTracks.push_back(std::move(track));
        out.kind="core_photos";out.status="review";out.message="岩心照片已按路径关联到井；未知深度、单位和基准保留待核对";
    } else {
        Attachment attachment;attachment.attachmentName=s(name);attachment.attachmentType=s(attachmentType(extension,name));attachment.filePath=out.path;
        record.attachments.push_back(std::move(attachment));
        if(out.status=="registered" || out.kind=="attachment") {
            out.status="registered";out.message="原始附件已按路径关联到井；未解析专有格式、PDF 正文、视频帧或压缩包内部数据";
        } else { out.status="review";out.message+="；原始文档已按资料路径关联到井，归属待核对"; }
    }
    out.wells.push_back(std::move(record));
}
}
