#include "operations/write_task.h"

#include "columnshard_impl.h"
#include <ydb/core/tx/columnshard/operations/write_data.h>
#include <ydb/core/tx/columnshard/overload_manager/overload_manager_service.h>
#include <ydb/core/tx/columnshard/tracing/probes.h>
#include <ydb/core/tx/columnshard/tracing/write_orbit.h>
#include <ydb/core/tx/data_events/write_data.h>

namespace NKikimr::NColumnShard {

LWTRACE_USING(YDB_CS);

namespace {
TString OverloadReason(const EOverloadStatus status) {
    switch (status) {
        case EOverloadStatus::SmallBlobsQuota:
            return "The small blobs quota has been exhausted. Please wait for compaction to finish or delete unused data.";
        case EOverloadStatus::OverloadMetadata:
            return "The index metadata limit has been exceeded. Please wait for compaction to finish or delete unused data.";
        case EOverloadStatus::OverloadCompaction:
            return "The compaction queue is overloaded. Please wait for compaction to finish or reduce the database load.";
        case EOverloadStatus::Disk:
            return "The disk quota has been exhausted. Please increase the available database disk resources or delete unused data.";
        case EOverloadStatus::ShardTxInFly:
            return "The local transaction limit has been exceeded. Please add more resources or reduce the database load.";
        case EOverloadStatus::ShardWritesInFly:
            return "The limit on the number of in-flight write requests to a shard has been exceeded. Please add more resources or reduce the "
                   "database load.";
        case EOverloadStatus::ShardWritesSizeInFly:
            return "The limit on the total size of in-flight write requests to the shard has been exceeded. Please add more resources or reduce "
                   "the database load.";
        case EOverloadStatus::RejectProbability:
            return "The local database is overloaded. Please add more resources or reduce the database load.";
        case EOverloadStatus::None:
            return {};
    }
}

}   // namespace

bool TColumnShard::ExecuteWriteTask(const TWriteTask& task, const TActorContext& ctx) {
    Counters.GetCSCounters().WritingCounters->OnWritingTaskDequeue(ctx.Monotonic() - task.Created);
    if (task.Orbit) {
        LWTRACK(WriteDequeued, *task.Orbit, task.PathId.GetInternalPathId().GetRawValue(), TabletID(), task.TxId, task.Cookie, ctx.Monotonic() - task.Created);
    }

    OperationsManager->RegisterLock(task.LockId, Generation());
    SubscribeLockIfNotAlready(task.LockId, task.LockNodeId);
    auto writeOperation =
        OperationsManager->CreateWriteOperation(task.PathId, task.LockId, task.Cookie, task.GranuleShardingVersionId, task.ModificationType, task.IsBulk);

    YDB_LOG_DEBUG_COMP(NKikimrServices::TX_COLUMNSHARD_WRITE, "",
        {"writingSize", task.ArrowData->GetSize()},
        {"operationId", writeOperation->GetIdentifier()},
        {"inFlight", NOverload::TOverloadManagerServiceOperator::GetShardWritesInFly()},
        {"sizeInFlight", NOverload::TOverloadManagerServiceOperator::GetShardWritesSizeInFly()});

    YDB_LOG_VERIFY(writeOperation);
    writeOperation->SetBehaviour(task.Behaviour);
    writeOperation->SetDeadline(task.GetDeadline());
    const auto applyToMvccSnapshot = task.MvccSnapshot.Valid() ? task.MvccSnapshot : GetMaxReadVersionForSchema(task.Schema->GetVersion());
    NOlap::TWritingContext wContext(TabletID(), SelfId(), task.Schema, StoragesManager,
        Counters.GetIndexationCounters().SplitterCounters, Counters.GetCSCounters().WritingCounters, applyToMvccSnapshot, task.LockId,
        task.LockMode, writeOperation->GetActivityChecker(), task.Behaviour == EOperationBehaviour::NoTxWrite, BufferizationPortionsWriteActorId,
        task.IsBulk, !TablesManager.GetPrimaryIndexSafe().AreAllPortionsLoaded(task.PathId.GetInternalPathId()));
    // We don't need to split here portions by the last level
    // task.ArrowData->SetSeparationPoints(GetIndexAs<NOlap::TColumnEngineForLogs>().GetGranulePtrVerified(task.PathId.InternalPathId)->GetBucketPositions());
    writeOperation->Start(*this, task.ArrowData, task.SourceId, wContext, task.Orbit, task.TxId, task.ReceivedAt);
    return true;
}

void TColumnShard::AbortWriteTask(
    const TWriteTask& task, const TString& reason, const TActorContext& ctx, const NKikimrDataEvents::TEvWriteResult::EStatus& status) {
    if (task.Orbit) {
        TrackWriteFailed(*task.Orbit, task.PathId.GetInternalPathId().GetRawValue(), TabletID(), task.TxId, task.Cookie, task.SourceId.ToString(), "write_queue",
            ToString(status), reason);
    }
    auto result = NEvents::TDataEvents::TEvWriteResult::BuildError(TabletID(), task.TxId, status, reason);
    Counters.GetWritesMonitor()->OnFinishWrite(task.ArrowData->GetSize());
    ctx.Send(task.SourceId, result.release(), 0, task.Cookie);
}

void TColumnShard::FailWriteTaskByOverload(const TWriteTask& task, const EOverloadStatus overloadStatus, const TActorContext& ctx) {
    YDB_LOG_VERIFY(overloadStatus != EOverloadStatus::None);
    const TString reason = TStringBuilder{} << "Column shard " << TabletID()
                                            << " is overloaded. Reason: " << OverloadReason(overloadStatus);
    if (task.Orbit) {
        TrackWriteFailed(*task.Orbit, task.PathId.GetInternalPathId().GetRawValue(), TabletID(), task.TxId, task.Cookie, task.SourceId.ToString(), "write_queue",
            ToString(NKikimrDataEvents::TEvWriteResult::STATUS_OVERLOADED), reason);
    }
    auto result =
        NEvents::TDataEvents::TEvWriteResult::BuildError(TabletID(), task.TxId, NKikimrDataEvents::TEvWriteResult::STATUS_OVERLOADED, reason);
    Counters.GetWritesMonitor()->OnFinishWrite(task.ArrowData->GetSize());
    // only a timeout answers here, and the writer gives up at the same deadline: a subscription would reach nobody
    OverloadWriteFail(overloadStatus, NEvWrite::TWriteMeta(0, task.PathId, task.SourceId, {}, TGUID::CreateTimebased().AsGuidString(),
                                                 Counters.GetCSCounters().WritingCounters->GetWriteFlowCounters()), task.ArrowData->GetSize(),
        task.Cookie, std::move(result), ctx);
}

}   // namespace NKikimr::NColumnShard
