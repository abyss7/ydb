#pragma once

#include <ydb/library/actors/core/defs.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/events/event.h>
#include <ydb/library/actors/core/actorid.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/core/debug/valgrind_check.h>
#include <util/generic/array_ref.h>
#include <util/generic/string.h>

namespace NKikimr {
    // actorlib is organic part of kikimr so we emulate global import by this directive
    using namespace NActors;
}
