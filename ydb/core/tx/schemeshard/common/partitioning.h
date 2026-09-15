#pragma once
#include <ydb/core/protos/flat_scheme_op.pb.h>

#include <util/system/types.h>

namespace NKikimr::NSchemeShard {

inline ui32 ShardsToCreate(const NKikimrSchemeOp::TTableDescription& descr) {
    if (descr.HasUniformPartitionsCount()) {
        return descr.GetUniformPartitionsCount();
    } else {
        return descr.SplitBoundarySize() + 1;
    }
}

}
