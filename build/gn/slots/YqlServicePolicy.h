#pragma once

#include "slot.h"

#include <util/system/types.h>

namespace NYql::NUdf {
class TBoxedValue;
}

#define GN_SLOT_YqlServicePolicy(CXX, C) \
    C([[noreturn]], void, UdfTerminate, (const char* message)) \
    C(, void, UdfRegisterObject, (::NYql::NUdf::TBoxedValue* object)) \
    C(, void, UdfUnregisterObject, (::NYql::NUdf::TBoxedValue* object)) \
    C(, void*, UdfAllocateWithSize, (ui64 size)) \
    C(, void, UdfFreeWithSize, (const void* mem, ui64 size)) \
    C(, void*, UdfArrowAllocate, (ui64 size)) \
    C(, void*, UdfArrowReallocate, (const void* mem, ui64 prevSize, ui64 size)) \
    C(, void, UdfArrowFree, (const void* mem, ui64 size))

#define GN_SLOT_GUARD_LIST GN_SLOT_YqlServicePolicy
