LIBRARY()

SRCS(
    kqp_gateway.cpp  # gn: interface
    kqp_ic_gateway.cpp
    kqp_metadata_loader.cpp
)
# gn: interface headers kqp_gateway.h

PEERDIR(
    ydb/core/actorlib_impl
    ydb/core/base
    ydb/core/kqp/common
    ydb/core/kqp/federated_query
    ydb/core/kqp/federated_query/actors
    ydb/core/kqp/gateway/actors
    ydb/core/kqp/gateway/behaviour/external_data_source
    ydb/core/kqp/gateway/behaviour/resource_pool
    ydb/core/kqp/gateway/behaviour/resource_pool_classifier
    ydb/core/kqp/gateway/behaviour/streaming_query
    ydb/core/kqp/gateway/behaviour/table
    ydb/core/kqp/gateway/behaviour/tablestore
    ydb/core/kqp/gateway/behaviour/view
    ydb/core/kqp/gateway/utils
    ydb/core/kqp/provider
    ydb/core/kqp/query_data
    ydb/core/kqp/topics
    ydb/core/statistics/service
    ydb/core/sys_view/common
    ydb/library/actors/core
    yql/essentials/providers/result/expr_nodes
    ydb/core/grpc_services
)

YQL_LAST_ABI_VERSION()

END()

RECURSE(
    actors
    behaviour
    local_rpc
    utils
)

RECURSE_FOR_TESTS(ut)
