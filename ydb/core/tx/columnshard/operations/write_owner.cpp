#include "write.h"

#include "batch_builder/builder.h"

#include <ydb/core/tx/columnshard/blobs_action/abstract/storages_manager.h>
#include <ydb/core/tx/columnshard/blobs_action/blob_manager_db.h>
#include <ydb/core/tx/columnshard/columnshard_impl.h>
#include <ydb/core/tx/columnshard/engines/column_engine_logs.h>
#include <ydb/core/tx/conveyor_composite/usage/service.h>

namespace NKikimr::NColumnShard {

void TWriteOperation::Start(
    TColumnShard& owner, const NEvWrite::IDataContainer::TPtr& data, const NActors::TActorId& source, const NOlap::TWritingContext& context) {
    Y_ABORT_UNLESS(Status == EOperationStatus::Draft);

    auto writeMeta = std::make_shared<NEvWrite::TWriteMeta>(
        (ui64)WriteId, PathId, source, GranuleShardingVersionId, GetIdentifier(),
        context.GetWritingCounters()->GetWriteFlowCounters());
    writeMeta->SetModificationType(ModificationType);
    writeMeta->SetBulk(IsBulk());
    auto writingAction = owner.StoragesManager->GetInsertOperator()->StartWritingAction(NOlap::NBlobOperations::EConsumer::WRITING_OPERATOR);
    writingAction->SetBulk(IsBulk());
    NEvWrite::TWriteData writeData(writeMeta, data, owner.TablesManager.GetPrimaryIndex()->GetReplaceKey(), std::move(writingAction));
    std::shared_ptr<NConveyor::ITask> task = std::make_shared<NOlap::TBuildBatchesTask>(std::move(writeData), context);
    NConveyorComposite::TInsertServiceOperator::SendTaskToExecute(task);

    Status = EOperationStatus::Started;
}

void TWriteOperation::CommitOnExecute(
    TColumnShard& owner, NTabletFlatExecutor::TTransactionContext& txc, const NOlap::TSnapshot& snapshot) const {
    Y_ABORT_UNLESS(Status == EOperationStatus::Prepared || InsertWriteIds.empty());

    TBlobGroupSelector dsGroupSelector(owner.Info());
    NOlap::TDbWrapper dbTable(txc.DB, &dsGroupSelector);

    for (auto&& i : InsertWriteIds) {
        owner.MutableIndexAs<NOlap::TColumnEngineForLogs>().MutableGranuleVerified(PathId.InternalPathId).CommitPortionOnExecute(txc, i, snapshot);
    }
}

void TWriteOperation::CommitOnComplete(TColumnShard& owner, const NOlap::TSnapshot& /*snapshot*/) const {
    Y_ABORT_UNLESS(Status == EOperationStatus::Prepared || InsertWriteIds.empty());
    for (auto&& i : InsertWriteIds) {
        owner.MutableIndexAs<NOlap::TColumnEngineForLogs>().MutableGranuleVerified(PathId.InternalPathId).CommitPortionOnComplete(
            i, owner.MutableIndexAs<NOlap::TColumnEngineForLogs>());
    }
}

void TWriteOperation::AbortOnExecute(TColumnShard& owner, NTabletFlatExecutor::TTransactionContext& txc) const {
    Y_ABORT_UNLESS(Status != EOperationStatus::Draft);
    StopWriting(TStringBuilder{} << "Transaction was aborted for column shard"  << owner.TabletID() << " and lock id " << LockId);
    TBlobGroupSelector dsGroupSelector(owner.Info());
    NOlap::TDbWrapper dbTable(txc.DB, &dsGroupSelector);

    auto abortSnapshot = owner.GetCurrentSnapshotForInternalModification();
    for (auto&& i : InsertWriteIds) {
        owner.MutableIndexAs<NOlap::TColumnEngineForLogs>().MutableGranuleVerified(PathId.InternalPathId).AbortPortionOnExecute(
            txc, i, abortSnapshot);
    }
}

void TWriteOperation::AbortOnComplete(TColumnShard& owner) const {
    Y_ABORT_UNLESS(Status != EOperationStatus::Draft);
    for (auto&& i : InsertWriteIds) {
        owner.MutableIndexAs<NOlap::TColumnEngineForLogs>().MutableGranuleVerified(PathId.InternalPathId).AbortPortionOnComplete(
            i, owner.MutableIndexAs<NOlap::TColumnEngineForLogs>());
    }
}

}   // namespace NKikimr::NColumnShard
