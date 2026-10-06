#pragma once

#include <ydb/library/actors/core/actorsystem_fwd.h>

namespace NKikimr {

class TTabletStorageInfo;

NActors::IActor* CreateDefaultHive(const NActors::TActorId& tablet, TTabletStorageInfo* info);

} // namespace NKikimr
