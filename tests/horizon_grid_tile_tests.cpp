#include "store/horizon_grid_sampler.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace paleo::data;
using namespace paleo::data::detail;
namespace {
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
bool same(double a,double b){return (std::isnan(a)&&std::isnan(b))||(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<1e-9);}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temp;ProjectStore store;std::string error;
        check(store.open(temp.path(),true,"tile oracle",error),error);
        check(store.exec("INSERT INTO horizons(id,name) VALUES(1,'synthetic')",{},error),error);
        check(store.begin(error),error);int seq=0;
        for(int il=-144;il<=144;il+=9)for(int xl=-152;xl<=152;xl+=8){
            if((il+xl)%7==0)continue;
            check(store.exec("INSERT INTO horizon_points(horizon_id,seq,inline_no,crossline_no,z) VALUES(1,?,?,?,?)",
                {seq++,il,xl,il*.25+xl*.5},error),error);
        }
        for(const auto& row:QList<QVariantList>{{-10,0,0,QVariant()},{-9,0,0,777.},{-8,1,500,999.},{-7,2,-100,17.},{-6,2,100,18.}})
            check(store.exec("INSERT INTO horizon_points(horizon_id,seq,inline_no,crossline_no,z) VALUES(1,?,?,?,?)",row,error),error);
        check(store.commit(error),error);int compared=0;
        for(int radius:{0,8,64,128})for(int direction:{-1,0,1}){
            GridSampler tile(store,1,radius,true,direction),sql(store,1,radius,false);
            check(tile.prepare(error)&&sql.prepare(error),error);unsigned state=7819;
            for(int n=0;n<500;++n){
                state=state*1664525u+1013904223u;const int il=int(state%385)-192;
                state=state*1664525u+1013904223u;const int xl=int(state%385)-192;
                const auto a=tile.sample(il,xl,error),b=sql.sample(il,xl,error);
                check(error.empty(),error);
                check(same(a,b),"tile and SQL differ at "+std::to_string(il)+","+std::to_string(xl)+" radius="+std::to_string(radius));++compared;
            }
            for(const auto& p:{std::pair{0,0},std::pair{-64,63},std::pair{64,-65},std::pair{2,0},std::pair{0,0}})
                check(same(tile.sample(p.first,p.second,error),sql.sample(p.first,p.second,error)),"edge/null/order oracle");
        }
        check(store.exec("INSERT INTO horizons(id,name) VALUES(2,'duplicates')",{},error),error);
        check(store.exec("WITH RECURSIVE n(x) AS (SELECT 0 UNION ALL SELECT x+1 FROM n WHERE x<131073) INSERT INTO horizon_points(horizon_id,seq,inline_no,crossline_no,z) SELECT 2,x,0,0,CASE WHEN x=0 THEN NULL ELSE 10 END FROM n",{},error),error);
        GridSampler fallback(store,2,64),oracle(store,2,64,false);
        check(fallback.prepare(error)&&oracle.prepare(error),error);
        check(same(fallback.sample(0,0,error),oracle.sample(0,0,error)),"oversized local tile falls back without losing explicit null");
        std::cout<<compared<<" tile/SQL comparisons passed, plus boundaries, duplicate/null precedence and bounded fallback\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
