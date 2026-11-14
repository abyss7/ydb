#include "query.h"

#include <ydb/core/protos/kqp_stats.pb.h>
#include <ydb/library/actors/core/actorsystem.h>

namespace NKikimr::NKqp::NPrivateEvents {

void TEvQueryRequest::SetClientLostAction(TActorId actorId, NActors::TActorSystem* as) {
    if (RequestCtx) {
        RequestCtx->SetFinishAction([actorId, as]() {
            as->Send(actorId, new NGRpcService::TEvClientLost());
            });
    } else if (Record.HasCancelationActor()) {
        auto cancelationActor = ActorIdFromProto(Record.GetCancelationActor());
        NGRpcService::SubscribeRemoteCancel(cancelationActor, actorId, as);
    }
}

void TEvQueryRequest::PrepareRemote() const {
    if (RequestCtx) {
        if (RequestCtx->GetSerializedToken()) {
            Record.SetUserToken(RequestCtx->GetSerializedToken());
        }
        Record.MutableRequest()->SetClientAddress(RequestCtx->GetPeerName());

        Record.MutableRequest()->SetDatabase(Database);
        ActorIdToProto(RequestActorId, Record.MutableCancelationActor());
        ActorIdToProto(RequestActorId, Record.MutableRequestActorId());

        if (auto traceId = RequestCtx->GetTraceId()) {
            Record.SetTraceId(traceId.GetRef());
        }

        if (auto requestType = RequestCtx->GetRequestType()) {
            Record.SetRequestType(requestType.GetRef());
        }

        if (TxControl) {
            Record.MutableRequest()->MutableTxControl()->CopyFrom(*TxControl);
        }

        if (YdbParameters) {
            Record.MutableRequest()->MutableYdbParameters()->insert(YdbParameters->begin(), YdbParameters->end());
        }

        if (QueryCachePolicy) {
            Record.MutableRequest()->MutableQueryCachePolicy()->CopyFrom(*QueryCachePolicy);
        }

        if (CollectStats) {
            Record.MutableRequest()->SetCollectStats(CollectStats);
        }

        if (!YqlText.empty()) {
            Record.MutableRequest()->SetQuery(YqlText);
        }

        if (!QueryId.empty()) {
            Record.MutableRequest()->SetPreparedQuery(QueryId);
        }

        if (!PoolId.empty()) {
            Record.MutableRequest()->SetPoolId(PoolId);
        }

        if (!DatabaseId.empty()) {
            Record.MutableRequest()->SetDatabaseId(DatabaseId);
        }

        if (ArrowFormatSettings) {
            ArrowFormatSettings->ExportToProto(Record.MutableRequest()->MutableArrowFormatSettings());
        }

        Record.MutableRequest()->SetUsePublicResponseDataFormat(true);
        Record.MutableRequest()->SetSessionId(SessionId);
        Record.MutableRequest()->SetAction(QueryAction);
        Record.MutableRequest()->SetType(QueryType);
        Record.MutableRequest()->SetSyntax(QuerySettings.Syntax);
        if (HasOperationParams) {
            Record.MutableRequest()->SetCancelAfterMs(CancelAfter.MilliSeconds());
            Record.MutableRequest()->SetTimeoutMs(OperationTimeout.MilliSeconds());
        }
        Record.MutableRequest()->SetIsInternalCall(RequestCtx->IsInternalCall());
        Record.MutableRequest()->SetOutputChunkMaxSize(QuerySettings.OutputChunkMaxSize);
        Record.MutableRequest()->SetSchemaInclusionMode(QuerySettings.SchemaInclusionMode);
        Record.MutableRequest()->SetResultSetFormat(QuerySettings.ResultSetFormat);

        RequestCtx.reset();
    }
}

TEvQueryResponse::TEvQueryResponse() = default;

TEvQueryResponse::TEvQueryResponse(TIntrusivePtr<NActors::TProtoArenaHolder> arena)
    : TEventPBBase(arena ? std::move(arena) : MakeIntrusive<NActors::TProtoArenaHolder>())
{}

TEvQueryResponse::~TEvQueryResponse() = default;

} // namespace NKikimr::NKqp::NPrivateEvents
