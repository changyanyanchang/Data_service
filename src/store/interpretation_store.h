#pragma once
#include "project_store.h"
#include <QJsonDocument>

namespace paleo::data::detail::interpretation {
bool ensureSchema(ProjectStore& store, std::string& error);
bool projectId(ProjectStore& store, std::string& result, std::string& error);
QString newId();
QString now();
QString digest(const QByteArray& value);
QByteArray bytes(const QJsonObject& value);
bool parse(const QByteArray& payload, const QString& expectedDigest, QJsonObject& result, std::string& error);
QJsonObject encode(const CorrelationInputSnapshot& value);
QJsonObject encode(const InterpretationDocument& value);
QJsonObject encode(const InterpretationTemplate& value);
QJsonObject encode(const InterpretationClustering& value);
bool decode(const QJsonObject& value, CorrelationInputSnapshot& result, std::string& error);
bool decode(const QJsonObject& value, InterpretationDocument& result, std::string& error);
bool decode(const QJsonObject& value, InterpretationTemplate& result, std::string& error);
bool decode(const QJsonObject& value, InterpretationClustering& result, std::string& error);
// Storage manifests reference immutable snapshots and content-addressed numerical
// arrays. Call encodeStored within the revision publication transaction.
bool encodeStored(ProjectStore& store, const InterpretationDocument& value, QJsonObject& result, std::string& error);
bool decodeStored(ProjectStore& store, const QJsonObject& value, InterpretationDocument& result, std::string& error);
bool validate(const InterpretationDocument& value, std::string& error);
bool validate(const InterpretationTemplate& value, std::string& error);
bool validate(const InterpretationClustering& value, std::string& error);
}
