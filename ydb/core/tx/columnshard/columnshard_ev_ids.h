#pragma once

#include <ydb/core/base/events.h>
#include <ydb/library/actors/core/events.h>

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

    // local DB backup actor <-> tablet, see local_db_backup/events.h
    EvLocalDbBackupStart,
    EvLocalDbBackupStarted,
    EvLocalDbBackupCancel,
    EvLocalDbBackupProgress,

    EvFindEmptyHistoryIntervalsScanFinished,

    EvEnd
};

static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_TX_COLUMNSHARD), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_TX_COLUMNSHARD)");

}   // namespace NKikimr::TEvColumnShard
