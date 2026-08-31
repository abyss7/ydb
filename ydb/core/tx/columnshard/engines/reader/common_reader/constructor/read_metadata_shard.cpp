#include "read_metadata_shard.h"

#include <ydb/core/kqp/compute_actor/kqp_compute_events.h>
#include <ydb/core/tx/columnshard/columnshard_impl.h>
#include <ydb/core/tx/columnshard/engines/reader/simple_reader/iterator/collections/constructors.h>
#include <ydb/core/tx/columnshard/transactions/locks/read_finished.h>
#include <ydb/core/tx/columnshard/transactions/locks/read_start.h>

namespace NKikimr::NOlap::NReader::NCommon {

TConclusionStatus TReadMetadata::Init(const NColumnShard::TColumnShard* owner, const TReadDescription& readDescription, const bool isPlain) {
    SetPKRangesFilter(readDescription.PKRangesFilter);
    InitShardingInfo(readDescription.TableMetadataAccessor);
    TxId = readDescription.TxId;
    LockId = readDescription.LockId;
    auto lockNodeId = readDescription.LockNodeId;
    LockMode = readDescription.LockMode;
    if (LockId) {
        owner->GetOperationsManager().RegisterLock(*LockId, owner->Generation());
        if (lockNodeId.has_value()) {
            owner->SubscribeLockIfNotAlready(LockId.value(), lockNodeId.value());
        }
        LockSharingInfo = owner->GetOperationsManager().GetLockVerified(*LockId).GetSharingInfo();
    }
    if (!owner->GetIndexOptional()) {
        SourcesConstructor = NReader::NSimple::TPortionsSources::BuildEmpty();
        SourcesConstructor->InitCursor(nullptr);
        return TConclusionStatus::Success();
    }

    ITableMetadataAccessor::TSelectMetadataContext context(owner->GetTablesManager(), owner->GetIndexVerified());
    SourcesConstructor = readDescription.TableMetadataAccessor->SelectMetadata(context, readDescription, isPlain);

    if (!SourcesConstructor) {
        return TConclusionStatus::Fail("cannot build sources constructor for " + readDescription.TableMetadataAccessor->GetTablePath());
    }
    if (readDescription.readConflictingPortions) {
        for (auto&& i : SourcesConstructor->GetUncommittedWriteIds()) {
            auto op = owner->GetOperationsManager().GetOperationByInsertWriteIdVerified(i);
            // we do not need to check our own uncommitted writes
            if (op->GetLockId() != *LockId) {
                AddMaybeConflictingWrite(i, op->GetLockId());
            }
        }
    }
    SourcesConstructor->InitCursor(readDescription.GetScanCursorVerified());

    {
        auto customConclusion = DoInitCustom(owner, readDescription);
        if (customConclusion.IsFail()) {
            return customConclusion;
        }
    }

    StatsMode = readDescription.StatsMode;
    DeduplicationPolicy = readDescription.DeduplicationPolicy;
    return TConclusionStatus::Success();
}

void TReadMetadataWithTablet::DoOnReadFinished(NColumnShard::TColumnShard& owner) const {
    auto alreadyAborted = LockId.has_value() && owner.GetOperationsManager().GetLockOptional(*GetLockId()) == nullptr;
    if (!NeedToDetectConflicts() || alreadyAborted) {
        return;
    }

    const ui64 lock = *GetLockId();
    if (GetBreakLockOnReadFinished()) {
        owner.GetOperationsManager().GetLockVerified(lock).SetBroken();
    } else {
        NOlap::NTxInteractions::TTxConflicts conflicts;
        for (auto&& lockIdToCommit : GetConflictingLockIds()) {
            // if lockIdToCommit commits, lock must be broken
            conflicts.Add(lockIdToCommit, lock);
        }
        if (!conflicts.IsEmpty()) {
            auto writer =
                std::make_shared<NOlap::NTxInteractions::TEvReadFinishedWriter>(TableMetadataAccessor->GetPathIdVerified().InternalPathId, conflicts);
            owner.GetOperationsManager().AddEventForLock(owner, lock, writer);
        }
    }
}

void TReadMetadataWithTablet::DoOnBeforeStartReading(NColumnShard::TColumnShard& owner) const {
    if (!NeedToDetectConflicts()) {
        return;
    }

    auto evWriter = std::make_shared<NOlap::NTxInteractions::TEvReadStartWriter>(TableMetadataAccessor->GetPathIdVerified(),
        GetResultSchema()->GetIndexInfo().GetPrimaryKey(), GetPKRangesFilterPtr(), GetMaybeConflictingLockIds());
    owner.GetOperationsManager().AddEventForLock(owner, *LockId, evWriter);
}

void TReadMetadataWithTablet::DoOnReplyConstruction(const ui64 tabletId, NKqp::NInternalImplementation::TEvScanData& scanData) const {
    if (LockSharingInfo) {
        NKikimrDataEvents::TLock lockInfo;
        lockInfo.SetLockId(LockSharingInfo->GetLockId());
        lockInfo.SetGeneration(LockSharingInfo->GetGeneration());
        lockInfo.SetDataShard(tabletId);
        lockInfo.SetCounter(LockSharingInfo->GetInternalGenerationCounter());
        TableMetadataAccessor->GetPathIdVerified().SchemeShardLocalPathId.ToProto(lockInfo);
        lockInfo.SetHasWrites(LockSharingInfo->HasWrites());
        if (LockSharingInfo->IsBroken()) {
            scanData.LocksInfo.BrokenLocks.emplace_back(std::move(lockInfo));
        } else {
            scanData.LocksInfo.Locks.emplace_back(std::move(lockInfo));
        }
    }
}

}   // namespace NKikimr::NOlap::NReader::NCommon
