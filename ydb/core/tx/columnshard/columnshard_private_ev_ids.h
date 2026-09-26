#pragma once

// Lightweight home for the TEvPrivate event-id enum of the column shard (see
// columnshard_ev_ids.h for TEvColumnShard): modules that declare private events
// reference the ids without pulling columnshard_private_events.h, which drags
// the column engine, the writers and the whole shard graph.

#include <ydb/library/actors/core/events.h>

namespace NKikimr::NColumnShard::NPrivateEvIds {

enum EEv {
    EvIndexing = EventSpaceBegin(NActors::TEvents::ES_PRIVATE),
    EvWriteIndex,
    EvScanStats,
    EvReadFinished,
    EvPeriodicWakeup,
    EvReportBaseStatistics,
    EvReportExecutorStatistics,
    EvEviction,
    EvS3Settings,
    EvExport,
    EvForget,
    EvGetExported,
    EvWriteBlobsResult,
    EvStartReadTask,
    EvWriteDraft,
    EvGarbageCollectionFinished,
    EvTieringModified,
    EvStartResourceUsageTask,
    EvNormalizerResult,

    EvWritingPortionsAddDataToBuffer,
    EvWritingPortionsFlushBuffer,

    EvExportCursorSaved,
    EvExportSaveCursor,

    EvTaskProcessedResult,
    EvPingSnapshotsUsage,
    EvWritePortionResult,
    EvStartCompaction,

    EvRegisterGranuleDataAccessor,
    EvUnregisterGranuleDataAccessor,
    EvAskTabletDataAccessors,
    EvAskServiceDataAccessors,
    EvAddPortionDataAccessor,
    EvRemovePortionDataAccessor,
    EvClearCacheDataAccessor,
    EvMetadataAccessorsInfo,
    EvAskColumnData,

    EvRequestFilter,
    EvFilterRequestResourcesAllocated,
    EvFilterConstructionResult,

    EvReportScanDiagnostics,
    EvReportScanIteratorDiagnostics,

    EvBackupExportRecordBatch,
    EvBackupExportRecordBatchResult,
    EvBackupExportState,
    EvBackupExportError,
    
    EvBackupImportRecordBatch,
    EvBackupImportRecordBatchResult,

    EvEnd
};

static_assert(EvEnd < EventSpaceEnd(NActors::TEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE)");

}   // namespace NKikimr::NColumnShard::NPrivateEvIds
