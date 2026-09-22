#pragma once

#include <yql/essentials/public/udf/udf_type_builder.h>
#include <yql/essentials/public/udf/arrow/block_item_comparator.h>
#include <yql/essentials/public/udf/arrow/block_item_hasher.h>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NKikimr {
namespace NMiniKQL {

class TPgType;

NUdf::IHash::TPtr MakePgHash(const TPgType* type);
NUdf::ICompare::TPtr MakePgCompare(const TPgType* type);
NUdf::IEquate::TPtr MakePgEquate(const TPgType* type);
NUdf::IBlockItemComparator::TPtr MakePgItemComparator(ui32 typeId);
NUdf::IBlockItemHasher::TPtr MakePgItemHasher(ui32 typeId);

} // namespace NMiniKQL
} // namespace NKikimr

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
