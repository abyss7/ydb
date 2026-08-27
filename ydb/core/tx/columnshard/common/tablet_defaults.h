#pragma once
#include <util/datetime/base.h>
#include <util/system/types.h>

namespace NKikimr::NColumnShard {

// Лёгкий заголовок с дефолтами таблета, вынесенными из TSettings (columnshard_impl.h),
// чтобы потребители (hooks/abstract) брали константы, не включая тяжёлый
// columnshard_impl.h (тот тянет engines/arrow и замыкал цикл
// blobs_action:abstract -> hooks/abstract -> columnshard). TSettings в
// columnshard_impl.h реэкспортит их (алиасами), поэтому TSettings::X продолжает работать.
struct TSettingsDefaults {
    static constexpr TDuration GuaranteeIndexationInterval = TDuration::Seconds(10);
    static constexpr TDuration DefaultStatsReportInterval = TDuration::Seconds(10);
    static constexpr i64 GuaranteeIndexationStartBytesLimit = (i64)5 * 1024 * 1024 * 1024;
};

}
