#pragma once

#include "task.h"

#include <ydb/core/base/events.h>

#include <ydb/library/accessor/accessor.h>
#include <ydb/library/actors/core/events/event_local.h>
#include <ydb/library/actors/core/events/events.h>

namespace NKikimr::NOlap::NResourceBroker::NSubscribe {

// NB: own private event space instead of reusing NColumnShard::TEvPrivate. That
// would force including columnshard_private_events.h, which transitively pulls the
// whole engine/schemeshard/sharding graph and re-creates a dependency cycle. This
// is a purely local event of the NSubscribe actor, so an ES_PRIVATE id is enough.
struct TEvPrivate {
    enum EEv {
        EvStartTask = EventSpaceBegin(TKikimrEvents::ES_PRIVATE),
        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_PRIVATE)");
};

class TEvStartTask: public NActors::TEventLocal<TEvStartTask, TEvPrivate::EvStartTask> {
private:
    YDB_READONLY_DEF(std::shared_ptr<ITask>, Task);

public:
    explicit TEvStartTask(std::shared_ptr<ITask> task)
        : Task(task) {
    }
};

}   // namespace NKikimr::NOlap::NResourceBroker::NSubscribe
