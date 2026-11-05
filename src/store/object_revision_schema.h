#pragma once
#include <QString>
#include <functional>
#include <string>
namespace paleo::data::detail {
class ProjectStore;
bool objectRevisionSchema(const ProjectStore& store,QString& epoch,bool& ready,std::string& error);
bool ensureObjectRevisionSchema(ProjectStore& store,std::string& error,const std::function<bool()>& cancelled={});
}
