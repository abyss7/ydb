#pragma once
#include <util/system/compiler.h>

namespace NYql {
namespace NUdf {
class TBoxedValue;
} // namespace NUdf
} // namespace NYql

// gn: weak references to the YqlServicePolicy link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_YqlServicePolicy)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif
extern "C" [[noreturn]] void UdfTerminate(const char* message);
extern "C" void UdfRegisterObject(::NYql::NUdf::TBoxedValue* object);
extern "C" void UdfUnregisterObject(::NYql::NUdf::TBoxedValue* object);
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_YqlServicePolicy)
#pragma clang attribute pop
#endif
