#pragma once
#include "paleo_data/service.h"

namespace paleo::data::detail {
// Source declarations win. Missing declarations inherit the selected project
// CRS (default EPSG:32650 / m); this never transforms XY or edits source data.
inline void inheritPlanarCoordinates(std::string& crs,std::string& unit,const ProjectCoordinates& project){
    if(crs.empty())crs=project.coordinateSystem;
    if(unit.empty()&&!crs.empty()&&crs==project.coordinateSystem)unit=project.coordinateUnit;
}
inline bool readPlanarCoordinates(const DataService& service,std::string& crs,std::string& unit,std::string& error){
    ProjectCoordinates project;if(!service.readProjectCoordinates(project,error))return false;
    inheritPlanarCoordinates(crs,unit,project);return true;
}
}
