#pragma once
#include <QString>
#include <string>
class QJsonObject;
namespace paleo::data::detail {
class ProjectStore;
QString normalizedWellQueryName(const QString& name);
bool ensureWellQueryIndex(ProjectStore& store,bool rebuild,std::string& error);
bool updateWellQueryIndex(const ProjectStore& store,long long id,bool deleted,const QJsonObject& image,std::string& error);
bool wellQueryIndexReady(const ProjectStore& store,std::string& error);
}
