#pragma once
#include "meta.h"

#include <ydb/core/tx/columnshard/engines/scheme/index_info.h>

namespace NKikimr::NOlap::NIndexes::NCountMinSketch {

std::shared_ptr<TIndexMeta> GetIndexMeta(const TIndexInfo& indexInfo, const std::set<ui32>& columnIds);

}   // namespace NKikimr::NOlap::NIndexes::NCountMinSketch
