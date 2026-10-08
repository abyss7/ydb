#pragma once

#include <ydb/core/base/tablet_pipe.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

#include <unordered_map>

namespace NKikimr::NTabletPipe {

class TPipeHelper {
public:
    static IActor* CreateClient(const TActorId& owner, ui64 tabletId, const TClientConfig& config = TClientConfig()) {
        return NKikimr::NTabletPipe::CreateClient(owner, tabletId, config);
    }

    static void SendData(const TActorContext& ctx, const TActorId& clientId, IEventBase* payload, ui64 cookie = 0, NWilson::TTraceId traceId = {}) {
        return NKikimr::NTabletPipe::SendData(ctx, clientId, payload, cookie, std::move(traceId));
    }
};

} // namespace NKikimr::NTabletPipe
