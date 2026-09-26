#pragma once

#include "path_id.h"
#include "snapshot.h"

#include <optional>

namespace NKikimr::NTable {
class TDatabase;
}

namespace NKikimr::NOlap {

// The tables of the shard as the column engine changes see them;
// implemented by NColumnShard::TTablesManager.
class ITablesPathsManager {
public:
    virtual ~ITablesPathsManager() = default;
    virtual bool HasTable(const TInternalPathId pathId, const bool withDeleted = false,
        const std::optional<TSnapshot> minReadSnapshot = std::nullopt) const = 0;
    virtual bool TryFinalizeDropPathOnExecute(NTable::TDatabase& dbTable, const TInternalPathId pathId) const = 0;
    virtual bool TryFinalizeDropPathOnComplete(const TInternalPathId pathId) = 0;
};

}   //namespace NKikimr::NOlap
