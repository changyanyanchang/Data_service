// Business layer. 三个范围接口：井口点 / 地震体 / 工区（两者并集）。
// 并集与约定校验都在服务内完成，调用方只消费结果；不做单位换算与投影。
#include "paleo_data/service.h"
#include <algorithm>
#include <cmath>

namespace paleo::data {
namespace {
bool stopped(const CancelCheck& cancel,std::string& error){
    if(cancel&&cancel()){error="工区范围读取已取消";return true;}
    return false;
}
// 累加点：首点初始化，之后扩展包围盒。非有限坐标不参与，由调用方另行计数。
void grow(PlanarExtent& extent,double x,double y){
    if(!std::isfinite(x)||!std::isfinite(y))return;
    if(!std::isfinite(extent.minX)){extent.minX=extent.maxX=x;extent.minY=extent.maxY=y;return;}
    extent.minX=std::min(extent.minX,x);extent.maxX=std::max(extent.maxX,x);
    extent.minY=std::min(extent.minY,y);extent.maxY=std::max(extent.maxY,y);
}
// 记录来源约定：首个非空声明生效，后续不同声明判冲突。返回 false 表示同一块内不一致。
bool declareConventions(std::string& system,std::string& unit,const std::string& sourceSystem,const std::string& sourceUnit){
    bool consistent=true;
    if(!sourceSystem.empty()){if(system.empty())system=sourceSystem;else consistent&=system==sourceSystem;}
    if(!sourceUnit.empty()){if(unit.empty())unit=sourceUnit;else consistent&=unit==sourceUnit;}
    return consistent;
}
void finish(PlanarExtent& extent){
    extent.valid=std::isfinite(extent.minX)&&std::isfinite(extent.minY)&&std::isfinite(extent.maxX)&&std::isfinite(extent.maxY);
}
// 声明冲突时不留半可信矩形：清空四项，强制调用方按 valid=false 处理。
void discard(PlanarExtent& extent){
    extent.minX=extent.minY=extent.maxX=extent.maxY=Missing;extent.valid=false;extent.pointCount=0;
}
std::string conventionsOf(const PlanarExtent& extent){
    if(extent.coordinateSystem.empty()&&extent.coordinateUnit.empty())return "未声明";
    return extent.coordinateSystem+(extent.coordinateUnit.empty()?std::string():std::string(" / ")+extent.coordinateUnit);
}
}
bool DataService::readWellHeadRange(WellHeadRange& result,std::string& error,const CancelCheck& cancel) const{
    result={};error.clear();try{
        if(stopped(cancel,error))return false;
        // 只取实际坐标，缺坐标的井如实计数，不按 0 或井名补位。
        std::vector<WellInfoRecord> wells;
        if(!listWellInfos(wells,error))return false;
        result.wellCount=static_cast<int>(wells.size());
        bool consistent=true;
        for(const auto& row:wells){
            if(stopped(cancel,error))return false;
            const auto& well=row.value;
            if(!std::isfinite(well.wellheadX)||!std::isfinite(well.wellheadY)){++result.wellsWithoutCoordinates;continue;}
            ++result.wellsWithCoordinates;
            grow(result.extent,well.wellheadX,well.wellheadY);
            consistent&=declareConventions(result.extent.coordinateSystem,result.extent.coordinateUnit,well.coordinateSystem,well.coordinateUnit);
        }
        result.extent.pointCount=result.wellsWithCoordinates;
        finish(result.extent);
        if(!consistent){
            discard(result.extent);
            result.status="conventions-mismatch";
            result.warnings.push_back("井口坐标声明不一致：同一批井出现多种坐标系或单位，井点矩形不予给出。");
        }else if(result.wellsWithCoordinates==0){
            result.status="unavailable";
            result.warnings.push_back(result.wellCount==0?"工区没有登记井。":"登记的"+std::to_string(result.wellCount)+"口井都没有井口坐标，井点矩形不可得。");
        }else{
            result.status="complete";
            if(result.wellsWithoutCoordinates>0)result.warnings.push_back("有"+std::to_string(result.wellsWithoutCoordinates)+"口井缺井口坐标，未参与井点矩形。");
        }
        return true;
    }catch(const std::exception& e){error=std::string("井口点范围读取失败：")+e.what();return false;}
}
bool DataService::readProjectPlanarRange(ProjectPlanarRange& result,std::string& error,const CancelCheck& cancel) const{
    result={};error.clear();try{
        if(stopped(cancel,error))return false;

        // 井位一侧由井口点接口统一给出，调用方与本节共用同一口径。
        WellHeadRange wellRange;
        if(!readWellHeadRange(wellRange,error,cancel))return false;
        result.wells=wellRange.extent;
        result.wellCount=wellRange.wellCount;
        result.wellsWithCoordinates=wellRange.wellsWithCoordinates;
        result.wellsWithoutCoordinates=wellRange.wellsWithoutCoordinates;
        for(const auto& warning:wellRange.warnings)result.warnings.push_back(warning);
        const bool wellMismatch=wellRange.status=="conventions-mismatch";

        // 地震体：逐个取测区范围；不可读或范围不可得的体记入清单，不让整体失败。
        std::vector<DataEntry> volumes;
        if(!listData("volumes",0,volumes,error))return false;
        result.volumeCount=static_cast<int>(volumes.size());
        bool seismicConventions=true;
        for(const auto& volume:volumes){
            if(stopped(cancel,error))return false;
            SeismicCoordinateRange range;
            if(!readSeismicCoordinateRange(volume.id,range,error,cancel)){
                // 取消必须整体中止，不能当成「这一个体范围不可得」继续算。
                if(cancel&&cancel()){error="工区范围读取已取消";return false;}
                result.warnings.push_back("地震体"+std::to_string(volume.id)+"范围读取失败："+error+"；未参与工区范围。");
                result.volumesWithoutRange.push_back(volume.id);
                error.clear();
                continue;
            }
            if(range.status!="complete"){
                result.warnings.push_back("地震体"+std::to_string(volume.id)+"没有可用的平面范围（"+range.status+"）；未参与工区范围。");
                result.volumesWithoutRange.push_back(volume.id);
                continue;
            }
            ++result.volumesWithRange;
            grow(result.seismic,range.minX,range.minY);
            grow(result.seismic,range.maxX,range.maxY);
            seismicConventions&=declareConventions(result.seismic.coordinateSystem,result.seismic.coordinateUnit,range.coordinateSystem,range.coordinateUnit);
        }
        result.seismic.pointCount=4LL*result.volumesWithRange;
        finish(result.seismic);
        if(!seismicConventions){
            discard(result.seismic);
            result.warnings.push_back("地震体坐标声明不一致：同一批地震体出现多种坐标系或单位，地震范围不予给出。");
        }

        // 合并：两侧都可信且约定已确认一致才合并，否则只给各块矩形。
        const bool hasWells=result.wells.valid,hasSeismic=result.seismic.valid;
        if(wellMismatch||!seismicConventions){
            result.status="conventions-mismatch";
            result.warnings.push_back("坐标声明冲突，未合并工区范围；需先统一坐标系与单位。");
        }else if(hasWells&&hasSeismic){
            if(result.wells.coordinateSystem.empty()||result.wells.coordinateUnit.empty()||result.seismic.coordinateSystem.empty()||result.seismic.coordinateUnit.empty()){
                result.status="conventions-unconfirmed";
                result.warnings.push_back("井口坐标（"+conventionsOf(result.wells)+"）与地震坐标（"+conventionsOf(result.seismic)+"）尚未全部确认，未合并工区范围。");
            }else if(result.wells.coordinateSystem!=result.seismic.coordinateSystem||result.wells.coordinateUnit!=result.seismic.coordinateUnit){
                result.status="conventions-mismatch";
                result.warnings.push_back("井口坐标（"+conventionsOf(result.wells)+"）与地震坐标（"+conventionsOf(result.seismic)+"）声明不同，未合并；需先明确转换。");
            }else{
                result.merged=result.wells;
                grow(result.merged,result.seismic.minX,result.seismic.minY);
                grow(result.merged,result.seismic.maxX,result.seismic.maxY);
                result.merged.pointCount=result.wells.pointCount+result.seismic.pointCount;
                finish(result.merged);
                result.status="complete";
            }
        }else if(hasWells){
            result.merged=result.wells;
            result.status="partial";
            result.warnings.push_back("没有可用的地震范围：工区范围暂只由井点矩形给出。");
        }else if(hasSeismic){
            result.merged=result.seismic;
            result.status="partial";
            result.warnings.push_back("没有可用的井口坐标：工区范围暂只由地震范围给出。");
        }else{
            result.status="unavailable";
            result.warnings.push_back("井口坐标与地震范围都不可用，未给出工区范围。");
        }
        return true;
    }catch(const std::exception& e){error=std::string("工区范围读取失败：")+e.what();return false;}
}
}
