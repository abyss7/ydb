#include "columnshard_schema.h"
#include "tables_manager.h"

#include <ydb/core/tablet_flat/tablet_flat_executor.h>

namespace NKikimr::NColumnShard {

bool TTablesManager::HasTable(
    const TInternalPathId pathId, const bool withDeleted, const std::optional<NOlap::TSnapshot> minReadSnapshot) const {
    auto it = Tables.find(pathId);
    if (it == Tables.end()) {
        return false;
    }
    if (it->second.IsDropped(minReadSnapshot)) {
        return withDeleted;
    }
    return true;
}


bool TTablesManager::TryFinalizeDropPathOnExecute(NTable::TDatabase& dbTable, const TInternalPathId pathId) const {
    const auto& itTable = Tables.find(pathId);
    AFL_VERIFY(itTable != Tables.end())("problem", "No schema for path")("path_id", pathId);
    
    if (!itTable->second.IsDropped()) {
        return true;
    }

    auto itDrop = PathsToDrop.find(itTable->second.GetDropVersionVerified());
    AFL_VERIFY(itDrop != PathsToDrop.end());
    AFL_VERIFY(itDrop->second.contains(pathId));

    AFL_VERIFY(!GetPrimaryIndexSafe().HasDataInPathId(pathId));
    NIceDb::TNiceDb db(dbTable);
    NColumnShard::Schema::EraseTableInfo(db, pathId); // v0
    for (const auto& unifiedPathId : itTable->second.GetPathIds()) {
        NColumnShard::Schema::EraseTableInfoV1(db, pathId, unifiedPathId.GetSchemeShardLocalPathId());
    }
    for (auto&& tableVersion : itTable->second.GetVersions()) {
        NColumnShard::Schema::EraseTableVersionInfo(db, pathId, tableVersion);
    }
    return true;
}


bool TTablesManager::TryFinalizeDropPathOnComplete(const TInternalPathId pathId) {
    const auto& itTable = Tables.find(pathId);
    AFL_VERIFY(itTable != Tables.end())("problem", "No schema for path")("path_id", pathId);
    AFL_VERIFY(itTable->second.IsDropped());
    {
        auto itDrop = PathsToDrop.find(itTable->second.GetDropVersionVerified());
        AFL_VERIFY(itDrop != PathsToDrop.end());
        AFL_VERIFY(itDrop->second.erase(pathId));
        if (itDrop->second.empty()) {
            PathsToDrop.erase(itDrop);
        }
    }
    AFL_VERIFY(!GetPrimaryIndexSafe().HasDataInPathId(pathId));
    AFL_VERIFY(MutablePrimaryIndex().ErasePathId(pathId));
    for (const auto& unifiedPathId : itTable->second.GetPathIds()) {
        AFL_VERIFY(SchemeShardLocalToInternal.erase(unifiedPathId.GetSchemeShardLocalPathId()));
    }
    Tables.erase(itTable);
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("method", "TryFinalizeDropPathOnComplete")("path_id", pathId)("size", Tables.size());
    return true;
}

}   // namespace NKikimr::NColumnShard
