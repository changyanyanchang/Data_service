#pragma once
#include <QSqlQuery>
#include <QString>
#include <QVariantList>
#include <string>
#include "paleo_data/model_changes.h"
namespace paleo::data::detail {
class ProjectStore;
bool ensureModelChangeJournal(ProjectStore& store,std::string& error);
bool executeJournalled(const ProjectStore& store,const QString& sql,const QVariantList& args,
                      std::string& error,QSqlQuery* result);
bool readJournalHead(const ProjectStore&,paleo::data::ModelChangeHead&,std::string&);
QString modelDatabaseFileIdentity(const QString& file,std::string& error);
}
