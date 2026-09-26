#pragma once

#include <ydb/core/tx/columnshard/columnshard_private_ev_ids.h>
#include <ydb/library/accessor/accessor.h>

#include <ydb/library/actors/core/event_local.h>

namespace NKikimr::NOlap::NBlobOperations::NRead {

class ITask;

class TEvStartReadTask: public NActors::TEventLocal<TEvStartReadTask, NColumnShard::NPrivateEvIds::EvStartReadTask> {
private:
    YDB_READONLY_DEF(std::shared_ptr<ITask>, Task);
public:

    explicit TEvStartReadTask(std::shared_ptr<ITask> task)
        : Task(task) {
    }

};


}
