#include "manager.h"

namespace NKikimr::NColumnShard {

void TOperationsManager::CommitTransactionOnExecute(
    TColumnShard& owner, const ui64 txId, NTabletFlatExecutor::TTransactionContext& txc, const NOlap::TSnapshot& snapshot) {
    auto& lock = GetLockFeaturesForTxVerified(txId);
    TLogContextGuard gLogging(
        NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD_TX)("commit_tx_id", txId)("commit_lock_id", lock.GetLockId()));
    TVector<TWriteOperation::TPtr> commited;
    for (auto&& opPtr : lock.GetWriteOperations()) {
        opPtr->CommitOnExecute(owner, txc, snapshot);
        commited.emplace_back(opPtr);
    }

    BreakConflictingTxs(lock, txc);

    OnTransactionFinishOnExecute(commited, lock, txId, txc);
}

void TOperationsManager::CommitTransactionOnComplete(
    TColumnShard& owner, const ui64 txId, const ui64 lockId, const NOlap::TSnapshot& snapshot) {
    TLogContextGuard gLogging(
        NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD_TX)("commit_tx_id", txId)("commit_lock_id", lockId));

    auto& lock = GetLockVerified(lockId);

    TVector<TWriteOperation::TPtr> commited;
    for (auto&& opPtr : lock.GetWriteOperations()) {
        opPtr->CommitOnComplete(owner, snapshot);
        commited.emplace_back(opPtr);
    }
    OnTransactionFinishOnComplete(commited, lock, txId);
}

void TOperationsManager::AbortTransactionOnExecute(TColumnShard& owner, const ui64 txId, const ui64 lockId, NTabletFlatExecutor::TTransactionContext& txc) {
    TLogContextGuard gLogging(
        NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD_TX)("tx_id", txId)("lock_id", lockId));

    auto& lock = GetLockVerified(lockId);
    lock.SetAborted(txId);

    TVector<TWriteOperation::TPtr> aborted;
    for (auto&& opPtr : lock.GetWriteOperations()) {
        opPtr->AbortOnExecute(owner, txc);
        aborted.emplace_back(opPtr);
    }

    OnTransactionFinishOnExecute(aborted, lock, txId, txc);
}

void TOperationsManager::AbortTransactionOnComplete(TColumnShard& owner, const ui64 txId, const ui64 lockId) {
    TLogContextGuard gLogging(
        NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD_TX)("tx_id", txId)("lock_id", lockId));

    auto& lock = GetLockVerified(lockId);
    AFL_VERIFY(lock.IsAborted())("lock_id", lockId)("tx_id", txId);

    TVector<TWriteOperation::TPtr> aborted;
    for (auto&& opPtr : lock.GetWriteOperations()) {
        opPtr->AbortOnComplete(owner);
        aborted.emplace_back(opPtr);
    }

    OnTransactionFinishOnComplete(aborted, lock, txId);
}

}   // namespace NKikimr::NColumnShard
