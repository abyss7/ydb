#pragma once

#include <functional>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NKikimr {
namespace NMiniKQL {

class IComputationNode;
class TCallable;
struct TComputationNodeFactoryContext;

} // namespace NMiniKQL
} // namespace NKikimr

namespace NYql {

std::function<NKikimr::NMiniKQL::IComputationNode*(NKikimr::NMiniKQL::TCallable&,
                                                   const NKikimr::NMiniKQL::TComputationNodeFactoryContext&)>
GetPgFactory();

} // namespace NYql

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
