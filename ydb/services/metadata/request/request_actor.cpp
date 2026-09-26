#include "request_actor.h"

#include <ydb/core/grpc_services/local_rpc/local_rpc.h>

namespace NKikimr::NMetadata::NRequest {

template <class TRequest, class TResponse>
NThreading::TFuture<TResponse> DoLocalRequest(TRequest&& request, const TString& database, const TString& token, NActors::TActorSystem* actorSystem) {
    using TRpcRequest = NGRpcService::TGrpcRequestOperationCall<TRequest, TResponse>;
    return NRpcService::DoLocalRpc<TRpcRequest>(std::move(request), database, token, actorSystem);
}

#define METADATA_LOCAL_REQUEST(TDialog) \
    template NThreading::TFuture<TDialog::TResponse> DoLocalRequest<TDialog::TRequest, TDialog::TResponse>( \
        TDialog::TRequest&&, const TString&, const TString&, NActors::TActorSystem*);

METADATA_LOCAL_REQUEST(TDialogCreatePath)
METADATA_LOCAL_REQUEST(TDialogCreateTable)
METADATA_LOCAL_REQUEST(TDialogAlterTable)
METADATA_LOCAL_REQUEST(TDialogDropTable)
METADATA_LOCAL_REQUEST(TDialogModifyPermissions)
METADATA_LOCAL_REQUEST(TDialogSelect)
METADATA_LOCAL_REQUEST(TDialogCreateSession)
METADATA_LOCAL_REQUEST(TDialogDeleteSession)

#undef METADATA_LOCAL_REQUEST

}
