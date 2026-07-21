#include "global.h"
#include "actor.h"
#include <ydb/library/services/services.pb.h>

#include <util/generic/refcount.h>
#include <util/generic/singleton.h>
#include <util/string/cast.h>

namespace NKikimr::NTracing {

namespace {
static TAtomicCounter ClientsCounter = 0;
}

// Static factories of TTraceClient (declared in tracing/usage/tracing.h) are
// defined here rather than in `usage`: they are built on top of the TTracing
// singleton below, and defining them in `usage` would make that library depend
// back on `service`, closing a dependency cycle.

TTraceClientGuard TTraceClient::GetClient(const TString& type, const TString& clientId, const TString& parentId) {
    return Singleton<TTracing>()->GetClient(type, clientId, parentId);
}

TTraceClientGuard TTraceClient::GetClientUnique(const TString& type, const TString& clientId, const TString& parentId) {
    return Singleton<TTracing>()->GetClient(type, clientId + "::" + ::ToString(ClientsCounter.Inc()), parentId);
}

TTraceClientGuard TTraceClient::GetLocalClient(const TString& type, const TString& clientId) {
    return Singleton<TTracing>()->GetLocalClient(type, clientId + "::" + ::ToString(ClientsCounter.Inc()));
}

TTraceClientGuard TTraceClient::GetTypeUnique(const TString& type, const TString& parentId) {
    return Singleton<TTracing>()->GetClient(type, type + "::" + ::ToString(ClientsCounter.Inc()), parentId);
}

std::shared_ptr<NKikimr::NTracing::TTraceClient> TTracing::GetClient(const TString& type, const TString& clientId, const TString& parentId) {
    TGuard<TMutex> g(Mutex);
    auto parent = CreateOrGetClient(parentId, "");
    auto client = CreateOrGetClient(clientId, parentId);
    AFL_VERIFY(client->GetParentId() == parentId);
    client->SetType(type);
    parent->RegisterChildren(client);
    return client;
}

std::shared_ptr<NKikimr::NTracing::TTraceClient> TTracing::GetLocalClient(const TString& type, const TString& clientId) {
    auto client = std::make_shared<TTraceClient>(clientId, "");
    client->SetType(type);
    return client;
}

TTracing::TTracing() {
    if (NActors::TlsActivationContext) {
        NActors::TActivationContext::Register(new TRegularTracesCleanerActor());
    }
}

void TTracing::Clean() {
    THashMap<TString, std::shared_ptr<TTraceClient>> idsToRemove;
    {
        TGuard<TMutex> g(Mutex);
        for (auto&& i : Clients) {
            AFL_NOTICE(NKikimrServices::TX_COLUMNSHARD)("name", i.first)("count", i.second.use_count())("children", i.second->CheckChildrenFree());
            if (i.second.use_count() == 1 && i.second->CheckChildrenFree()) {
                idsToRemove.emplace(i.first, i.second);
            }
        }
        for (auto&& i : idsToRemove) {
            Clients.erase(i.first);
        }
    }
    for (auto&& i : idsToRemove) {
        AFL_NOTICE(NKikimrServices::TX_COLUMNSHARD)("event", "dump")("name", i.first)("parent", i.second->GetParentId());
        i.second->Dump();
    }
}

}
