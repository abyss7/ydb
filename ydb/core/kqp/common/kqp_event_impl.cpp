#include "kqp.h"

#include <ydb/core/base/path.h>
#include <ydb/core/util/proto_duration.h>

#include <ydb/library/aclib/user_context.h>

namespace NKikimr::NKqp {

TEvKqp::TEvQueryRequest::TEvQueryRequest(
    NKikimrKqp::EQueryAction queryAction,
    NKikimrKqp::EQueryType queryType,
    TActorId requestActorId,
    const std::shared_ptr<NGRpcService::IRequestCtxMtSafe>& ctx,
    const TString& sessionId,
    TString&& yqlText,
    TString&& queryId,
    const ::Ydb::Table::TransactionControl* txControl,
    const ::google::protobuf::Map<TProtoStringType, ::Ydb::TypedValue>* ydbParameters,
    const ::Ydb::Table::QueryStatsCollection::Mode collectStats,
    const ::Ydb::Table::QueryCachePolicy* queryCachePolicy,
    const ::Ydb::Operations::OperationParams* operationParams,
    const TQueryRequestSettings& querySettings,
    const TString& poolId,
    std::optional<NKqp::NFormats::TArrowFormatSettings> arrowFormatSettings)
    : RequestCtx(ctx)
    , RequestActorId(requestActorId)
    , Database(CanonizePath(ctx->GetDatabaseName().GetOrElse("")))
    , SessionId(sessionId)
    , YqlText(std::move(yqlText))
    , QueryId(std::move(queryId))
    , PoolId(poolId)
    , QueryAction(queryAction)
    , QueryType(queryType)
    , TxControl(txControl)
    , YdbParameters(ydbParameters)
    , CollectStats(collectStats)
    , QueryCachePolicy(queryCachePolicy)
    , HasOperationParams(operationParams)
    , QuerySettings(querySettings)
    , ArrowFormatSettings(std::move(arrowFormatSettings))
{

    if (HasOperationParams) {
        OperationTimeout = GetDuration(operationParams->operation_timeout());
        if (QuerySettings.UseCancelAfter) {
            CancelAfter = GetDuration(operationParams->cancel_after());
        }
    }

    NACLib::TUserContextBuilder builder;
    auto token = GetUserToken();
    if (token != nullptr) {
        builder.WithUserSID(token->GetUserSID());
    }
    if (ctx->GetWilsonTraceId()) {
        builder.WithUserTraceId(ctx->GetWilsonTraceId());
    }

    UserCtx = builder.Build();
}

TEvKqp::TEvQueryRequest::TEvQueryRequest(TIntrusivePtr<NACLib::TUserContext> userCtx) : TEvQueryRequest()
{
    UserCtx = userCtx;
    if (userCtx != nullptr) {
        NACLib::TUserToken::TUserTokenInitFields fields {
            .UserSID = userCtx->GetUserSID()
        };
        Token_ = new NACLib::TUserToken(fields);
    }
}

TIntrusivePtr<NACLib::TUserContext> TEvKqp::TEvQueryRequest::GetUserCtx()
{
    if (UserCtx != nullptr) {
        return UserCtx;
    }

    NACLib::TUserContextBuilder builder;
    auto token = GetUserToken();
    if (token != nullptr) {
        builder.WithUserSID(token->GetUserSID());
    }

    UserCtx = builder.Build();
    return UserCtx;
}

}
