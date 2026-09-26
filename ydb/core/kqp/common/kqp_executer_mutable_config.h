#pragma once

#include <ydb/core/protos/table_service_config.pb.h>

#include <util/generic/ptr.h>

#include <atomic>

namespace NKikimr::NKqp {

struct TExecuterMutableConfig : public TAtomicRefCount<TExecuterMutableConfig>{
    std::atomic<bool> EnableRowsDuplicationCheck = false;
    std::atomic<bool> VerboseMemoryLimitException = false;
    std::atomic<i32> RuntimeParameterSizeLimit = 0;

    void ApplyFromTableServiceConfig(const NKikimrConfig::TTableServiceConfig& tableServiceConfig) {
        EnableRowsDuplicationCheck.store(tableServiceConfig.GetEnableRowsDuplicationCheck());
        VerboseMemoryLimitException.store(tableServiceConfig.GetResourceManager().GetVerboseMemoryLimitException());
        RuntimeParameterSizeLimit.store(tableServiceConfig.GetExtractPredicateParameterListSizeLimit());
    }
};

} // namespace NKikimr::NKqp
