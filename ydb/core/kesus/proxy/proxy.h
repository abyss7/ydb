#pragma once

#include "defs.h"

#include <ydb/library/actors/core/actor.h>

namespace NKikimr {
namespace NKesus {

TActorId MakeKesusProxyServiceId();

IActor* CreateKesusProxyService();

}
}
