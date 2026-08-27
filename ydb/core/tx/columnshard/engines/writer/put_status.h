#pragma once

#include <ydb/core/protos/base.pb.h>
#include <util/generic/hash_set.h>
#include <util/generic/vector.h>
#include <memory>

namespace NKikimr::NColumnShard {

class TPutStatus {
public:
    NKikimrProto::EReplyStatus GetPutStatus() const {
        return PutStatus;
    }

    void SetPutStatus(NKikimrProto::EReplyStatus status) {
        PutStatus = status;
    }

    void SetPutStatus(NKikimrProto::EReplyStatus status,
                    THashSet<ui32>&& yellowMoveChannels, THashSet<ui32>&& yellowStopChannels) {
        PutStatus = status;
        YellowMoveChannels = std::move(yellowMoveChannels);
        YellowStopChannels = std::move(yellowStopChannels);
    }

    template <typename T>
    void OnYellowChannels(T* executor) const {
        if (YellowMoveChannels.size() || YellowStopChannels.size()) {
            executor->OnYellowChannels(
                TVector<ui32>(YellowMoveChannels.begin(), YellowMoveChannels.end()),
                TVector<ui32>(YellowStopChannels.begin(), YellowStopChannels.end()));
        }
    }

private:
    NKikimrProto::EReplyStatus PutStatus = NKikimrProto::UNKNOWN;
    THashSet<ui32> YellowMoveChannels;
    THashSet<ui32> YellowStopChannels;
};

// Перенесён из write_controller.h (тот тянет blobs_action/abstract и замыкал цикл
// blobs_action:abstract -> hooks/abstract -> engines/writer -> blobs_action:abstract).
// TBlobPutResult самодостаточен (нужен только TPutStatus), потому живёт здесь, в
// лёгком put_status.h — hooks/abstract берёт его по пути без dep на engines/writer.
class TBlobPutResult: public TPutStatus {
public:
    using TPtr = std::shared_ptr<TBlobPutResult>;

    TBlobPutResult(NKikimrProto::EReplyStatus status,
        THashSet<ui32>&& yellowMoveChannels,
        THashSet<ui32>&& yellowStopChannels)
    {
        SetPutStatus(status, std::move(yellowMoveChannels), std::move(yellowStopChannels));
    }

    TBlobPutResult(NKikimrProto::EReplyStatus status) {
        SetPutStatus(status);
    }
};

}
