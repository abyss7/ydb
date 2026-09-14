#include "explicit_message_groups.h"

#include <ydb/core/persqueue/public/partition_key_range/partition_key_range.h>

#include <util/system/yassert.h>

namespace NKikimr::NPQ {

TEvPQ::TMessageGroupsPtr CreateExplicitMessageGroups(const NKikimrPQ::TBootstrapConfig& bootstrapCfg, const NKikimrPQ::TPartitions& partitionsData, const TPartitionGraph& graph, ui32 partitionId) {
    TEvPQ::TMessageGroupsPtr explicitMessageGroups = std::make_shared<TEvPQ::TMessageGroups>();

    for (const auto& mg : bootstrapCfg.GetExplicitMessageGroups()) {
        TPartitionKeyRange keyRange;
        if (mg.HasKeyRange()) {
            keyRange = TPartitionKeyRange::Parse(mg.GetKeyRange());
        }

        (*explicitMessageGroups)[mg.GetId()] = {0, std::move(keyRange)};
    }

    if (graph) {
        auto* node = graph.GetPartition(partitionId);
        Y_VERIFY_S(node, "Partition " << partitionId << " not found. Known partitions " << graph.DebugString());
        for (const auto& p : partitionsData.GetPartition()) {
            if (node->IsParent(p.GetPartitionId())) {
                for (const auto& g : p.GetMessageGroup()) {
                    auto& group = (*explicitMessageGroups)[g.GetId()];
                    group.SeqNo = std::max(group.SeqNo, g.GetSeqNo());
                }
            }
        }
    }

    return explicitMessageGroups;
}

}
