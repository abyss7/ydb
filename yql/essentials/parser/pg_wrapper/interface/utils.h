#pragma once

#include <yql/essentials/public/udf/udf_data_type.h>
#include <yql/essentials/public/udf/udf_value_builder.h>

#include <util/generic/maybe.h>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NYql {

ui32 ConvertToPgType(NKikimr::NUdf::EDataSlot slot);
TMaybe<NKikimr::NUdf::EDataSlot> ConvertFromPgType(ui32 typeId);

bool ParsePgIntervalModifier(const TString& str, i32& ret);

std::unique_ptr<NUdf::IPgBuilder> CreatePgBuilder();
bool HasPgKernel(ui32 procOid);

ui64 HexEncode(const char* src, size_t len, char* dst);
} // namespace NYql

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
