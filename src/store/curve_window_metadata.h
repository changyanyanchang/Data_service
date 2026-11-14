#pragma once
#include "project_store.h"
#include <QStringList>
namespace paleo::data::detail {
struct CurveWindowMetadata {
    long long setId=0,count=0,depthBytes=0,tvdBytes=0,ssBytes=0,rawBytes=0,processedBytes=0,qualityBytes=0;
    bool hasProcessed=false;
};
bool boundedCurveMetadata(ProjectStore &store,long long id,const CurveReadOptions &options,
    CurveReadStats &stats,CurveWindowMetadata &metadata,QStringList &text,std::string &error);
}
