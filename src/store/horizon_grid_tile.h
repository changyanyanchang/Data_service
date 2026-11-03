#pragma once
#include "store/project_store.h"
#include <QSqlError>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace paleo::data::detail {
// One bounded local tile per sampler; no full-surface scan or shared mutable state.
// A query and every neighbouring interpolation pick are inside the padded tile.
class HorizonGridTile {
    struct Pick {int xl;double z;};
    using Row=std::vector<Pick>;
    QSqlQuery query_;
    std::map<int,Row> rows_;
    long long horizon_;int radius_,sideIl_,sideXl_;
    long long tileIl_=0,tileXl_=0;bool loaded_=false,complete_=false;
    static constexpr int Limit=131072;
    static long long base(int n,int side){const long long v=n;return (v<0?(v-side+1)/side:v/side)*side;}
    bool rowValue(const Row& row,int xl,double& z)const{
        const auto right=std::lower_bound(row.begin(),row.end(),xl,[](const Pick& p,int x){return p.xl<x;});
        if(right==row.end()||(right==row.begin()&&right->xl>xl))return false;
        const auto left=right->xl==xl?right:std::prev(right);
        if(left->xl<xl-radius_||right->xl>xl+radius_||right->xl-left->xl>radius_)return false;
        const double a=left->z,b=right->z;
        z=std::isfinite(a)&&std::isfinite(b)?(left==right?a:a+(b-a)*double(xl-left->xl)/(right->xl-left->xl)):Missing;
        return true; // Explicit-null support still wins over a more distant row.
    }
public:
    HorizonGridTile(const ProjectStore& store,long long horizon,int radius,int direction):query_(store.db),horizon_(horizon),radius_(radius),
        sideIl_(direction>0?1:direction<0?512:64),sideXl_(direction<0?1:direction>0?512:64){}
    bool prepare(std::string& error){
        // Equality on inline lets the composite index bound BOTH dimensions.
        // OFFSET 0 keeps the bounded inner query from flattening into an unbounded sort.
        if(query_.prepare("WITH RECURSIVE lines(n) AS (SELECT ? UNION ALL SELECT n+1 FROM lines WHERE n<?) SELECT inline_no,crossline_no,z FROM (SELECT p.inline_no,p.crossline_no,p.z,p.seq,p.id FROM lines CROSS JOIN horizon_points AS p INDEXED BY horizon_line WHERE p.horizon_id=? AND p.inline_no=lines.n AND p.crossline_no BETWEEN ? AND ? LIMIT 131073 OFFSET 0) ORDER BY inline_no,crossline_no,seq,id"))return true;
        error=s(query_.lastError().text());return false;
    }
    // false with no error means oversized/duplicate-heavy data: use the exact SQL path.
    bool load(int il,int xl,std::string& error){
        const auto bi=base(il,sideIl_),bx=base(xl,sideXl_);if(loaded_&&bi==tileIl_&&bx==tileXl_)return complete_;
        rows_.clear();loaded_=true;complete_=false;tileIl_=bi;tileXl_=bx;
        const auto bound=[](long long v){return std::clamp(v,static_cast<long long>(std::numeric_limits<int>::min()),static_cast<long long>(std::numeric_limits<int>::max()));};
        query_.finish();query_.bindValue(0,qlonglong(bound(bi-radius_)));query_.bindValue(1,qlonglong(bound(bi+sideIl_-1+radius_)));
        query_.bindValue(2,horizon_);query_.bindValue(3,qlonglong(bound(bx-radius_)));query_.bindValue(4,qlonglong(bound(bx+sideXl_-1+radius_)));
        if(!query_.exec()){error=s(query_.lastError().text());return false;}
        int count=0;
        while(query_.next()){
            if(++count>Limit){rows_.clear();query_.finish();return false;}
            auto& row=rows_[query_.value(0).toInt()];const int x=query_.value(1).toInt();
            if(row.empty()||row.back().xl!=x)row.push_back({x,value(query_.value(2))}); // seq/id precedence, including null.
        }
        if(query_.lastError().isValid()){error=s(query_.lastError().text());query_.finish();return false;}
        query_.finish();complete_=true;return true;
    }
    double sample(int il,int xl)const{
        if(const auto exact=rows_.find(il);exact!=rows_.end()){
            const auto at=std::lower_bound(exact->second.begin(),exact->second.end(),xl,[](const Pick& p,int x){return p.xl<x;});
            if(at!=exact->second.end()&&at->xl==xl)return at->z;
        }
        if(!radius_)return Missing;
        auto below=rows_.upper_bound(il);int lo=il,hi=il;double a=Missing,b=Missing;bool haveLow=false,haveHigh=false;
        while(below!=rows_.begin()){
            --below;if(below->first<il-radius_)break;
            if(rowValue(below->second,xl,a)){lo=below->first;haveLow=true;break;}
        }
        for(auto above=rows_.lower_bound(il);above!=rows_.end()&&above->first<=il+radius_;++above)
            if(rowValue(above->second,xl,b)){hi=above->first;haveHigh=true;break;}
        if(!haveLow||!haveHigh||hi-lo>radius_||!std::isfinite(a)||!std::isfinite(b))return Missing;
        return hi==lo?a:a+(b-a)*double(il-lo)/(hi-lo);
    }
};
}
