#include "paleo_data/parser.h"
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QHash>
#include <QXmlStreamReader>

namespace paleo::data {
namespace {
std::string u(const QString& v){return v.toUtf8().toStdString();}
void issue(ParsedFile& p,const std::string& code,const std::string& text,int row=0,bool blocking=false){p.issues.push_back({code,text,"","",row,blocking});}
ParsedFile failed(ParsedFile p,const std::string& error){p.status="failed";p.message=error;p.wells.clear();p.unassignedTimeDepth.clear();p.sourceTables.clear();issue(p,"TIME_DEPTH_ERROR",error,0,true);return p;}
QStringList tokens(const QString& line){QStringList out;static QRegularExpression rx(R"re("([^"]*)"|(\S+))re");auto matches=rx.globalMatch(line);while(matches.hasNext()){auto m=matches.next();out.push_back(m.capturedStart(1)>=0?m.captured(1):m.captured(2));}return out;}
double numeric(const QString& text){bool ok=false;double v=text.toDouble(&ok);return ok&&std::isfinite(v)?v:Missing;}
}
ParsedFile parseTimeDepthFile(const std::string& path,const ParseOptions& options,const CancelCheck& cancelled){
    ParsedFile out;out.path=path;out.kind="time_depth";QFile f(QString::fromUtf8(path));if(!f.open(QIODevice::ReadOnly))return failed(out,u(f.errorString()));
    QTextStream in(&f);QStringList header;QString comments;bool inside=false,ended=false;int row=0;std::string depthUnit=options.depthUnit,timeUnit;
    std::string coordinateSystem=options.coordinateSystem,coordinateUnit=options.coordinateUnit;
    std::string timeReference,mdReference,zType,zReference,zDirection;
    std::vector<double> declaredNulls;
    SourceTable preview;preview.sheetName="时深原文预览（前33物理行，完整数值见原文件）";preview.columnCount=1;
    while(!in.atEnd()){
        auto original=in.readLine();auto line=original.trimmed();++row;if(row<=33){preview.rowCount=row;preview.cells.push_back({row,1,u(original)});}if(cancelled&&cancelled())return failed(out,"时深解析已取消");
        if(row>300)return failed(out,"未找到有效 Petrel checkshots 表头");
        if(line.startsWith('#')){
            comments+=line+'\n';
            const auto key=line.section(':',0,0).mid(1).trimmed().toLower();
            const auto value=line.section(':',1).trimmed();
            if(key=="unit in depth")depthUnit=u(value);
            else if(key=="unit in time")timeUnit=u(value);
            else if(key=="unit in x and y direction")coordinateUnit=u(value);
            else if(key=="coordinate reference system x, y")coordinateSystem=u(value);
            else if(key=="time reference")timeReference=u(value);
            else if(key=="md reference")mdReference=u(value);
            else if(key=="z depth type")zType=u(value);
            else if(key=="z reference")zReference=u(value);
            else if(key=="z positive direction")zDirection=u(value);
            else if(key=="undefined value"||key=="null value"){
                const auto v=numeric(value);if(std::isfinite(v))declaredNulls.push_back(v);
            }
            continue;
        }
        if(line=="BEGIN HEADER"){inside=true;continue;}if(line=="END HEADER"){ended=true;break;}if(inside)header.push_back(line);
    }
    if(!ended)return failed(out,"时深表头不完整");auto column=[&](const QString& name){for(int i=0;i<header.size();++i)if(header[i].compare(name,Qt::CaseInsensitive)==0)return i;return -1;};
    const int md=column("MD"),well=column("Well"),x=column("X"),y=column("Y"),z=column("Z");int time=column("TWT picked");if(time<0)time=column("TWT");std::string timeType="TWT";if(time<0){time=column("OWT");timeType="OWT";}
    if(md<0||well<0||time<0)return failed(out,"Petrel时深表未包含明确 MD、Well 和 TWT/OWT 列");
    QHash<QString,int> byWell;int dataRows=0,invalid=0,negative=0;bool duplicateDepth=false;static const QRegularExpression placeholder("^(WELL[ _]*[0-9]+|UNKNOWN|UNDEFINED)$",QRegularExpression::CaseInsensitiveOption);
    while(!in.atEnd()){
        const auto original=in.readLine();const auto line=original.trimmed();++row;if(row<=33){preview.rowCount=row;preview.cells.push_back({row,1,u(original)});}if(line.isEmpty()||line.startsWith('#'))continue;if(cancelled&&cancelled())return failed(out,"时深解析已取消");
        const auto cells=tokens(line);if(cells.size()!=header.size())return failed(out,"时深记录列数与表头不符，第 "+std::to_string(row)+" 行");++dataRows;
        const auto originalName=u(cells[well]);auto name=QString::fromUtf8(canonicalWellName(originalName));
        if(!byWell.contains(name)){
            WellDataset w;w.wellInfo.wellName=u(name);w.sourceFiles.push_back(path);w.defaultDepthInfo.depthUnit=depthUnit;
            TimeDepthTable t;t.relationName=u(QFileInfo(f).fileName());t.sourceFile=path;
            t.depthInfo.depthType="MD";t.depthInfo.depthUnit=depthUnit;t.depthInfo.mdReference=mdReference;
            t.timeType=timeType;t.timeUnit=timeUnit;t.timeReference=timeReference;
            t.coordinateSystem=coordinateSystem;t.coordinateUnit=coordinateUnit;
            if(z>=0){t.positionDepthInfo.depthUnit=depthUnit;t.positionDepthInfo.depthType=zType;t.positionDepthInfo.tvdssReference=zReference;t.positionDepthInfo.tvdssDirection=zDirection;}
            w.timeDepthRelations.push_back(std::move(t));byWell.insert(name,int(out.wells.size()));out.wells.push_back(std::move(w));
        }
        const auto value=[&](int index){
            if(index<0)return Missing;const auto v=numeric(cells[index]);
            for(const auto nullValue:declaredNulls)if(v==nullValue)return Missing;
            return v;
        };
        auto& pairs=out.wells[size_t(byWell[name])].timeDepthRelations[0].depthTimePairs;
        const double depth=value(md),tm=value(time);if(!std::isfinite(depth)||!std::isfinite(tm))++invalid;if(std::isfinite(tm)&&tm<0)++negative;
        if(!pairs.empty()&&std::isfinite(depth)&&depth<=pairs.back().depthValue)duplicateDepth=true;
        pairs.push_back({depth,tm,value(x),value(y),value(z)});
    }
    if(!dataRows){out.sourceTables.push_back(std::move(preview));out.status="registered";out.message="已识别 Petrel 时深表头，但文件仅含模板、没有数据行；没有生成虚假时深点";out.documentText=u(comments);return out;}
    if(!options.wellNameOverride.empty()){
        if(out.wells.size()!=1){issue(out,"MULTIWELL_OVERRIDE_REJECTED","此文件包含多个井名组，不能把所有组覆盖为一个井名；请逐组关联",0,true);}
        else out.wells[0].wellInfo.wellName=canonicalWellName(options.wellNameOverride);
    }
    std::vector<WellDataset> assigned;for(auto& w:out.wells){const auto name=QString::fromUtf8(w.wellInfo.wellName);if(name.isEmpty()||placeholder.match(name).hasMatch()){auto relation=std::move(w.timeDepthRelations[0]);relation.relationName+=" · 源井名："+(w.wellInfo.wellName.empty()?std::string("空"):w.wellInfo.wellName);out.unassignedTimeDepth.push_back(std::move(relation));}else assigned.push_back(std::move(w));}out.wells=std::move(assigned);
    if(!out.unassignedTimeDepth.empty())issue(out,"TIME_DEPTH_WELL_MISSING",std::to_string(out.unassignedTimeDepth.size())+" 组井名为空或通用占位名（如 Well 1），完整数值已保存为待归井记录，未创建占位井",0,true);
    if(depthUnit.empty())issue(out,"DEPTH_UNIT_UNKNOWN","时深资料未声明深度单位");
    if(timeUnit.empty())issue(out,"TIME_UNIT_UNKNOWN","表头声明了 TWT/OWT，但未写明时间单位；保留数值，不能默认认为是毫秒");
    if(x>=0||y>=0||z>=0){
        if(coordinateSystem.empty()||coordinateUnit.empty())issue(out,"TIME_DEPTH_COORDINATES_REVIEW","沿井 XYZ 已保留，坐标系或 XY 单位尚未确认；不能从井口表或数值大小推断");
        if(z>=0&&(zType.empty()||zReference.empty()||zDirection.empty()))issue(out,"TIME_DEPTH_Z_REVIEW","Z 原值已保留，其深度类型、正方向或参考面尚未确认，不能自动当作 TVDSS");
    }
    issue(out,"TIME_REFERENCE_UNKNOWN","时间正方向、零点及 MD 起算面尚未核实；不自动改变时间符号或转成 TVDSS");
    if(negative)issue(out,"TIME_SIGN_REVIEW",std::to_string(negative)+" 个时间值为负，保留原符号供核对");
    if(invalid)issue(out,"TIME_DEPTH_NULL",std::to_string(invalid)+" 行包含缺失时深值，保留位置，原始文字见文件");
    if(duplicateDepth)issue(out,"TIME_DEPTH_ORDER_REVIEW","同一井的 MD 存在重复或非递增值，保留来源顺序，不自动排序、合并或插值");
    out.sourceTables.push_back(std::move(preview));out.documentText=u(comments);out.status=out.issues.empty()?"parsed":"review";out.message="已解析 Petrel 时深关系，"+std::to_string(out.wells.size())+" 个井名、"+std::to_string(out.unassignedTimeDepth.size())+" 组待归井、"+std::to_string(dataRows)+" 个时深点；各解释文件分别保存";return out;
}
ParsedFile parseSpatialCompanion(const std::string& path,const CancelCheck& cancelled){
    ParsedFile out;out.path=path;out.kind="spatial_metadata";QFile f(QString::fromUtf8(path));if(!f.open(QIODevice::ReadOnly))return failed(out,u(f.errorString()));if(f.size()>4*1024*1024)return failed(out,"空间元信息超过读取预算");auto bytes=f.readAll();QXmlStreamReader xml(bytes);SourceTable t;t.sheetName="空间伴随信息";t.columnCount=2;
    bool rootSeen=false;while(!xml.atEnd()){xml.readNext();if(cancelled&&cancelled())return failed(out,"解析已取消");if(xml.isDTD())return failed(out,"不支持带 DTD 的空间元信息");if(!xml.isStartElement())continue;if(!rootSeen){if(xml.name()!=QStringLiteral("SpatialCompanion"))return failed(out,"不是支持的空间伴随元信息");rootSeen=true;}
        for(const auto& a:xml.attributes()){++t.rowCount;t.cells.push_back({t.rowCount,1,u(xml.name().toString()+"."+a.name().toString())});t.cells.push_back({t.rowCount,2,u(a.value().toString())});}
        if(xml.name()==QStringLiteral("WKT")||xml.name()==QStringLiteral("AuthorityCode")||xml.name()==QStringLiteral("Description")){auto name=xml.name().toString();auto text=xml.readElementText();++t.rowCount;t.cells.push_back({t.rowCount,1,u(name)});t.cells.push_back({t.rowCount,2,u(text)});if(name=="AuthorityCode"&&!text.trimmed().isEmpty())out.coordinateSystems.push_back({u(text.trimmed()),{}});}
    }if(xml.hasError())return failed(out,u(xml.errorString()));out.sourceTables.push_back(std::move(t));out.documentText=u(QString::fromUtf8(bytes));out.status="parsed";out.message="已解析 CRS 空间伴随信息；元文件仅描述空间背景，不据此推断时间单位或时深基准";return out;
}
}
