#pragma once
#include "paleo_data/numeric_table.h"
#include <string>
namespace paleo::data::detail {
class ProjectStore;
bool numericTableOptionsValid(const NumericTableReadOptions&,std::string&);
bool numericTableStopped(const NumericTableReadOptions&,std::string&);
bool numericTableSchemaReady(const ProjectStore&,bool&,std::string&);
bool ensureNumericTableSchema(ProjectStore&,std::string&,const std::function<bool()>&);
bool confirmNumericTable(ProjectStore&,const NumericTableProjection&,const ModelChangeHead&,const std::string&,
                         std::string&,std::string&,const std::function<bool()>&);
bool loadNumericTableDescriptor(const ProjectStore&,std::int64_t,NumericTableDescriptor&,const NumericTableReadOptions&,
                                NumericTableReadStats&,std::string&);
bool readNumericTable(const ProjectStore&,const NumericTableWindowQuery&,NumericTableWindow&,const NumericTableReadOptions&,
                      NumericTableReadStats&,std::string&);
}
