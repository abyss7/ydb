#pragma once

#include "defs.h"

#include <ydb/library/actors/core/actor.h>

namespace NKikimr {
namespace NKesus {

IActor* CreateKesusProxyActor(const TActorId& meta, ui64 tabletId, const TString& kesusPath);

}
}
