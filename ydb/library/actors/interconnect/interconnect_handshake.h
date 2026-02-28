#pragma once

#include "interconnect_common.h"
#include "interconnect_stream.h"
#include "types.h"

namespace NActors {
    static constexpr TDuration DEFAULT_HANDSHAKE_TIMEOUT = TDuration::Seconds(5);
    static constexpr ui64 INTERCONNECT_PROTOCOL_VERSION = 2;
    static constexpr ui64 INTERCONNECT_XDC_CONTINUATION_VERSION = 3;
    static constexpr ui64 INTERCONNECT_XDC_STREAM_VERSION = 4;

    using TSocketPtr = TIntrusivePtr<NInterconnect::TStreamSocket>;

    IActor* CreateOutgoingHandshakeActor(TInterconnectProxyCommon::TPtr common, const TActorId& self,
                                         const TActorId& peer, ui32 nodeId, ui64 nextPacket, TString peerHostName,
                                         TSessionParams params);

    IActor* CreateIncomingHandshakeActor(TInterconnectProxyCommon::TPtr common, TSocketPtr socket);

}
