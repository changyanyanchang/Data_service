#include "store/project_store.h"
#include <QFileInfo>
#include <map>

namespace paleo::data {
using namespace detail;
namespace {
std::string graphNodeId(const std::string& kind,long long id){return kind+":"+std::to_string(id);}
class GraphBuilder {
public:
    explicit GraphBuilder(const FileGraphOptions& options):options_(options){}
    bool has(const std::string& id)const{return nodes_.find(id)!=nodes_.end();}
    bool addNode(FileGraphNode node,std::string& error){
        if(has(node.id))return true;
        if(static_cast<long long>(nodes_.size())>=options_.maxNodes){error="图谱节点数量超过预算";return false;}
        const auto id=node.id;nodes_.emplace(id,std::move(node));return true;
    }
    bool connect(const std::string& relation,const std::string& from,const std::string& to,
                 const std::string& basis,std::string& error,std::string id={},
                 long long relationId=0,const std::string& note={}){
        if(!has(from)||!has(to))return true;
        if(id.empty())id=relation+":"+from+":"+to;
        if(edges_.find(id)!=edges_.end())return true;
        if(static_cast<long long>(edges_.size())>=options_.maxEdges){error="图谱边数量超过预算";return false;}
        FileGraphEdge edge;edge.id=std::move(id);edge.from=from;edge.to=to;edge.relation=relation;
        edge.basis=basis;edge.relationId=relationId;edge.note=note;edges_.emplace(edge.id,std::move(edge));return true;
    }
    FileGraph finish(){
        FileGraph graph;
        for(auto& item:nodes_)graph.nodes.push_back(std::move(item.second));
        for(auto& item:edges_)graph.edges.push_back(std::move(item.second));
        return graph;
    }
private:
    const FileGraphOptions& options_;
    std::map<std::string,FileGraphNode> nodes_;
    std::map<std::string,FileGraphEdge> edges_;
};
bool hasRelationTable(const ProjectStore& store,bool& exists,std::string& error){
    QSqlQuery rows;exists=false;
    if(!store.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='source_relations'",{},error,&rows))return false;
    exists=rows.next();return true;
}
class ReadSnapshot {
public:
    ReadSnapshot(const ProjectStore& store,std::string& error):store_(store),active_(store.exec("BEGIN",{},error)){}
    ~ReadSnapshot(){if(active_){std::string ignored;store_.exec("ROLLBACK",{},ignored);}}
    bool active()const{return active_;}
private:
    const ProjectStore& store_;
    bool active_;
};
}

bool DataService::relateSources(long long sourceId,long long targetSourceId,const std::string& relation,
                                const std::string& note,long long& relationId,std::string& error){
    relationId=0;error.clear();auto& store=impl_->store;
    const auto type=q(relation).trimmed();const auto description=q(note).trimmed();
    if(store.readOnly){error="只读工区不能修改文件关系";return false;}
    if(sourceId<=0||targetSourceId<=0||sourceId==targetSourceId){error="文件关系需要两个不同的有效来源编号";return false;}
    if(type.isEmpty()||type.size()>64){error="关系类型须为1到64个字符";return false;}
    if(description.size()>4096){error="关系说明不能超过4096个字符";return false;}
    Transaction tx(store,error);if(!tx.active)return false;
    QSqlQuery rows;
    if(!store.exec("SELECT count(DISTINCT id) FROM sources WHERE id IN (?,?)",{sourceId,targetSourceId},error,&rows))return false;
    if(!rows.next()||rows.value(0).toLongLong()!=2){error="文件关系引用了不存在的来源";return false;}
    rows.finish();
    if(!store.exec("INSERT INTO source_relations(source_id,target_source_id,relation,note) VALUES(?,?,?,?) ON CONFLICT(source_id,target_source_id,relation) DO UPDATE SET note=excluded.note",
                   {sourceId,targetSourceId,type,description},error))return false;
    if(!store.exec("SELECT id FROM source_relations WHERE source_id=? AND target_source_id=? AND relation=?",
                   {sourceId,targetSourceId,type},error,&rows)||!rows.next()){if(error.empty())error="文件关系保存失败";return false;}
    const auto id=rows.value(0).toLongLong();rows.finish();
    if(!tx.commit(error))return false;relationId=id;return true;
}

bool DataService::removeSourceRelation(long long relationId,std::string& error){
    error.clear();
    if(impl_->store.readOnly){error="只读工区不能修改文件关系";return false;}
    if(relationId<=0){error="文件关系编号无效";return false;}
    QSqlQuery rows;
    if(!impl_->store.exec("DELETE FROM source_relations WHERE id=?",{relationId},error,&rows))return false;
    if(rows.numRowsAffected()!=1){error="文件关系不存在";return false;}return true;
}

bool DataService::listSourceRelations(std::vector<SourceRelation>& result,std::string& error)const{
    result.clear();error.clear();bool exists=false;
    if(!hasRelationTable(impl_->store,exists,error))return false;
    if(!exists)return true;
    QSqlQuery rows;
    if(!impl_->store.exec("SELECT id,source_id,target_source_id,relation,note FROM source_relations ORDER BY id",{},error,&rows))return false;
    while(rows.next())result.push_back({rows.value(0).toLongLong(),rows.value(1).toLongLong(),rows.value(2).toLongLong(),
                                       s(rows.value(3).toString()),s(rows.value(4).toString())});
    return true;
}

bool DataService::buildFileGraph(const FileGraphOptions& options,FileGraph& result,std::string& error,
                                 const CancelCheck& cancelled)const{
    result={};error.clear();auto& store=impl_->store;
    if(options.maxNodes<=0||options.maxNodes>1000000||options.maxEdges<=0||options.maxEdges>2000000){
        error="图谱规模预算无效";return false;
    }
    ReadSnapshot snapshot(store,error);if(!snapshot.active())return false;
    const auto stopped=[&]{
        if(cancelled&&cancelled()){error="已取消";return true;}return false;
    };
    if(stopped())return false;
    GraphBuilder builder(options);QSqlQuery rows;long long visited=0;
    if(!store.exec("SELECT id,original_path,stored_path,kind,status,bytes,hash FROM sources WHERE (?=1 OR active=1) ORDER BY id",
                   {options.includeInactiveSources?1:0},error,&rows))return false;
    while(rows.next()){
        if(visited++%256==0&&stopped())return false;
        const auto sourceId=rows.value(0).toLongLong();FileGraphNode node;
        node.id=graphNodeId("file",sourceId);node.kind="file";node.refId=sourceId;node.sourceId=sourceId;
        node.path=s(rows.value(1).toString());node.storedPath=s(rows.value(2).toString());
        node.name=s(QFileInfo(rows.value(1).toString()).fileName());if(node.name.empty())node.name=node.path;
        node.status=s(rows.value(4).toString());node.bytes=rows.value(5).toLongLong();node.sha256=s(rows.value(6).toString());
        node.category=s(rows.value(3).toString());
        if(!builder.addNode(std::move(node),error))return false;
    }
    rows.finish();

    if(options.includeObjects){
        std::map<long long,std::string> wellNames;
        if(!store.exec("SELECT id,name FROM wells ORDER BY id",{},error,&rows))return false;
        visited=0;
        while(rows.next()){
            if(visited++%256==0&&stopped())return false;
            wellNames.emplace(rows.value(0).toLongLong(),s(rows.value(1).toString()));
        }
        rows.finish();
        const QString objects=R"SQL(
SELECT source_id,id,'log_set' AS kind,name,well_id FROM log_sets
UNION ALL SELECT source_id,id,'interval_track',name,well_id FROM tracks
UNION ALL SELECT source_id,id,'sample_table',name,well_id FROM sample_tables
UNION ALL SELECT source_id,id,kind,name,well_id FROM well_objects
UNION ALL SELECT source_id,id,'volume',name,NULL FROM volumes
UNION ALL SELECT source_id,id,'horizon',name,NULL FROM horizons
UNION ALL SELECT source_id,id,'map',name,NULL FROM maps
UNION ALL SELECT source_id,id,'table',name,NULL FROM source_tables
ORDER BY source_id,kind,id
)SQL";
        if(!store.exec(objects,{},error,&rows))return false;
        visited=0;
        while(rows.next()){
            if(visited++%256==0&&stopped())return false;
            const auto sourceId=rows.value(0).toLongLong();if(sourceId<=0)continue;
            const auto fileId=graphNodeId("file",sourceId);if(!builder.has(fileId))continue;
            const auto objectId=rows.value(1).toLongLong();const auto kind=s(rows.value(2).toString());
            FileGraphNode object;object.id=graphNodeId(kind,objectId);object.kind=kind;object.refId=objectId;
            object.sourceId=sourceId;object.name=s(rows.value(3).toString());
            if(!builder.addNode(std::move(object),error)||
               !builder.connect("contains",fileId,graphNodeId(kind,objectId),"stored",error))return false;
            const auto wellId=rows.value(4).toLongLong();if(wellId<=0)continue;
            FileGraphNode well;well.id=graphNodeId("well",wellId);well.kind="well";well.refId=wellId;
            well.name=wellNames[wellId];
            if(!builder.addNode(std::move(well),error)||
               !builder.connect("belongs_to",graphNodeId(kind,objectId),graphNodeId("well",wellId),"stored",error))return false;
        }
        rows.finish();
        if(!store.exec("SELECT id,volume_id FROM horizons WHERE volume_id IS NOT NULL ORDER BY id",{},error,&rows))return false;
        visited=0;
        while(rows.next()){
            if(visited++%256==0&&stopped())return false;
            const auto horizonId=graphNodeId("horizon",rows.value(0).toLongLong());
            const auto volumeId=graphNodeId("volume",rows.value(1).toLongLong());
            if(!builder.connect("references",horizonId,volumeId,"stored",error))return false;
        }
        rows.finish();
    }

    bool hasRelations=false;if(!hasRelationTable(store,hasRelations,error))return false;
    if(hasRelations){
        if(!store.exec("SELECT id,source_id,target_source_id,relation,note FROM source_relations ORDER BY id",{},error,&rows))return false;
        visited=0;
        while(rows.next()){
            if(visited++%256==0&&stopped())return false;
            const auto relationId=rows.value(0).toLongLong();
            if(!builder.connect(s(rows.value(3).toString()),graphNodeId("file",rows.value(1).toLongLong()),
                                graphNodeId("file",rows.value(2).toLongLong()),"manual",error,
                                "source_relation:"+std::to_string(relationId),relationId,s(rows.value(4).toString())))return false;
        }
        rows.finish();
    }
    if(stopped())return false;
    FileGraph graph=builder.finish();
    if(!store.exec("SELECT key,value FROM ds_meta WHERE key IN ('project_crs','project_crs_unit')",{},error,&rows))return false;
    while(rows.next()){
        if(rows.value(0).toString()=="project_crs")graph.projectCrs=s(rows.value(1).toString());
        else graph.projectCrsUnit=s(rows.value(1).toString());
    }
    result=std::move(graph);return true;
}
}
