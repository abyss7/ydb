#include "dynamic_nameserver.h"

#include <ydb/core/protos/blobstorage_distributed_config.pb.h>
#include <ydb/core/protos/config.pb.h>

namespace NKikimr {
namespace NNodeBroker {

TIntrusivePtr<TTableNameserverSetup> BuildNameserverTable(const NKikimrConfig::TStaticNameserviceConfig& nsConfig) {
    auto table = MakeIntrusive<TTableNameserverSetup>();
    for (const auto &node : nsConfig.GetNode()) {
        const ui32 nodeId = node.GetNodeId();
        const TString host = node.HasHost() ? node.GetHost() : TString();
        const ui32 port = node.GetPort();
        const TString resolveHost = node.HasInterconnectHost() ?  node.GetInterconnectHost() : host;
        const TString addr = resolveHost ? TString() : node.GetAddress();
        TNodeLocation location;
        if (node.HasWalleLocation()) {
            location = TNodeLocation(node.GetWalleLocation());
        } else if (node.HasLocation()) {
            location = TNodeLocation(node.GetLocation());
        }
        table->StaticNodeTable[nodeId] = TTableNameserverSetup::TNodeInfo(addr, host, resolveHost, port, location);
    }
    return table;
}

TIntrusivePtr<TTableNameserverSetup> BuildNameserverTable(const NKikimrBlobStorage::TStorageConfig& config) {
    auto table = MakeIntrusive<TTableNameserverSetup>();
    for (const auto &node : config.GetAllNodes()) {
        table->StaticNodeTable[node.GetNodeId()] = TTableNameserverSetup::TNodeInfo(
            TString(), node.GetHost(), node.GetHost(), node.GetPort(), TNodeLocation(node.GetLocation())
        );
    }
    return table;
}

} // NNodeBroker
} // NKikimr
