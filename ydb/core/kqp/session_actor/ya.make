LIBRARY()

SRCS(
    kqp_response.cpp  # gn: response
    kqp_session_actor.cpp
    kqp_worker_actor.cpp
    kqp_worker_common.cpp  # gn: worker_common
    kqp_query_state.cpp
    kqp_query_stats.cpp
    kqp_temp_tables_manager.cpp
)
# gn: worker_common headers kqp_worker_common.h

PEERDIR(
    ydb/core/docapi
    ydb/core/kqp/common
    ydb/core/kqp/federated_query
    ydb/core/kqp/topics
    ydb/public/sdk/cpp/src/library/operation_id
    ydb/core/tx/schemeshard
)
# gn: worker_common peerdir ydb/core/kqp/session_actor:response

YQL_LAST_ABI_VERSION()

END()
