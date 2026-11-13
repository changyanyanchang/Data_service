#include "store/project_store.h"
#include "store/well_query_index.h"

namespace paleo::data {
using namespace detail;
bool DataService::readWellQueryValue(long long id,const std::string& name,WellQueryValue& out,bool& found,std::string& error)const {
    out={};found=false;error.clear();QSqlQuery r;
    if(id<=0&&name.size()>4096){error="InvalidWellQueryBudget";return false;}
    const auto predicate=id>0?QStringLiteral("id=?"):QStringLiteral("name=?");
    // Numeric metadata only. Never fetch remarks, curve lists or other payloads.
    if(!impl_->store.exec("SELECT id,CASE WHEN length(CAST(name AS BLOB))<=4096 THEN name END,total_depth FROM wells WHERE "+predicate,
                         {id>0?QVariant(id):QVariant(q(name))},error,&r))return false;
    if(!r.next())return true;
    if(r.value(1).isNull()){error="WellQueryValueBudgetExceeded";return false;}
    found=true;out.id=r.value(0).toLongLong();out.name=s(r.value(1).toString());out.totalDepth=value(r.value(2));return true;
}
bool DataService::findWellIdsByQueryName(const std::string& name,long long afterId,int limit,
                                      std::vector<long long>& out,bool& more,std::string& error)const {
    out.clear();more=false;error.clear();
    if(limit<1||limit>256||afterId<0||name.size()>4096){error="InvalidWellQueryBudget";return false;}
    if(!wellQueryIndexReady(impl_->store,error))return false;
    const auto key=normalizedWellQueryName(q(name));QSqlQuery query;
    if(!impl_->store.exec("SELECT well_id FROM pds_well_query_names WHERE name_key=? AND well_id>? ORDER BY well_id LIMIT ?",
                         {key,afterId,limit+1},error,&query))return false;
    while(query.next()){if(out.size()==size_t(limit)){more=true;break;}out.push_back(query.value(0).toLongLong());}
    return true;
}
}
