#pragma once
#include "common.h"
#include "index_constructor.h"
#include <ydb/library/actors/util/local_process_key.h>

namespace NActors {

class TActorTypeOperator {
public:
    static constexpr ui32 GetMaxAvailableActorsCount() {
        return TLocalProcessKeyStateIndexLimiter::GetMaxKeysCount();
    }

    template <class TEnum>
    static ui32 GetEnumActivityType(const TEnum enumValue) {
        return TEnumProcessKey<TActorActivityTag, TEnum>::GetIndex(enumValue);
    }

    // out of line: TEnumProcessKey<TActorActivityTag, EInternalActorType> is
    // instantiated (and registers its names at startup) here only, not in
    // every translation unit including this header
    static ui32 GetActorSystemIndex();
    static ui32 GetOtherActivityIndex();
    static ui32 GetActorActivityIncorrectIndex();
};
}
