#pragma once
#include "paleo_data/service.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QString>

namespace paleo::data::detail {
inline QString q(const std::string& s){return QString::fromUtf8(s.data(),qsizetype(s.size()));}
inline std::string s(const QString& v){return v.toUtf8().toStdString();}
QVariant number(double v);
double value(const QVariant& v);
QByteArray pack(const std::vector<double>& values);
bool unpack(const QByteArray& bytes,std::vector<double>& values,std::string& error);
QString json(const QJsonObject& object);
QString json(const QJsonArray& array);
QJsonObject object(const QString& text);
QJsonObject depthJson(const DepthInfo& depth);
DepthInfo readDepth(const QJsonObject& object);
QJsonArray measurementsJson(const std::vector<Measurement>& values);
std::vector<Measurement> readMeasurements(const QJsonArray& values);
class ProjectStore {
public:
    QSqlDatabase db;
    QString root, name, connection;
    bool readOnly=false;
    ~ProjectStore(){close();}
    bool open(const QString& directory,bool create,const QString& projectName,std::string& error,bool readOnly=false);
    void close();
    bool exec(const QString& sql,const QVariantList& args,std::string& error,QSqlQuery* result=nullptr) const;
    bool begin(std::string& error);
    bool commit(std::string& error);
    void rollback();
    bool saveParsed(long long sourceId,const ParsedFile& file,const ParseOptions& options,std::string& error,const CancelCheck& cancelled={},bool inputNullsResolved=false);
    bool saveCoordinateSystemCandidate(long long sourceId,const std::string& objectKind,long long objectId,const std::string& crs,const std::string& unit,std::string& error);
    bool saveParsedCoordinateSystems(long long sourceId,const ParsedFile& file,std::string& error);
    bool updateProjectCoordinateSystem(std::string& error);
    bool removeSourceObjects(long long sourceId,std::string& error);
    bool ensureWell(const WellInfo& info,long long& id,std::string& error);
    QString absolutePath(const QString& storedPath) const;
};
class Transaction {
    ProjectStore& store_;
public:
    bool active;
    Transaction(ProjectStore& store,std::string& error):store_(store),active(store.begin(error)){}
    ~Transaction(){if(active)store_.rollback();}
    bool commit(std::string& error){if(!active||!store_.commit(error))return false;active=false;return true;}
    Transaction(const Transaction&)=delete;
    Transaction& operator=(const Transaction&)=delete;
};
}

namespace paleo::data {
struct DataService::Impl { detail::ProjectStore store; };
}
