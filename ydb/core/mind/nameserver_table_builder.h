#pragma once

#include <ydb/library/actors/interconnect/interconnect.h>

namespace NKikimrConfig {
    class TStaticNameserviceConfig;
} // NKikimrConfig

namespace NKikimrBlobStorage {
    class TStorageConfig;
} // NKikimrBlobStorage

namespace NKikimr::NNodeBroker {

TIntrusivePtr<NActors::TTableNameserverSetup> BuildNameserverTable(const NKikimrConfig::TStaticNameserviceConfig& nsConfig);
TIntrusivePtr<NActors::TTableNameserverSetup> BuildNameserverTable(const NKikimrBlobStorage::TStorageConfig& config);

} // NKikimr::NNodeBroker
