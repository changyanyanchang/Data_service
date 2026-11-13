#pragma once
#include <string>
#include <QString>
namespace paleo::data::detail {
class ProjectStore;
bool ensureWellDataQueryIndex(ProjectStore& store,bool rebuild,std::string& error);
bool wellDataQueryIndexReady(const ProjectStore& store,std::string& error);
QString wellDataPageSql(const std::string& kind,bool filterWell);
}
