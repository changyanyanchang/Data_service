#pragma once
#include "project_store.h"
#include "paleo_data/curve_window.h"

namespace paleo::data::detail {
inline constexpr long long CurveChunkSamples=4096;
bool ensureCurveWindowSchema(ProjectStore&,std::string&);
bool writeCurveAxisWindows(ProjectStore&,long long,const SamplingAxis&,std::string&,const CancelCheck& = {});
bool writeCurveValueWindows(ProjectStore&,long long,long long,const std::vector<double>&,
                           const std::vector<double>*,const QByteArray&,std::string&,const CancelCheck& = {});
bool refreshCurveValueWindows(ProjectStore&,long long,std::string&);
bool upgradeCurveWindows(ProjectStore&,std::string&,const CancelCheck&,long long);
bool readCurveWindows(ProjectStore&,long long,long long,long long,double,double,bool,bool,
                      CurveSlice&,DepthInfo&,std::vector<double>&,std::vector<double>&,
                      long long&,const CurveReadOptions&,CurveReadStats&,std::string&);
}
