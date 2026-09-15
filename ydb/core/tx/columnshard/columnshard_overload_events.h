#pragma once

#include "columnshard_ev_ids.h"

#include <ydb/core/protos/tx_columnshard.pb.h>
#include <ydb/library/actors/core/events/event_pb.h>

namespace NKikimr::TEvColumnShard {

struct TEvOverloadReady
    : public NActors::TEventPB<
          TEvOverloadReady,
          NKikimrTxColumnShard::TEvOverloadReady,
          EvOverloadReady> {
    TEvOverloadReady() = default;

    explicit TEvOverloadReady(ui64 tabletId, ui64 seqNo) {
        Record.SetTabletID(tabletId);
        Record.SetSeqNo(seqNo);
    }
};

struct TEvOverloadUnsubscribe
    : public NActors::TEventPB<
          TEvOverloadUnsubscribe,
          NKikimrTxColumnShard::TEvOverloadUnsubscribe,
          EvOverloadUnsubscribe> {
    TEvOverloadUnsubscribe() = default;

    explicit TEvOverloadUnsubscribe(ui64 seqNo) {
        Record.SetSeqNo(seqNo);
    }
};

} // namespace NKikimr::TEvColumnShard
