#pragma once

#include <yql/essentials/public/udf/udf_value.h>

#include <util/generic/buffer.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NKikimr {
namespace NMiniKQL {

class TPgType;
class TPagedBuffer;
namespace NDetails {
class TChunkedInputBuffer;
} // namespace NDetails

void PGPackImpl(bool stable, const TPgType* type, const NUdf::TUnboxedValuePod& value, TBuffer& buf);
void PGPackImpl(bool stable, const TPgType* type, const NUdf::TUnboxedValuePod& value, TPagedBuffer& buf);

NUdf::TUnboxedValue PGUnpackImpl(const TPgType* type, TStringBuf& buf);
NUdf::TUnboxedValue PGUnpackImpl(const TPgType* type, NDetails::TChunkedInputBuffer& buf);

void EncodePresortPGValue(TPgType* type, const NUdf::TUnboxedValue& value, TVector<ui8>& output);
NUdf::TUnboxedValue DecodePresortPGValue(TPgType* type, TStringBuf& input, TVector<ui8>& buffer);

ui64 PgValueSize(const NUdf::TUnboxedValuePod& value, i32 typeLen);
ui64 PgValueSize(ui32 pgTypeId, const NUdf::TUnboxedValuePod& value);
ui64 PgValueSize(const TPgType* type, const NUdf::TUnboxedValuePod& value);

} // namespace NMiniKQL
} // namespace NKikimr

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
