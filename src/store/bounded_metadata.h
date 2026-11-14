#pragma once
#include "paleo_data/metadata_read.h"
#include <QStringList>
#include <QVector>
#include <QByteArray>
#include <string>
class QSqlQuery;
namespace paleo::data::detail {
class ProjectStore;
bool metadataBudgetValid(const MetadataReadOptions &options);
bool metadataBudgetAvailable(const ProjectStore &store,std::string &error,const char *prefix);
QString metadataGuard(const QStringList &columns,const MetadataReadOptions &options);
QString guardedMetadataColumns(const QStringList &columns,const QString &guard);
bool metadataTextValues(const QSqlQuery &query,int first,const QStringList &columns,
    const MetadataReadOptions &options,QVector<QByteArray> &values,long long &bytes,std::string &error,const char *prefix);
class MetadataReadSnapshot {
    const ProjectStore &store_;QString name_;bool active_=false;
public:
    MetadataReadSnapshot(const ProjectStore &store,std::string &error);
    ~MetadataReadSnapshot();
    bool active()const{return active_;}
    bool finish(std::string &error);
};
}
