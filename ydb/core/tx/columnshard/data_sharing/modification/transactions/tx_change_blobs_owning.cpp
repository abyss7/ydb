#include "tx_change_blobs_owning.h"
#include <ydb/core/tx/columnshard/data_sharing/modification/events/change_owning.h>
#include <ydb/core/tx/columnshard/data_sharing/modification/tasks/modification.h>

namespace NKikimr::NOlap::NDataSharing {

// Defined here rather than in modification/tasks: it constructs
// TTxApplyLinksModification, so keeping it there made the whole task module
// depend on this one -- and on the tablet behind it.
NKikimr::TConclusion<std::unique_ptr<NKikimr::NTabletFlatExecutor::ITransaction>> TTaskForTablet::BuildModificationTransaction(NColumnShard::TColumnShard* self, const TTabletId initiator, const TString& sessionId, const ui64 packIdx, const std::shared_ptr<TTaskForTablet>& selfPtr) {
    return std::unique_ptr<NTabletFlatExecutor::ITransaction>(new TTxApplyLinksModification(self, selfPtr, sessionId, initiator, packIdx));
}

bool TTxApplyLinksModification::DoExecute(TTransactionContext& txc, const TActorContext&) {
    NActors::TLogContextGuard logGuard = NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD)("tablet_id", Self->TabletID())("tx_state", "execute");
    Task->ApplyForDB(txc, Self->GetStoragesManager()->GetSharedBlobsManager());
    return true;
}

void TTxApplyLinksModification::DoComplete(const TActorContext& /*ctx*/) {
    NActors::TLogContextGuard logGuard = NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD)("tablet_id", Self->TabletID())("tx_state", "complete");
    Task->ApplyForRuntime(Self->GetStoragesManager()->GetSharedBlobsManager());

    auto ev = std::make_unique<NOlap::NDataSharing::NEvents::TEvApplyLinksModificationFinished>(Task->GetTabletId(), SessionId, PackIdx);
    NActors::TActivationContext::AsActorContext().Send(MakePipePerNodeCacheID(false),
        new TEvPipeCache::TEvForward(ev.release(), (ui64)InitiatorTabletId, true), IEventHandle::FlagTrackDelivery);
}

}
