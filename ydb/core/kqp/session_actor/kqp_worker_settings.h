#pragma once

#include <ydb/core/base/appdata.h>
#include <ydb/core/control/lib/immediate_control_board_impl.h>
#include <ydb/core/control/lib/immediate_control_board_wrapper.h>
#include <ydb/core/kqp/common/kqp_executer_mutable_config.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/protos/config.pb.h>
#include <ydb/core/protos/table_service_config.pb.h>

#include <util/generic/maybe.h>
#include <util/generic/string.h>

namespace NKikimr::NKqp {

struct TKqpWorkerSettings {
    TString Cluster;
    TString Database;
    TMaybe<TString> ApplicationName;
    TMaybe<TString> UserName;
    bool LongSession = false;

    TIntrusivePtr<TExecuterMutableConfig> MutableExecuterConfig;
    NKikimrConfig::TTableServiceConfig TableService;
    NKikimrConfig::TQueryServiceConfig QueryService;

    TControlWrapper MkqlInitialMemoryLimit;
    TControlWrapper MkqlMaxMemoryLimit;

    TKqpDbCountersPtr DbCounters;

    explicit TKqpWorkerSettings(const TString& cluster, const TString& database,
            const TMaybe<TString>& applicationName, const TMaybe<TString>& userName, const TIntrusivePtr<TExecuterMutableConfig> mutableExecuterConfig, const NKikimrConfig::TTableServiceConfig& tableServiceConfig,
            const  NKikimrConfig::TQueryServiceConfig& queryServiceConfig, TKqpDbCountersPtr dbCounters)
        : Cluster(cluster)
        , Database(database)
        , ApplicationName(applicationName)
        , UserName(userName)
        , MutableExecuterConfig(mutableExecuterConfig)
        , TableService(tableServiceConfig)
        , QueryService(queryServiceConfig)
        , MkqlInitialMemoryLimit(2097152, 1, Max<i64>())
        , MkqlMaxMemoryLimit(1073741824, 1, Max<i64>())
        , DbCounters(dbCounters)
    {
        auto& icb = *AppData()->Icb;
        TControlBoard::RegisterSharedControl(
            MkqlInitialMemoryLimit, icb.KQPSessionControls.MkqlInitialMemoryLimit);
        TControlBoard::RegisterSharedControl(
            MkqlMaxMemoryLimit, icb.KQPSessionControls.MkqlMaxMemoryLimit);
    }
};

} // namespace NKikimr::NKqp
