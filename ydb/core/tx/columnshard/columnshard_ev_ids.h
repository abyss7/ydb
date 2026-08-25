#pragma once

// Lightweight home for the TEvColumnShard event-id enum. It is split out of
// columnshard.h so low-level modules (e.g. blobs_action/events) can reference the
// ids (which live in the ES_TX_COLUMNSHARD space and must stay stable) without
// pulling columnshard.h, which drags tx.h -> appdata.h and the whole tablet graph
// and creates dependency cycles.

#include <ydb/core/base/events.h>

#include <ydb/library/actors/core/events/events.h>

namespace NKikimr::TEvColumnShard {

enum EEv {
    EvProposeTransaction = EventSpaceBegin(TKikimrEvents::ES_TX_COLUMNSHARD),
    EvCancelTransactionProposal,
    EvProposeTransactionResult,
    EvNotifyTxCompletion,
    EvNotifyTxCompletionResult,
    EvReadBlobRanges,
    EvReadBlobRangesResult,
    EvCheckPlannedTransaction,

    EvWrite = EvProposeTransaction + 256,
    EvRead,
    EvWriteResult,
    EvReadResult,

    EvDeleteSharedBlobs,
    EvDeleteSharedBlobsFinished,

    EvDataSharingProposeFromInitiator,
    EvDataSharingConfirmFromInitiator,
    EvDataSharingAckFinishFromInitiator,
    EvDataSharingStartToSource,
    EvDataSharingSendDataFromSource,
    EvDataSharingAckDataToSource,
    EvDataSharingFinishedFromSource,
    EvDataSharingAckFinishToSource,
    EvDataSharingCheckStatusFromInitiator,
    EvDataSharingCheckStatusResult,
    EvApplyLinksModification,
    EvApplyLinksModificationFinished,
    EvInternalScan,

    EvOverloadReady,
    EvOverloadUnsubscribe,

    EvEnd
};

static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_TX_COLUMNSHARD),
              "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_TX_COLUMNSHARD)");

}   // namespace NKikimr::TEvColumnShard
