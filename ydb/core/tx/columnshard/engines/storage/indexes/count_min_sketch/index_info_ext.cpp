#include "index_info_ext.h"

namespace NKikimr::NOlap::NIndexes::NCountMinSketch {

std::shared_ptr<TIndexMeta> GetIndexMeta(const TIndexInfo& indexInfo, const std::set<ui32>& columnIds) {
    for (auto&& i : indexInfo.GetIndexes()) {
        if (i.second->GetClassName() != TIndexMeta::GetClassNameStatic()) {
            continue;
        }
        auto index = static_pointer_cast<TIndexMeta>(i.second.GetObjectPtr());
        if (index->GetColumnIds() == columnIds) {
            return index;
        }
    }
    return nullptr;
}

}   // namespace NKikimr::NOlap::NIndexes::NCountMinSketch
