#include "abstract.h"

// Вместо тяжёлого columnshard_impl.h (тянет engines/arrow и замыкал цикл
// blobs_action:abstract -> hooks/abstract -> columnshard) берём только нужное:
// дефолты таблета из лёгкого common/tablet_defaults.h и TPortionInfo из
// portion_info.h (используется header-only: HasRuntimeFeature inline,
// GetPortionType виртуальный — линк-символов engines/portions нет).
#include <ydb/core/tx/columnshard/common/tablet_defaults.h>
#include <ydb/core/tx/columnshard/engines/portions/portion_info.h>

namespace NKikimr::NYDBTest {

TDuration ICSController::GetGuaranteeIndexationInterval() const {
    const TDuration defaultValue = NColumnShard::TSettingsDefaults::GuaranteeIndexationInterval;
    return DoGetGuaranteeIndexationInterval(defaultValue);
}

TDuration ICSController::GetPeriodicWakeupActivationPeriod() const {
    const TDuration defaultValue = TDuration::MilliSeconds(GetConfig().GetPeriodicWakeupActivationPeriodMs());
    return DoGetPeriodicWakeupActivationPeriod(defaultValue);
}

TDuration ICSController::GetStatsReportInterval() const {
    const TDuration defaultValue = NColumnShard::TSettingsDefaults::DefaultStatsReportInterval;
    return DoGetStatsReportInterval(defaultValue);
}

ui64 ICSController::GetGuaranteeIndexationStartBytesLimit() const {
    const ui64 defaultValue = NColumnShard::TSettingsDefaults::GuaranteeIndexationStartBytesLimit;
    return DoGetGuaranteeIndexationStartBytesLimit(defaultValue);
}

bool ICSController::CheckPortionForEvict(const NOlap::TPortionInfo& portion) const {
    return portion.HasRuntimeFeature(NOlap::TPortionInfo::ERuntimeFeature::Optimized) && portion.GetPortionType() == NOlap::EPortionType::Compacted;
}

bool ICSController::CheckPortionsToMergeOnCompaction(const ui64 memoryAfterAdd, const ui32 /*currentSubsetsCount*/) {
    return memoryAfterAdd > GetConfig().GetMemoryLimitMergeOnCompactionRawData();
}

}
