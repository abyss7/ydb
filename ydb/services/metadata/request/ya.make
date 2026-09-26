LIBRARY()

SRCS(
    request_actor.cpp
    request_actor_cb.cpp
    config.cpp  # gn: config
    common.cpp
)
# gn: config headers config.h

PEERDIR(
    library/cpp/threading/future
    ydb/library/actors/core
    ydb/core/base
    ydb/core/grpc_services/local_rpc
    ydb/core/grpc_services/base
    ydb/core/grpc_services
    ydb/library/yql/public/ydb_issue
)

END()
