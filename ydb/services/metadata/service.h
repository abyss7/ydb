#pragma once

#include <ydb/library/actors/core/actorid.h>
#include <ydb/services/metadata/ds_table/config.h>

#include <shared_mutex>

namespace NKikimr::NMetadata::NProvider {

inline NActors::TActorId MakeServiceId(const ui32 nodeId) {
    return NActors::TActorId(nodeId, "SrvcMetaData");
}

class TConfig;

class TServiceOperator {
private:
    friend class TService;
    std::shared_mutex Lock;
    bool EnabledFlag = false;
    TString Path = ".metadata";

    static void Register(const TConfig& config) {
        auto* service = Singleton<TServiceOperator>();
        std::unique_lock<std::shared_mutex> lock(service->Lock);
        service->EnabledFlag = true;
        service->Path = config.GetPath();
    }
public:
    static bool IsEnabled() {
        auto* service = Singleton<TServiceOperator>();
        std::shared_lock<std::shared_mutex> lock(service->Lock);
        return service->EnabledFlag;
    }

    static TString GetPath() {
        auto* service = Singleton<TServiceOperator>();
        std::shared_lock<std::shared_mutex> lock(service->Lock);
        return service->Path;
    }
};

} // namespace NKikimr::NMetadata::NProvider
