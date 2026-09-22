#pragma once
#include <yql/essentials/providers/common/codec/yt_arrow_converter_interface/yt_arrow_converter.h>
#include <yql/essentials/providers/common/codec/yt_arrow_converter_interface/yt_arrow_converter_details.h>

#include <yql/essentials/minikql/mkql_node.h>
#include <yql/essentials/public/udf/arrow/block_item.h>
#include <yql/essentials/public/udf/arrow/block_builder.h>

#include <arrow/datum.h>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NYql {

arrow::Datum MakePgScalar(NKikimr::NMiniKQL::TPgType* type, const NKikimr::NUdf::TUnboxedValuePod& value, arrow::MemoryPool& pool);
arrow::Datum MakePgScalar(NKikimr::NMiniKQL::TPgType* type, const NUdf::TBlockItem& value, arrow::MemoryPool& pool);

using TColumnConverter = std::function<std::shared_ptr<arrow::Array>(const std::shared_ptr<arrow::Array>&)>;
TColumnConverter BuildPgColumnConverter(const std::shared_ptr<arrow::DataType>& originalType, NKikimr::NMiniKQL::TPgType* targetType);

std::unique_ptr<IYsonComplexTypeReader> BuildPgYsonColumnReader(const NUdf::TPgTypeDescription& desc);
std::unique_ptr<IYtColumnConverter> BuildPgTopLevelColumnReader(std::unique_ptr<NKikimr::NUdf::IArrayBuilder>&& builder, const NKikimr::NMiniKQL::TPgType* targetType);
} // namespace NYql

namespace NKikimr {
namespace NMiniKQL {

class IBlockAggregatorFactory;
void RegisterPgBlockAggs(THashMap<TString, std::unique_ptr<IBlockAggregatorFactory>>& registry);

} // namespace NMiniKQL
} // namespace NKikimr

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
