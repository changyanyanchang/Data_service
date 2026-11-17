#include "store/project_store.h"
#include "store/bounded_metadata.h"
#include "store/numeric_table_projection.h"

namespace paleo::data {
bool DataService::upgradeNumericTableProjectionSchema(std::string& error,const CancelCheck& cancelled) {
    return detail::ensureNumericTableSchema(impl_->store,error,cancelled);
}
bool DataService::confirmNumericTableProjection(const NumericTableProjection& declaration,const ModelChangeHead& expected,
    const std::string& previous,std::string& revision,std::string& error,const CancelCheck& cancelled) {
    return detail::confirmNumericTable(impl_->store,declaration,expected,previous,revision,error,cancelled);
}
bool DataService::describeNumericTableProjection(long long tableId,NumericTableDescriptor& result,std::string& error,
    const NumericTableReadOptions& options,NumericTableReadStats* stats) const {
    result={};error.clear();NumericTableReadStats usage;NumericTableDescriptor pending;
    const auto perform=[&] {
        if(tableId<=0 || !detail::numericTableOptionsValid(options,error)) {
            if(error.empty())error="NumericTableInvalidRequest";return false;
        }
        if(detail::numericTableStopped(options,error))return false;
        detail::MetadataReadSnapshot snapshot(impl_->store,error);if(!snapshot.active())return false;
        if(!detail::loadNumericTableDescriptor(impl_->store,tableId,pending,options,usage,error) ||
           detail::numericTableStopped(options,error) || !snapshot.finish(error))return false;
        result=std::move(pending);return true;
    };
    const bool ok=perform();if(stats)*stats=usage;return ok;
}
bool DataService::readNumericTableWindow(const NumericTableWindowQuery& query,NumericTableWindow& result,std::string& error,
    const NumericTableReadOptions& options,NumericTableReadStats* stats) const {
    result={};error.clear();NumericTableReadStats usage;
    const bool ok=detail::readNumericTable(impl_->store,query,result,options,usage,error);if(stats)*stats=usage;return ok;
}
}
