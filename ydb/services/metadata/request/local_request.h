#pragma once

#include <library/cpp/threading/future/future.h>

#include <util/generic/string.h>

namespace NActors {
class TActorSystem;
}

namespace NKikimr::NMetadata::NRequest {

// Local call of a table/scheme service RPC. Instantiated in request_actor.cpp
// for the request/response pairs of the dialogs from common.h.
template <class TRequest, class TResponse>
NThreading::TFuture<TResponse> DoLocalRequest(TRequest&& request, const TString& database, const TString& token, NActors::TActorSystem* actorSystem);

}
