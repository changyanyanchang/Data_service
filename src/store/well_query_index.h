#pragma once
#include <QString>
#include <string>
namespace paleo::data::detail {
class ProjectStore;
QString normalizedWellQueryName(const QString& name);
bool ensureWellQueryIndex(ProjectStore& store,bool rebuild,std::string& error);
bool updateWellQueryIndex(const ProjectStore& store,long long id,bool deleted,const QString& name,std::string& error);
bool wellQueryIndexReady(const ProjectStore& store,std::string& error);
}
