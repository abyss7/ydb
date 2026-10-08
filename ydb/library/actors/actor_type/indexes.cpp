#include "indexes.h"

namespace NActors {

ui32 TActorTypeOperator::GetActorSystemIndex() {
    return TEnumProcessKey<TActorActivityTag, EInternalActorType>::GetIndex(EInternalActorType::ACTOR_SYSTEM);
}

ui32 TActorTypeOperator::GetOtherActivityIndex() {
    return TEnumProcessKey<TActorActivityTag, EInternalActorType>::GetIndex(EInternalActorType::OTHER);
}

ui32 TActorTypeOperator::GetActorActivityIncorrectIndex() {
    return TEnumProcessKey<TActorActivityTag, EInternalActorType>::GetIndex(EInternalActorType::INCORRECT_ACTOR_TYPE_INDEX);
}

}
