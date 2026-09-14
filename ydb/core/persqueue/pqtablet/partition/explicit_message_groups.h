#pragma once

#include <ydb/core/persqueue/events/internal.h>
#include <ydb/core/persqueue/public/utils.h>
#include <ydb/core/protos/pqconfig.pb.h>

namespace NKikimr::NPQ {

// Builds a partition's explicit message groups out of the tablet's bootstrap
// config. Lives in partition/ rather than in pqtablet/pq_impl.cpp, where it used
// to be defined: partition.cpp needs it as well, and it reached the definition
// through a hand-written local declaration, which made the partition library
// depend on the tablet library and closed a dependency cycle.
TEvPQ::TMessageGroupsPtr CreateExplicitMessageGroups(const NKikimrPQ::TBootstrapConfig& bootstrapCfg,
                                                     const NKikimrPQ::TPartitions& partitionsData,
                                                     const TPartitionGraph& graph,
                                                     ui32 partitionId);

}
