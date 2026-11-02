#pragma once
#include "store/project_store.h"
#include <QSqlError>
#include <cmath>
#include <limits>
namespace paleo::data::detail {
// Exact picks win, including explicit nulls. Only a complete enclosing local
// grid cell may be interpolated; neither null corners nor outside coverage extend.
class GridSampler {
    QSqlQuery exact_,below_,above_,left_,right_;
    long long horizon_;int radius_;
    double row(int il,int xl,int& lo,int& hi,std::string& error){
        const auto at=[&](QSqlQuery& q,int lo,int hi,int& x){
            q.finish();q.bindValue(0,horizon_);q.bindValue(1,il);q.bindValue(2,lo);q.bindValue(3,hi);
            if(!q.exec()){error=s(q.lastError().text());return Missing;}
            if(!q.next())return Missing;
            x=q.value(0).toInt();return value(q.value(1));
        };
        const double a=at(left_,xl-radius_,xl,lo),b=at(right_,xl,xl+radius_,hi);
        if(!std::isfinite(a)||!std::isfinite(b)||hi-lo>radius_)return Missing;
        return hi==lo?a:a+(b-a)*double(xl-lo)/(hi-lo);
    }
public:
    GridSampler(const ProjectStore& store,long long horizon,int radius)
      :exact_(store.db),below_(store.db),above_(store.db),left_(store.db),right_(store.db),horizon_(horizon),radius_(radius){}
    bool prepare(std::string& error){
        if(exact_.prepare("SELECT z FROM horizon_points INDEXED BY horizon_line WHERE horizon_id=? AND inline_no=? AND crossline_no=? ORDER BY seq,id LIMIT 1")&&
           below_.prepare("SELECT inline_no FROM horizon_points INDEXED BY horizon_line WHERE horizon_id=? AND inline_no BETWEEN ? AND ? ORDER BY inline_no DESC LIMIT 1")&&
           above_.prepare("SELECT inline_no FROM horizon_points INDEXED BY horizon_line WHERE horizon_id=? AND inline_no BETWEEN ? AND ? ORDER BY inline_no LIMIT 1")&&
           left_.prepare("SELECT crossline_no,z FROM horizon_points INDEXED BY horizon_line WHERE horizon_id=? AND inline_no=? AND crossline_no BETWEEN ? AND ? ORDER BY crossline_no DESC,seq,id LIMIT 1")&&
           right_.prepare("SELECT crossline_no,z FROM horizon_points INDEXED BY horizon_line WHERE horizon_id=? AND inline_no=? AND crossline_no BETWEEN ? AND ? ORDER BY crossline_no,seq,id LIMIT 1"))return true;
        error="层位网格查询准备失败";return false;
    }
    double sample(int il,int xl,std::string& error){
        if(il<std::numeric_limits<int>::min()+radius_||il>std::numeric_limits<int>::max()-radius_||
           xl<std::numeric_limits<int>::min()+radius_||xl>std::numeric_limits<int>::max()-radius_){error="层位插值测线编号超出范围";return Missing;}
        exact_.finish();exact_.bindValue(0,horizon_);exact_.bindValue(1,il);exact_.bindValue(2,xl);
        if(!exact_.exec()){error=s(exact_.lastError().text());return Missing;}
        if(exact_.next())return value(exact_.value(0));
        if(!radius_)return Missing;
        const auto line=[&](QSqlQuery& q,int lo,int hi,int& number){
            q.finish();q.bindValue(0,horizon_);q.bindValue(1,lo);q.bindValue(2,hi);
            if(!q.exec()){error=s(q.lastError().text());return false;}
            if(!q.next())return false;
            number=q.value(0).toInt();return true;
        };
        int lo=il,hi=il;
        if(!line(below_,il-radius_,il,lo)||!line(above_,il,il+radius_,hi))return Missing;
        if(hi-lo>radius_)return Missing;
        int leftA=xl,rightA=xl,leftB=xl,rightB=xl;
        const double a=row(lo,xl,leftA,rightA,error);if(lo==hi)return a;
        const double b=row(hi,xl,leftB,rightB,error);
        if(!std::isfinite(a)||!std::isfinite(b)||leftA!=leftB||rightA!=rightB)return Missing;
        return a+(b-a)*double(il-lo)/(hi-lo);
    }
};
}
