#include "metadata_accessor.h"

#include <ydb/core/formats/arrow/accessor/abstract/accessor.h>
#include <ydb/core/formats/arrow/accessor/plain/accessor.h>
#include <ydb/core/tx/conveyor_composite/usage/service.h>

#include <ydb/library/actors/core/log.h>
#include <ydb/library/formats/arrow/simple_arrays_cache.h>

#include <util/folder/path.h>

namespace NKikimr::NOlap {
ITableMetadataAccessor::ITableMetadataAccessor(const TString& tablePath)
    : TablePath(tablePath) {
    AFL_VERIFY(!!TablePath);
}

std::vector<TNameTypeInfo> ITableMetadataAccessor::GetPrimaryKeyInfo(const TVersionedPresetSchemas& vSchemas) const {
    return GetSnapshotSchemaVerified(vSchemas, TSnapshot::Max())->GetIndexInfo().GetPrimaryKeyColumns();
}

const std::shared_ptr<arrow::Schema>& ITableMetadataAccessor::GetPrimaryKeyScheme(const TVersionedPresetSchemas& vSchemas) const {
    return GetSnapshotSchemaVerified(vSchemas, TSnapshot::Max())->GetIndexInfo().GetPrimaryKey();
}

TString ITableMetadataAccessor::GetTableName() const {
    return TFsPath(TablePath).Fix().GetName();
}

}   // namespace NKikimr::NOlap
