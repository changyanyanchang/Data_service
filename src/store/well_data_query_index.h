#pragma once
#include <string>
#include <QString>
#include <vector>
class QSqlQuery;
namespace paleo::data { struct DataEntry; }
namespace paleo::data::detail {
class ProjectStore;
bool ensureWellDataQueryIndex(ProjectStore& store,bool rebuild,std::string& error);
bool wellDataQueryIndexReady(const ProjectStore& store,std::string& error);
QString wellDataPageSql(const std::string& kind,bool filterWell);
bool finishDataPage(QSqlQuery& query,std::vector<DataEntry>& staged,std::vector<DataEntry>& result,
                    bool& hasMore,std::string& error);
}
