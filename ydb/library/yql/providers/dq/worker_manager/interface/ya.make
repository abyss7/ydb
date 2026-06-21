LIBRARY()

PEERDIR(
    ydb/library/actors/core
    ydb/library/actors/interconnect
    yql/essentials/utils/log
    ydb/library/yql/dq/common
    yql/essentials/providers/common/metrics
    ydb/library/yql/providers/dq/api/grpc
    ydb/library/yql/providers/dq/api/protos
)

YQL_LAST_ABI_VERSION()

SRCS(
    events.cpp
    worker_info.cpp
    counters.cpp
)

END()
